"""Independent client for the matching-engine wire protocol.

Written from the format documented in wire.hpp rather than from the C++ code,
so it catches byte-layout and endianness mistakes that a same-code round trip
cannot.

Run it against a built server:
    python3 tests/wire_smoke_test.py ./matching_engine_server
"""
import os
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time

VERSION = 1
(ADD_SYMBOL, LIMIT, MARKET, MODIFY, CANCEL, RESPONSE, AUTHENTICATE, SUBSCRIBE, UNSUBSCRIBE,
 MARKET_DATA, RESYNC_MARKET_DATA) = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
REASONS = {
    0: "ok", 1: "duplicate-id", 2: "unknown-order", 3: "bad-quantity", 4: "unknown-symbol",
    5: "not-authenticated", 6: "authentication-failed", 7: "rate-limited",
}


def frame(msg_type, correlation, payload):
    header = struct.pack(">IIBBH", len(payload), correlation, msg_type, VERSION, 0)
    return header + payload


def sym(s):
    assert len(s) <= 8
    return s.encode() + b"\x00" * (8 - len(s))


def add_symbol(corr, s):
    return frame(ADD_SYMBOL, corr, sym(s))


def limit(corr, s, oid, side, price, qty, participant=0):
    return frame(LIMIT, corr, sym(s) + struct.pack(">QBqQQ", oid, side, price, qty, participant))


def market(corr, s, oid, side, qty, participant=0):
    return frame(MARKET, corr, sym(s) + struct.pack(">QBQQ", oid, side, qty, participant))


def modify(corr, s, oid, price, qty):
    return frame(MODIFY, corr, sym(s) + struct.pack(">QqQ", oid, price, qty))


def cancel(corr, s, oid):
    return frame(CANCEL, corr, sym(s) + struct.pack(">Q", oid))


def authenticate(corr, token):
    # No symbol prefix here, unlike every other message type: the payload is
    # just the raw token bytes, with nothing else in front of them.
    return frame(AUTHENTICATE, corr, token.encode())


def subscribe(corr, s):
    return frame(SUBSCRIBE, corr, sym(s))


def unsubscribe(corr, s):
    return frame(UNSUBSCRIBE, corr, sym(s))


def resync_market_data(corr, s, since_sequence):
    return frame(RESYNC_MARKET_DATA, corr, sym(s) + struct.pack(">Q", since_sequence))


def read_exactly(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("server closed")
        buf += chunk
    return buf


def read_response(sock):
    header = read_exactly(sock, 12)
    length, corr, mtype, version, _ = struct.unpack(">IIBBH", header)
    assert mtype == RESPONSE, f"expected response, got type {mtype}"
    assert version == VERSION
    body = read_exactly(sock, length)
    reason, flags = body[0], body[1]
    (count,) = struct.unpack(">I", body[2:6])
    (unfilled,) = struct.unpack(">Q", body[6:14])
    trades = []
    off = 14
    for _ in range(count):
        b, s, p, q = struct.unpack(">QQqQ", body[off:off + 32])
        trades.append((b, s, p, q))
        off += 32
    assert off == len(body), "trailing bytes in response"
    return {"corr": corr, "reason": REASONS.get(reason, reason), "self_trade": bool(flags),
            "unfilled": unfilled, "trades": trades}


def read_market_data(sock):
    header = read_exactly(sock, 12)
    length, corr, mtype, version, _ = struct.unpack(">IIBBH", header)
    assert mtype == MARKET_DATA, f"expected MarketData, got type {mtype}"
    assert version == VERSION
    body = read_exactly(sock, length)
    symbol = body[0:8].split(b"\x00", 1)[0].decode()
    (sequence,) = struct.unpack(">Q", body[8:16])
    has_bid = body[16]
    (bid_price,) = struct.unpack(">q", body[17:25])
    has_ask = body[25]
    (ask_price,) = struct.unpack(">q", body[26:34])
    off = 34

    def read_levels():
        nonlocal off
        (count,) = struct.unpack(">I", body[off:off + 4])
        off += 4
        levels = []
        for _ in range(count):
            price, qty = struct.unpack(">qQ", body[off:off + 16])
            levels.append((price, qty))
            off += 16
        return levels

    bid_levels = read_levels()
    ask_levels = read_levels()

    (count,) = struct.unpack(">I", body[off:off + 4])
    off += 4
    trades = []
    for _ in range(count):
        b, s, p, q = struct.unpack(">QQqQ", body[off:off + 32])
        trades.append((b, s, p, q))
        off += 32
    assert off == len(body), "trailing bytes in market data message"
    return {"corr": corr, "symbol": symbol, "sequence": sequence,
            "bid": bid_price if has_bid else None, "ask": ask_price if has_ask else None,
            "bid_levels": bid_levels, "ask_levels": ask_levels, "trades": trades}


def clean_env(overrides=None):
    # Strips the two auth-related variables from the ambient environment
    # before applying `overrides`, so a token left over from the calling
    # shell -- or from an earlier scenario in this same run -- can never
    # leak into a server a scenario expects to be running without one.
    env = dict(os.environ)
    env.pop("MATCHING_ENGINE_TOKEN", None)
    env.pop("MATCHING_ENGINE_BIND", None)
    env.pop("MATCHING_ENGINE_MAX_AUTH_FAILURES", None)
    env.pop("MATCHING_ENGINE_AUTH_LOCKOUT_SECONDS", None)
    env.pop("MATCHING_ENGINE_MARKET_DATA_DEPTH", None)
    env.pop("MATCHING_ENGINE_MARKET_DATA_HISTORY_LIMIT", None)
    env.pop("MATCHING_ENGINE_TLS_CERT", None)
    env.pop("MATCHING_ENGINE_TLS_KEY", None)
    env.pop("MATCHING_ENGINE_TLS_HANDSHAKE_TIMEOUT_SECONDS", None)
    if overrides:
        env.update(overrides)
    return env


def start_server(binary, port, log_path, out_path, env_overrides=None):
    out = open(out_path, "w")
    proc = subprocess.Popen([binary, str(port), log_path], stdout=out, stderr=subprocess.STDOUT,
                            env=clean_env(env_overrides))
    for _ in range(100):
        if os.path.exists(out_path):
            with open(out_path) as f:
                if "listening" in f.read():
                    return proc, out
        if proc.poll() is not None:
            break
        time.sleep(0.05)
    proc.kill()
    out.close()
    raise RuntimeError(f"server failed to start; see {out_path}")


def stop_server(proc, out):
    proc.terminate()
    proc.wait(timeout=5)
    out.close()


def expect_start_failure(binary, port, log_path, env_overrides, expected_substring):
    # For a server that must refuse to start at all (an unsafe bind request):
    # run it to completion rather than through start_server's "wait for
    # listening" loop, and check both the exit code and the message.
    result = subprocess.run([binary, str(port), log_path], env=clean_env(env_overrides),
                            capture_output=True, text=True, timeout=5)
    return result.returncode != 0 and expected_substring in (result.stdout + result.stderr)


def read_after_close_probe(sock, timeout=2):
    # Distinguishes "the peer closed the connection" (recv returns b"" right
    # away) from "the peer left it open with nothing to say" (recv blocks
    # until our timeout) -- the two outcomes a bare recv() cannot tell apart
    # without one.
    sock.settimeout(timeout)
    try:
        data = sock.recv(16)
    except socket.timeout:
        return "still-open"
    except OSError:
        return "closed"
    return "closed" if data == b"" else "unexpected-data"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def generate_self_signed_cert(cert_path, key_path):
    # Shells out to the openssl CLI rather than a Python crypto library:
    # this test file otherwise has zero dependencies beyond the standard
    # library, and openssl itself is a safe thing to assume is on hand for
    # testing a server that, in this mode, links OpenSSL to run at all.
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-keyout", key_path, "-out", cert_path,
         "-days", "1", "-nodes", "-subj", "/CN=localhost"],
        check=True, capture_output=True, timeout=30)


def wrap_tls(sock):
    # CERT_NONE and hostname checking off: this is verifying the connection
    # is actually encrypted end-to-end, not standing up a trust chain --
    # the cert above is self-signed and would fail real verification by
    # design, the same as any other self-signed test cert would.
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx.wrap_socket(sock, server_hostname="localhost")


def main():
    if len(sys.argv) < 2:
        print("usage: wire_smoke_test.py <path to matching_engine_server>")
        return 2
    binary = sys.argv[1]
    failures = []

    def check(label, got, want):
        if got != want:
            failures.append(label)
            print(f"  FAIL {label}: got {got!r} want {want!r}")
        else:
            print(f"  ok   {label}")

    workdir = tempfile.mkdtemp(prefix="wire-smoke-")
    log_path = os.path.join(workdir, "book.log")
    port = free_port()
    proc, out = start_server(binary, port, log_path, os.path.join(workdir, "server.out"))

    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        sock.sendall(add_symbol(1, "AAPL"))
        check("register AAPL", read_response(sock)["reason"], "ok")

        sock.sendall(limit(2, "AAPL", 1, 1, 10050, 10, 7))
        r = read_response(sock)
        check("resting sell unfilled", r["unfilled"], 10)
        check("resting sell no trades", len(r["trades"]), 0)

        # Several messages in ONE write: the server must split them itself.
        sock.sendall(limit(3, "AAPL", 2, 0, 10050, 4, 9) + limit(4, "AAPL", 3, 0, 10050, 3, 9))
        r3 = read_response(sock)
        r4 = read_response(sock)
        check("batched msg 1", (r3["corr"], r3["trades"]), (3, [(2, 1, 10050, 4)]))
        check("batched msg 2", (r4["corr"], r4["trades"]), (4, [(3, 1, 10050, 3)]))

        # One message split across TWO writes, mid-payload.
        msg = market(5, "AAPL", 4, 0, 2, 9)
        sock.sendall(msg[:7])
        time.sleep(0.05)
        sock.sendall(msg[7:])
        check("split message", read_response(sock)["trades"], [(4, 1, 10050, 2)])

        # Self-trade prevention: participant 7 owns the resting sell, so under
        # CancelOldest that sell is cancelled and this buy rests in its place.
        sock.sendall(limit(6, "AAPL", 5, 0, 10050, 1, 7))
        r = read_response(sock)
        check("self-trade prevented", (len(r["trades"]), r["unfilled"]), (0, 1))

        sock.sendall(limit(7, "NVDA", 9, 0, 10000, 1, 0))
        check("unknown symbol", read_response(sock)["reason"], "unknown-symbol")
        sock.sendall(modify(8, "AAPL", 5, 9900, 0))
        check("zero quantity", read_response(sock)["reason"], "bad-quantity")
        sock.sendall(cancel(9, "AAPL", 4242))
        check("cancel unknown", read_response(sock)["reason"], "unknown-order")

        # Signed price field: the classic hand-rolled-codec bug.
        sock.sendall(add_symbol(10, "NEG"))
        read_response(sock)
        sock.sendall(limit(11, "NEG", 1, 0, -1234, 5, 0))
        check("negative price accepted", read_response(sock)["reason"], "ok")
        sock.sendall(limit(12, "NEG", 2, 1, -1234, 5, 0))
        check("negative price matches", read_response(sock)["trades"], [(1, 2, -1234, 5)])
        sock.close()

        # A separate connection sees the same book. AAPL now holds order 5 as a
        # resting buy, so a market sell is what crosses it.
        sock2 = socket.create_connection(("127.0.0.1", port), timeout=5)
        sock2.sendall(market(20, "AAPL", 30, 1, 1, 9))
        check("second connection", read_response(sock2)["trades"], [(5, 30, 10050, 1)])

        # Leave something resting, then restart and confirm it survived.
        sock2.sendall(limit(21, "AAPL", 100, 1, 20025, 8, 4))
        read_response(sock2)
        sock2.close()
    finally:
        stop_server(proc, out)

    proc, out = start_server(binary, port, log_path, os.path.join(workdir, "server2.out"))
    try:
        sock3 = socket.create_connection(("127.0.0.1", port), timeout=5)
        sock3.sendall(market(30, "AAPL", 200, 0, 3, 9))
        check("survives restart", read_response(sock3)["trades"], [(200, 100, 20025, 3)])
        sock3.close()
    finally:
        stop_server(proc, out)

    # ---- authentication ----
    auth_port = free_port()
    proc, out = start_server(binary, auth_port, os.path.join(workdir, "auth.log"),
                             os.path.join(workdir, "auth.out"),
                             env_overrides={"MATCHING_ENGINE_TOKEN": "s3cr3t-token"})
    try:
        s = socket.create_connection(("127.0.0.1", auth_port), timeout=5)
        s.sendall(add_symbol(1, "AAPL"))
        check("unauthenticated request rejected", read_response(s)["reason"], "not-authenticated")

        s.sendall(authenticate(2, "wrong-token"))
        check("wrong token rejected", read_response(s)["reason"], "authentication-failed")
        check("connection closed after wrong token", read_after_close_probe(s), "closed")
        s.close()

        # A fresh connection, with the right token this time.
        s2 = socket.create_connection(("127.0.0.1", auth_port), timeout=5)
        s2.sendall(authenticate(3, "s3cr3t-token"))
        check("correct token accepted", read_response(s2)["reason"], "ok")
        s2.sendall(add_symbol(4, "AAPL"))
        check("request after authenticating", read_response(s2)["reason"], "ok")
        s2.close()
    finally:
        stop_server(proc, out)

    # ---- rate limiting on repeated failed authentication ----
    # A short fuse (2 failures, 1 second) so this test doesn't sit around --
    # the mechanism being exercised is "after enough failures, locked out for
    # a while", not any particular threshold or duration.
    rl_port = free_port()
    proc, out = start_server(binary, rl_port, os.path.join(workdir, "ratelimit.log"),
                             os.path.join(workdir, "ratelimit.out"),
                             env_overrides={"MATCHING_ENGINE_TOKEN": "s3cr3t-token",
                                            "MATCHING_ENGINE_MAX_AUTH_FAILURES": "2",
                                            "MATCHING_ENGINE_AUTH_LOCKOUT_SECONDS": "1"})
    try:
        # Two wrong-token attempts, each its own fresh connection -- exactly
        # what a real attacker gets to do for free, and exactly what rate
        # limiting has to stop being sufficient on its own.
        s = socket.create_connection(("127.0.0.1", rl_port), timeout=5)
        s.sendall(authenticate(1, "wrong-token"))
        check("first wrong attempt just fails", read_response(s)["reason"], "authentication-failed")
        s.close()

        s = socket.create_connection(("127.0.0.1", rl_port), timeout=5)
        s.sendall(authenticate(2, "wrong-token"))
        check("second wrong attempt trips the lockout", read_response(s)["reason"],
              "authentication-failed")
        s.close()

        # Third attempt, correct token this time, but the source is locked
        # out: the token is never even looked at.
        s = socket.create_connection(("127.0.0.1", rl_port), timeout=5)
        s.sendall(authenticate(3, "s3cr3t-token"))
        check("correct token rejected while locked out", read_response(s)["reason"], "rate-limited")
        check("connection closed while locked out", read_after_close_probe(s), "closed")
        s.close()

        time.sleep(1.3)  # past the one-second lockout

        s = socket.create_connection(("127.0.0.1", rl_port), timeout=5)
        s.sendall(authenticate(4, "s3cr3t-token"))
        check("correct token accepted once the lockout expires", read_response(s)["reason"], "ok")
        s.close()
    finally:
        stop_server(proc, out)

    # A non-loopback bind must be refused when no token is configured --
    # otherwise authentication existing at all would not stop the server
    # from being reachable, unauthenticated, from the network.
    refused = expect_start_failure(binary, free_port(), os.path.join(workdir, "refuse.log"),
                                   {"MATCHING_ENGINE_BIND": "0.0.0.0"},
                                   "authentication is not configured")
    check("non-loopback bind refused without a token", refused, True)

    # ...but a token unlocks it. 0.0.0.0 also accepts a loopback connection,
    # which is what this checks without needing a second, non-loopback NIC.
    bind_port = free_port()
    proc, out = start_server(binary, bind_port, os.path.join(workdir, "bind.log"),
                             os.path.join(workdir, "bind.out"),
                             env_overrides={"MATCHING_ENGINE_TOKEN": "s3cr3t-token",
                                            "MATCHING_ENGINE_BIND": "0.0.0.0"})
    try:
        s3 = socket.create_connection(("127.0.0.1", bind_port), timeout=5)
        s3.close()
        check("non-loopback bind with a token accepts a connection", True, True)
    finally:
        stop_server(proc, out)

    # ---- market data ----
    md_port = free_port()
    proc, out = start_server(binary, md_port, os.path.join(workdir, "md.log"),
                             os.path.join(workdir, "md.out"))
    try:
        # Connection A subscribes; connection B never does, and is what
        # confirms broadcasts are scoped to subscribers, not sent to everyone.
        a = socket.create_connection(("127.0.0.1", md_port), timeout=5)
        a.sendall(add_symbol(1, "AAPL"))
        read_response(a)
        a.sendall(subscribe(2, "AAPL"))
        snapshot = read_market_data(a)
        check("subscribe snapshot corr", snapshot["corr"], 2)
        check("subscribe snapshot on an empty book",
              (snapshot["sequence"], snapshot["bid"], snapshot["ask"], snapshot["bid_levels"],
               snapshot["ask_levels"], snapshot["trades"]),
              (0, None, None, [], [], []))

        b = socket.create_connection(("127.0.0.1", md_port), timeout=5)
        b.sendall(limit(3, "AAPL", 100, 1, 10050, 5, 0))  # rest a sell; no trade yet
        check("B's own response", read_response(b)["reason"], "ok")

        push1 = read_market_data(a)
        check("broadcast is unsolicited (corr 0)", push1["corr"], 0)
        check("broadcast after a resting order",
              (push1["sequence"], push1["ask"], push1["ask_levels"], push1["trades"]),
              (1, 10050, [(10050, 5)], []))

        # A crosses the resting sell itself: it gets its own Response, and
        # then, separately, the public broadcast -- both, since it is
        # subscribed and it is also the one who just traded.
        a.sendall(limit(4, "AAPL", 200, 0, 10050, 3, 0))
        own = read_response(a)
        check("A's own trade", own["trades"], [(200, 100, 10050, 3)])
        push2 = read_market_data(a)
        check("broadcast reflects A's own trade",
              (push2["sequence"], push2["ask_levels"], push2["trades"]),
              (2, [(10050, 2)], [(200, 100, 10050, 3)]))

        # B, never subscribed, has nothing extra waiting.
        check("unsubscribed connection gets nothing extra", read_after_close_probe(b, timeout=1),
              "still-open")

        a.sendall(unsubscribe(5, "AAPL"))
        check("unsubscribe acknowledged", read_response(a)["reason"], "ok")

        b.sendall(limit(6, "AAPL", 300, 1, 10100, 2, 0))
        read_response(b)
        check("no further broadcast after unsubscribing", read_after_close_probe(a, timeout=1),
              "still-open")

        a.sendall(subscribe(7, "NVDA"))
        check("subscribe to an unregistered symbol", read_response(a)["reason"], "unknown-symbol")

        a.close()
        b.close()
    finally:
        stop_server(proc, out)

    # ---- market data depth ----
    # Six distinct ask price levels, one more than the server's default
    # depth of 5 -- checks both that depth is aggregated per price (not per
    # order) and that a level beyond the configured depth is dropped rather
    # than silently included.
    depth_port = free_port()
    proc, out = start_server(binary, depth_port, os.path.join(workdir, "depth.log"),
                             os.path.join(workdir, "depth.out"))
    try:
        d = socket.create_connection(("127.0.0.1", depth_port), timeout=5)
        d.sendall(add_symbol(1, "AAPL"))
        read_response(d)
        prices = [10000, 10010, 10020, 10030, 10040, 10050]
        for i, price in enumerate(prices):
            d.sendall(limit(2 + i, "AAPL", 100 + i, 1, price, 1, 0))
            check(f"depth setup order {i}", read_response(d)["reason"], "ok")

        d.sendall(subscribe(99, "AAPL"))
        snap = read_market_data(d)
        check("depth truncates to the server's configured limit", len(snap["ask_levels"]), 5)
        check("depth keeps the best five prices, best first", snap["ask_levels"],
              [(p, 1) for p in prices[:5]])
        d.close()
    finally:
        stop_server(proc, out)

    # ---- market data gap recovery (ResyncMarketData) ----
    # A short history (2 pushes) so the "gap too wide, fall back to a fresh
    # snapshot" path is reachable without generating hundreds of events.
    resync_port = free_port()
    proc, out = start_server(binary, resync_port, os.path.join(workdir, "resync.log"),
                             os.path.join(workdir, "resync.out"),
                             env_overrides={"MATCHING_ENGINE_MARKET_DATA_HISTORY_LIMIT": "2"})
    try:
        d = socket.create_connection(("127.0.0.1", resync_port), timeout=5)
        d.sendall(add_symbol(1, "AAPL"))
        read_response(d)
        d.sendall(limit(2, "AAPL", 100, 1, 10050, 5, 0))  # seq 1: rest a sell, no trade
        read_response(d)
        d.sendall(limit(3, "AAPL", 200, 0, 10050, 3, 0))  # seq 2: crosses, trades
        read_response(d)
        d.sendall(limit(4, "AAPL", 300, 1, 10100, 4, 0))  # seq 3: rests a second level
        read_response(d)
        # History now holds sequences 2 and 3 only; 1 has been evicted.

        c = socket.create_connection(("127.0.0.1", resync_port), timeout=5)

        # Within history: replays exactly the missed pushes, each carrying
        # this request's correlation id rather than the usual unsolicited 0.
        c.sendall(resync_market_data(50, "AAPL", 1))
        replay1 = read_market_data(c)
        replay2 = read_market_data(c)
        check("resync replays the first missed push", (replay1["corr"], replay1["sequence"],
                                                        replay1["trades"], replay1["ask_levels"]),
              (50, 2, [(200, 100, 10050, 3)], [(10050, 2)]))
        check("resync replays the second missed push", (replay2["corr"], replay2["sequence"],
                                                        replay2["trades"], replay2["ask_levels"]),
              (50, 3, [], [(10050, 2), (10100, 4)]))

        # Gap wider than the retained history (asking since sequence 0, but
        # only 2 and 3 are still buffered): falls back to a single fresh
        # snapshot, same shape a plain re-subscribe would have produced.
        c.sendall(resync_market_data(51, "AAPL", 0))
        fallback = read_market_data(c)
        check("resync falls back to a snapshot when the gap exceeds history",
              (fallback["corr"], fallback["sequence"], fallback["trades"]), (51, 3, []))

        # Already caught up (since_sequence == current): also a snapshot,
        # not zero messages -- so the correlation id still gets a reply.
        c.sendall(resync_market_data(52, "AAPL", 3))
        caught_up = read_market_data(c)
        check("resync when already caught up still replies",
              (caught_up["corr"], caught_up["sequence"], caught_up["trades"]), (52, 3, []))

        # An unregistered symbol is rejected the same way Subscribe rejects
        # one, and resync neither requires nor creates a subscription.
        c.sendall(resync_market_data(53, "NVDA", 0))
        check("resync to an unregistered symbol is rejected", read_response(c)["reason"],
              "unknown-symbol")

        d.close()
        c.close()
    finally:
        stop_server(proc, out)

    # ---- TLS ----
    # Setting only one of the pair is refused before either TLS or a build
    # without it is even relevant -- server_main.cpp checks this first.
    partial_tls_refused = expect_start_failure(
        binary, free_port(), os.path.join(workdir, "partial_tls.log"),
        {"MATCHING_ENGINE_TLS_CERT": "/nonexistent/cert.pem"},
        "must both be set")
    check("TLS refused when only the cert is set", partial_tls_refused, True)

    cert_path = os.path.join(workdir, "test_cert.pem")
    key_path = os.path.join(workdir, "test_key.pem")
    generate_self_signed_cert(cert_path, key_path)

    tls_port = free_port()
    tls_env = {"MATCHING_ENGINE_TLS_CERT": cert_path, "MATCHING_ENGINE_TLS_KEY": key_path}
    try:
        proc, out = start_server(binary, tls_port, os.path.join(workdir, "tls.log"),
                                 os.path.join(workdir, "tls.out"), env_overrides=tls_env)
    except RuntimeError:
        # This binary was built without WITH_TLS -- server_main.cpp refuses
        # to start rather than silently falling back to plaintext, which is
        # exactly what this checks, then stops: nothing below this point can
        # run against a binary that has no TLS support to exercise.
        with open(os.path.join(workdir, "tls.out")) as f:
            startup_output = f.read()
        check("TLS-less binary refuses to start with TLS configured",
              "not built with TLS support" in startup_output, True)
    else:
        try:
            raw = socket.create_connection(("127.0.0.1", tls_port), timeout=5)
            e = wrap_tls(raw)
            check("TLS handshake negotiates a real protocol version",
                  e.version() in ("TLSv1.2", "TLSv1.3"), True)

            e.sendall(add_symbol(1, "AAPL"))
            check("AddSymbol over TLS", read_response(e)["reason"], "ok")
            e.sendall(limit(2, "AAPL", 100, 1, 10050, 5, 0))
            check("LimitOrder over TLS", read_response(e)["reason"], "ok")
            e.close()

            # A plaintext client's bytes are not a valid TLS ClientHello, so
            # the handshake fails and the connection is dropped -- the
            # server itself must not be affected, which the next check
            # (a fresh, correct TLS connection working right after) confirms.
            plain = socket.create_connection(("127.0.0.1", tls_port), timeout=5)
            plain.sendall(add_symbol(9, "AAPL"))
            check("plaintext client against a TLS server gets dropped",
                  read_after_close_probe(plain, timeout=3), "closed")
            plain.close()

            raw2 = socket.create_connection(("127.0.0.1", tls_port), timeout=5)
            e2 = wrap_tls(raw2)
            e2.sendall(add_symbol(10, "MSFT"))
            check("server survives a rejected plaintext connection",
                  read_response(e2)["reason"], "ok")
            e2.close()
        finally:
            stop_server(proc, out)

        # A connection that opens a socket and never speaks TLS at all must
        # not occupy it forever -- MATCHING_ENGINE_TLS_HANDSHAKE_TIMEOUT_SECONDS
        # bounds it. Its own short-lived server, so the 1-second timeout
        # below doesn't affect any of the checks above.
        timeout_port = free_port()
        timeout_env = {"MATCHING_ENGINE_TLS_CERT": cert_path, "MATCHING_ENGINE_TLS_KEY": key_path,
                       "MATCHING_ENGINE_TLS_HANDSHAKE_TIMEOUT_SECONDS": "1"}
        proc, out = start_server(binary, timeout_port, os.path.join(workdir, "tls_timeout.log"),
                                 os.path.join(workdir, "tls_timeout.out"), env_overrides=timeout_env)
        try:
            silent = socket.create_connection(("127.0.0.1", timeout_port), timeout=5)
            check("a connection that never speaks TLS is dropped after the handshake timeout",
                  read_after_close_probe(silent, timeout=3), "closed")
            silent.close()

            # The timeout must never touch a connection that already
            # finished its handshake, no matter how long it then sits idle.
            raw = socket.create_connection(("127.0.0.1", timeout_port), timeout=5)
            established = wrap_tls(raw)
            time.sleep(1.5)  # past the 1-second handshake timeout
            established.sendall(add_symbol(1, "AAPL"))
            check("an established TLS connection is unaffected by the handshake timeout",
                  read_response(established)["reason"], "ok")
            established.close()
        finally:
            stop_server(proc, out)

    shutil.rmtree(workdir, ignore_errors=True)
    print()
    if failures:
        print(f"{len(failures)} FAILURES: {', '.join(failures)}")
        return 1
    print("all wire checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
