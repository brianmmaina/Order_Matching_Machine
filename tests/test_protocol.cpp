// Wire protocol v1. Spec: docs/PROTOCOL.md.
//
// The malformed-input cases matter as much as the round trips: this codec will
// be fed bytes from the network by session 1.2, and every way a peer can lie
// about a length or a type has to end in a clean rejection.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "ome/protocol.hpp"
#include "ome/protocol_json.hpp"
#include "ome/reject_reason.hpp"

using namespace ome::protocol;

namespace {

// encode a payload on its own (no header), as decode() expects.
template <typename T>
std::vector<std::uint8_t> payload_of(const T& m) {
    std::vector<std::uint8_t> out;
    encode(out, m);
    return out;
}

template <typename T>
T round_trip(const T& in) {
    const auto bytes = payload_of(in);
    const auto out = decode<T>(bytes.data(), bytes.size());
    EXPECT_TRUE(out.has_value());
    return out.value_or(T{});
}

}  // namespace

// --- round trips -----------------------------------------------------------

TEST(Protocol, new_order_round_trip) {
    NewOrder in{};
    in.client_order_id = 0xDEADBEEFCAFEULL;
    in.price_ticks = 2241700;
    in.quantity = 250;
    in.side = Side::Ask;
    in.order_type = OrderType::Limit;

    const NewOrder out = round_trip(in);
    EXPECT_EQ(out.client_order_id, in.client_order_id);
    EXPECT_EQ(out.price_ticks, in.price_ticks);
    EXPECT_EQ(out.quantity, in.quantity);
    EXPECT_EQ(out.side, Side::Ask);
    EXPECT_EQ(out.order_type, OrderType::Limit);
}

TEST(Protocol, negative_and_extreme_prices_round_trip) {
    // int64 is encoded as two's complement bits, so the sign must survive and
    // the range must reach the type's limits.
    for (const std::int64_t px : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1},
                                  std::int64_t{0}, std::int64_t{1},
                                  std::numeric_limits<std::int64_t>::max()}) {
        NewOrder in{};
        in.price_ticks = px;
        in.quantity = 1;
        EXPECT_EQ(round_trip(in).price_ticks, px);
    }
}

TEST(Protocol, cancel_round_trip) {
    Cancel in{};
    in.client_order_id = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(round_trip(in).client_order_id, in.client_order_id);
}

TEST(Protocol, modify_round_trip) {
    Modify in{};
    in.client_order_id = 77;
    in.new_price_ticks = -12345;
    in.new_quantity = 999;
    const Modify out = round_trip(in);
    EXPECT_EQ(out.client_order_id, 77u);
    EXPECT_EQ(out.new_price_ticks, -12345);
    EXPECT_EQ(out.new_quantity, 999u);
}

TEST(Protocol, subscribe_round_trip) {
    Subscribe in{};
    in.depth = 10;
    EXPECT_EQ(round_trip(in).depth, 10u);
}

TEST(Protocol, ack_round_trip) {
    Ack in{};
    in.client_order_id = 5;
    in.exchange_order_id = 900000;
    const Ack out = round_trip(in);
    EXPECT_EQ(out.client_order_id, 5u);
    EXPECT_EQ(out.exchange_order_id, 900000u);
}

TEST(Protocol, reject_round_trip_preserves_reason) {
    Reject in{};
    in.client_order_id = 42;
    in.reason = ome::RejectReason::RISK_PRICE_BAND;
    const Reject out = round_trip(in);
    EXPECT_EQ(out.client_order_id, 42u);
    EXPECT_EQ(out.reason, ome::RejectReason::RISK_PRICE_BAND);
}

TEST(Protocol, fill_round_trip) {
    Fill in{};
    in.exchange_order_id = 12345;
    in.price_ticks = 2243600;
    in.quantity = 40;
    in.remaining_quantity = 60;
    const Fill out = round_trip(in);
    EXPECT_EQ(out.exchange_order_id, 12345u);
    EXPECT_EQ(out.price_ticks, 2243600);
    EXPECT_EQ(out.quantity, 40u);
    EXPECT_EQ(out.remaining_quantity, 60u);
}

TEST(Protocol, heartbeat_round_trip) {
    Heartbeat in{};
    in.timestamp_ns = 1723575600000000000ULL;
    EXPECT_EQ(round_trip(in).timestamp_ns, in.timestamp_ns);
}

TEST(Protocol, book_update_round_trip_full_depth) {
    BookUpdate in{};
    in.seq = 4242;
    for (int i = 0; i < 10; ++i) {
        in.bids.push_back({2240000 - i * 100, static_cast<std::uint64_t>(100 + i)});
        in.asks.push_back({2241000 + i * 100, static_cast<std::uint64_t>(200 + i)});
    }
    const BookUpdate out = round_trip(in);
    ASSERT_EQ(out.bids.size(), 10u);
    ASSERT_EQ(out.asks.size(), 10u);
    EXPECT_EQ(out.seq, 4242u);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(out.bids[static_cast<std::size_t>(i)].price_ticks, in.bids[static_cast<std::size_t>(i)].price_ticks);
        EXPECT_EQ(out.asks[static_cast<std::size_t>(i)].quantity, in.asks[static_cast<std::size_t>(i)].quantity);
    }
}

TEST(Protocol, book_update_round_trip_empty_book) {
    BookUpdate in{};
    in.seq = 1;
    const BookUpdate out = round_trip(in);
    EXPECT_EQ(out.seq, 1u);
    EXPECT_TRUE(out.bids.empty());
    EXPECT_TRUE(out.asks.empty());
}

// --- truncation ------------------------------------------------------------

TEST(Protocol, every_truncation_of_every_message_is_rejected) {
    // Not just "one byte short": every prefix length from 0 to n-1. An
    // off-by-one in a single field's bounds check hides from a single-case test.
    const auto no = payload_of(NewOrder{1, 2, 3, Side::Bid, OrderType::Limit});
    for (std::size_t n = 0; n < no.size(); ++n) {
        EXPECT_FALSE(decode<NewOrder>(no.data(), n).has_value()) << "NewOrder accepted " << n;
    }

    const auto bu = payload_of([] {
        BookUpdate b{};
        b.seq = 9;
        b.bids.push_back({100, 1});
        b.asks.push_back({200, 2});
        return b;
    }());
    for (std::size_t n = 0; n < bu.size(); ++n) {
        EXPECT_FALSE(decode<BookUpdate>(bu.data(), n).has_value()) << "BookUpdate accepted " << n;
    }

    const auto fl = payload_of(Fill{1, 2, 3, 4});
    for (std::size_t n = 0; n < fl.size(); ++n) {
        EXPECT_FALSE(decode<Fill>(fl.data(), n).has_value()) << "Fill accepted " << n;
    }
}

TEST(Protocol, trailing_bytes_are_rejected) {
    // Within one version a message has exactly one valid length. Ignoring extra
    // bytes would hide a peer encoding something else entirely.
    auto bytes = payload_of(Cancel{7});
    bytes.push_back(0x00);
    EXPECT_FALSE(decode<Cancel>(bytes.data(), bytes.size()).has_value());
}

// --- header ----------------------------------------------------------------

TEST(Protocol, header_round_trip_and_frame_length_matches_payload) {
    NewOrder m{};
    m.client_order_id = 1;
    m.quantity = 5;
    const auto frame = encode_frame(MessageType::NewOrder, m);

    ASSERT_GE(frame.size(), kHeaderSize);
    const auto h = decode_header(frame.data(), frame.size());
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->type, static_cast<std::uint16_t>(MessageType::NewOrder));
    EXPECT_EQ(h->version, kVersion);
    // the length is the ENCODED payload size, not sizeof(NewOrder) — which is
    // the entire reason this protocol does not memcpy structs.
    EXPECT_EQ(h->length, frame.size() - kHeaderSize);
    EXPECT_EQ(h->length, 22u);  // 8+8+4+1+1, and NOT sizeof(NewOrder)

    const auto decoded = decode<NewOrder>(frame.data() + kHeaderSize, h->length);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->client_order_id, 1u);
}

TEST(Protocol, short_header_is_rejected) {
    const std::uint8_t buf[kHeaderSize] = {};
    for (std::size_t n = 0; n < kHeaderSize; ++n) {
        EXPECT_FALSE(decode_header(buf, n).has_value()) << "accepted header of " << n;
    }
}

TEST(Protocol, oversized_length_claim_is_rejected) {
    // A corrupt or hostile length must not become an allocation.
    std::vector<std::uint8_t> h;
    MessageHeader hdr{};
    hdr.length = kMaxPayloadSize + 1;
    hdr.type = static_cast<std::uint16_t>(MessageType::NewOrder);
    hdr.version = kVersion;
    encode_header(h, hdr);
    EXPECT_FALSE(decode_header(h.data(), h.size()).has_value());
}

TEST(Protocol, header_decode_does_not_validate_type_or_version) {
    // Both are the dispatcher's business: it needs the decoded header to pick
    // between Reject{UNKNOWN_MESSAGE_TYPE} and Reject{MALFORMED}.
    std::vector<std::uint8_t> h;
    MessageHeader hdr{};
    hdr.length = 0;
    hdr.type = 9999;
    hdr.version = 42;
    encode_header(h, hdr);

    const auto out = decode_header(h.data(), h.size());
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->type, 9999);
    EXPECT_EQ(out->version, 42);
}

// --- undefined enumerators on the wire -------------------------------------

TEST(Protocol, undefined_side_or_order_type_is_rejected) {
    auto bytes = payload_of(NewOrder{1, 100, 5, Side::Bid, OrderType::Limit});
    ASSERT_EQ(bytes.size(), 22u);

    auto bad_side = bytes;
    bad_side[20] = 7;  // side
    EXPECT_FALSE(decode<NewOrder>(bad_side.data(), bad_side.size()).has_value());

    auto bad_type = bytes;
    bad_type[21] = 7;  // order_type
    EXPECT_FALSE(decode<NewOrder>(bad_type.data(), bad_type.size()).has_value());
}

TEST(Protocol, unrecognized_reject_reason_still_decodes) {
    // Forward compatibility: a newer server may send a code this build predates.
    // The message must still be readable, or a client cannot even log it.
    std::vector<std::uint8_t> bytes;
    Reject r{};
    r.client_order_id = 3;
    encode(bytes, r);
    bytes[8] = 0xFF;
    bytes[9] = 0xFF;

    const auto out = decode<Reject>(bytes.data(), bytes.size());
    ASSERT_TRUE(out.has_value());
    EXPECT_STREQ(to_string(out->reason), "UNRECOGNIZED");
}

TEST(Protocol, book_update_depth_claim_beyond_max_is_rejected) {
    // The count field is attacker-controlled; it must be bounds-checked before
    // it is used to size anything.
    std::vector<std::uint8_t> bytes;
    BookUpdate b{};
    b.seq = 1;
    encode(bytes, b);
    bytes[8] = 200;  // bid_count
    EXPECT_FALSE(decode<BookUpdate>(bytes.data(), bytes.size()).has_value());
}

// --- endianness ------------------------------------------------------------

TEST(Protocol, encoding_is_little_endian_regardless_of_host) {
    // Pinned bytes, not a round trip: a round trip passes even if the codec is
    // consistently wrong. This is the check that two different machines agree.
    std::vector<std::uint8_t> bytes;
    Cancel c{};
    c.client_order_id = 0x0102030405060708ULL;
    encode(bytes, c);

    const std::vector<std::uint8_t> expected = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    EXPECT_EQ(bytes, expected);
}

TEST(Protocol, header_encoding_is_little_endian) {
    std::vector<std::uint8_t> bytes;
    MessageHeader h{};
    h.length = 0x11223344;
    h.type = 0x5566;
    h.version = 0x7788;
    encode_header(bytes, h);

    const std::vector<std::uint8_t> expected = {0x44, 0x33, 0x22, 0x11, 0x66, 0x55, 0x88, 0x77};
    EXPECT_EQ(bytes, expected);
}

// --- JSON debug mode -------------------------------------------------------

TEST(ProtocolJson, renders_expected_shape) {
    NewOrder m{};
    m.client_order_id = 1;
    m.price_ticks = 1000000;
    m.quantity = 10;
    m.side = Side::Bid;
    m.order_type = OrderType::Limit;

    EXPECT_EQ(json::to_json(m),
              R"({"type":"NewOrder","client_order_id":1,"price_ticks":1000000,"quantity":10,)"
              R"("side":"BID","order_type":"LIMIT"})");
}

TEST(ProtocolJson, reject_carries_both_name_and_code) {
    // the name is for a human reading a log; the numeric code is what actually
    // went over the wire and what a machine should match on.
    Reject r{};
    r.client_order_id = 8;
    r.reason = ome::RejectReason::RATE_LIMITED;
    EXPECT_EQ(json::to_json(r),
              R"({"type":"Reject","client_order_id":8,"reason":"RATE_LIMITED","reason_code":7})");
}

TEST(ProtocolJson, book_update_uses_the_same_level_shape_as_the_replay_format) {
    // [[price_ticks, qty], ...] — matches include/ome/book_jsonl.hpp so the
    // live feed in session 1.6 can bridge to tools/book_replay.html unchanged.
    BookUpdate b{};
    b.seq = 2;
    b.bids.push_back({100, 5});
    b.asks.push_back({200, 7});
    EXPECT_EQ(json::to_json(b),
              R"({"type":"BookUpdate","seq":2,"bids":[[100,5]],"asks":[[200,7]]})");
}
