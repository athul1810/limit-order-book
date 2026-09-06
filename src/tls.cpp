#include "tls.hpp"

#include <algorithm>
#include <limits>

#ifdef MATCHING_ENGINE_WITH_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace matching_engine {

bool tlsSupported() {
#ifdef MATCHING_ENGINE_WITH_TLS
    return true;
#else
    return false;
#endif
}

#ifdef MATCHING_ENGINE_WITH_TLS

namespace {

// OpenSSL's error queue is thread-local and cumulative; this drains the
// single most recent entry, which is always the one relevant to whichever
// call just failed, since every call site below clears the queue first.
std::string lastOpenSslError() {
    const unsigned long code = ERR_get_error();
    if (code == 0) return "no further OpenSSL error detail available";
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    return std::string(buf);
}

// SSL_read/SSL_write take an int length, not a size_t. Every real call site
// in this codebase passes a small, bounded buffer (a 16KB read chunk, or an
// outbox no attacker directly controls the size of), so this only exists to
// turn a theoretical oversized length into a smaller, still-correct partial
// operation instead of undefined behaviour from a silently truncated cast.
int clampToInt(std::size_t length) {
    constexpr std::size_t kMax = static_cast<std::size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(length, kMax));
}

}  // namespace

struct TlsContext::Impl {
    SSL_CTX* ctx = nullptr;
    ~Impl() {
        if (ctx != nullptr) SSL_CTX_free(ctx);
    }
};

struct TlsConnection::Impl {
    SSL* ssl = nullptr;
    TlsWant want = TlsWant::None;
    ~Impl() {
        if (ssl != nullptr) {
            // Best-effort, one-shot: a full bidirectional TLS shutdown would
            // need its own non-blocking retry loop, for a connection that is
            // being torn down anyway. The peer sees the TCP close either
            // way; this just gives well-behaved peers a chance at a clean
            // close_notify first.
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
    }
};

std::unique_ptr<TlsContext> TlsContext::create(const std::string& cert_path,
                                               const std::string& key_path, std::string& error) {
    std::unique_ptr<TlsContext> context(new TlsContext());
    context->impl_ = std::make_unique<Impl>();

    context->impl_->ctx = SSL_CTX_new(TLS_server_method());
    if (context->impl_->ctx == nullptr) {
        error = "SSL_CTX_new failed: " + lastOpenSslError();
        return nullptr;
    }
    SSL_CTX* ctx = context->impl_->ctx;

    // No SSLv3/TLS1.0/TLS1.1: all three have known, practical attacks.
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    // Both needed for the outbox-is-a-growing-buffer pattern serviceWritable
    // (server.cpp) uses: a WANT_WRITE retry there recomputes "from the write
    // cursor to the current end of outbox" each time, which can be a longer
    // buffer than the previous attempt if a broadcast queued more bytes in
    // between. Plain OpenSSL requires retrying with the exact same pointer
    // and length; ACCEPT_MOVING_WRITE_BUFFER relaxes that to "same pending
    // bytes at the front, more allowed after them", which is exactly this
    // shape. ENABLE_PARTIAL_WRITE lets a single SSL_write return less than
    // asked for instead of internally looping, matching how the raw ::write
    // path already treats a partial write as normal, not an error.
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    // The chain variant (not SSL_CTX_use_certificate_file) so a cert file
    // that also bundles intermediate certificates works without a separate
    // API call for them.
    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path.c_str()) != 1) {
        error = "loading certificate '" + cert_path + "': " + lastOpenSslError();
        return nullptr;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        error = "loading private key '" + key_path + "': " + lastOpenSslError();
        return nullptr;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        error = "certificate and private key do not match: " + lastOpenSslError();
        return nullptr;
    }

    return context;
}

TlsContext::~TlsContext() = default;

bool TlsContext::requireClientCertificate(const std::string& ca_path, std::string& error) {
    if (SSL_CTX_load_verify_locations(impl_->ctx, ca_path.c_str(), nullptr) != 1) {
        error = "loading client CA '" + ca_path + "': " + lastOpenSslError();
        return false;
    }
    // FAIL_IF_NO_PEER_CERT is what actually makes this mandatory rather than
    // merely requested: without it, VERIFY_PEER alone still accepts a
    // handshake that presented no certificate at all, verifying only the
    // ones that were.
    SSL_CTX_set_verify(impl_->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    return true;
}

std::unique_ptr<TlsConnection> TlsContext::wrap(int fd) {
    SSL* ssl = SSL_new(impl_->ctx);
    if (ssl == nullptr) return nullptr;
    // NOCLOSE is SSL_set_fd's default behaviour: freeing `ssl` will not
    // close `fd`. That is deliberate -- OrderServer's Connection already
    // owns the fd's lifecycle (see closeConnection in server.cpp)
    // regardless of whether TLS is in use, and closing it twice would be a
    // real bug, not a harmless no-op.
    SSL_set_fd(ssl, fd);
    SSL_set_accept_state(ssl);  // this end is the server

    std::unique_ptr<TlsConnection> connection(new TlsConnection());
    connection->impl_ = std::make_unique<TlsConnection::Impl>();
    connection->impl_->ssl = ssl;
    return connection;
}

TlsConnection::~TlsConnection() = default;

TlsConnection::Status TlsConnection::handshake() {
    ERR_clear_error();
    const int rc = SSL_accept(impl_->ssl);
    if (rc == 1) {
        impl_->want = TlsWant::None;
        return Status::Established;
    }
    switch (SSL_get_error(impl_->ssl, rc)) {
        case SSL_ERROR_WANT_READ:
            impl_->want = TlsWant::Read;
            return Status::Again;
        case SSL_ERROR_WANT_WRITE:
            impl_->want = TlsWant::Write;
            return Status::Again;
        default:
            // A protocol error, a peer not speaking TLS at all, a
            // certificate the peer rejected -- none of it is recoverable
            // for this connection.
            return Status::Failed;
    }
}

TlsWant TlsConnection::handshakeWant() const { return impl_->want; }

long TlsConnection::read(std::uint8_t* buffer, std::size_t length, TlsWant& want) {
    want = TlsWant::None;
    ERR_clear_error();
    const int rc = SSL_read(impl_->ssl, buffer, clampToInt(length));
    if (rc > 0) return rc;
    switch (SSL_get_error(impl_->ssl, rc)) {
        case SSL_ERROR_ZERO_RETURN:
            return 0;  // a clean TLS-level close (close_notify received)
        case SSL_ERROR_WANT_READ:
            want = TlsWant::Read;
            return -1;
        case SSL_ERROR_WANT_WRITE:
            want = TlsWant::Write;
            return -1;
        case SSL_ERROR_SYSCALL:
            // The underlying socket closed without a TLS close_notify --
            // treated the same as a graceful close (a raw ::read() peer
            // hangup is also not otherwise distinguished here) rather than
            // a hard error, since this is what an ordinary client
            // disconnecting looks like on many stacks.
            return 0;
        default:
            return -1;  // want stays None: a real error
    }
}

long TlsConnection::write(const std::uint8_t* data, std::size_t length, TlsWant& want) {
    want = TlsWant::None;
    ERR_clear_error();
    const int rc = SSL_write(impl_->ssl, data, clampToInt(length));
    if (rc > 0) return rc;
    switch (SSL_get_error(impl_->ssl, rc)) {
        case SSL_ERROR_WANT_READ:
            want = TlsWant::Read;
            return -1;
        case SSL_ERROR_WANT_WRITE:
            want = TlsWant::Write;
            return -1;
        default:
            return -1;  // want stays None: a real error
    }
}

#else  // !MATCHING_ENGINE_WITH_TLS

// Every method below is unreachable in a binary built this way: create()
// always fails, so no caller ever obtains a TlsContext to call wrap() on,
// and no TlsConnection is ever constructed for handshake()/read()/write() to
// be called on. They still have to exist, and compile without an OpenSSL
// header on the include path, purely to satisfy the linker -- server.cpp's
// calls into this API are the same in both builds.

struct TlsContext::Impl {};
struct TlsConnection::Impl {};

std::unique_ptr<TlsContext> TlsContext::create(const std::string& cert_path,
                                               const std::string& key_path, std::string& error) {
    (void)cert_path;
    (void)key_path;
    error = "this binary was not built with TLS support (rebuild with -DWITH_TLS=ON)";
    return nullptr;
}

TlsContext::~TlsContext() = default;

bool TlsContext::requireClientCertificate(const std::string& ca_path, std::string& error) {
    (void)ca_path;
    error = "this binary was not built with TLS support (rebuild with -DWITH_TLS=ON)";
    return false;
}

std::unique_ptr<TlsConnection> TlsContext::wrap(int fd) {
    (void)fd;
    return nullptr;
}

TlsConnection::~TlsConnection() = default;

TlsConnection::Status TlsConnection::handshake() { return Status::Failed; }

TlsWant TlsConnection::handshakeWant() const { return TlsWant::None; }

long TlsConnection::read(std::uint8_t* buffer, std::size_t length, TlsWant& want) {
    (void)buffer;
    (void)length;
    want = TlsWant::None;
    return -1;
}

long TlsConnection::write(const std::uint8_t* data, std::size_t length, TlsWant& want) {
    (void)data;
    (void)length;
    want = TlsWant::None;
    return -1;
}

#endif  // MATCHING_ENGINE_WITH_TLS

}  // namespace matching_engine
