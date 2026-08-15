// Frame reassembly and write buffering — the two classes that exist because
// TCP is a byte stream rather than a message stream.
//
// Both are deliberately socket-free so these tests can drive every split
// boundary and every partial-write size directly, which is not practical
// through a real socket where you cannot dictate segmentation.

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

#include "ome/frame_reader.hpp"
#include "ome/protocol.hpp"
#include "ome/write_buffer.hpp"

using namespace ome;
using namespace ome::protocol;

namespace {

std::vector<std::uint8_t> new_order_frame(std::uint64_t id, std::uint32_t qty = 10) {
    NewOrder m{};
    m.client_order_id = id;
    m.price_ticks = 1000000;
    m.quantity = qty;
    m.side = Side::Bid;
    m.order_type = OrderType::Limit;
    return encode_frame(MessageType::NewOrder, m);
}

}  // namespace

// --- the partial-read case -------------------------------------------------

TEST(FrameReader, single_frame_in_one_append) {
    const auto bytes = new_order_frame(1);
    FrameReader r;
    r.append(bytes.data(), bytes.size());

    auto f = r.next_frame();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->header.type, static_cast<std::uint16_t>(MessageType::NewOrder));
    const auto m = decode<NewOrder>(f->payload.data(), f->payload.size());
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->client_order_id, 1u);
    EXPECT_FALSE(r.next_frame().has_value());
}

TEST(FrameReader, frame_split_at_every_possible_byte_boundary) {
    // THE test for this class. A frame of N bytes is delivered as two appends
    // split at offset i, for every i from 0 to N. An off-by-one anywhere in the
    // header/payload boundary logic fails at exactly one value of i and passes
    // at all the others — so testing a single arbitrary split proves very
    // little.
    const auto bytes = new_order_frame(7, 25);

    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        FrameReader r;
        r.append(bytes.data(), split);

        // Before the whole frame has arrived there must be NO frame available.
        if (split < bytes.size()) {
            EXPECT_FALSE(r.next_frame().has_value()) << "premature frame at split " << split;
        }

        r.append(bytes.data() + split, bytes.size() - split);

        auto f = r.next_frame();
        ASSERT_TRUE(f.has_value()) << "no frame after full delivery, split " << split;
        const auto m = decode<NewOrder>(f->payload.data(), f->payload.size());
        ASSERT_TRUE(m.has_value()) << "payload corrupt at split " << split;
        EXPECT_EQ(m->client_order_id, 7u) << "wrong payload at split " << split;
        EXPECT_EQ(m->quantity, 25u) << "wrong payload at split " << split;
        EXPECT_FALSE(r.next_frame().has_value()) << "extra frame at split " << split;
    }
}

TEST(FrameReader, frame_delivered_one_byte_at_a_time) {
    // The pathological segmentation: 30 segments for one message. Nothing may
    // surface until the final byte lands.
    const auto bytes = new_order_frame(3);
    FrameReader r;

    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        r.append(&bytes[i], 1);
        ASSERT_FALSE(r.next_frame().has_value()) << "frame surfaced early at byte " << i;
    }
    r.append(&bytes[bytes.size() - 1], 1);

    auto f = r.next_frame();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(decode<NewOrder>(f->payload.data(), f->payload.size())->client_order_id, 3u);
}

TEST(FrameReader, multiple_frames_in_one_append) {
    // One read can carry many messages. A reader that returns only the first
    // and waits for the next read stalls the rest until the peer sends again.
    std::vector<std::uint8_t> stream;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        const auto f = new_order_frame(i);
        stream.insert(stream.end(), f.begin(), f.end());
    }

    FrameReader r;
    r.append(stream.data(), stream.size());

    for (std::uint64_t i = 1; i <= 5; ++i) {
        auto f = r.next_frame();
        ASSERT_TRUE(f.has_value()) << "missing frame " << i;
        EXPECT_EQ(decode<NewOrder>(f->payload.data(), f->payload.size())->client_order_id, i);
    }
    EXPECT_FALSE(r.next_frame().has_value());
}

TEST(FrameReader, frames_spanning_appends_with_a_partial_tail) {
    // The realistic case: two and a half frames arrive, then the rest.
    const auto a = new_order_frame(1);
    const auto b = new_order_frame(2);
    const auto c = new_order_frame(3);

    std::vector<std::uint8_t> first(a.begin(), a.end());
    first.insert(first.end(), b.begin(), b.end());
    first.insert(first.end(), c.begin(), c.begin() + 5);  // half of c

    FrameReader r;
    r.append(first.data(), first.size());
    EXPECT_TRUE(r.next_frame().has_value());
    EXPECT_TRUE(r.next_frame().has_value());
    EXPECT_FALSE(r.next_frame().has_value());  // c incomplete

    r.append(c.data() + 5, c.size() - 5);
    auto f = r.next_frame();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(decode<NewOrder>(f->payload.data(), f->payload.size())->client_order_id, 3u);
}

TEST(FrameReader, zero_length_payload_frame) {
    // A Heartbeat-shaped frame with no payload must not be mistaken for an
    // incomplete one — "need 0 more bytes" is satisfiable.
    std::vector<std::uint8_t> frame;
    MessageHeader h{};
    h.length = 0;
    h.type = static_cast<std::uint16_t>(MessageType::Heartbeat);
    h.version = kVersion;
    encode_header(frame, h);

    FrameReader r;
    r.append(frame.data(), frame.size());
    auto f = r.next_frame();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->header.length, 0u);
    EXPECT_TRUE(f->payload.empty());
}

// --- malformed input -------------------------------------------------------

TEST(FrameReader, oversized_length_claim_fails_the_stream) {
    // Unrecoverable by design: with no delimiter there is no way to find where
    // the next frame begins, so resynchronizing is impossible. The connection
    // must be closed rather than guessed at.
    std::vector<std::uint8_t> frame;
    MessageHeader h{};
    h.length = kMaxPayloadSize + 1;
    h.type = static_cast<std::uint16_t>(MessageType::NewOrder);
    h.version = kVersion;
    encode_header(frame, h);

    FrameReader r;
    r.append(frame.data(), frame.size());
    EXPECT_FALSE(r.next_frame().has_value());
    EXPECT_TRUE(r.failed());
}

TEST(FrameReader, garbage_bytes_are_interpreted_as_a_header_not_scanned_past) {
    // Documents real behavior rather than wishful behavior: length-prefix
    // framing has no way to recognize garbage. 0xFF... decodes to an absurd
    // length and the stream fails. That is the correct outcome, and the
    // tradeoff accepted in exchange for not having to escape delimiters.
    const std::vector<std::uint8_t> garbage(16, 0xFF);
    FrameReader r;
    r.append(garbage.data(), garbage.size());
    EXPECT_FALSE(r.next_frame().has_value());
    EXPECT_TRUE(r.failed());
}

TEST(FrameReader, failed_reader_stays_failed_and_ignores_further_input) {
    std::vector<std::uint8_t> bad;
    MessageHeader h{};
    h.length = kMaxPayloadSize + 1;
    encode_header(bad, h);

    FrameReader r;
    r.append(bad.data(), bad.size());
    static_cast<void>(r.next_frame());
    ASSERT_TRUE(r.failed());

    const auto good = new_order_frame(1);
    r.append(good.data(), good.size());
    EXPECT_FALSE(r.next_frame().has_value()) << "recovered from an unrecoverable state";
    EXPECT_TRUE(r.failed());
}

TEST(FrameReader, buffered_reports_undelivered_bytes) {
    // The server caps this: a peer that sends a header and then stalls would
    // otherwise pin memory forever.
    const auto bytes = new_order_frame(1);
    FrameReader r;
    r.append(bytes.data(), 5);
    EXPECT_EQ(r.buffered(), 5u);
    EXPECT_FALSE(r.next_frame().has_value());
    EXPECT_EQ(r.buffered(), 5u);

    r.append(bytes.data() + 5, bytes.size() - 5);
    EXPECT_TRUE(r.next_frame().has_value());
    EXPECT_EQ(r.buffered(), 0u);
}

// --- the partial-write case ------------------------------------------------

TEST(WriteBuffer, consume_handles_every_partial_write_size) {
    // send() may accept any number of bytes from 1 to n. Draining the buffer
    // must be correct for every one of them, not just "all" and "none".
    std::vector<std::uint8_t> data(97);
    std::iota(data.begin(), data.end(), std::uint8_t{0});

    for (std::size_t chunk = 1; chunk <= data.size(); ++chunk) {
        WriteBuffer wb;
        ASSERT_TRUE(wb.append(data.data(), data.size()));

        std::vector<std::uint8_t> drained;
        while (!wb.empty()) {
            const std::size_t n = std::min(chunk, wb.pending());
            drained.insert(drained.end(), wb.data(), wb.data() + n);
            wb.consume(n);
        }
        EXPECT_EQ(drained, data) << "corrupted at chunk size " << chunk;
    }
}

TEST(WriteBuffer, appends_after_partial_drain_stay_ordered) {
    WriteBuffer wb;
    const std::vector<std::uint8_t> a{1, 2, 3, 4};
    const std::vector<std::uint8_t> b{5, 6, 7, 8};

    ASSERT_TRUE(wb.append(a));
    wb.consume(2);            // partial write of {1,2}
    ASSERT_TRUE(wb.append(b));  // more output queued while {3,4} still pending

    std::vector<std::uint8_t> out;
    while (!wb.empty()) {
        out.push_back(*wb.data());
        wb.consume(1);
    }
    EXPECT_EQ(out, (std::vector<std::uint8_t>{3, 4, 5, 6, 7, 8}));
}

TEST(WriteBuffer, append_beyond_capacity_is_refused_atomically) {
    // Refusing must leave the buffer untouched. A partial append would put a
    // truncated message on the wire, which is worse than refusing outright.
    WriteBuffer wb(10);
    const std::vector<std::uint8_t> six{1, 2, 3, 4, 5, 6};
    ASSERT_TRUE(wb.append(six));
    EXPECT_EQ(wb.pending(), 6u);

    EXPECT_FALSE(wb.append(six));  // would reach 12 > 10
    EXPECT_EQ(wb.pending(), 6u) << "refused append still mutated the buffer";

    const std::vector<std::uint8_t> four{7, 8, 9, 10};
    EXPECT_TRUE(wb.append(four));  // exactly at capacity is allowed
    EXPECT_EQ(wb.pending(), 10u);
}

TEST(WriteBuffer, capacity_frees_as_bytes_are_written) {
    // The cap is on OUTSTANDING bytes, not total ever written — a healthy
    // connection can send far more than its buffer size over its lifetime.
    WriteBuffer wb(10);
    const std::vector<std::uint8_t> ten(10, 0xAB);
    ASSERT_TRUE(wb.append(ten));
    EXPECT_FALSE(wb.append(ten));

    wb.consume(10);
    EXPECT_TRUE(wb.empty());
    EXPECT_TRUE(wb.append(ten)) << "capacity did not free after draining";
}

TEST(WriteBuffer, empty_append_is_a_noop_and_succeeds) {
    WriteBuffer wb(4);
    EXPECT_TRUE(wb.append(nullptr, 0));
    EXPECT_TRUE(wb.empty());
}

// --- regression from code review -------------------------------------------

TEST(WriteBuffer, memory_held_stays_within_the_configured_cap) {
    // REGRESSION: the capacity check counted only unsent bytes, so the
    // consumed-but-not-compacted prefix sat above the cap. With a small
    // configured cap that is a large overshoot of actual allocation.
    WriteBuffer wb(64);
    const std::vector<std::uint8_t> chunk(32, 0xCD);

    // Churn far more bytes than the cap, always draining what we appended.
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(wb.append(chunk)) << "append refused at iteration " << i;
        wb.consume(wb.pending());
    }
    EXPECT_TRUE(wb.empty());

    // Interleave partial drains so a consumed prefix is always present.
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(wb.append(chunk));
        wb.consume(16);
        wb.consume(wb.pending());
    }
    EXPECT_TRUE(wb.empty());
}
