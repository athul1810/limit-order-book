#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "matching_engine.hpp"
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
    explicit OrderServer(MatchingEngine& engine) : engine_(engine) {}
    ~OrderServer();

    OrderServer(const OrderServer&) = delete;
    OrderServer& operator=(const OrderServer&) = delete;

    // Binds and listens. Port 0 asks the OS for an ephemeral port, which
    // boundPort() then reports -- what tests use to avoid racing over a
    // hard-coded port number.
    bool listenOn(std::uint16_t port, std::string& error);
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

   private:
    struct Connection {
        int fd = -1;
        FrameReader reader;
        std::vector<std::uint8_t> outbox;
        std::size_t sent = 0;  // how much of outbox has already gone out
    };

    void acceptPending();
    // False if the connection should be closed.
    bool serviceReadable(Connection& connection);
    bool serviceWritable(Connection& connection);
    void closeConnection(std::size_t index);

    MatchingEngine& engine_;
    int listener_ = -1;
    std::uint16_t bound_port_ = 0;
    std::vector<Connection> connections_;
    std::atomic<bool> running_{false};
    std::uint64_t requests_handled_ = 0;
    std::function<void()> idle_hook_;
};

}  // namespace matching_engine
