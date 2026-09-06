#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace matching_engine {

// Optional TLS support for OrderServer, backed by OpenSSL. Optional in two
// senses: at compile time (CMake's WITH_TLS option; off by default, so the
// ordinary build stays true to this project's "no external dependencies"
// baseline) and at run time (a server that never calls TlsContext::create
// never touches any of this).
//
// Everything here is pimpl'd specifically so this header never has to
// include an OpenSSL header. That keeps server.hpp -- and everything that
// includes it -- buildable without OpenSSL's headers on the include path
// even in a WITH_TLS build; only tls.cpp itself needs them.

// True if this binary was compiled with -DWITH_TLS=ON. A binary built
// without it still links and runs fine -- TlsContext::create() below just
// always fails, with an error saying so -- so a caller can give a clear,
// actionable message instead of a confusing runtime failure deep inside
// OpenSSL code that was never actually compiled in.
bool tlsSupported();

class TlsConnection;

// What poll() should ask for next. OpenSSL's non-blocking read and write can
// each report either direction, unlike a raw socket where a read always
// waits on readability and a write on writability: a handshake step or a
// mid-stream renegotiation can need to send before it can receive, or the
// reverse. Server code that gets this back does not try to be clever about
// which -- see server.cpp -- it just polls both, accepting an occasional
// spurious wakeup as the cost of never guessing the wrong direction and
// stalling a connection until an unrelated event happens to wake it.
enum class TlsWant { None, Read, Write };

// One server's OpenSSL state: the loaded certificate and private key,
// shared by every connection accepted while this server is configured for
// TLS. One per OrderServer, not one per connection -- validating a
// certificate and key pair on every accept() would be needless repeated
// work for something that never changes after startup.
class TlsContext {
   public:
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    // Loads a PEM certificate (chain) and private key from disk. Null on
    // failure, with `error` set -- a bad path or a mismatched key pair is a
    // startup-time misconfiguration, worth catching before the first
    // connection ever arrives rather than on it.
    //
    // Always fails with a "not built with TLS support" error if
    // tlsSupported() is false.
    static std::unique_ptr<TlsContext> create(const std::string& cert_path,
                                              const std::string& key_path, std::string& error);

    // Wraps one already-accepted, already-non-blocking file descriptor for
    // a server-side TLS handshake. Ownership of `fd` stays with the caller;
    // this never closes it, mirroring how OrderServer already manages every
    // connection's fd lifetime itself regardless of TLS. Null only if the
    // context itself is somehow broken, which in practice can't happen for
    // a context that came back from a successful create().
    std::unique_ptr<TlsConnection> wrap(int fd);

    // Requires every connection wrapped from here on to present a client
    // certificate, verified against the CA certificate(s) in `ca_path` (a
    // PEM file; more than one CA can simply be concatenated in it). A
    // connection that presents none, or one that doesn't verify against
    // this CA, fails its handshake exactly like a peer not speaking TLS at
    // all does. False (with `error` set) if `ca_path` can't be loaded --
    // call before wrap() is ever called, in practice right after create().
    //
    // This is a second, independent layer alongside whatever
    // application-level authentication OrderServer itself is configured
    // with (wire.hpp's Authenticate): it neither substitutes for nor is
    // substituted by a correct token. A source can be required to prove
    // both who signed its certificate and that it knows a shared secret,
    // since those are two different claims.
    bool requireClientCertificate(const std::string& ca_path, std::string& error);

    // Reloads the certificate, private key, and (if requireClientCertificate
    // was used) client CA from the same paths originally given, replacing
    // what every connection wrap()ped from this point on will present or
    // check. False (with `error` set) if the new files can't be loaded --
    // on failure, the previous certificate is still in effect, exactly as
    // if reload() had never been called; a bad renewal must never leave the
    // server running with no certificate at all.
    //
    // Never disrupts a connection already wrap()ped: each holds its own
    // independent OpenSSL state once created, unaffected by anything this
    // TlsContext does afterwards. A reload only changes what happens on the
    // *next* accept() -- existing connections keep talking under whichever
    // certificate they started with until they eventually close.
    bool reload(std::string& error);

   private:
    TlsContext() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One connection's TLS state machine, from handshake through established
// application data. Every call here is non-blocking and returns instead of
// waiting: there is nothing in this class that can ever block the server's
// single thread on network I/O, which is the entire reason it exists
// instead of just calling OpenSSL's simpler blocking API directly.
class TlsConnection {
   public:
    ~TlsConnection();
    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    enum class Status { Again, Established, Failed };

    // Advances the handshake by one step. Call again whenever poll() next
    // reports the direction handshakeWant() asked for -- either direction
    // is fine to call this on speculatively, since a spurious call just
    // reports Again with an unchanged want.
    Status handshake();
    TlsWant handshakeWant() const;

    // Non-blocking read/write once the handshake has reported Established.
    // Returns the number of bytes transferred, 0 on a clean TLS-level
    // close, or -1 if nothing could be transferred right now. On -1, check
    // `want`: None means a real error (drop the connection, the same as a
    // raw read/write error), Read or Write means try again once poll()
    // reports that direction ready, exactly like handshake()/handshakeWant()
    // above.
    long read(std::uint8_t* buffer, std::size_t length, TlsWant& want);
    long write(const std::uint8_t* data, std::size_t length, TlsWant& want);

   private:
    friend class TlsContext;
    TlsConnection() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace matching_engine
