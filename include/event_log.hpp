#pragma once

#include <cstdint>
#include <iosfwd>

#include "matching_engine.hpp"

namespace matching_engine {

// Append-only write-ahead log of every request submitted to a MatchingEngine,
// and the replay that rebuilds an engine from one.
//
// It logs *requests*, before they are applied, and it logs all of them --
// including the ones that go on to be rejected. That is deliberate, and the
// alternative is tempting enough to be worth ruling out explicitly: logging
// only accepted requests would mean knowing the outcome before writing, which
// means applying first and logging second, which is exactly the window a
// write-ahead log exists to close. A request that crashed the process between
// apply and log would be lost from a log that claimed to be complete.
//
// Replaying rejected requests is harmless because the engine is deterministic:
// the same request sequence produces the same state, so a request that was
// rejected the first time is rejected again, changing nothing. That same
// determinism is what makes the log sufficient as a state snapshot -- there is
// no need to serialise the book itself.
//
// Durability, stated precisely: each record is flushed to the stream, so the
// log survives the process dying. It is NOT fsync'd, so it does not survive
// the machine dying. Closing that gap needs a real file descriptor and fsync,
// which is out of reach of a std::ostream.
//
// Format is one record per line: `<seq> <KIND> <fields...>`, prices in ticks.
// Text rather than binary because a log you can read with `tail` is worth more
// than a compact one at this size. Fields are whitespace-separated, which is
// why symbols may not contain whitespace (MatchingEngine::addSymbol enforces
// this).
class EventLog {
   public:
    // `first_seq` is the sequence number the next record will carry. It must
    // be set when continuing an existing log -- appending records numbered
    // from zero onto a log that already has records would leave a sequence
    // that replay reads as a gap, silently truncating the recovery there.
    // Pass ReplayResult::applied after replaying the existing log.
    explicit EventLog(std::ostream& out, std::uint64_t first_seq = 0)
        : out_(out), next_seq_(first_seq) {}

    void recordAddSymbol(const Symbol& symbol);
    void recordLimitOrder(const Symbol& symbol, OrderId id, Side side, Price price, Quantity quantity,
                          ParticipantId participant);
    void recordMarketOrder(const Symbol& symbol, OrderId id, Side side, Quantity quantity,
                           ParticipantId participant);
    void recordModifyOrder(const Symbol& symbol, OrderId id, Price new_price, Quantity new_quantity);
    void recordCancelOrder(const Symbol& symbol, OrderId id);

    // The sequence number the next record will carry. For a log started from
    // scratch this is also the number of records written so far.
    std::uint64_t nextSequence() const { return next_seq_; }

   private:
    std::ostream& out_;
    std::uint64_t next_seq_;
};

struct ReplayResult {
    std::uint64_t applied = 0;  // records read and applied, in order
    // True if replay stopped before end-of-stream: a malformed record, or a
    // sequence number that didn't follow the previous one. The realistic cause
    // is a process killed mid-write, leaving a torn final line. Everything up
    // to that point is still valid, which is why `applied` is reported rather
    // than the whole replay being failed.
    bool truncated = false;
    std::uint64_t stopped_at_line = 0;  // 1-based; only meaningful when truncated
};

// Rebuilds `engine` by replaying records from `in`, in order.
//
// `first_seq` is where replay begins -- 0 for a full log, or a snapshot's
// next_seq when recovering from one. Records numbered below it are skipped
// rather than treated as an error, and that distinction is what makes
// compaction crash-safe: compaction writes the snapshot before it truncates
// the log, so a crash in between leaves a snapshot at N alongside a log that
// still starts at 0. Skipping lets that recover cleanly, which in turn means
// truncating the log is only ever an optimisation, never a step recovery
// depends on having completed.
//
// Any event log attached to `engine` is detached for the duration and restored
// afterwards. Without that, replaying a log into an engine that is already
// logging would append every replayed record back onto the log, doubling it on
// every restart.
ReplayResult replay(std::istream& in, MatchingEngine& engine, std::uint64_t first_seq = 0);

}  // namespace matching_engine
