#pragma once

#include <vector>

#include "order.h"
#include "order_book/order_book.hpp"
#include "trade.h"

// orchestrates: book resting + trade log; dispatches by Order::Type.
class MatchingEngine {
public:
    void processOrder(Order order);
    // pass-by-value Order: callee receives a copy (or move from caller); cheap for small-ish pod-like structs.

    // const overload accessor: callers read the log without copying the whole vector.
    [[nodiscard]] const std::vector<Trade>& trade_log() const noexcept { return trade_log_; }

    // tooling / replay: direct access for lobster validation and partial cancel application.
    [[nodiscard]] order_book::OrderBook& book() noexcept { return book_; }
    [[nodiscard]] const order_book::OrderBook& book() const noexcept { return book_; }

private:
    order_book::OrderBook book_;
    std::vector<Trade> trade_log_;
};
