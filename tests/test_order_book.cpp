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
