// engine-level behavior: order routing, cross loops, market sweeps, trade log.
// plus the benchmarker smoke test, which drives the engine.

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "benchmarker.h"
#include "matching_engine/matching_engine.hpp"
#include "ome/reject_reason.hpp"
#include "order.h"
#include "trade.h"

TEST(MatchingEngine, limit_cross_single_fill) {
    MatchingEngine eng;
    auto ask = Order::make(1000000, 10, Order::ASK, Order::LIMIT, 10);
    const uint64_t ask_id = ask.id;
    auto bid = Order::make(1050000, 4, Order::BID, Order::LIMIT, 20);
    const uint64_t bid_id = bid.id;
    eng.processOrder(std::move(ask));
    eng.processOrder(std::move(bid));

    ASSERT_EQ(eng.trade_log().size(), 1u);
    const Trade& t = eng.trade_log()[0];
    EXPECT_EQ(t.buyer_id, bid_id);
    EXPECT_EQ(t.seller_id, ask_id);
    EXPECT_EQ(t.price_ticks, 1000000);
    EXPECT_EQ(t.quantity, 4u);
    EXPECT_EQ(t.timestamp, 20u);
}

TEST(MatchingEngine, limit_cross_multiple_trades_until_incoming_done) {
    MatchingEngine eng;
    eng.processOrder(Order::make(1000000, 3, Order::ASK, Order::LIMIT, 1));
    eng.processOrder(Order::make(1000000, 2, Order::ASK, Order::LIMIT, 2));
    auto bid = Order::make(1050000, 5, Order::BID, Order::LIMIT, 3);
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
    eng.processOrder(Order::make(1010000, 2, Order::ASK, Order::LIMIT, 1));
    eng.processOrder(Order::make(1000000, 2, Order::ASK, Order::LIMIT, 2));
    auto bid_rest = Order::make(900000, 10, Order::BID, Order::LIMIT, 3);
    const uint64_t bid_rest_id = bid_rest.id;
    eng.processOrder(std::move(bid_rest));

    // 1) Market buy: match immediately at best available ask(s), multiple price levels.
    auto mb = Order::make(0, 3, Order::BID, Order::MARKET, 10);
    const uint64_t mb_id = mb.id;
    eng.processOrder(std::move(mb));
    ASSERT_EQ(eng.trade_log().size(), 2u);
    EXPECT_EQ(eng.trade_log()[0].buyer_id, mb_id);
    EXPECT_EQ(eng.trade_log()[0].price_ticks, 1000000);
    EXPECT_EQ(eng.trade_log()[0].quantity, 2u);
    EXPECT_EQ(eng.trade_log()[1].price_ticks, 1010000);
    EXPECT_EQ(eng.trade_log()[1].quantity, 1u);

    // Add tight ask; remaining book had one unit at 101 — now best is 99.
    eng.processOrder(Order::make(990000, 1, Order::ASK, Order::LIMIT, 4));

    // 2) Market buy larger than total liquidity: fill 1 @99 and 1 @101, discard remainder.
    const std::size_t n_before = eng.trade_log().size();
    auto mb_big = Order::make(0, 50, Order::BID, Order::MARKET, 11);
    eng.processOrder(std::move(mb_big));
    ASSERT_EQ(eng.trade_log().size(), n_before + 2);
    EXPECT_EQ(eng.trade_log()[n_before].price_ticks, 990000);
    EXPECT_EQ(eng.trade_log()[n_before].quantity, 1u);
    EXPECT_EQ(eng.trade_log()[n_before + 1].price_ticks, 1010000);
    EXPECT_EQ(eng.trade_log()[n_before + 1].quantity, 1u);

    // 3) Cancel resting bid by id; empty price level is removed (verified via no trade on sell).
    Order cancel{};
    cancel.id = bid_rest_id;
    cancel.type = Order::CANCEL;
    const std::size_t n_after_cancel = eng.trade_log().size();
    eng.processOrder(cancel);
    EXPECT_EQ(eng.trade_log().size(), n_after_cancel);

    eng.processOrder(Order::make(0, 5, Order::ASK, Order::MARKET, 20));
    EXPECT_EQ(eng.trade_log().size(), n_after_cancel);
}

// --- ApplyResult: what the gateway needs back from an apply -----------------

TEST(ApplyResult, resting_limit_is_accepted_and_unfilled) {
    MatchingEngine eng;
    const ApplyResult r = eng.processOrder(Order::make(1000000, 10, Order::BID, Order::LIMIT, 1));
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.reason, ome::RejectReason::NONE);
    EXPECT_EQ(r.filled_qty, 0u);
    EXPECT_FALSE(r.fully_filled);
}

TEST(ApplyResult, marketable_limit_reports_partial_and_full_fills) {
    MatchingEngine eng;
    eng.processOrder(Order::make(1000000, 4, Order::ASK, Order::LIMIT, 1));

    // takes all 4 resting, 6 of its 10 rest: partial.
    const ApplyResult partial =
        eng.processOrder(Order::make(1010000, 10, Order::BID, Order::LIMIT, 2));
    EXPECT_TRUE(partial.accepted);
    EXPECT_EQ(partial.filled_qty, 4u);
    EXPECT_FALSE(partial.fully_filled);

    // hits the 6 now resting on the bid: full.
    const ApplyResult full = eng.processOrder(Order::make(1000000, 6, Order::ASK, Order::LIMIT, 3));
    EXPECT_TRUE(full.accepted);
    EXPECT_EQ(full.filled_qty, 6u);
    EXPECT_TRUE(full.fully_filled);
}

TEST(ApplyResult, cancel_of_unknown_id_is_rejected_with_reason) {
    MatchingEngine eng;
    Order cancel{};
    cancel.id = 987654;
    cancel.type = Order::CANCEL;

    const ApplyResult r = eng.processOrder(cancel);
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.reason, ome::RejectReason::UNKNOWN_ORDER);
}

TEST(ApplyResult, cancel_of_resting_order_is_accepted) {
    MatchingEngine eng;
    auto bid = Order::make(1000000, 10, Order::BID, Order::LIMIT, 1);
    const uint64_t id = bid.id;
    eng.processOrder(std::move(bid));

    Order cancel{};
    cancel.id = id;
    cancel.type = Order::CANCEL;

    const ApplyResult r = eng.processOrder(cancel);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.reason, ome::RejectReason::NONE);
}

TEST(ApplyResult, market_order_reports_what_it_actually_took) {
    MatchingEngine eng;
    eng.processOrder(Order::make(1000000, 3, Order::ASK, Order::LIMIT, 1));

    // asks for 10 against 3 of liquidity; the remainder is dropped, not rested,
    // so the order is done regardless.
    const ApplyResult r = eng.processOrder(Order::make(0, 10, Order::BID, Order::MARKET, 2));
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.filled_qty, 3u);
    EXPECT_TRUE(r.fully_filled);
}

// --- trade drain: reading the trades one command produced -------------------

TEST(MatchingEngine, trade_log_slice_isolates_one_command) {
    MatchingEngine eng;
    eng.processOrder(Order::make(1000000, 5, Order::ASK, Order::LIMIT, 1));
    eng.processOrder(Order::make(1010000, 5, Order::ASK, Order::LIMIT, 2));

    // sweeps both levels: this one command should yield exactly two trades.
    const std::size_t before = eng.trade_log_size();
    eng.processOrder(Order::make(0, 10, Order::BID, Order::MARKET, 3));
    const std::size_t after = eng.trade_log_size();

    ASSERT_EQ(after - before, 2u);
    EXPECT_EQ(eng.trade_log()[before].price_ticks, 1000000);
    EXPECT_EQ(eng.trade_log()[before + 1].price_ticks, 1010000);
}

TEST(MatchingEngine, clear_trade_log_bounds_growth_without_touching_the_book) {
    MatchingEngine eng;
    eng.processOrder(Order::make(1000000, 5, Order::ASK, Order::LIMIT, 1));
    eng.processOrder(Order::make(1010000, 5, Order::BID, Order::LIMIT, 2));
    ASSERT_GT(eng.trade_log_size(), 0u);

    eng.clear_trade_log();
    EXPECT_EQ(eng.trade_log_size(), 0u);

    // clearing the log must not disturb resting liquidity — the 5 that crossed
    // are gone, but the book still functions and keeps producing trades.
    eng.processOrder(Order::make(1010000, 3, Order::ASK, Order::LIMIT, 3));
    eng.processOrder(Order::make(1010000, 3, Order::BID, Order::LIMIT, 4));
    EXPECT_GT(eng.trade_log_size(), 0u);
}

TEST(Benchmarker, latency_and_throughput_smoke) {
    MatchingEngine eng;
    Benchmarker bench(eng);
    std::vector<Order> orders;
    for (int i = 0; i < 8; ++i) {
        // one currency unit apart: 100.0000, 100.0001, ... in tick terms.
        orders.push_back(Order::make(1000000 + static_cast<std::int64_t>(i), 1, Order::BID,
                                     Order::LIMIT, static_cast<std::uint64_t>(i)));
    }
    bench.run_latency(orders);
    EXPECT_EQ(bench.last_latency_ns().size(), orders.size());
    bench.run_throughput(orders);
}
