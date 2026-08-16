// book-level behavior: resting, cancelling, level bookkeeping, cross detection.
// matching/engine behavior lives in test_matching.cpp.

#include <gtest/gtest.h>

#include "order.h"
#include "order_book/order_book.hpp"
#include "trade.h"

TEST(Order, make_auto_increments_id) {
    const auto a = Order::make(100000, 100, Order::BID, Order::LIMIT, 1);
    const auto b = Order::make(105000, 50, Order::ASK, Order::MARKET, 2);
    EXPECT_EQ(a.id + 1, b.id);
    EXPECT_EQ(a.price_ticks, 100000);
    EXPECT_EQ(a.quantity, 100u);
    EXPECT_EQ(a.side, Order::BID);
    EXPECT_EQ(a.type, Order::LIMIT);
    EXPECT_EQ(a.timestamp, 1u);
}

TEST(OrderBook, add_limit_resting_and_cancel) {
    order_book::OrderBook book;
    auto bid = Order::make(1000000, 10, Order::BID, Order::LIMIT, 1);
    const uint64_t bid_id = bid.id;
    book.addOrder(std::move(bid));
    EXPECT_TRUE(book.cancelOrder(bid_id));
    EXPECT_FALSE(book.cancelOrder(bid_id));
}

TEST(OrderBook, non_limit_types_do_not_rest) {
    order_book::OrderBook book;
    auto m = Order::make(500000, 1, Order::BID, Order::MARKET, 1);
    const uint64_t id = m.id;
    book.addOrder(std::move(m));
    EXPECT_FALSE(book.cancelOrder(id));
}

TEST(OrderBook, cancel_second_order_at_same_price_level) {
    order_book::OrderBook book;
    auto a = Order::make(2000000, 1, Order::ASK, Order::LIMIT, 1);
    auto b = Order::make(2000000, 2, Order::ASK, Order::LIMIT, 2);
    const uint64_t id_a = a.id;
    const uint64_t id_b = b.id;
    book.addOrder(std::move(a));
    book.addOrder(std::move(b));
    EXPECT_TRUE(book.cancelOrder(id_a));
    EXPECT_TRUE(book.cancelOrder(id_b));
}

TEST(OrderBook, execute_top_cross_not_crossed) {
    order_book::OrderBook book;
    book.addOrder(Order::make(1000000, 1, Order::ASK, Order::LIMIT, 1));
    book.addOrder(Order::make(990000, 1, Order::BID, Order::LIMIT, 2));
    EXPECT_FALSE(book.is_crossed());
    Trade t{};
    EXPECT_FALSE(book.execute_top_cross(t));
}

// --- cached level quantity invariant ---------------------------------------
//
// PriceLevel::total_qty is maintained incrementally so the level accessors do
// not walk every order on the matching thread's hot path. That is only safe if
// EVERY mutation site keeps it in step, so these assert the invariant after
// each kind of operation rather than trusting the six call sites.

TEST(OrderBook, level_totals_stay_consistent_through_rest_and_cancel) {
    order_book::OrderBook book;
    std::vector<uint64_t> ids;
    for (int i = 0; i < 20; ++i) {
        auto o = Order::make(1000000 + (i % 3) * 100, static_cast<uint32_t>(i + 1),
                             (i % 2) ? Order::BID : Order::ASK, Order::LIMIT,
                             static_cast<uint64_t>(i));
        ids.push_back(o.id);
        book.addOrder(std::move(o));
        ASSERT_TRUE(book.levels_consistent()) << "inconsistent after add " << i;
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        static_cast<void>(book.cancelOrder(ids[i]));
        ASSERT_TRUE(book.levels_consistent()) << "inconsistent after cancel " << i;
    }
}

TEST(OrderBook, level_totals_stay_consistent_through_crossing) {
    order_book::OrderBook book;
    book.addOrder(Order::make(1000000, 10, Order::ASK, Order::LIMIT, 1));
    book.addOrder(Order::make(1000000, 7, Order::ASK, Order::LIMIT, 2));
    book.addOrder(Order::make(1010000, 12, Order::BID, Order::LIMIT, 3));
    ASSERT_TRUE(book.levels_consistent());

    while (book.is_crossed()) {
        Trade t{};
        if (!book.execute_top_cross(t)) break;
        ASSERT_TRUE(book.levels_consistent()) << "inconsistent after a cross";
    }
}

TEST(OrderBook, level_totals_stay_consistent_through_market_sweep) {
    order_book::OrderBook book;
    for (int i = 0; i < 5; ++i) {
        book.addOrder(Order::make(1000000 + i * 100, 4, Order::ASK, Order::LIMIT,
                                  static_cast<uint64_t>(i)));
    }
    ASSERT_TRUE(book.levels_consistent());

    std::vector<Trade> trades;
    std::uint32_t qty = 13;  // partially consumes the third level
    book.match_market(999, Order::BID, qty, 100, trades);
    EXPECT_TRUE(book.levels_consistent()) << "inconsistent after a market sweep";
}

TEST(OrderBook, level_totals_stay_consistent_through_quantity_replace) {
    order_book::OrderBook book;
    auto a = Order::make(1000000, 10, Order::BID, Order::LIMIT, 1);
    const uint64_t id = a.id;
    book.addOrder(std::move(a));
    book.addOrder(Order::make(1000000, 5, Order::BID, Order::LIMIT, 2));

    EXPECT_TRUE(book.replace_remaining_quantity(id, 3));  // reduce
    EXPECT_TRUE(book.levels_consistent()) << "inconsistent after a reduction";
    EXPECT_EQ(book.bid_levels_ticks(1)[0].second, 8u);

    EXPECT_TRUE(book.replace_remaining_quantity(id, 20));  // increase
    EXPECT_TRUE(book.levels_consistent()) << "inconsistent after an increase";
    EXPECT_EQ(book.bid_levels_ticks(1)[0].second, 25u);

    EXPECT_TRUE(book.replace_remaining_quantity(id, 0));  // removes the order
    EXPECT_TRUE(book.levels_consistent()) << "inconsistent after removal";
    EXPECT_EQ(book.bid_levels_ticks(1)[0].second, 5u);
}

TEST(OrderBook, best_price_accessors_report_top_of_book_without_quantities) {
    order_book::OrderBook book;
    std::int64_t px = 0;
    EXPECT_FALSE(book.best_bid_ticks(px)) << "empty book reported a best bid";
    EXPECT_FALSE(book.best_ask_ticks(px));

    book.addOrder(Order::make(999900, 5, Order::BID, Order::LIMIT, 1));
    book.addOrder(Order::make(999800, 5, Order::BID, Order::LIMIT, 2));
    book.addOrder(Order::make(1000100, 5, Order::ASK, Order::LIMIT, 3));
    book.addOrder(Order::make(1000200, 5, Order::ASK, Order::LIMIT, 4));

    ASSERT_TRUE(book.best_bid_ticks(px));
    EXPECT_EQ(px, 999900) << "best bid is the HIGHEST bid";
    ASSERT_TRUE(book.best_ask_ticks(px));
    EXPECT_EQ(px, 1000100) << "best ask is the LOWEST ask";
}
