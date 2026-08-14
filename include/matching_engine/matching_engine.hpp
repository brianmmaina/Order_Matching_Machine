#pragma once

#include <cstddef>
#include <vector>

#include "ome/reject_reason.hpp"
#include "order.h"
#include "order_book/order_book.hpp"
#include "trade.h"

// outcome of applying one command to the engine.
//
// processOrder used to return void, which made an unknown-id cancel
// indistinguishable from a successful one — the gateway needs to send a
// Reject with a reason, so the engine has to say which happened.
//
// deliberately NOT carrying the trades: they live in trade_log(), and copying
// a vector out of every apply would allocate on the matching thread's hot path.
// see trade_log_size() below for how a caller reads the trades one command
// produced.
struct ApplyResult {
    bool accepted{};
    ome::RejectReason reason{ome::RejectReason::NONE};
    // quantity that traded immediately. 0 for a limit order that rested whole.
    std::uint32_t filled_qty{};
    // true when nothing remains: fully traded, or a market order that exhausted
    // the book and had its remainder dropped.
    bool fully_filled{};
};

// orchestrates: book resting + trade log; dispatches by Order::Type.
class MatchingEngine {
public:
    ApplyResult processOrder(Order order);
    // pass-by-value Order: callee receives a copy (or move from caller); cheap for small-ish pod-like structs.

    // const overload accessor: callers read the log without copying the whole vector.
    [[nodiscard]] const std::vector<Trade>& trade_log() const noexcept { return trade_log_; }

    // Reading the trades ONE command produced, without copying:
    //
    //     const std::size_t before = eng.trade_log_size();
    //     const ApplyResult r = eng.processOrder(cmd);
    //     for (std::size_t i = before; i < eng.trade_log_size(); ++i) { ... }
    //     eng.clear_trade_log();
    //
    // The gateway's matching thread clears every iteration, which also bounds
    // the vector — it otherwise grows for the lifetime of the process. The
    // LOBSTER replay path never clears, because it wants the full history.
    [[nodiscard]] std::size_t trade_log_size() const noexcept { return trade_log_.size(); }
    void clear_trade_log() noexcept { trade_log_.clear(); }

    // tooling / replay: direct access for lobster validation and partial cancel application.
    [[nodiscard]] order_book::OrderBook& book() noexcept { return book_; }
    [[nodiscard]] const order_book::OrderBook& book() const noexcept { return book_; }

private:
    order_book::OrderBook book_;
    std::vector<Trade> trade_log_;
};
