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

}  // namespace

OrderServer::~OrderServer() {
    for (const Connection& connection : connections_) {
        if (connection.fd >= 0) ::close(connection.fd);
    }
    if (listener_ >= 0) ::close(listener_);
}

bool OrderServer::listenOn(std::uint16_t port, std::string& error) {
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
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // loopback only: no auth in this protocol
    address.sin_port = htons(port);

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
        const Response response = applyRequest(request, engine_);
        encodeResponse(response, connection.outbox);
        ++requests_handled_;
    }

    return !connection.reader.failed();
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
