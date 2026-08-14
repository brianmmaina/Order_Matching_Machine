#pragma once

// ---------------------------------------------------------------------------
// JSON rendering of protocol messages — a DEBUGGING AFFORDANCE, not the
// protocol. Same framing, same message set, same field names; only the payload
// encoding differs.
//
// It exists so that while building the network layer (sessions 1.2-1.3) a
// message can be read by a human, and so a failing binary round-trip can be
// eyeballed rather than hexdumped.
//
// Deliberately ENCODE-ONLY. A JSON parser is a real parser — quoting, escapes,
// numeric edge cases, nesting — and hand-rolling one to accept untrusted input
// would be a larger and more dangerous surface than the binary codec it is
// meant to help debug. Writing JSON out is a few dozen lines with no parsing
// risk; reading it back is not. If a JSON *input* mode is ever wanted, it
// should come with a real library and its own threat model.
//
// Consequences, stated so nobody assumes otherwise: JSON is never benchmarked,
// never versioned independently, and no client should depend on it.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>

#include "ome/protocol.hpp"
#include "ome/reject_reason.hpp"

namespace ome::protocol::json {

namespace detail {

inline const char* side_name(Side s) noexcept { return s == Side::Bid ? "BID" : "ASK"; }

inline const char* order_type_name(OrderType t) noexcept {
    return t == OrderType::Market ? "MARKET" : "LIMIT";
}

// integers only — no string field in this protocol carries user input, so
// there is nothing here that needs escaping.
inline std::string levels(const std::vector<BookLevel>& ls) {
    std::string s = "[";
    for (std::size_t i = 0; i < ls.size(); ++i) {
        if (i != 0) s += ',';
        s += '[';
        s += std::to_string(ls[i].price_ticks);
        s += ',';
        s += std::to_string(ls[i].quantity);
        s += ']';
    }
    s += ']';
    return s;
}

}  // namespace detail

[[nodiscard]] inline std::string to_json(const NewOrder& m) {
    return R"({"type":"NewOrder","client_order_id":)" + std::to_string(m.client_order_id) +
           R"(,"price_ticks":)" + std::to_string(m.price_ticks) + R"(,"quantity":)" +
           std::to_string(m.quantity) + R"(,"side":")" + detail::side_name(m.side) +
           R"(","order_type":")" + detail::order_type_name(m.order_type) + R"("})";
}

[[nodiscard]] inline std::string to_json(const Cancel& m) {
    return R"({"type":"Cancel","client_order_id":)" + std::to_string(m.client_order_id) + "}";
}

[[nodiscard]] inline std::string to_json(const Modify& m) {
    return R"({"type":"Modify","client_order_id":)" + std::to_string(m.client_order_id) +
           R"(,"new_price_ticks":)" + std::to_string(m.new_price_ticks) + R"(,"new_quantity":)" +
           std::to_string(m.new_quantity) + "}";
}

[[nodiscard]] inline std::string to_json(const Subscribe& m) {
    return R"({"type":"Subscribe","depth":)" + std::to_string(m.depth) + "}";
}

[[nodiscard]] inline std::string to_json(const Ack& m) {
    return R"({"type":"Ack","client_order_id":)" + std::to_string(m.client_order_id) +
           R"(,"exchange_order_id":)" + std::to_string(m.exchange_order_id) + "}";
}

[[nodiscard]] inline std::string to_json(const Reject& m) {
    return R"({"type":"Reject","client_order_id":)" + std::to_string(m.client_order_id) +
           R"(,"reason":")" + to_string(m.reason) + R"(","reason_code":)" +
           std::to_string(static_cast<std::uint16_t>(m.reason)) + "}";
}

[[nodiscard]] inline std::string to_json(const Fill& m) {
    return R"({"type":"Fill","exchange_order_id":)" + std::to_string(m.exchange_order_id) +
           R"(,"price_ticks":)" + std::to_string(m.price_ticks) + R"(,"quantity":)" +
           std::to_string(m.quantity) + R"(,"remaining_quantity":)" +
           std::to_string(m.remaining_quantity) + "}";
}

[[nodiscard]] inline std::string to_json(const BookUpdate& m) {
    return R"({"type":"BookUpdate","seq":)" + std::to_string(m.seq) + R"(,"bids":)" +
           detail::levels(m.bids) + R"(,"asks":)" + detail::levels(m.asks) + "}";
}

[[nodiscard]] inline std::string to_json(const Heartbeat& m) {
    return R"({"type":"Heartbeat","timestamp_ns":)" + std::to_string(m.timestamp_ns) + "}";
}

}  // namespace ome::protocol::json
