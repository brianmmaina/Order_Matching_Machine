// engine-level behavior: order routing, cross loops, market sweeps, trade log.
// plus the benchmarker smoke test, which drives the engine.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "benchmarker.h"
#include "matching_engine/matching_engine.hpp"
#include "order.h"
#include "trade.h"

TEST(MatchingEngine, limit_cross_single_fill) {
    MatchingEngine eng;
    auto ask = Order::make(100.0, 10, Order::ASK, Order::LIMIT, 10);
    const uint64_t ask_id = ask.id;
    auto bid = Order::make(105.0, 4, Order::BID, Order::LIMIT, 20);
    const uint64_t bid_id = bid.id;
    eng.processOrder(std::move(ask));
    eng.processOrder(std::move(bid));

    ASSERT_EQ(eng.trade_log().size(), 1u);
    const Trade& t = eng.trade_log()[0];
    EXPECT_EQ(t.buyer_id, bid_id);
    EXPECT_EQ(t.seller_id, ask_id);
    EXPECT_DOUBLE_EQ(t.price, 100.0);
    EXPECT_EQ(t.quantity, 4u);
    EXPECT_EQ(t.timestamp, 20u);
}

TEST(MatchingEngine, limit_cross_multiple_trades_until_incoming_done) {
    MatchingEngine eng;
    eng.processOrder(Order::make(100.0, 3, Order::ASK, Order::LIMIT, 1));
    eng.processOrder(Order::make(100.0, 2, Order::ASK, Order::LIMIT, 2));
    auto bid = Order::make(105.0, 5, Order::BID, Order::LIMIT, 3);
    const uint64_t bid_id = bid.id;
    eng.processOrder(std::move(bid));

    ASSERT_EQ(eng.trade_log().size(), 2u);
    EXPECT_EQ(eng.trade_log()[0].quantity, 3u);
    EXPECT_EQ(eng.trade_log()[1].quantity, 2u);
    EXPECT_EQ(eng.trade_log()[0].buyer_id, bid_id);
    EXPECT_EQ(eng.trade_log()[1].buyer_id, bid_id);
}

TEST(MatchingEngine, market_walks_prices_partial_liquidity_and_cancel) {
    MatchingEngine eng;
    // Resting asks at 101 (worse) and 100 (better) — book keeps asks ascending by price.
    eng.processOrder(Order::make(101.0, 2, Order::ASK, Order::LIMIT, 1));
    eng.processOrder(Order::make(100.0, 2, Order::ASK, Order::LIMIT, 2));
    auto bid_rest = Order::make(90.0, 10, Order::BID, Order::LIMIT, 3);
    const uint64_t bid_rest_id = bid_rest.id;
    eng.processOrder(std::move(bid_rest));

    // 1) Market buy: match immediately at best available ask(s), multiple price levels.
    auto mb = Order::make(0.0, 3, Order::BID, Order::MARKET, 10);
    const uint64_t mb_id = mb.id;
    eng.processOrder(std::move(mb));
    ASSERT_EQ(eng.trade_log().size(), 2u);
    EXPECT_EQ(eng.trade_log()[0].buyer_id, mb_id);
    EXPECT_DOUBLE_EQ(eng.trade_log()[0].price, 100.0);
    EXPECT_EQ(eng.trade_log()[0].quantity, 2u);
    EXPECT_DOUBLE_EQ(eng.trade_log()[1].price, 101.0);
    EXPECT_EQ(eng.trade_log()[1].quantity, 1u);

    // Add tight ask; remaining book had one unit at 101 — now best is 99.
    eng.processOrder(Order::make(99.0, 1, Order::ASK, Order::LIMIT, 4));

    // 2) Market buy larger than total liquidity: fill 1 @99 and 1 @101, discard remainder.
    const std::size_t n_before = eng.trade_log().size();
    auto mb_big = Order::make(0.0, 50, Order::BID, Order::MARKET, 11);
    eng.processOrder(std::move(mb_big));
    ASSERT_EQ(eng.trade_log().size(), n_before + 2);
    EXPECT_DOUBLE_EQ(eng.trade_log()[n_before].price, 99.0);
    EXPECT_EQ(eng.trade_log()[n_before].quantity, 1u);
    EXPECT_DOUBLE_EQ(eng.trade_log()[n_before + 1].price, 101.0);
    EXPECT_EQ(eng.trade_log()[n_before + 1].quantity, 1u);

    // 3) Cancel resting bid by id; empty price level is removed (verified via no trade on sell).
    Order cancel{};
    cancel.id = bid_rest_id;
    cancel.type = Order::CANCEL;
    const std::size_t n_after_cancel = eng.trade_log().size();
    eng.processOrder(cancel);
    EXPECT_EQ(eng.trade_log().size(), n_after_cancel);

    eng.processOrder(Order::make(0.0, 5, Order::ASK, Order::MARKET, 20));
    EXPECT_EQ(eng.trade_log().size(), n_after_cancel);
}

TEST(Benchmarker, latency_and_throughput_smoke) {
    MatchingEngine eng;
    Benchmarker bench(eng);
    std::vector<Order> orders;
    for (int i = 0; i < 8; ++i) {
        orders.push_back(
            Order::make(100.0 + i, 1, Order::BID, Order::LIMIT, static_cast<std::uint64_t>(i)));
    }
    bench.run_latency(orders);
    EXPECT_EQ(bench.last_latency_ns().size(), orders.size());
    bench.run_throughput(orders);
}
