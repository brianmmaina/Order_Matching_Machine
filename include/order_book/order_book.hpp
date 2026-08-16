#pragma once

#include <cstdint>
#include <utility>
#include <deque>
// std::deque: sequence container with o(1) push/pop at both ends; iterators can invalidate on inserts unlike list but better locality than list for fifo queues.

#include <unordered_map>
// hash table: average o(1) lookup by order id for cancel path; tradeoff: no stable iteration order.

#include <vector>
// contiguous storage for price levels; pairs with std::lower_bound for sorted insertion.

#include "order.h"
#include "trade.h"

namespace order_book {

// one price level: key price plus time-priority fifo of resting orders.
struct PriceLevel {
    // integer ticks; level identity is exact equality on this field.
    std::int64_t price_ticks{};
    // Running sum of orders' quantities at this level.
    //
    // Maintained incrementally rather than recomputed because the level
    // accessors below sit on the matching thread's hot path — the price-band
    // risk check consults the top of book on every order, and market-data
    // publication walks the top ten levels after every book change. Summing a
    // deque on each call makes both O(orders at the level), which at a hundred
    // thousand resting orders is not a constant factor.
    //
    // Every mutation site must keep this in step; OrderBook::levels_consistent()
    // is the invariant, and the tests assert it after every operation.
    std::uint64_t total_qty{};
    std::deque<Order> orders;
};

class OrderBook {
public:
    OrderBook() = default;
    // = default: compiler-generated ctor; trivial here (members initialize themselves or are empty).

    void addOrder(Order order);
    // [[nodiscard]] warns if caller ignores return value (here: bool means "found and removed").
    [[nodiscard]] bool cancelOrder(uint64_t id);

    [[nodiscard]] bool is_crossed() const noexcept;
    // const member: cannot mutate *this; noexcept: promises no exceptions (used for cheap observers).
    // top-of-book cross: best bid >= best ask after limits rest.

    [[nodiscard]] bool execute_top_cross(Trade& trade);
    // output parameter: fills trade in place; avoids allocating a return struct each call.

    // out-parameter quantity_io: remaining aggressive qty updated in place; std::vector<Trade>& appends prints.
    void match_market(uint64_t market_order_id, Order::Side side, uint32_t& quantity_io,
                      uint64_t market_timestamp, std::vector<Trade>& trades_out);

    // lobster-style partial delete: set resting quantity; 0 removes the order.
    [[nodiscard]] bool replace_remaining_quantity(uint64_t id, uint32_t new_remaining);

    // LOBSTER types 4/5: remove traded_size from passive side at price (FIFO within the level).
    // Missing level or insufficient size returns false; missing level may be a no-op if allow_missing_level.
    [[nodiscard]] bool reduce_level_after_lobster_execution(Order::Side passive_side,
                                                           std::int64_t price_ticks,
                                                           uint32_t traded_size,
                                                           bool allow_missing_level = false);

    // Best price only, no quantity — O(1) and does not touch the orders at all.
    // This is what the price-band check needs, and using the full level
    // accessor for it was walking the whole top level on every order.
    [[nodiscard]] bool best_bid_ticks(std::int64_t& out) const noexcept;
    [[nodiscard]] bool best_ask_ticks(std::int64_t& out) const noexcept;

    // Test-only: verifies every level's cached total_qty equals the sum of its
    // orders. Cheap to call, O(orders), and never used in production.
    [[nodiscard]] bool levels_consistent() const;

    // aggregated sizes per price level, best levels first. prices are already ticks.
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::uint64_t>> bid_levels_ticks(
        std::size_t max_levels) const;
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::uint64_t>> ask_levels_ticks(
        std::size_t max_levels) const;

private:
    // bids sorted descending by price so bids_[0] is best bid; asks ascending so asks_[0] is best ask.
    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;
    // std::pair in map value: side + price to locate level for cancel without scanning all levels.
    std::unordered_map<uint64_t, std::pair<Order::Side, std::int64_t>> order_loc_;
};

}  // namespace order_book
