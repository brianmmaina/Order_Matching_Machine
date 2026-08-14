# Order Gateway Protocol v1

A length-prefixed binary protocol over TCP. This document is the contract; if
the code and this file disagree, that is a bug in one of them.

---

## Design rules

**No floating point anywhere in this protocol.** Prices are `int64` ticks
(1 tick = 1/10000 of a currency unit), matching the engine's internal
representation. See `include/ome/ticks.hpp`. A protocol carrying decimals
invites a producer that computes them, and price equality stops being exact.

**Little-endian on the wire, always.** Every multi-byte integer is encoded
little-endian regardless of host byte order. The codec converts explicitly; it
never relies on the host already being little-endian.

**Field-by-field serialization, never `memcpy` of a struct.** Three reasons:
- *Padding.* The compiler inserts padding between members to satisfy alignment.
  This is not hypothetical here: on this build `sizeof(NewOrder)` is **24**
  while its wire encoding is **22**. Two bytes of padding sit after the two
  `uint8_t` fields, they are uninitialized, and their placement can differ
  between compilers, versions, and build flags. `memcpy`ing the struct would put
  uninitialized memory on the wire and make the message length an artifact of
  the compiler rather than a property of the protocol.
- *Endianness.* A struct copy writes host byte order. Two machines that
  disagree then silently misread every integer.
- *Versioning.* Explicit per-field encoding lets v2 add a field and still decode
  v1 messages. A struct copy pins you to one exact memory layout forever.

**Every rejection carries a reason code.** No silent drops. Codes come from
`include/ome/reject_reason.hpp`, which is shared with the engine, metrics, and
logs — there is no separate protocol-side enum to drift out of sync.

---

## Framing

```
+----------------+------------------------+
| MessageHeader  | payload (length bytes) |
+----------------+------------------------+
```

`MessageHeader` is 8 bytes:

| Offset | Size | Field     | Notes                                        |
|--------|------|-----------|----------------------------------------------|
| 0      | 4    | `length`  | payload bytes following the header, not incl. header |
| 4      | 2    | `type`    | `MessageType` value                          |
| 6      | 2    | `version` | protocol version; `1` for everything here    |

**Length prefix, not a delimiter.** A delimiter has to be escaped everywhere it
appears in the payload, and binary payloads contain every byte value including
whatever delimiter you picked. Length-prefixing means the reader knows exactly
how many bytes to wait for before it parses anything.

`length` is capped at `kMaxPayloadSize` (64 KiB). A header claiming more is
malformed — the cap exists so a corrupt or hostile length field cannot make the
server allocate an arbitrary buffer.

**A frame does not correspond to a `recv()` call.** TCP is a byte stream: one
frame can arrive in several segments, and several frames can arrive in one read.
The reassembly logic is Session 1.2's `FrameReader`.

---

## Message types

| Value | Message      | Direction       |
|-------|--------------|-----------------|
| 1     | `NewOrder`   | client → server |
| 2     | `Cancel`     | client → server |
| 3     | `Modify`     | client → server |
| 4     | `Subscribe`  | client → server |
| 10    | `Ack`        | server → client |
| 11    | `Reject`     | server → client |
| 12    | `Fill`       | server → client |
| 13    | `BookUpdate` | server → client |
| 14    | `Heartbeat`  | both            |

Values are **append-only**, like reject codes. Client→server occupies 1–9,
server→client 10+.

### Client → server

**`NewOrder`** (22 bytes)

| Size | Field             | Notes                              |
|------|-------------------|------------------------------------|
| 8    | `client_order_id` | unique within the session          |
| 8    | `price_ticks`     | ignored for `MARKET`               |
| 4    | `quantity`        | must be > 0                        |
| 1    | `side`            | 0 = BID, 1 = ASK                   |
| 1    | `order_type`      | 0 = MARKET, 1 = LIMIT              |

**`Cancel`** (8 bytes) — `client_order_id`.

**`Modify`** (20 bytes) — `client_order_id`, `new_price_ticks`, `new_quantity`.

**`Subscribe`** (1 byte) — `depth`, clamped to 10.

### Server → client

**`Ack`** (16 bytes) — `client_order_id`, `exchange_order_id`.

**`Reject`** (10 bytes) — `client_order_id`, `reason` (u16, `RejectReason`).

**`Fill`** (24 bytes) — `exchange_order_id`, `price_ticks`, `quantity`,
`remaining_quantity`.

**`BookUpdate`** (variable) — `u64 seq`, `u8 bid_count`, `u8 ask_count`, then
`bid_count` levels followed by `ask_count` levels, each `{i64 price_ticks,
u64 quantity}`. Bids descending, asks ascending, each side ≤ 10.

**`Heartbeat`** (8 bytes) — `timestamp_ns`.

---

## Behavior the client must know

These are protocol-level facts, not implementation details. A client that
assumes otherwise will mis-model its own order state.

### Modify is cancel-and-replace, and it costs queue position

The engine has no in-place modify. `Modify` is executed as a cancel of the
existing order followed by a new order, applied as a unit on the matching
thread. Therefore:

| Change              | Time priority |
|---------------------|---------------|
| Price changed       | **lost**      |
| Quantity increased  | **lost**      |
| Quantity decreased  | **retained**  |
| Quantity unchanged  | retained      |

This matches how real exchanges treat amendments: you may reduce your
commitment without penalty, but asking for more — at a better price or in
greater size — puts you behind everyone who was already asking for it. A
successful `Modify` returns an `Ack` with a **new** `exchange_order_id` when
priority was lost.

### Ack precedes Fill, always

A marketable limit order is inserted into the book first and matched second.
An order that trades immediately therefore produces `Ack` and *then* `Fill`,
never the reverse. Clients must not treat an `Ack` as "did not trade".

### A client can trade with itself

There is no self-trade prevention. A session resting a bid and then sending a
crossing ask will match against its own order and receive both sides' fills.
This is a known limitation, not an accident.

### Unknown message types are rejected, not ignored

A well-formed frame with an unrecognized `type` gets
`Reject{UNKNOWN_MESSAGE_TYPE}`. A frame whose payload does not decode gets
`Reject{MALFORMED}`. Neither is silently dropped — a client that is talking
nonsense should be told, not left waiting.

### Version mismatch

A header whose `version` is not `1` is rejected with `MALFORMED`. v1 has no
negotiation: the field exists so that a future version can add one without
having to change the header layout, which would be impossible to do compatibly
if the field were not already there.

---

## Reason codes

From `include/ome/reject_reason.hpp`. Numeric values are append-only forever;
a client built against v1 must keep decoding them correctly against a later
server.

| Value | Code                   | Meaning                                        |
|-------|------------------------|------------------------------------------------|
| 1     | `UNKNOWN_ORDER`        | cancel/modify named an id the book lacks       |
| 2     | `INVALID_PRICE`        | non-positive, or not tick-aligned              |
| 3     | `INVALID_QTY`          | zero quantity                                  |
| 4     | `RISK_MAX_ORDER_SIZE`  | above the per-order cap                        |
| 5     | `RISK_PRICE_BAND`      | too far from the reference price               |
| 6     | `MALFORMED`            | payload did not decode, or bad version         |
| 7     | `RATE_LIMITED`         | session exceeded its token bucket              |
| 8     | `NOT_SUBSCRIBED`       | market-data request without a subscription     |
| 9     | `DUPLICATE_ORDER_ID`   | `client_order_id` reused within a session      |
| 10    | `UNKNOWN_MESSAGE_TYPE` | well-formed frame, unrecognized type           |

---

## JSON debug mode

The gateway can be started with JSON framing instead of binary. **Same framing
(4-byte length prefix), same message set, same field names** — only the payload
encoding differs. It exists so the server can be poked by hand while building
the network layer, and so a failing binary case can be eyeballed.

```json
{"type":"NewOrder","client_order_id":1,"price_ticks":1000000,"quantity":10,"side":"BID","order_type":"LIMIT"}
```

Binary is the real protocol. JSON is a debugging affordance and is not
benchmarked, not versioned independently, and not something a client should
depend on.

---

## Not in v1

Named so they are known gaps rather than oversights:

- **No authentication, no TLS.** Anyone who can reach the port can trade.
- **No delta encoding for market data.** `BookUpdate` is a full top-N snapshot.
  Deltas are a real optimization; they are not required to make the point.
- **Single symbol.** No instrument identifier in any message.
- **No sequence numbers on order flow.** Only `BookUpdate` carries a `seq`.
  Detecting a gap in one's own order events is not possible in v1.
