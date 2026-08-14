#include "matching_engine/matching_engine.hpp"

ApplyResult MatchingEngine::processOrder(Order order) {
    if (order.type == Order::CANCEL) {
        // the bool used to be discarded, which meant a cancel naming an id the
        // book never held looked exactly like a successful one. the gateway has
        // to tell the client which it was.
        const bool removed = book_.cancelOrder(order.id);
        return ApplyResult{removed,
                           removed ? ome::RejectReason::NONE : ome::RejectReason::UNKNOWN_ORDER,
                           0, false};
    }

    if (order.type == Order::MARKET) {
        const std::uint32_t requested = order.quantity;
        std::uint32_t remaining = requested;
        // output params: remaining decremented inside match_market; leftovers are dropped (not reinserted).
        book_.match_market(order.id, order.side, remaining, order.timestamp, trade_log_);
        // a market order is never "left over" — an unfilled remainder is
        // discarded rather than rested, so it is done either way.
        return ApplyResult{true, ome::RejectReason::NONE, requested - remaining, true};
    }

    if (order.type != Order::LIMIT) {
        // unreachable while Order::Type has three values; kept so a future type
        // added without a branch here is refused loudly instead of silently
        // vanishing.
        return ApplyResult{false, ome::RejectReason::UNKNOWN_MESSAGE_TYPE, 0, false};
    }

    const uint64_t incoming_id = order.id;
    const std::uint32_t requested = order.quantity;
    std::uint32_t remaining = requested;
    // std::move transfers resources from local order into addOrder (cheap for small Order = copies fields).
    //
    // note the ordering: the aggressive order RESTS FIRST and is then matched by
    // the cross loop below. that is why the gateway emits Ack before any Fill,
    // and why a client holding both sides can match against itself.
    book_.addOrder(std::move(order));

    while (remaining > 0 && book_.is_crossed()) {
        Trade t{};
        if (!book_.execute_top_cross(t)) {
            break;
        }
        trade_log_.push_back(t);
        if (t.buyer_id == incoming_id || t.seller_id == incoming_id) {
            remaining -= t.quantity;
        }
    }

    return ApplyResult{true, ome::RejectReason::NONE, requested - remaining, remaining == 0};
}
