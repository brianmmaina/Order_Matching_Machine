// Reject codes are a wire format, not an implementation detail.
//
// These tests exist to fail loudly if someone renumbers or reorders the enum.
// A client compiled against an older server must keep decoding correctly, and
// the only thing guaranteeing that is that these numbers never move.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "ome/reject_reason.hpp"

using ome::RejectReason;

TEST(RejectReason, wire_values_are_pinned) {
    // If a change to reject_reason.hpp breaks this test, the fix is almost
    // never to update these numbers — it is to give the new code the next
    // free value and leave the existing ones alone.
    EXPECT_EQ(static_cast<std::uint16_t>(RejectReason::NONE), 0u);
    EXPECT_EQ(static_cast<std::uint16_t>(RejectReason::UNKNOWN_ORDER), 1u);
    EXPECT_EQ(static_cast<std::uint16_t>(RejectReason::INVALID_PRICE), 2u);
    EXPECT_EQ(static_cast<std::uint16_t>(RejectReason::INVALID_QTY), 3u);
    EXPECT_EQ(static_cast<std::uint16_t>(RejectReason::RISK_MAX_ORDER_SIZE), 4u);
    EXPECT_EQ(static_cast<std::uint16_t>(RejectReason::RISK_PRICE_BAND), 5u);
    EXPECT_EQ(static_cast<std::uint16_t>(RejectReason::MALFORMED), 6u);
    EXPECT_EQ(static_cast<std::uint16_t>(RejectReason::RATE_LIMITED), 7u);
    EXPECT_EQ(static_cast<std::uint16_t>(RejectReason::NOT_SUBSCRIBED), 8u);
    EXPECT_EQ(static_cast<std::uint16_t>(RejectReason::DUPLICATE_ORDER_ID), 9u);
    EXPECT_EQ(static_cast<std::uint16_t>(RejectReason::UNKNOWN_MESSAGE_TYPE), 10u);
}

TEST(RejectReason, to_string_covers_every_code) {
    // metrics label values and log fields are built from these; an unmapped
    // code would silently become "UNRECOGNIZED" in a dashboard.
    const RejectReason all[] = {
        RejectReason::NONE,          RejectReason::UNKNOWN_ORDER,
        RejectReason::INVALID_PRICE, RejectReason::INVALID_QTY,
        RejectReason::RISK_MAX_ORDER_SIZE, RejectReason::RISK_PRICE_BAND,
        RejectReason::MALFORMED,     RejectReason::RATE_LIMITED,
        RejectReason::NOT_SUBSCRIBED, RejectReason::DUPLICATE_ORDER_ID,
        RejectReason::UNKNOWN_MESSAGE_TYPE};
    for (const RejectReason r : all) {
        EXPECT_STRNE(to_string(r), "UNRECOGNIZED") << "unmapped code " << static_cast<int>(r);
    }
    EXPECT_STREQ(to_string(RejectReason::RATE_LIMITED), "RATE_LIMITED");
}

TEST(RejectReason, unrecognized_wire_value_is_handled) {
    // a v2 server can send a code this build has never heard of. decoding must
    // not be undefined behavior — it must produce something a log can print.
    const auto from_wire = static_cast<RejectReason>(9999);
    EXPECT_STREQ(to_string(from_wire), "UNRECOGNIZED");
}
