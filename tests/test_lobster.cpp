// LOBSTER ingestion: CSV parsing, orderbook-row conversion, and snapshot validation.

#include <gtest/gtest.h>

#include <sstream>
#include <string>
// std::istringstream: in-memory std::istream for lobster csv strings without temp files.

#include "lobster/lobster_orderbook_converter.hpp"
#include "lobster/lobster_parser.hpp"
#include "lobster/lobster_validator.hpp"
#include "order.h"

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
    EXPECT_EQ(v[0].price_ticks, 100500);
    EXPECT_EQ(v[0].quantity, 250u);
    EXPECT_EQ(v[0].timestamp, 34200123456u);

    EXPECT_EQ(v[1].type, Order::CANCEL);
    EXPECT_EQ(v[1].id, 1002u);
    EXPECT_EQ(v[1].side, Order::ASK);
    EXPECT_EQ(v[1].price_ticks, 99999);

    EXPECT_EQ(v[2].type, Order::CANCEL);
    EXPECT_EQ(v[2].id, 1003u);

    EXPECT_EQ(v[3].type, Order::MARKET);
    EXPECT_EQ(v[3].id, 1004u);
    EXPECT_EQ(v[3].side, Order::ASK);

    EXPECT_EQ(v[4].type, Order::MARKET);
    EXPECT_EQ(v[4].id, 1005u);
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
    EXPECT_EQ(v[0].price_ticks, 20000);
    EXPECT_EQ(v[0].side, Order::ASK);
}
