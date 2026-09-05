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
import struct
import subprocess
import sys
import tempfile
import time

VERSION = 1
ADD_SYMBOL, LIMIT, MARKET, MODIFY, CANCEL, RESPONSE = 1, 2, 3, 4, 5, 6
REASONS = {0: "ok", 1: "duplicate-id", 2: "unknown-order", 3: "bad-quantity", 4: "unknown-symbol"}


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


def start_server(binary, port, log_path, out_path):
    out = open(out_path, "w")
    proc = subprocess.Popen([binary, str(port), log_path], stdout=out, stderr=subprocess.STDOUT)
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


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


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

    shutil.rmtree(workdir, ignore_errors=True)
    print()
    if failures:
        print(f"{len(failures)} FAILURES: {', '.join(failures)}")
        return 1
    print("all wire checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
