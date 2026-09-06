#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "matching_engine.hpp"
#include "tls.hpp"
#include "wire.hpp"

namespace matching_engine {

// A TCP front end for a MatchingEngine, speaking the protocol in wire.hpp.
//
// Single-threaded, on purpose, using poll() to multiplex every connection onto
// one thread. That is not a simplification to be apologised for -- it is the
// architecture the book was designed around. Matching is inherently serial:
// price-time priority is a statement about a total order over arriving orders,
// so the moment two threads match into one book you need a lock around the
// whole operation and have bought nothing. Concurrency belongs at the I/O
// boundary, which is exactly what multiplexing many sockets onto one matching
// thread gives you.
//
// POSIX only (poll, sockets). Windows would need its own implementation.
class OrderServer {
   public:
    // `required_token`, when set, is compared against every Authenticate
    // request on every connection (see wire.hpp's MessageType::Authenticate).
    // No other request is applied to the engine on an unauthenticated
    // connection until one matches. Left unset (the default), authentication
    // is not required at all, and an Authenticate request always trivially
    // succeeds -- useful for a client that always authenticates regardless
    // of whether the server it happens to be talking to requires it.
    //
    // `max_auth_failures` and `auth_lockout_duration` govern rate limiting on
    // repeated wrong tokens: a source (identified by peer IP -- see
    // Connection::peer_ip below) that fails that many Authenticate attempts
    // in a row is locked out for that long, meaning every further
    // Authenticate from it is rejected with RejectReason::RateLimited without
    // even comparing the token, until the lockout expires. Meaningless when
    // `required_token` is unset, since nothing there can ever fail. The
    // defaults are deliberately conservative rather than tuned; a real
    // deployment with actual attack data to look at would want its own
    // numbers.
    static constexpr std::uint32_t kDefaultMaxAuthFailures = 5;
    static constexpr std::chrono::steady_clock::duration kDefaultAuthLockoutDuration =
        std::chrono::seconds(30);

    // `market_data_depth` bounds how many aggregated price levels per side
    // go into every MarketData message (snapshot, push, or resync reply) --
    // see OrderBook::bidLevels/askLevels for what the number means. Kept
    // small by default because a deep book multiplied by every subscribed
    // connection is exactly the cost this bounds.
    //
    // `market_data_history_limit` bounds how many past pushes per symbol
    // ResyncMarketData can still hand back verbatim (see broadcastMarketData
    // and market_data_history_ below) -- how far back a gap can be closed
    // before falling back to a fresh snapshot, the same outcome resubscribing
    // would have produced anyway. Older entries are simply forgotten, not
    // persisted, so this is a bound on memory, not a durability guarantee.
    static constexpr std::size_t kDefaultMarketDataDepth = 5;
    static constexpr std::size_t kDefaultMarketDataHistoryLimit = 200;

    explicit OrderServer(MatchingEngine& engine, std::optional<std::string> required_token = std::nullopt,
                         std::uint32_t max_auth_failures = kDefaultMaxAuthFailures,
                         std::chrono::steady_clock::duration auth_lockout_duration =
                             kDefaultAuthLockoutDuration,
                         std::size_t market_data_depth = kDefaultMarketDataDepth,
                         std::size_t market_data_history_limit = kDefaultMarketDataHistoryLimit)
        : engine_(engine),
          required_token_(std::move(required_token)),
          max_auth_failures_(max_auth_failures),
          auth_lockout_duration_(auth_lockout_duration),
          market_data_depth_(market_data_depth),
          market_data_history_limit_(market_data_history_limit) {}
    ~OrderServer();

    OrderServer(const OrderServer&) = delete;
    OrderServer& operator=(const OrderServer&) = delete;

    // Binds and listens. Port 0 asks the OS for an ephemeral port, which
    // boundPort() then reports -- what tests use to avoid racing over a
    // hard-coded port number.
    //
    // `bind_address` defaults to loopback, matching this server's original,
    // always-safe behaviour. Binding anywhere else requires a token to
    // already be configured (constructor above) -- enforced here rather than
    // left to every caller to remember, since an unauthenticated server
    // reachable from the network is the exact failure this feature exists to
    // prevent.
    bool listenOn(std::uint16_t port, std::string& error, const std::string& bind_address = "127.0.0.1");
    std::uint16_t boundPort() const { return bound_port_; }

    // Serves until stop() is called. Returns the number of requests handled.
    std::uint64_t runUntilStopped();

    // Safe to call from a signal handler: it only stores to an atomic flag.
    void stop() { running_ = false; }

    // Invoked once per poll iteration, after that iteration's I/O has been
    // serviced -- roughly every kPollTimeoutMs at minimum, more often under
    // load. This is the server's only notion of "meanwhile": there is no
    // second thread, so background work (automatic compaction on a time
    // trigger, say) has to be driven from here rather than off a timer.
    // Exceptions are not caught; a throwing hook takes the server down, on
    // the theory that a broken background task should be loud, not silently
    // skipped forever.
    void setIdleHook(std::function<void()> hook) { idle_hook_ = std::move(hook); }

    // Enables TLS: every connection accepted from here on is wrapped in a
    // server-side handshake before any application byte is read from or
    // written to it. Must be called before listenOn() has accepted anything
    // (in practice, before it is called at all) -- a connection already
    // accepted in plaintext cannot be retroactively upgraded.
    //
    // False (with `error` set) if the certificate/key can't be loaded, or if
    // this binary was not built with TLS support at all (CMake's WITH_TLS
    // option; see tls.hpp's tlsSupported()) -- the two are deliberately
    // reported the same way, since both mean "TLS is not available", and a
    // caller's response to either is the same: don't start.
    bool enableTls(const std::string& cert_path, const std::string& key_path, std::string& error);

   private:
    struct Connection {
        int fd = -1;
        FrameReader reader;
        std::vector<std::uint8_t> outbox;
        std::size_t sent = 0;  // how much of outbox has already gone out
        // Meaningless (never consulted) when required_token_ is unset.
        bool authenticated = false;
        // Set once, from getpeername() at accept() time. What rate limiting
        // keys on: a fresh connection is free (a new TCP handshake), but it
        // still arrives from the same address, so tracking failures per
        // connection would let a bad token be retried indefinitely just by
        // reconnecting.
        std::string peer_ip;
        // Symbols this connection has Subscribed to and not since
        // Unsubscribed from. Checked on every broadcastMarketData() call, so
        // membership testing matters more here than insertion order.
        std::unordered_set<Symbol> subscriptions;

        // Null unless this server has TLS enabled, in which case every
        // connection gets one from acceptPending() onward. `tls_state`
        // starts Handshaking for a TLS connection (Established, trivially,
        // for a plaintext one) and `tls_want` records the last direction
        // OpenSSL asked for -- see TlsWant in tls.hpp -- so runUntilStopped
        // knows to also poll for writability on a connection with nothing
        // of its own (yet) queued to send: a raw socket's read always waits
        // on readability and a write on writability, but a TLS handshake
        // step, or occasionally even a post-handshake read or write, can
        // need the opposite direction to make progress. POLLIN itself is
        // always requested regardless (see runUntilStopped), so the only
        // thing tls_want actually needs to add is "also ask for POLLOUT".
        //
        // One shared field for both directions of I/O rather than two, which
        // is not perfectly precise: driveHandshake, serviceReadable and
        // serviceWritable each set it from their own latest OpenSSL result,
        // and only serviceWritable clears it, once its outbox is fully
        // drained. A read that reported wanting a write, immediately
        // followed by a write call that succeeds and clears the flag, could
        // in principle drop that read's own pending want. This is accepted
        // rather than tracked separately: it can only matter for a mid-
        // session renegotiation, which this server never initiates and
        // TLS 1.3 (the default floor is 1.2, but 1.3 is what any current
        // client will actually negotiate) does not have in the first place.
        std::unique_ptr<TlsConnection> tls;
        enum class TlsState { Handshaking, Established } tls_state = TlsState::Established;
        TlsWant tls_want = TlsWant::None;
    };

    void acceptPending();
    // False if the connection should be closed.
    bool serviceReadable(Connection& connection);
    bool serviceWritable(Connection& connection);
    // Advances a connection's TLS handshake by one step, from either a
    // readable or a writable poll event -- either can be the one that moves
    // it forward, since which direction a given handshake step needs is
    // OpenSSL's decision, not this server's. Returns false if the
    // connection should be closed, the same convention as
    // serviceReadable/serviceWritable, since a failed handshake is as fatal
    // to a connection as a decode failure is.
    bool driveHandshake(Connection& connection);
    void closeConnection(std::size_t index);
    // Queues a Response carrying only `reason` (no trades, no unfilled) and
    // counts it toward requests_handled_. Used for the outcomes that never
    // touch the engine: an Authenticate attempt succeeding or failing, a
    // non-Authenticate request arriving before one has, and Unsubscribe
    // (always this, with RejectReason::None -- it is unconditional).
    void queueResponse(Connection& connection, std::uint32_t correlation_id, RejectReason reason);
    // Pushes one MarketData message, carrying `trades`, to every connection
    // subscribed to `symbol` -- called after every accepted LimitOrder,
    // MarketOrder, ModifyOrder or CancelOrder that touched a symbol, whether
    // or not that request happened to produce a trade or move the best
    // price. A simple, uniform trigger ("this request type succeeded")
    // costs an occasional no-op push (an unfilled market order against an
    // empty book, say) in exchange for not having to detect "did the book
    // actually visibly change" as a second, easy-to-get-wrong condition.
    void broadcastMarketData(const Symbol& symbol, const std::vector<Trade>& trades);
    // Builds the "current state, no events" message both a Subscribe and a
    // ResyncMarketData that can't be satisfied from history send back:
    // current best bid/ask and depth, `sequence` at whatever it already is
    // (not incremented -- a snapshot reports what already happened, it isn't
    // itself an event), empty trades, and `correlation_id` echoing whichever
    // request asked for it.
    MarketDataMessage buildSnapshot(const Symbol& symbol, std::uint32_t correlation_id) const;
    // Handles ResyncMarketData: replays buffered pushes after
    // request.since_sequence if market_data_history_ for the symbol still
    // covers that far back, otherwise falls back to buildSnapshot(). Returns
    // false if the connection should be closed (mirrors serviceReadable's
    // own convention), which in practice never happens here -- kept as a
    // bool anyway so this reads the same as every other request handler
    // serviceReadable calls out to.
    bool handleResyncMarketData(Connection& connection, const Request& request);

    // Rate-limiting state for one source: how many Authenticate attempts it
    // has failed in a row since its last success (or since the last time a
    // lockout was imposed), and, once that reaches max_auth_failures_, the
    // point in time the lockout it triggered expires. consecutive_failures
    // resets to 0 the moment a lockout is imposed, not just on eventual
    // success -- so a source that keeps trying gets a fresh run at the
    // threshold each time its lockout ends, rather than staying locked out
    // forever on an ever-growing count.
    struct AuthFailureState {
        std::uint32_t consecutive_failures = 0;
        std::chrono::steady_clock::time_point locked_until{};
    };

    MatchingEngine& engine_;
    int listener_ = -1;
    std::uint16_t bound_port_ = 0;
    // Null unless enableTls() has succeeded. Every connection accepted while
    // this is set gets wrapped -- see Connection::tls above.
    std::unique_ptr<TlsContext> tls_context_;
    std::vector<Connection> connections_;
    std::atomic<bool> running_{false};
    std::uint64_t requests_handled_ = 0;
    std::function<void()> idle_hook_;
    std::optional<std::string> required_token_;
    std::uint32_t max_auth_failures_;
    std::chrono::steady_clock::duration auth_lockout_duration_;
    // Keyed by peer IP, not by connection -- see Connection::peer_ip. Absent
    // entry means "no recent failures", the common case, so a source that has
    // never gotten anything wrong never occupies a slot here.
    std::unordered_map<std::string, AuthFailureState> auth_failures_;
    std::size_t market_data_depth_;
    std::size_t market_data_history_limit_;
    // The sequence of the LAST MarketData broadcast for each symbol (0 if
    // none yet) -- see MarketDataMessage in wire.hpp for the exact contract
    // this is the server-side half of.
    std::unordered_map<Symbol, std::uint64_t> market_data_sequence_;
    // The last market_data_history_limit_ pushes broadcastMarketData() has
    // sent for each symbol, oldest first -- what ResyncMarketData replays
    // from. A symbol with no entry here (or one whose front is already past
    // the requested since_sequence) can't be replayed and falls back to
    // buildSnapshot() instead.
    std::unordered_map<Symbol, std::deque<MarketDataMessage>> market_data_history_;
};

}  // namespace matching_engine
