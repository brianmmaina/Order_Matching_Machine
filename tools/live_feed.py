#!/usr/bin/env python3
"""Bridge the live gateway to tools/book_replay.html.

Connects, subscribes to market data, and re-emits each BookUpdate as one line of
the JSONL format the visualizer already consumes — so the same page that plays
back a LOBSTER file becomes a live view of a running exchange, with no changes
to it at all.

That is by construction rather than luck: the JSONL record and the BookUpdate
message both carry (int64 ticks, uint64 qty) level pairs, because the format was
defined in terms of the engine's accessors before either existed. See
include/ome/book_jsonl.hpp.

    ./build/gateway --port 9001 &
    python3 tools/live_feed.py --port 9001 --out /tmp/live.jsonl &
    open tools/book_replay.html          # then load /tmp/live.jsonl

Like tools/smoke_client.py, the wire format is packed by hand rather than shared
with the C++ codec: two independent implementations agreeing is evidence the
spec is unambiguous, not that one misunderstanding was copied twice.
"""

import argparse
import json
import socket
import struct
import sys
import time

HEADER = struct.Struct("<IHH")  # length, type, version
VERSION = 1

MSG_SUBSCRIBE = 4
MSG_BOOK_UPDATE = 13
MSG_HEARTBEAT = 14
MSG_REJECT = 11

REASONS = {
    6: "MALFORMED", 7: "RATE_LIMITED", 8: "NOT_SUBSCRIBED", 11: "NOT_IMPLEMENTED",
}


def frame(msg_type, payload):
    return HEADER.pack(len(payload), msg_type, VERSION) + payload


def subscribe(depth):
    return frame(MSG_SUBSCRIBE, struct.pack("<B", depth))


def recv_exactly(sock, n):
    """recv() returning fewer bytes than asked is normal, not an error."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError(f"gateway closed after {len(buf)} of {n} bytes")
        buf += chunk
    return buf


def recv_frame(sock):
    length, msg_type, version = HEADER.unpack(recv_exactly(sock, HEADER.size))
    return msg_type, version, recv_exactly(sock, length)


def parse_book_update(payload):
    """u64 seq, u8 n_bids, u8 n_asks, then the levels: {i64 ticks, u64 qty}."""
    seq, n_bids, n_asks = struct.unpack_from("<QBB", payload, 0)
    off = 10
    levels = []
    for _ in range(n_bids + n_asks):
        ticks, qty = struct.unpack_from("<qQ", payload, off)
        levels.append([ticks, qty])
        off += 16
    return seq, levels[:n_bids], levels[n_bids:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9001)
    ap.add_argument("--depth", type=int, default=10)
    ap.add_argument("--out", help="JSONL output file (default: stdout)")
    ap.add_argument("--limit", type=int, default=0, help="stop after N updates (0 = forever)")
    args = ap.parse_args()

    out = open(args.out, "w") if args.out else sys.stdout
    count = 0

    with socket.create_connection((args.host, args.port), timeout=10) as sock:
        sock.sendall(subscribe(args.depth))
        print(f"subscribed at depth {args.depth}", file=sys.stderr)

        while True:
            try:
                msg_type, _, payload = recv_frame(sock)
            except (ConnectionError, socket.timeout) as e:
                print(f"disconnected: {e}", file=sys.stderr)
                break

            if msg_type == MSG_BOOK_UPDATE:
                seq, bids, asks = parse_book_update(payload)
                # Wall clock at publish. The visualizer uses `t` for pacing only
                # and `seq` for ordering, which matters here because conflation
                # skips seq values — a consumer treating gaps as loss would flag
                # normal operation.
                rec = {"t": time.time_ns(), "seq": seq, "bids": bids, "asks": asks}
                out.write(json.dumps(rec, separators=(",", ":")) + "\n")
                out.flush()
                count += 1
                if args.limit and count >= args.limit:
                    break
            elif msg_type == MSG_HEARTBEAT:
                # Any inbound message proves liveness to the server, and the
                # server's own heartbeat is the natural thing to echo. Without
                # this a passive subscriber is timed out for being quiet.
                sock.sendall(frame(MSG_HEARTBEAT, payload))
            elif msg_type == MSG_REJECT:
                _, reason = struct.unpack("<QH", payload)
                print(f"rejected: {REASONS.get(reason, reason)}", file=sys.stderr)
                return 1

    print(f"wrote {count} book updates", file=sys.stderr)
    if args.out:
        out.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
