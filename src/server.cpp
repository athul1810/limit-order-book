#include "server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <utility>

namespace matching_engine {

namespace {

constexpr std::size_t kReadChunkBytes = 16 * 1024;
constexpr int kPollTimeoutMs = 200;  // bounds how long stop() takes to take effect

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Compares two byte strings without early-exiting on the first mismatch, so
// a wrong token takes the same time regardless of how many leading bytes
// happened to match -- closing the usual timing side channel on a secret
// comparison. The length check up front still leaks length, not content;
// that is the standard, accepted trade-off for a check shaped like this one.
bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

}  // namespace

OrderServer::~OrderServer() {
    for (const Connection& connection : connections_) {
        if (connection.fd >= 0) ::close(connection.fd);
    }
    if (listener_ >= 0) ::close(listener_);
}

bool OrderServer::listenOn(std::uint16_t port, std::string& error, const std::string& bind_address) {
    constexpr const char* kLoopback = "127.0.0.1";

    // Binding anywhere but loopback without a token configured would put an
    // unauthenticated server on the network -- exactly the failure mode
    // authentication exists to prevent. Checked before any socket is even
    // opened, so there is nothing to unwind on this path.
    if (bind_address != kLoopback && !required_token_.has_value()) {
        // Deliberately caller-agnostic: OrderServer is constructed directly
        // by tests with no environment involved at all, so this cannot name
        // a specific env var or flag without being wrong for some caller.
        error = "refusing to bind " + bind_address + ": authentication is not configured";
        return false;
    }

    // Writing to a socket the peer has already closed raises SIGPIPE, whose
    // default action is to kill the process. A server must not die because one
    // client hung up mid-response.
    std::signal(SIGPIPE, SIG_IGN);

    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }

    // Without SO_REUSEADDR, restarting the server fails for as long as the old
    // socket sits in TIME_WAIT.
    int one = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (bind_address == kLoopback) {
        // The exact original behaviour, kept as its own branch rather than
        // routed through inet_pton so this specific, always-safe default
        // path is untouched by adding the general case below.
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (::inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
        error = "invalid bind address: " + bind_address;
        return false;
    }

    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error = std::string("bind: ") + std::strerror(errno);
        return false;
    }
    if (::listen(listener_, 16) != 0) {
        error = std::string("listen: ") + std::strerror(errno);
        return false;
    }
    if (!setNonBlocking(listener_)) {
        error = std::string("fcntl: ") + std::strerror(errno);
        return false;
    }

    // Report the port actually bound, which matters when 0 was requested.
    sockaddr_in actual{};
    socklen_t actual_len = sizeof(actual);
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0) {
        bound_port_ = ntohs(actual.sin_port);
    }
    return true;
}

void OrderServer::acceptPending() {
    // Loop: level-triggered poll would return again, but draining the backlog
    // here avoids a syscall round trip per pending connection.
    while (true) {
        const int fd = ::accept(listener_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;  // EAGAIN/EWOULDBLOCK: backlog drained
        }
        if (!setNonBlocking(fd)) {
            ::close(fd);
            continue;
        }
        // Order entry is latency-sensitive and messages are small, so waiting
        // to coalesce them is the wrong trade.
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        Connection connection;
        connection.fd = fd;
        // Best-effort: an address that can't be resolved (should not happen
        // for an already-accepted AF_INET connection) just means this source
        // never matches another attempt's peer_ip, so rate limiting quietly
        // stops applying to it rather than the accept itself failing.
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0) {
            char buf[INET_ADDRSTRLEN];
            if (::inet_ntop(AF_INET, &peer.sin_addr, buf, sizeof(buf)) != nullptr) {
                connection.peer_ip = buf;
            }
        }
        connections_.push_back(std::move(connection));
    }
}

bool OrderServer::serviceReadable(Connection& connection) {
    std::uint8_t chunk[kReadChunkBytes];

    while (true) {
        const ssize_t count = ::read(connection.fd, chunk, sizeof(chunk));
        if (count == 0) return false;  // peer closed
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;
        }
        connection.reader.append(chunk, static_cast<std::size_t>(count));
        if (static_cast<std::size_t>(count) < sizeof(chunk)) break;
    }

    MessageType type;
    std::uint32_t correlation_id = 0;
    std::vector<std::uint8_t> payload;

    while (connection.reader.next(type, correlation_id, payload)) {
        Request request;
        if (!decodeRequest(type, correlation_id, payload.data(), payload.size(), request)) {
            // A frame that doesn't decode means the peer is speaking something
            // else. Framing can't be trusted from here, so drop the connection
            // rather than guess where the next message starts.
            return false;
        }

        if (type == MessageType::Authenticate) {
            const auto now = std::chrono::steady_clock::now();
            const auto locked = auth_failures_.find(connection.peer_ip);
            if (locked != auth_failures_.end() && now < locked->second.locked_until) {
                // Locked out: rejected without even looking at the token, so
                // a source serving out a lockout learns nothing more from
                // trying again than "still locked out" -- not, say, "that
                // one would have worked".
                queueResponse(connection, correlation_id, RejectReason::RateLimited);
                serviceWritable(connection);
                return false;
            }

            if (!required_token_.has_value() || constantTimeEquals(request.token, *required_token_)) {
                connection.authenticated = true;
                // A real success clears any failure history for this
                // source -- it is who it says it is now, and holding a grudge
                // from before would only punish it for having mistyped a
                // token once, previously.
                auth_failures_.erase(connection.peer_ip);
                queueResponse(connection, correlation_id, RejectReason::None);
                continue;
            }
            // Wrong token: tell the client why, then close the connection.
            // Closing costs a fresh TCP handshake per retry on its own; the
            // failure count below is what actually stops sustained guessing,
            // by locking the source out entirely once it crosses the
            // threshold, handshake cost or not.
            AuthFailureState& state = auth_failures_[connection.peer_ip];
            if (++state.consecutive_failures >= max_auth_failures_) {
                state.locked_until = now + auth_lockout_duration_;
                state.consecutive_failures = 0;
            }
            queueResponse(connection, correlation_id, RejectReason::AuthenticationFailed);
            serviceWritable(connection);  // best-effort flush before closing
            return false;
        }

        if (required_token_.has_value() && !connection.authenticated) {
            // Left open, unlike a wrong token above: this connection simply
            // has not authenticated yet, which by itself is not hostile.
            queueResponse(connection, correlation_id, RejectReason::NotAuthenticated);
            continue;
        }

        if (type == MessageType::Subscribe) {
            if (!engine_.hasSymbol(request.symbol)) {
                queueResponse(connection, correlation_id, RejectReason::UnknownSymbol);
                continue;
            }
            connection.subscriptions.insert(request.symbol);
            // The snapshot: current book state, current sequence (what has
            // already happened), no trades -- this is not itself an event.
            MarketDataMessage snapshot;
            snapshot.correlation_id = correlation_id;
            snapshot.symbol = request.symbol;
            snapshot.sequence = market_data_sequence_[request.symbol];  // 0 if untouched so far
            snapshot.best_bid = engine_.bestBid(request.symbol);
            snapshot.best_ask = engine_.bestAsk(request.symbol);
            encodeMarketData(snapshot, connection.outbox);
            ++requests_handled_;
            continue;
        }
        if (type == MessageType::Unsubscribe) {
            connection.subscriptions.erase(request.symbol);  // no-op if absent -- idempotent
            queueResponse(connection, correlation_id, RejectReason::None);
            continue;
        }

        const Response response = applyRequest(request, engine_);
        encodeResponse(response, connection.outbox);
        ++requests_handled_;

        if (response.reason == RejectReason::None && !request.symbol.empty() &&
            (type == MessageType::LimitOrder || type == MessageType::MarketOrder ||
             type == MessageType::ModifyOrder || type == MessageType::CancelOrder)) {
            broadcastMarketData(request.symbol, response.trades);
        }
    }

    return !connection.reader.failed();
}

void OrderServer::queueResponse(Connection& connection, std::uint32_t correlation_id,
                                RejectReason reason) {
    Response response;
    response.correlation_id = correlation_id;
    response.reason = reason;
    encodeResponse(response, connection.outbox);
    ++requests_handled_;
}

void OrderServer::broadcastMarketData(const Symbol& symbol, const std::vector<Trade>& trades) {
    // Increment before reading: this push's sequence is one past whatever a
    // concurrent Subscribe's snapshot would have just reported.
    const std::uint64_t sequence = ++market_data_sequence_[symbol];

    MarketDataMessage message;
    // correlation_id left at its default (0): nothing requested this push,
    // unlike the snapshot sent in direct reply to a Subscribe.
    message.symbol = symbol;
    message.sequence = sequence;
    message.best_bid = engine_.bestBid(symbol);
    message.best_ask = engine_.bestAsk(symbol);
    message.trades = trades;

    std::vector<std::uint8_t> encoded;
    encodeMarketData(message, encoded);

    // Every subscribed connection, including the one that just caused this
    // -- its own Response already told it what happened to its order; this
    // is the separate, public "the book changed" push everyone subscribed
    // gets, itself included.
    //
    // This only appends to outbox; it deliberately does not also try to
    // flush a recipient's socket here. serviceWritable() can decide a
    // connection needs closing, and closeConnection() erases from
    // connections_ -- which this loop is iterating, and which the caller's
    // own `connection` (a reference into this same vector, from whichever
    // index is currently being serviced in serviceReadable) is a reference
    // into. Closing one here could invalidate the other. Queuing and
    // leaving the flush to the next poll() iteration's ordinary
    // serviceWritable pass costs at most kPollTimeoutMs of latency on an
    // otherwise fully idle server, and sidesteps that risk entirely.
    for (Connection& other : connections_) {
        if (other.subscriptions.count(symbol) != 0) {
            other.outbox.insert(other.outbox.end(), encoded.begin(), encoded.end());
        }
    }
}

bool OrderServer::serviceWritable(Connection& connection) {
    while (connection.sent < connection.outbox.size()) {
        const ssize_t count = ::write(connection.fd, connection.outbox.data() + connection.sent,
                                      connection.outbox.size() - connection.sent);
        if (count < 0) {
            if (errno == EINTR) continue;
            // A partial write is normal, not an error: the kernel buffer is
            // full and the rest goes out when poll says the socket is writable
            // again. This is why responses are buffered rather than assumed
            // sent.
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            return false;
        }
        connection.sent += static_cast<std::size_t>(count);
    }

    connection.outbox.clear();
    connection.sent = 0;
    return true;
}

void OrderServer::closeConnection(std::size_t index) {
    if (connections_[index].fd >= 0) ::close(connections_[index].fd);
    connections_.erase(connections_.begin() + static_cast<std::ptrdiff_t>(index));
}

std::uint64_t OrderServer::runUntilStopped() {
    running_ = true;
    std::vector<pollfd> fds;

    while (running_) {
        fds.clear();
        fds.push_back(pollfd{listener_, POLLIN, 0});
        // Number of connections this fds array describes. accept() below can
        // append more, and those have no entry here -- indexing fds by the
        // grown connections_.size() would read past its end. They get serviced
        // on the next pass instead.
        const std::size_t polled_connections = connections_.size();
        for (const Connection& connection : connections_) {
            short events = POLLIN;
            // Only ask about writability when there is something to write;
            // otherwise poll returns immediately, forever.
            if (connection.sent < connection.outbox.size()) events |= POLLOUT;
            fds.push_back(pollfd{connection.fd, events, 0});
        }

        const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), kPollTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR) continue;  // a signal, including our own stop()
            break;
        }

        // Runs every iteration -- including a bare timeout with nothing
        // ready -- so a time-based background trigger still fires on an idle
        // server, not just a busy one.
        if (idle_hook_) idle_hook_();

        if (ready == 0) continue;

        if ((fds[0].revents & POLLIN) != 0) acceptPending();

        // Backwards, so erasing a connection doesn't disturb indices not yet
        // visited. Connections accepted this pass sit above polled_connections
        // and are deliberately skipped.
        for (std::size_t i = polled_connections; i-- > 0;) {
            const pollfd& entry = fds[i + 1];
            bool keep = true;

            if ((entry.revents & (POLLERR | POLLNVAL)) != 0) {
                keep = false;
            } else {
                if (keep && (entry.revents & POLLIN) != 0) keep = serviceReadable(connections_[i]);
                if (keep && (entry.revents & POLLOUT) != 0) keep = serviceWritable(connections_[i]);
                // POLLHUP with data still buffered means the peer is done
                // sending but may still be reading, so only hang up once the
                // outbox has drained.
                if (keep && (entry.revents & POLLHUP) != 0 &&
                    connections_[i].sent >= connections_[i].outbox.size()) {
                    keep = false;
                }
            }

            if (!keep) closeConnection(i);
        }
    }

    return requests_handled_;
}

}  // namespace matching_engine
