#!/usr/bin/env python3
"""Manual smoke test for the order gateway.

Connects, sends one NewOrder, prints the Ack. Protocol: docs/PROTOCOL.md.

    ./build/gateway --port 9001 &
    python3 tools/smoke_client.py --port 9001

Deliberately hand-rolled struct packing rather than importing anything: this is
an independent implementation of the wire format, so if it agrees with the C++
codec that is real evidence the spec is unambiguous rather than two copies of
the same misunderstanding.
"""

import argparse
import socket
import struct
import sys

HEADER = struct.Struct("<IHH")  # length, type, version
VERSION = 1

MSG_NEW_ORDER = 1
MSG_ACK = 10
MSG_REJECT = 11

REASONS = {
    0: "NONE", 1: "UNKNOWN_ORDER", 2: "INVALID_PRICE", 3: "INVALID_QTY",
    4: "RISK_MAX_ORDER_SIZE", 5: "RISK_PRICE_BAND", 6: "MALFORMED",
    7: "RATE_LIMITED", 8: "NOT_SUBSCRIBED", 9: "DUPLICATE_ORDER_ID",
    10: "UNKNOWN_MESSAGE_TYPE",
}


def new_order(client_order_id, price_ticks, quantity, side=0, order_type=1):
    payload = struct.pack("<QqIBB", client_order_id, price_ticks, quantity, side, order_type)
    return HEADER.pack(len(payload), MSG_NEW_ORDER, VERSION) + payload


def recv_exactly(sock, n):
    """Read exactly n bytes. recv() returning fewer is normal, not an error."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError(f"peer closed after {len(buf)} of {n} bytes")
        buf += chunk
    return buf


def recv_frame(sock):
    length, msg_type, version = HEADER.unpack(recv_exactly(sock, HEADER.size))
    return msg_type, version, recv_exactly(sock, length)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9001)
    ap.add_argument("--price", type=int, default=1000000, help="price in ticks (1/10000 unit)")
    ap.add_argument("--qty", type=int, default=10)
    ap.add_argument("--split", action="store_true",
                    help="send the frame one byte at a time, to exercise reassembly")
    args = ap.parse_args()

    with socket.create_connection((args.host, args.port), timeout=5) as sock:
        frame = new_order(client_order_id=1, price_ticks=args.price, quantity=args.qty)
        print(f"-> NewOrder id=1 price_ticks={args.price} qty={args.qty} ({len(frame)} bytes)")

        if args.split:
            # Forces the server to reassemble across 30 separate reads.
            print("   sending one byte per packet")
            for b in frame:
                sock.sendall(bytes([b]))
        else:
            sock.sendall(frame)

        msg_type, version, payload = recv_frame(sock)
        if msg_type == MSG_ACK:
            coid, xoid = struct.unpack("<QQ", payload)
            print(f"<- Ack client_order_id={coid} exchange_order_id={xoid} (v{version})")
            return 0
        if msg_type == MSG_REJECT:
            coid, reason = struct.unpack("<QH", payload)
            print(f"<- Reject client_order_id={coid} reason={REASONS.get(reason, reason)}")
            return 1
        print(f"<- unexpected message type {msg_type}")
        return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ConnectionError, socket.timeout, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
