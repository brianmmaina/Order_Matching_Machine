#include "matching_engine/matching_engine.hpp"

void MatchingEngine::processOrder(Order order) {
    if (order.type == Order::CANCEL) {
        // [[nodiscard]] on cancelOrder: cast to void explicitly discards result (suppresses compiler warning).
        static_cast<void>(book_.cancelOrder(order.id));
        return;
    }

    if (order.type == Order::MARKET) {
        uint32_t remaining = order.quantity;
        // output params: remaining decremented inside match_market; leftovers are dropped (not reinserted).
        book_.match_market(order.id, order.side, remaining, order.timestamp, trade_log_);
        return;
    }

    if (order.type != Order::LIMIT) {
        return;
    }

    const uint64_t incoming_id = order.id;
    uint32_t remaining = order.quantity;
    // std::move transfers resources from local order into addOrder (cheap for small Order = copies fields).
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
}
