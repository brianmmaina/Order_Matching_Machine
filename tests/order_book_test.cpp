// google test: macro-based test suite (TEST) and assertions (EXPECT_* / ASSERT_*).

#include <gtest/gtest.h>

#include <sstream>
#include <vector>
// std::istringstream: in-memory std::istream for lobster csv strings without temp files.

#include "order.h"
#include "lobster/lobster_orderbook_converter.hpp"
#include "lobster/lobster_parser.hpp"
#include "lobster/lobster_validator.hpp"
#include "trade.h"
#include "matching_engine/matching_engine.hpp"
#include "benchmarker.h"
#include "order_book/order_book.hpp"
#include "spsc_queue.h"

TEST(Order, make_auto_increments_id) {
    const auto a = Order::make(10.0, 100, Order::BID, Order::LIMIT, 1);
    const auto b = Order::make(10.5, 50, Order::ASK, Order::MARKET, 2);
    EXPECT_EQ(a.id + 1, b.id);
    EXPECT_DOUBLE_EQ(a.price, 10.0);
    EXPECT_EQ(a.quantity, 100u);
    EXPECT_EQ(a.side, Order::BID);
    EXPECT_EQ(a.type, Order::LIMIT);
    EXPECT_EQ(a.timestamp, 1u);
}

TEST(OrderBook, add_limit_resting_and_cancel) {
    order_book::OrderBook book;
    auto bid = Order::make(100.0, 10, Order::BID, Order::LIMIT, 1);
    const uint64_t bid_id = bid.id;
    book.addOrder(std::move(bid));
    EXPECT_TRUE(book.cancelOrder(bid_id));
    EXPECT_FALSE(book.cancelOrder(bid_id));
}

TEST(OrderBook, non_limit_types_do_not_rest) {
    order_book::OrderBook book;
    auto m = Order::make(50.0, 1, Order::BID, Order::MARKET, 1);
    const uint64_t id = m.id;
    book.addOrder(std::move(m));
    EXPECT_FALSE(book.cancelOrder(id));
}

TEST(OrderBook, cancel_second_order_at_same_price_level) {
    order_book::OrderBook book;
    auto a = Order::make(200.0, 1, Order::ASK, Order::LIMIT, 1);
    auto b = Order::make(200.0, 2, Order::ASK, Order::LIMIT, 2);
    const uint64_t id_a = a.id;
    const uint64_t id_b = b.id;
    book.addOrder(std::move(a));
    book.addOrder(std::move(b));
    EXPECT_TRUE(book.cancelOrder(id_a));
    EXPECT_TRUE(book.cancelOrder(id_b));
}

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

TEST(OrderBook, execute_top_cross_not_crossed) {
    order_book::OrderBook book;
    book.addOrder(Order::make(100.0, 1, Order::ASK, Order::LIMIT, 1));
    book.addOrder(Order::make(99.0, 1, Order::BID, Order::LIMIT, 2));
    EXPECT_FALSE(book.is_crossed());
    Trade t{};
    EXPECT_FALSE(book.execute_top_cross(t));
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

TEST(LobsterOrderbookConverter, lobster_row_to_validator_snapshot) {
    // minimal 1-level lobster row: ask 10100 x 3, bid 10000 x 7
    const std::string row = "10100,3,10000,7";
    std::ostringstream o;
    ASSERT_TRUE(write_validator_snapshot_from_lobster_orderbook_row(row, o));
    std::istringstream snap_in(o.str());
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(snap_in, line)));
    EXPECT_EQ(line, "10000,7");
    for (int i = 0; i < 9; ++i) {
        ASSERT_TRUE(static_cast<bool>(std::getline(snap_in, line)));
        EXPECT_EQ(line, "0,0");
    }
    ASSERT_TRUE(static_cast<bool>(std::getline(snap_in, line)));
    EXPECT_EQ(line, "10100,3");
}

TEST(LobsterValidator, matches_snapshot_after_two_limits) {
    std::istringstream msg(
        "1.0,1,10,100,100000,-1\n"
        "1.0,1,11,50,99000,1\n");
    std::ostringstream snap;
    snap << "99000,50\n";
    for (int i = 0; i < 9; ++i) {
        snap << "0,0\n";
    }
    snap << "100000,100\n";
    for (int i = 0; i < 9; ++i) {
        snap << "0,0\n";
    }
    std::istringstream snap_in(snap.str());
    const auto r = LobsterValidator::validate(msg, snap_in, 2);
    EXPECT_DOUBLE_EQ(r.accuracy_percent, 100.0);
    EXPECT_FALSE(r.first_mismatch_level_index.has_value());
}

TEST(LobsterValidator, lobster_execution_reduces_passive_side_not_market_sweep) {
    // LOBSTER direction on execution = side of resting limit (-1 => ask). Must not walk the bid book.
    std::istringstream msg(
        "1.0,1,10,100,100000,-1\n"
        "1.0,4,10,30,100000,-1\n");
    std::ostringstream snap;
    for (int i = 0; i < 10; ++i) {
        snap << "0,0\n";
    }
    snap << "100000,70\n";
    for (int i = 0; i < 9; ++i) {
        snap << "0,0\n";
    }
    std::istringstream snap_in(snap.str());
    const auto r = LobsterValidator::validate(msg, snap_in, 2);
    EXPECT_DOUBLE_EQ(r.accuracy_percent, 100.0);
    EXPECT_FALSE(r.first_mismatch_level_index.has_value());
}

TEST(LobsterValidator, detects_level_mismatch) {
    std::istringstream msg(
        "1.0,1,10,100,100000,-1\n"
        "1.0,1,11,50,99000,1\n");
    std::ostringstream snap;
    snap << "99000,49\n";  // wrong aggregate size
    for (int i = 0; i < 9; ++i) {
        snap << "0,0\n";
    }
    snap << "100000,100\n";
    for (int i = 0; i < 9; ++i) {
        snap << "0,0\n";
    }
    std::istringstream snap_in(snap.str());
    const auto r = LobsterValidator::validate(msg, snap_in, 2);
    EXPECT_LT(r.accuracy_percent, 100.0);
    ASSERT_TRUE(r.first_mismatch_level_index.has_value());
    EXPECT_EQ(*r.first_mismatch_level_index, 0u);
}

TEST(LobsterParser, maps_columns_and_lobster_types) {
    std::istringstream in(
        "34200.123456,1,1001,250,100500,1\n"
        "34201.0,2,1002,50,99999,-1\n"
        "34202.5,3,1003,10,100000,1\n"
        "34203.0,4,1004,5,100000,-1\n"
        "34204.0,5,1005,2,100000,1\n");
    const auto v = LobsterParser::parse(in);
    ASSERT_EQ(v.size(), 5u);

    EXPECT_EQ(v[0].id, 1001u);
    EXPECT_EQ(v[0].type, Order::LIMIT);
    EXPECT_EQ(v[0].side, Order::BID);
    EXPECT_DOUBLE_EQ(v[0].price, 10.05);
    EXPECT_EQ(v[0].quantity, 250u);
    EXPECT_EQ(v[0].timestamp, 34200123456u);

    EXPECT_EQ(v[1].type, Order::CANCEL);
    EXPECT_EQ(v[1].id, 1002u);
    EXPECT_EQ(v[1].side, Order::ASK);
    EXPECT_DOUBLE_EQ(v[1].price, 9.9999);

    EXPECT_EQ(v[2].type, Order::CANCEL);
    EXPECT_EQ(v[2].id, 1003u);

    EXPECT_EQ(v[3].type, Order::MARKET);
    EXPECT_EQ(v[3].id, 1004u);
    EXPECT_EQ(v[3].side, Order::ASK);

    EXPECT_EQ(v[4].type, Order::MARKET);
    EXPECT_EQ(v[4].id, 1005u);
}

TEST(Benchmarker, latency_and_throughput_smoke) {
    MatchingEngine eng;
    Benchmarker bench(eng);
    std::vector<Order> orders;
    for (int i = 0; i < 8; ++i) {
        orders.push_back(Order::make(100.0 + i, 1, Order::BID, Order::LIMIT, static_cast<std::uint64_t>(i)));
    }
    bench.run_latency(orders);
    EXPECT_EQ(bench.last_latency_ns().size(), orders.size());
    bench.run_throughput(orders);
}

TEST(SpscQueue, push_pop_roundtrip_single_thread) {
    // one thread acting as both roles is allowed for testing; real spsc uses one thread per role.
    SpscQueue<4> q;
    Order o = Order::make(1.0, 2, Order::BID, Order::LIMIT, 9);
    const uint64_t id = o.id;
    EXPECT_TRUE(q.push(std::move(o)));
    auto out = q.pop();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->id, id);
    EXPECT_FALSE(q.pop().has_value());
}

TEST(SpscQueue, full_rejects_push) {
    SpscQueue<2> q;
    EXPECT_TRUE(q.push(Order::make(1.0, 1, Order::BID, Order::LIMIT, 1)));
    EXPECT_TRUE(q.push(Order::make(2.0, 1, Order::BID, Order::LIMIT, 2)));
    EXPECT_FALSE(q.push(Order::make(3.0, 1, Order::BID, Order::LIMIT, 3)));
}

TEST(LobsterParser, skips_malformed_rows) {
    std::istringstream in(
        "\n"
        "not_a_time,1,1,1,10000,1\n"
        "1.0,7,1,1,10000,1\n"
        "1.0,1,0,1,10000,1\n"
        "1.0,1,1,99999999999999999999,10000,1\n"
        "1.0,1,1,1,-1,1\n"
        "1.0,1,1,1,10000,2\n"
        "1.0,1,1,1,10000\n"
        "2.0,1,42,7,20000,-1\n");
    const auto v = LobsterParser::parse(in);
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].id, 42u);
    EXPECT_EQ(v[0].quantity, 7u);
    EXPECT_DOUBLE_EQ(v[0].price, 2.0);
    EXPECT_EQ(v[0].side, Order::ASK);
}
