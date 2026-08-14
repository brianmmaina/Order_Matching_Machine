#pragma once

// ---------------------------------------------------------------------------
// Why an order was refused. Single source of truth, shared by the engine, the
// wire protocol, metrics, and structured logs.
//
// There is exactly one of these enums in the codebase. A parallel "protocol
// reject code" that has to be mapped to an "engine reject code" is two things
// that drift, and the drift shows up as a client being told the wrong reason.
//
// THE NUMERIC VALUES GO ON THE WIRE. They are append-only, forever:
//   - never renumber an existing code
//   - never reuse the number of a code you delete (don't delete codes)
//   - new codes take the next free value
// A client built against v1 must keep decoding v1 codes correctly against a v2
// server, and the only thing making that true is that 4 always means 4.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace ome {

enum class RejectReason : std::uint16_t {
    NONE = 0,  // accepted; not a rejection

    // order-level
    UNKNOWN_ORDER = 1,   // cancel/modify naming an id the book does not hold
    INVALID_PRICE = 2,   // non-positive, or not aligned to the configured tick size
    INVALID_QTY = 3,     // zero quantity
    DUPLICATE_ORDER_ID = 9,  // client reused a client_order_id within one session

    // risk limits
    RISK_MAX_ORDER_SIZE = 4,  // quantity above the configured per-order cap
    RISK_PRICE_BAND = 5,      // price too far from the reference price

    // session / transport
    MALFORMED = 6,             // frame did not decode
    RATE_LIMITED = 7,          // session exceeded its token bucket
    NOT_SUBSCRIBED = 8,        // market-data request from a session that never subscribed
    UNKNOWN_MESSAGE_TYPE = 10  // well-formed frame, unrecognized type field
};

// Stable identifiers for logs and Prometheus label values. Deliberately the
// enumerator spelling: a metrics query written against these should not break
// because someone reworded a human-facing string.
[[nodiscard]] constexpr const char* to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::NONE:                 return "NONE";
        case RejectReason::UNKNOWN_ORDER:        return "UNKNOWN_ORDER";
        case RejectReason::INVALID_PRICE:        return "INVALID_PRICE";
        case RejectReason::INVALID_QTY:          return "INVALID_QTY";
        case RejectReason::RISK_MAX_ORDER_SIZE:  return "RISK_MAX_ORDER_SIZE";
        case RejectReason::RISK_PRICE_BAND:      return "RISK_PRICE_BAND";
        case RejectReason::MALFORMED:            return "MALFORMED";
        case RejectReason::RATE_LIMITED:         return "RATE_LIMITED";
        case RejectReason::NOT_SUBSCRIBED:       return "NOT_SUBSCRIBED";
        case RejectReason::DUPLICATE_ORDER_ID:   return "DUPLICATE_ORDER_ID";
        case RejectReason::UNKNOWN_MESSAGE_TYPE: return "UNKNOWN_MESSAGE_TYPE";
    }
    // reached only if a code was added above without a case here; -Wswitch
    // catches that at compile time, so this is belt-and-braces for a value
    // cast in from the wire.
    return "UNRECOGNIZED";
}

}  // namespace ome
