# limit-order-book

[![CI](https://github.com/athul1810/limit-order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/athul1810/limit-order-book/actions/workflows/ci.yml)

A single-instrument limit order book with price-time priority matching, written in C++17.

This is a from-scratch implementation, not a wrapper around an existing library. The goal
was a correct, readable core: the kind of thing you could explain line by line in an
interview, not a black box.

## What it does

- **Limit orders**: match immediately against the opposite side while prices cross;
  any unfilled remainder rests in the book.
- **Market orders**: sweep the best available prices until filled or the book side is
  empty. Never rest — an unfilled market order is simply discarded.
- **Cancellation**: O(1) average-case removal of a resting order by id.
- **Cancel-replace**: repoint a resting order at a new price and quantity, keeping
  its id. Shrinking at an unchanged price keeps queue position; a reprice or a size
  increase surrenders it, and re-enters the book as a fresh arrival — so repricing
  aggressively enough to cross will trade immediately.
- **Duplicate-id rejection**: a submission reusing the id of an order that is
  still resting is rejected outright, leaving the book untouched. An id becomes
  free again once its order is cancelled or fully filled.
- **Price-time priority**: better price always wins; within the same price level,
  first-in-first-filled.
- **Exact prices**: prices are integer ticks, so equal prices are always equal —
  no float drift splitting a price level in two or leaving a crossed book unmatched.
- **Self-trade prevention**: an incoming order never trades against a resting order
  belonging to the same participant. Two policies — cancel the resting order and let
  the aggressor continue (the default), or cancel the aggressor.
- **Multiple instruments**: a `MatchingEngine` routes orders to a book per symbol.
- **Persistence**: an append-only write-ahead log of every request, replayable to
  rebuild engine state exactly. The CLI is durable across restarts.
- **Compaction**: a point-in-time snapshot bounds both log size and recovery time,
  turning recovery into "load the snapshot, replay only what came after it". Runs on
  demand or automatically, on a record-count or wall-clock trigger.
- **Network access**: a length-prefixed binary protocol over TCP, served by a
  single-threaded `poll()` loop that multiplexes every client onto one matching thread.
- **Authentication**: an optional shared token, checked on every connection before
  anything else is accepted. A wrong token closes the connection; no token configured
  at all leaves it off entirely, and the server refuses to bind anywhere but loopback
  until one is set.
- **Market data**: subscribe to a symbol over the same connection and get a snapshot
  of its current best bid/ask, then a push after every subsequent trade or resting
  order that touches it, with a sequence number for detecting a missed one.

## Design

- `Price` is an `int64_t` count of ticks (100 ticks to one currency unit), not a
  `double`. The book compares prices for crossing and uses them as `std::map` keys,
  so both operations depend on exact equality. With a `double` they aren't exact:
  `100.00` stepped up ten times by `0.01` is `100.10000000000005`, while the literal
  `100.10` is `100.09999999999999`. That single-bit gap is enough to open a second
  price level for what is economically one price, and to leave a buy and a sell at
  "the same" price sitting crossed in the book without ever matching. The matching
  core performs no floating-point arithmetic at all; decimals are converted at the
  I/O boundary, in the CLI.
- Bids are kept in a `std::map<Price, std::list<Order>, std::greater<Price>>` (best
  bid first), asks in the same structure ordered ascending (best ask first). A price
  level is a `std::list` rather than a `std::deque` or `std::vector` so that cancelling
  an order from the middle of a level doesn't invalidate iterators to any other order.
- An `unordered_map<OrderId, Location>` tracks where every resting order lives (side,
  price, list iterator), which is what makes cancellation O(1) average case instead of
  a linear scan through price levels.
- Matching walks price levels outward from the best price, consuming resting orders
  FIFO within a level, until the incoming order is filled or (for limit orders) the
  next level no longer crosses.
- Submissions return a `SubmitResult` (`accepted`, `unfilled`, and the trades) rather
  than a bare trade vector. `accepted` distinguishes a rejected order from an accepted
  one that simply didn't cross — both produce no trades, but only one changed the book.
  `unfilled` is the remainder, which means different things for the two order types
  because they dispose of it differently: a limit order rests it, a market order throws
  it away. Without it a caller had no way to learn how much of a market order evaporated.
- Cancel-replace is deliberately built on top of cancel + resubmit rather than splicing
  the order between levels. Going through `cancelOrder` is what clears the `locations_`
  entry, and without that the resubmission would be rejected by the duplicate-id check
  as a collision with the very order being modified. The one case that *doesn't* go
  through that path is a size reduction at an unchanged price, which is edited in place
  precisely so it keeps its place in the queue.

- Self-trade prevention has to cancel *something* — letting the trade through would
  produce a wash trade, and there is no third option — so the policy is an explicit
  choice rather than a hidden default. `CancelOldest` favours the aggressor and costs
  the participant the queue position they had earned; `CancelNewest` favours the
  resting order, and importantly does **not** rest the cancelled remainder, since
  doing so would leave the participant sitting crossed against their own book.
- `kNoParticipant` is exempt from self-trade prevention rather than being treated as
  an ordinary id. It means "ownership not modelled", not "everyone is the same
  person" — without the exemption every unattributed order would block every other
  unattributed order.
- `MatchingEngine` takes the symbol on *every* operation, cancel and modify included.
  The tempting alternative is a global order-id → symbol index so `cancel(id)` can
  find its own book, but that index has to stay in step with removals the books make
  on their own — a resting order being filled, or cancelled by self-trade prevention —
  which the engine never observes. It would accumulate entries for orders that no
  longer exist and start rejecting ids that are genuinely free. Carrying the symbol
  keeps cancellation O(1) with nothing to synchronise, and is what FIX does. The
  consequence, stated rather than discovered: order ids are unique per symbol, not
  globally.
- Instruments are registered up front instead of created on first use, so a typo'd
  symbol is a rejected order rather than a silently-opened new instrument.
- The event log stores *requests*, written before they are applied, and stores all of
  them — including the ones that go on to be rejected. Logging only accepted requests
  would mean knowing the outcome before writing, which means applying first and
  logging second, which is precisely the window a write-ahead log exists to close.
  Replaying the rejected ones is harmless because the engine is deterministic: they
  are rejected again and change nothing. That same determinism is why the log needs
  no state snapshot — the request sequence *is* the state.
- `replay()` detaches whatever log is attached to the engine for its duration. A
  restart means replaying a log into an engine that is already logging to that same
  file, and without detaching, every replayed record would be written straight back
  out — doubling the log on every restart.
- Records carry a sequence number and replay checks it is contiguous, so a log torn
  by a process dying mid-write stops recovery at the tear instead of feeding a
  half-parsed record to the engine. `EventLog` therefore has to be told which sequence
  number to continue from when appending; restarting the numbering at zero would
  create a gap that all future replays would stop at.
- A snapshot has to capture resting orders *in queue order*, because time priority
  isn't derivable from anything else in the record — and registered symbols
  separately from their orders, because an instrument with an empty book is still a
  registered instrument. Nothing else is state: trades are outputs, not something the
  engine holds. Re-inserting the orders in snapshot order can't produce trades on the
  way in, because a resting book is never crossed — no bid in it can cross any ask
  in it.
- Compaction writes the snapshot first, renames it into place, and only then discards
  the log. A crash in that window leaves a snapshot at N beside a log that still
  starts at 0, so `replay()` *skips* records below its start sequence rather than
  treating them as a gap. That one decision is what makes truncating the log purely
  an optimisation rather than a step recovery depends on having completed. There is a
  test that kills the process in exactly that window.
- Snapshots are written to a temporary and renamed into place, so a crash mid-write
  can never replace a complete snapshot with half of one. Symbols are serialised in
  sorted order so identical state produces a byte-identical snapshot.

- The wire protocol is length-prefixed because TCP is a byte stream, not a message
  stream: one `read()` can deliver half a message, three messages, or two and a half.
  Every integer is big-endian and written byte by byte — structs are never memcpy'd
  onto the wire, since their layout depends on the compiler's padding and the host's
  endianness, neither of which is something two independent builds have agreed on.
- The payload length is `u32`, not `u16`. A 16-bit length caps a response at roughly
  2000 trades, and one market order sweeping a deep book can exceed that — a limit
  that would only ever surface in production, on the largest and most interesting
  order of the day.
- The server is single-threaded and that is the design, not a shortcut. Matching is
  inherently serial: price-time priority is a claim about a total order over arriving
  orders, so the moment two threads match into one book you need a lock around the
  whole operation and have bought nothing. Concurrency belongs at the I/O boundary,
  which is precisely what multiplexing many sockets onto one matching thread gives.
- Responses are buffered per connection rather than written and assumed sent. A
  partial write is normal, not an error — the kernel buffer fills and the rest goes
  out when the socket is writable again — so `POLLOUT` is only requested when there
  is something pending, or `poll()` would return immediately forever.

## What's deliberately left out (for now)

This is a matching core, not a trading system. Missing on purpose, not by oversight:

- Networking / wire protocol (no FIX, no socket layer)
- Configurable tick size — the tick is a compile-time constant, not a per-instrument
  property, which is fine while there is exactly one instrument
- Durability against machine failure — the log is flushed to the stream on every
  record, so it survives the process dying, but it is not `fsync`'d, so it does not
  survive the machine dying. Closing that needs a real file descriptor, which is out
  of reach of a `std::ostream`
- Encryption on the wire. The token that authentication checks travels in plain text,
  so it is only meaningfully secret on a link nothing else can observe, which loopback
  is and a routable network generally is not. Encrypting the connection itself (TLS,
  most plausibly) is a separate piece of work from checking a credential once it
  arrives, and is still missing
- Rate limiting and account lockout on repeated wrong tokens. A failed attempt closes
  that one connection, but nothing stops a fresh connection from trying again
  immediately, so this is not a defence against a sustained guessing attack
- Depth of book. A market data push carries only the best bid and best ask, not the
  levels behind them; a subscriber sees that the top of the book moved, not why
- Recovering a missed market data message other than by re-subscribing. There is no
  way to ask for "everything since sequence N" -- a gap means re-subscribing for a
  fresh snapshot and picking the sequence back up from there
- Windows. The server uses POSIX sockets and `poll()`
- Concurrency — the book is single-threaded by design; a real engine gets its
  concurrency at the I/O boundary, not inside the matching core itself. Per-symbol
  books do make the obvious sharding possible (one thread per instrument, nothing
  shared), but that would be a change to `MatchingEngine`, not a property it has

## Build and run

Requires a C++17 compiler. No external dependencies.

```bash
# Compile everything directly (no CMake required). Each target only needs the
# sources it actually uses: the CLI doesn't touch the wire protocol, and the
# benchmark doesn't touch persistence recovery at all.
g++ -std=c++17 -O2 -Iinclude src/order_book.cpp src/matching_engine.cpp \
    src/event_log.cpp src/snapshot.cpp src/wire.cpp src/recovery.cpp \
    src/compaction.cpp tests/test_order_book.cpp -o test_runner
g++ -std=c++17 -O2 -Iinclude src/order_book.cpp src/matching_engine.cpp \
    src/event_log.cpp src/snapshot.cpp src/recovery.cpp src/compaction.cpp \
    src/main.cpp -o matching_engine_cli
g++ -std=c++17 -O2 -Iinclude src/order_book.cpp src/matching_engine.cpp \
    src/event_log.cpp src/snapshot.cpp \
    benchmark/benchmark.cpp -o benchmark_runner
g++ -std=c++17 -O2 -Iinclude src/order_book.cpp src/matching_engine.cpp \
    src/event_log.cpp src/snapshot.cpp src/wire.cpp src/server.cpp \
    src/recovery.cpp src/compaction.cpp src/server_main.cpp -o matching_engine_server

# Or with CMake, which tracks each target's sources in CMakeLists.txt so this
# list can't drift the way the commands above can:
mkdir build && cd build && cmake .. && make
```

Run the tests:

```bash
./test_runner
```

Try the interactive CLI:

```bash
./matching_engine_cli
SYMBOL AAPL
LIMIT AAPL SELL 1 100.5 10
LIMIT AAPL BUY 2 100.5 4
MARKET AAPL BUY 3 8
LIMIT AAPL BUY 4 99.0 5
MODIFY AAPL 4 99.5 8
CANCEL AAPL 4
```

Rejections say which rule was hit rather than failing silently:

```
LIMIT NVDA BUY 1 100 5      -> REJECTED: unknown symbol
LIMIT AAPL BUY 4 99.0 5
LIMIT AAPL BUY 4 98.0 5     -> REJECTED: duplicate order id
MODIFY AAPL 4 99.0 0        -> REJECTED: quantity must be greater than zero
```

Pass a file path and the session becomes durable — every request is logged before
it is applied, and a later run replays it:

```bash
./matching_engine_cli book.log
SYMBOL AAPL
LIMIT AAPL SELL 1 100.5 10 7
LIMIT AAPL BUY 2 100.5 4 9
QUIT
```

```bash
./matching_engine_cli book.log
replayed 3 records from book.log
BOOK AAPL
  AAPL: bid=- ask=100.50 resting=1
```

The log is plain text, one record per line, prices in ticks:

```
0 SYMBOL AAPL
1 LIMIT AAPL 1 SELL 10050 10 7
2 LIMIT AAPL 2 BUY 10050 4 9
```

If a log is torn — a process killed mid-write — recovery stops at the tear and the
CLI refuses to append past it rather than producing a log that would silently stop
replaying at the same point forever.

`COMPACT` writes a snapshot beside the log and truncates it, so neither the file nor
recovery time grows with total history:

```
COMPACT
  compacted at sequence 7: snapshot book.log.snapshot, log truncated
```

The next run loads the snapshot and replays only what came after:

```
loaded snapshot: 4 orders, sequence 7
replayed 2 records from book.log
```

Compaction can also run on its own, so nobody has to remember to type `COMPACT`.
Pass a trailing flag or two after the log path:

```bash
./matching_engine_cli book.log --auto-compact-records=5000
./matching_engine_server 9001 book.log --auto-compact-seconds=300
```

`--auto-compact-records=N` compacts once N records have accumulated since the last
compaction; `--auto-compact-seconds=S` compacts once that much time has passed. Both
can be given together, and either trips it. The two entry points differ in when a
wall-clock trigger actually gets checked: the server's `poll()` loop has a genuine
idle tick independent of request traffic, so `--auto-compact-seconds` fires on a
quiet server exactly as promptly as a busy one. The CLI has no such tick while it is
blocked on `std::getline` waiting for the next command, so there it is only checked
after each command runs, meaning a wall-clock trigger there fires on the *next*
command to arrive after the interval has passed, not the instant it passes.

The last trailing number on `LIMIT` and `MARKET` is an optional participant id,
which is what self-trade prevention keys on. Here participant 7 never trades with
themselves, but does trade with participant 9:

```
LIMIT AAPL SELL 1 100.0 5 7
LIMIT AAPL BUY  2 100.0 5 7   -> no trade; the resting sell is cancelled
LIMIT AAPL SELL 3 100.0 5 9   -> trades against 2
```

Run the benchmark:

```bash
./benchmark_runner 1000000
```

It reports two things separately, because they answer different questions:

- **Throughput** — one timer around the whole loop, so per-order clock reads don't
  inflate it.
- **Latency** — every `addLimitOrder` timed individually, against a book already
  warmed to a realistic depth rather than an empty one, reported as p50/p90/p99/p99.9
  and max. Percentiles are nearest-rank with no interpolation: every number printed
  is an observation that actually happened.

The cost of two clock reads with no work between them is measured and printed too.
At these magnitudes it is not a rounding error — it is a meaningful share of the p50,
so it is reported rather than quietly folded in.

Read the numbers with the obvious caveats: this is a single-threaded process on a
general-purpose machine, not pinned to a core, with no isolation from the scheduler
or from allocator behaviour. The tail shows it — the max is routinely three orders of
magnitude above p99.9, which is the OS and the allocator talking, not the matching
logic. The percentiles up to p99.9 are the informative part; treat the max as noise
until it is measured somewhere that can actually control for that.

## Over the network

The server takes orders over TCP instead of stdin, with the same persistence:

```bash
./matching_engine_server 9001 book.log
listening on 127.0.0.1:9001, logging to book.log
```

Messages are length-prefixed and big-endian. The header is 12 bytes — payload length
(`u32`), correlation id (`u32`, echoed back so a client can match replies to requests
without assuming ordering), message type, version, and two reserved bytes — followed
by the payload. `wire.hpp` documents each message's layout.

There's an integration test that speaks the protocol from scratch in Python, over a
real socket, rather than round-tripping through the same C++ codec that wrote it:

```bash
python3 tests/wire_smoke_test.py ./matching_engine_server
```

It starts a server on a free port and checks framing (several messages in one write,
one message split across two writes), self-trade prevention, each rejection reason,
negative prices, a second connection seeing the same book, state surviving a restart,
authentication in both directions, and market data: a subscriber's snapshot, the
unsolicited push after another connection's order, an unsubscribed connection getting
nothing extra, and a stopped one after unsubscribing. It found a real bug the C++
tests could not: `poll()` was being indexed by a
connection count that `accept()` had already grown, so a newly accepted connection
read past the end of the descriptor array and was dropped on the spot.

### Authentication

Set `MATCHING_ENGINE_TOKEN` before starting the server to require it:

```bash
MATCHING_ENGINE_TOKEN=some-shared-secret ./matching_engine_server 9001 book.log
listening on 127.0.0.1:9001 (authentication required), logging to book.log
```

A connection has to send an `Authenticate` message, whose payload is the raw token
and nothing else, before anything else is accepted. Any other request arriving first
is rejected with `NotAuthenticated`, but the connection stays open, since simply not
having authenticated yet is not by itself hostile. A wrong token gets rejected with
`AuthenticationFailed`, and the connection is then closed: there is no rate limiting
here, so leaving it open for repeated guesses would make the connection a free
brute-force oracle. A fresh connection can still try again, which at least costs a
TCP handshake per attempt, not nothing, but not a real defence against a sustained
attack either.

The token comparison is constant-time (it does not short-circuit on the first
mismatching byte), which closes the usual timing side-channel on comparing a secret,
though the check still leaks the two lengths matching or not before that, the
accepted trade-off for a comparison shaped like this one.

`MATCHING_ENGINE_BIND` sets the address to listen on, defaulting to loopback exactly
as before. Binding anywhere else is refused unless `MATCHING_ENGINE_TOKEN` is also
set:

```bash
MATCHING_ENGINE_BIND=0.0.0.0 ./matching_engine_server 9001
cannot listen on 0.0.0.0:9001: refusing to bind 0.0.0.0: authentication is not configured
set MATCHING_ENGINE_TOKEN to allow it.

MATCHING_ENGINE_TOKEN=some-shared-secret MATCHING_ENGINE_BIND=0.0.0.0 ./matching_engine_server 9001
listening on 0.0.0.0:9001 (authentication required)
```

Both come from the environment rather than a command-line flag. A secret has no
business showing up in `ps`, which is where an argv-based flag would put it.

### Market data

Any connection can subscribe to a symbol's public book state, in addition to
whatever orders it is submitting on the same connection:

```
Subscribe "AAPL"  -> a MarketData snapshot: current best bid/ask, sequence 0,
                     no trades (a snapshot isn't itself an event)
```

From then on, that connection gets an unsolicited `MarketData` push after every
accepted limit order, market order, modify or cancel that touches AAPL, on *any*
connection, itself included:

```
someone submits an order on AAPL
  -> MarketData: new best bid/ask, sequence 1, and the trades that just happened
     (empty if the order simply rested without crossing anything)
```

The push and a connection's own `Response` to its own order are two separate
messages, sent to a subscribed, acting connection in that order: `Response` says what
happened to *your* order, `MarketData` says what the book's public state now is.
Everyone subscribed gets the second one, including whoever caused it.

`sequence` is what a gap looks like: a snapshot reports what has already happened, so
the very next push should be exactly one more, and every push after that one more
again. If a push ever arrives out of that order, something was missed, and the fix is
to `Subscribe` again for a fresh snapshot rather than trust what has been received so
far. There is no way to ask for "replay everything since sequence N" -- resubscribing
is the only recovery path.

Two design choices worth stating plainly. First, the trigger for a push is simple on
purpose: any of those four request types succeeding, whether or not it happened to
move the best price or produce a trade. An unfilled market order against an empty
book still triggers a push (an unchanged bid/ask, no trades) rather than the server
trying to detect "did anything actually change," which would be a second condition to
get right for very little benefit. Second, subscribing requires the symbol to already
exist -- `Subscribe` to one that doesn't is rejected the same way trading on it would
be -- so a brand-new `SYMBOL`/`AddSymbol` never itself needs to push anything: nothing
could be subscribed to a symbol that did not exist a moment earlier.

`Unsubscribe` always succeeds, including when not currently subscribed, the same as
`Subscribe` succeeding again while already subscribed just resends the snapshot. Both
are idempotent on purpose, so a client doesn't have to track its own subscription
state just to avoid an error from restating it.

## Tests and CI

```bash
./test_runner                                              # unit tests
python3 tests/wire_smoke_test.py ./matching_engine_server   # protocol, over a real socket
```

CI builds with GCC and Clang on Linux and Clang on macOS, with `-Wpedantic -Werror`,
and runs the unit tests, the wire integration test, a CLI restart-recovery check, and
a short benchmark. Two further jobs run everything under AddressSanitizer and
UndefinedBehaviorSanitizer, and fuzz the wire decoder with libFuzzer. Three toolchains rather than one because that is where a whole
class of bug lives: a standard-library symbol used without including its header
compiles fine under whichever implementation you happened to develop against, and
fails on the other. Adding CI turned up six such cases in this repository.

`tests/fuzz_wire.cpp` targets `FrameReader` and `decodeRequest` — the only code here
that reads bytes chosen by someone else, since anyone who can open a socket feeds them
directly. It has two entry points: `LLVMFuzzerTestOneInput` for libFuzzer, and a
standalone corpus-replay driver so the harness runs under sanitizers on toolchains
that ship no libFuzzer runtime (Apple clang does not).

Two details in that setup are load-bearing, and both are the same kind of trap:

- Sanitizer builds pass `-fno-sanitize-recover=all`. UBSan's default is to print a
  diagnostic and keep going, exiting 0 — so a job built without it stays green while
  reporting real undefined behaviour in its own logs.
- The unit tests use a `CHECK` macro rather than `assert`.   `assert` expands to nothing when `NDEBUG` is defined, and `NDEBUG` is exactly what
  a CMake `Release` build defines — so an assert-based suite compiled in Release
  prints "passed" for every test while verifying nothing. `CHECK` is evaluated in
  every build type.

Both were verified the only way worth trusting: by breaking something on purpose and
confirming the check goes red. Removing a length check from the decoder makes the
fuzzer abort with an ASan `container-overflow` naming the exact line, and
reintroducing the `poll()` indexing bug makes the sanitized server report a
`heap-buffer-overflow` on the first connection — a bug that, uninstrumented, all 60
unit tests passed straight through.

## Editor setup

`cmake -S . -B build` emits `build/compile_commands.json`, which is what lets an
editor's C++ IntelliSense (VSCode's cpptools or clangd) resolve this project's quoted
`#include "order_book.hpp"`-style headers against `-Iinclude`. Without it, every such
include is flagged as an error in the editor even though the code builds fine.
`.vscode/c_cpp_properties.json` and `.vscode/settings.json` point both extensions at
it; run the build once (see below) and reload the window.

## Recovery is shared, not duplicated

`recoverAndOpenLog()` (`recovery.hpp`/`recovery.cpp`) is the one implementation of
"load the snapshot if present, replay the log from its sequence, refuse to proceed
past a torn log, then open the log for continued append starting at the right
sequence number." Both the CLI and the server call it. It used to be ~50 lines
duplicated verbatim in `main.cpp` and `server_main.cpp`, two copies of subtle
persistence logic that could silently drift apart, which is exactly the shape of bug
that goes unnoticed until a fix lands in one copy and not the other.

Compaction itself (`compaction.hpp`/`compaction.cpp`) followed the same rule from the
start rather than learning it the hard way: `compact()` is the one place that writes
a snapshot and truncates the log, and both the manual `COMPACT` command and the
automatic, policy-driven trigger call it. `AutoCompactor` only decides *when*, not
*how*; it has no idea what compaction actually does, so a caller wiring in a new
trigger can never accidentally reimplement the crash-safety ordering (snapshot
written and installed before the log is ever touched) instead of reusing it.

## Roadmap

What would come next with more time:

1. Encryption on the wire, so the token that authentication checks is not sent in
   plain text over anything but loopback
2. Rate limiting on repeated failed authentication attempts
3. Depth of book in a market data push, not just the best bid and ask
4. A way to recover a missed market data message other than re-subscribing from
   scratch
