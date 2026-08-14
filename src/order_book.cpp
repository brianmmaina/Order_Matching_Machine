#include "order_book/order_book.hpp"

#include <algorithm>
// std::lower_bound: binary search on sorted range; std::min/max on scalars.

namespace order_book {

// note: vector-of-levels vs map — contiguous levels help cache locality; inserts shift elements o(n) per level count.

void OrderBook::addOrder(Order order) {
    if (order.type != Order::LIMIT) {
        return;
    }

    const uint64_t id = order.id;
    const std::int64_t price = order.price_ticks;
    const Order::Side side = order.side;

    if (side == Order::BID) {
        // lower_bound with custom comparator: finds insert position maintaining descending
        // price (pl.price_ticks > p). integer compare — level identity is exact, no epsilon.
        auto it = std::lower_bound(
            bids_.begin(), bids_.end(), price,
            [](const PriceLevel& pl, std::int64_t p) { return pl.price_ticks > p; });
        if (it != bids_.end() && it->price_ticks == price) {
            it->orders.push_back(std::move(order));
        } else {
            PriceLevel level;
            level.price_ticks = price;
            level.orders.push_back(std::move(order));
            // vector::insert before iterator; may reallocate and shift — o(n) in number of levels.
            bids_.insert(it, std::move(level));
        }
    } else {
        auto it = std::lower_bound(
            asks_.begin(), asks_.end(), price,
            [](const PriceLevel& pl, std::int64_t p) { return pl.price_ticks < p; });
        if (it != asks_.end() && it->price_ticks == price) {
            it->orders.push_back(std::move(order));
        } else {
            PriceLevel level;
            level.price_ticks = price;
            level.orders.push_back(std::move(order));
            asks_.insert(it, std::move(level));
        }
    }

    // unordered_map::operator[] default-constructs value if missing, then assigns pair.
    order_loc_[id] = {side, price};
}

bool OrderBook::cancelOrder(uint64_t id) {
    const auto loc_it = order_loc_.find(id);
    if (loc_it == order_loc_.end()) {
        return false;
    }

    const Order::Side side = loc_it->second.first;
    const std::int64_t price = loc_it->second.second;

    // generic lambda (c++14): operator() is a template; instantiated for each vector<pricelevel> book_side type.
    auto erase_from = [&](std::vector<PriceLevel>& book, bool bid_side) -> bool {
        auto it = bid_side
                      ? std::lower_bound(
                            book.begin(), book.end(), price,
                            [](const PriceLevel& pl, std::int64_t p) { return pl.price_ticks > p; })
                      : std::lower_bound(
                            book.begin(), book.end(), price,
                            [](const PriceLevel& pl, std::int64_t p) { return pl.price_ticks < p; });
        if (it == book.end() || it->price_ticks != price) {
            return false;
        }
        auto& dq = it->orders;
        for (auto qit = dq.begin(); qit != dq.end(); ++qit) {
            if (qit->id == id) {
                dq.erase(qit);
                if (dq.empty()) {
                    book.erase(it);
                }
                return true;
            }
        }
        return false;
    };

    const bool removed =
        (side == Order::BID) ? erase_from(bids_, true) : erase_from(asks_, false);
    if (removed) {
        order_loc_.erase(loc_it);
    }
    return removed;
}

bool OrderBook::is_crossed() const noexcept {
    if (bids_.empty() || asks_.empty()) {
        return false;
    }
    return bids_.front().price_ticks >= asks_.front().price_ticks;
}

bool OrderBook::execute_top_cross(Trade& trade) {
    if (!is_crossed()) {
        return false;
    }

    auto& bid_level = bids_.front();
    auto& ask_level = asks_.front();
    auto& bid_order = bid_level.orders.front();
    auto& ask_order = ask_level.orders.front();

    const uint32_t qty = std::min(bid_order.quantity, ask_order.quantity);
    trade.buyer_id = bid_order.id;
    trade.seller_id = ask_order.id;
    trade.price_ticks = ask_order.price_ticks;
    trade.quantity = qty;
    trade.timestamp = std::max(bid_order.timestamp, ask_order.timestamp);

    bid_order.quantity -= qty;
    ask_order.quantity -= qty;

    const bool bid_gone = (bid_order.quantity == 0);
    const bool ask_gone = (ask_order.quantity == 0);
    const uint64_t bid_id = bid_order.id;
    const uint64_t ask_id = ask_order.id;

    if (bid_gone) {
        bid_level.orders.pop_front();
        order_loc_.erase(bid_id);
        if (bid_level.orders.empty()) {
            bids_.erase(bids_.begin());
        }
    }

    if (asks_.empty()) {
        return true;
    }

    if (ask_gone) {
        // re-take asks_.front() after possible bid-side erase — avoids dangling reference to old ask container.
        auto& ask_level_front = asks_.front();
        ask_level_front.orders.pop_front();
        order_loc_.erase(ask_id);
        if (ask_level_front.orders.empty()) {
            asks_.erase(asks_.begin());
        }
    }

    return true;
}

void OrderBook::match_market(uint64_t market_order_id, Order::Side side, uint32_t& quantity_io,
                             uint64_t market_timestamp, std::vector<Trade>& trades_out) {
    if (side == Order::BID) {
        while (quantity_io > 0 && !asks_.empty()) {
            auto& ask_level = asks_.front();
            Order& ask = ask_level.orders.front();
            const uint32_t take = std::min(quantity_io, ask.quantity);
            Trade t{};
            t.buyer_id = market_order_id;
            t.seller_id = ask.id;
            t.price_ticks = ask.price_ticks;
            t.quantity = take;
            t.timestamp = std::max(market_timestamp, ask.timestamp);
            trades_out.push_back(t);
            quantity_io -= take;
            ask.quantity -= take;
            if (ask.quantity == 0) {
                const uint64_t rid = ask.id;
                ask_level.orders.pop_front();
                order_loc_.erase(rid);
                if (ask_level.orders.empty()) {
                    asks_.erase(asks_.begin());
                }
            }
        }
        return;
    }

    while (quantity_io > 0 && !bids_.empty()) {
        auto& bid_level = bids_.front();
        Order& bid = bid_level.orders.front();
        const uint32_t take = std::min(quantity_io, bid.quantity);
        Trade t{};
        t.buyer_id = bid.id;
        t.seller_id = market_order_id;
        t.price_ticks = bid.price_ticks;
        t.quantity = take;
        t.timestamp = std::max(market_timestamp, bid.timestamp);
        trades_out.push_back(t);
        quantity_io -= take;
        bid.quantity -= take;
        if (bid.quantity == 0) {
            const uint64_t rid = bid.id;
            bid_level.orders.pop_front();
            order_loc_.erase(rid);
            if (bid_level.orders.empty()) {
                bids_.erase(bids_.begin());
            }
        }
    }
}

bool OrderBook::reduce_level_after_lobster_execution(Order::Side passive_side, std::int64_t price,
                                                     uint32_t traded_size, bool allow_missing_level) {
    if (traded_size == 0) {
        return true;
    }

    auto erase_empty_level = [](std::vector<PriceLevel>& book, std::vector<PriceLevel>::iterator it) {
        if (it->orders.empty()) {
            book.erase(it);
        }
    };

    auto& book = (passive_side == Order::BID) ? bids_ : asks_;
    const bool bid_side = (passive_side == Order::BID);
    auto it = bid_side ? std::lower_bound(book.begin(), book.end(), price,
                                          [](const PriceLevel& pl, std::int64_t p) { return pl.price_ticks > p; })
                       : std::lower_bound(book.begin(), book.end(), price,
                                          [](const PriceLevel& pl, std::int64_t p) { return pl.price_ticks < p; });
    if (it == book.end() || it->price_ticks != price) {
        return allow_missing_level;
    }

    std::uint64_t total = 0;
    for (const auto& o : it->orders) {
        total += o.quantity;
    }
    if (total < traded_size) {
        return false;
    }

    std::uint32_t remaining = traded_size;
    while (remaining > 0) {
        if (it->orders.empty()) {
            return false;
        }
        Order& front = it->orders.front();
        const std::uint32_t take = std::min(remaining, front.quantity);
        front.quantity -= take;
        remaining -= take;
        if (front.quantity == 0) {
            const std::uint64_t rid = front.id;
            it->orders.pop_front();
            order_loc_.erase(rid);
        }
    }
    erase_empty_level(book, it);
    return true;
}

bool OrderBook::replace_remaining_quantity(uint64_t id, uint32_t new_remaining) {
    if (new_remaining == 0) {
        return cancelOrder(id);
    }

    const auto loc_it = order_loc_.find(id);
    if (loc_it == order_loc_.end()) {
        return false;
    }

    const Order::Side side = loc_it->second.first;
    const std::int64_t price = loc_it->second.second;

    auto touch = [&](std::vector<PriceLevel>& book, bool bid_side) -> bool {
        auto it = bid_side
                      ? std::lower_bound(
                            book.begin(), book.end(), price,
                            [](const PriceLevel& pl, std::int64_t p) { return pl.price_ticks > p; })
                      : std::lower_bound(
                            book.begin(), book.end(), price,
                            [](const PriceLevel& pl, std::int64_t p) { return pl.price_ticks < p; });
        if (it == book.end() || it->price_ticks != price) {
            return false;
        }
        for (auto& o : it->orders) {
            if (o.id == id) {
                o.quantity = new_remaining;
                return true;
            }
        }
        return false;
    };

    return (side == Order::BID) ? touch(bids_, true) : touch(asks_, false);
}

std::vector<std::pair<std::int64_t, std::uint64_t>> OrderBook::bid_levels_ticks(
    std::size_t max_levels) const {
    std::vector<std::pair<std::int64_t, std::uint64_t>> out;
    const std::size_t n = std::min(max_levels, bids_.size());
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& lvl = bids_[i];
        std::uint64_t sum = 0;
        for (const auto& o : lvl.orders) {
            sum += o.quantity;
        }
        out.emplace_back(lvl.price_ticks, sum);
    }
    return out;
}

std::vector<std::pair<std::int64_t, std::uint64_t>> OrderBook::ask_levels_ticks(
    std::size_t max_levels) const {
    std::vector<std::pair<std::int64_t, std::uint64_t>> out;
    const std::size_t n = std::min(max_levels, asks_.size());
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& lvl = asks_[i];
        std::uint64_t sum = 0;
        for (const auto& o : lvl.orders) {
            sum += o.quantity;
        }
        out.emplace_back(lvl.price_ticks, sum);
    }
    return out;
}

}  // namespace order_book
