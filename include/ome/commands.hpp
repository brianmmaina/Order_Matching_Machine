#pragma once

// ---------------------------------------------------------------------------
// The ONLY two types that cross the thread boundary.
//
//   network thread --OrderCommand--> [SPSC queue] --> matching thread
//   matching thread --OrderEvent--> [per-session SPSC queue] --> network thread
//
// Everything else is owned by exactly one thread. The book, the order-to-session
// map, and the exchange id counter belong to the matching thread; sockets, read
// buffers and write buffers belong to the network thread. Neither reaches into
// the other's state, which is what makes the book single-writer without a lock
// anywhere near it.
//
// DELIBERATELY TRIVIALLY COPYABLE, fixed size, no heap.
//
// The SPSC ring stores elements by value, so a type owning heap memory would
// mean allocating on the network thread and freeing on the matching thread for
// every single message — cross-thread allocator traffic on the hot path, and a
// destructor running wherever the ring slot happens to be reused. Flat structs
// with a tag and a union-like payload keep the queue a memcpy and keep
// ownership unambiguous.
//
// The cost is that a command cannot carry variable-length data. Nothing here
// needs to; if something ever does, it belongs in a side channel with its own
// lifetime, not smuggled through the ring as a pointer.
// ---------------------------------------------------------------------------

#include <cstdint>

#include "ome/protocol.hpp"
#include "ome/reject_reason.hpp"
#include "ome/session.hpp"

namespace ome {

class EgressQueue;  // pointer only; defined in egress.hpp

enum class CommandType : std::uint8_t {
    // Hands the matching thread the pointer to this session's egress queue.
    // Registration travels the command queue like everything else — a side
    // channel would be shared mutable state between the two threads, which is
    // the thing this design does not have.
    SessionOpened,
    NewOrder,
    Cancel,
    Modify,
    // Emitted when a session dies. Travels the SAME queue as everything else
    // rather than reaching into the book directly — which is the entire reason
    // cancel-on-disconnect needs no lock. The network thread notices the death;
    // the matching thread performs the cancels, in order, with every other
    // command.
    CancelAllForSession,
};

struct OrderCommand {
    CommandType type{CommandType::NewOrder};
    SessionId session{0};
    std::uint64_t client_order_id{0};
    std::int64_t price_ticks{0};
    std::uint32_t quantity{0};
    protocol::Side side{protocol::Side::Bid};
    protocol::OrderType order_type{protocol::OrderType::Limit};
    // Only meaningful for SessionOpened. A raw pointer is fine here precisely
    // because the tombstone protocol in egress.hpp defines its lifetime: the
    // Connection owns the queue and cannot free it until it observes
    // SessionRetired coming back the other way.
    EgressQueue* egress{nullptr};

    static OrderCommand session_opened(SessionId s, EgressQueue* q) {
        OrderCommand c{};
        c.type = CommandType::SessionOpened;
        c.session = s;
        c.egress = q;
        return c;
    }

    static OrderCommand new_order(SessionId s, const protocol::NewOrder& m) {
        OrderCommand c{};
        c.type = CommandType::NewOrder;
        c.session = s;
        c.client_order_id = m.client_order_id;
        c.price_ticks = m.price_ticks;
        c.quantity = m.quantity;
        c.side = m.side;
        c.order_type = m.order_type;
        return c;
    }

    static OrderCommand cancel(SessionId s, std::uint64_t client_order_id) {
        OrderCommand c{};
        c.type = CommandType::Cancel;
        c.session = s;
        c.client_order_id = client_order_id;
        return c;
    }

    static OrderCommand modify(SessionId s, const protocol::Modify& m) {
        OrderCommand c{};
        c.type = CommandType::Modify;
        c.session = s;
        c.client_order_id = m.client_order_id;
        c.price_ticks = m.new_price_ticks;
        c.quantity = m.new_quantity;
        return c;
    }

    static OrderCommand cancel_all(SessionId s) {
        OrderCommand c{};
        c.type = CommandType::CancelAllForSession;
        c.session = s;
        return c;
    }
};

enum class EventType : std::uint8_t {
    Ack,
    Reject,
    Fill,
    // The matching thread has finished with this session and will never push to
    // its egress queue again. See egress.hpp — this is the tombstone that makes
    // freeing the queue safe.
    SessionRetired,
};

struct OrderEvent {
    EventType type{EventType::Ack};
    SessionId session{0};
    std::uint64_t client_order_id{0};
    std::uint64_t exchange_order_id{0};
    std::int64_t price_ticks{0};
    std::uint32_t quantity{0};
    std::uint32_t remaining_quantity{0};
    RejectReason reason{RejectReason::NONE};
    // The order is no longer live: cancelled, fully filled, or rejected. Only
    // the matching thread knows this — an order can fill in the window between
    // a client sending Cancel and the cancel being applied — so it has to be
    // stated on the event rather than inferred on the network side.
    bool closes_order{false};

    static OrderEvent ack(SessionId s, std::uint64_t coid, std::uint64_t xoid,
                          bool closes = false) {
        OrderEvent e{};
        e.type = EventType::Ack;
        e.session = s;
        e.client_order_id = coid;
        e.exchange_order_id = xoid;
        e.closes_order = closes;
        return e;
    }

    static OrderEvent reject(SessionId s, std::uint64_t coid, RejectReason r) {
        OrderEvent e{};
        e.type = EventType::Reject;
        e.session = s;
        e.client_order_id = coid;
        e.reason = r;
        e.closes_order = true;  // a rejected order never rested
        return e;
    }

    static OrderEvent fill(SessionId s, std::uint64_t coid, std::uint64_t xoid, std::int64_t px,
                           std::uint32_t qty, std::uint32_t remaining) {
        OrderEvent e{};
        e.type = EventType::Fill;
        e.session = s;
        e.client_order_id = coid;
        e.closes_order = (remaining == 0);
        e.exchange_order_id = xoid;
        e.price_ticks = px;
        e.quantity = qty;
        e.remaining_quantity = remaining;
        return e;
    }

    static OrderEvent session_retired(SessionId s) {
        OrderEvent e{};
        e.type = EventType::SessionRetired;
        e.session = s;
        return e;
    }
};

static_assert(std::is_trivially_copyable<OrderCommand>::value,
              "OrderCommand crosses a lock-free ring by value");
static_assert(std::is_trivially_copyable<OrderEvent>::value,
              "OrderEvent crosses a lock-free ring by value");

}  // namespace ome
