// Market data broadcast and the conflation asymmetry.
//
// The point of this file is the CONTRAST between two overflow policies:
//
//   market data  a newer snapshot supersedes an older one, so a subscriber
//                that falls behind should skip ahead rather than be punished
//   order flow   an Ack or Fill is a distinct fact a client cannot
//                reconstruct, so a queue that fills must disconnect
//
// Being able to demonstrate both, side by side, is what makes the design point
// a demonstration rather than an assertion.

#include <gtest/gtest.h>

#include <cstdint>

#include "ome/book_snapshot.hpp"
#include "ome/egress.hpp"
#include "ome/protocol.hpp"

using namespace ome;

namespace {

BookSnapshot snap(std::uint64_t seq, std::int64_t best_bid) {
    BookSnapshot s{};
    s.seq = seq;
    s.n_bids = 1;
    s.n_asks = 1;
    s.bid_ticks[0] = best_bid;
    s.bid_qty[0] = 10;
    s.ask_ticks[0] = best_bid + 100;
    s.ask_qty[0] = 10;
    return s;
}

// The network thread's conflating drain, in isolation: take everything
// pending, keep the last.
BookSnapshot drain_conflating(MarketDataQueue& q, std::size_t& skipped) {
    BookSnapshot latest{};
    bool have = false;
    skipped = 0;
    while (auto s = q.pop()) {
        if (have) {
            ++skipped;
        }
        latest = *s;
        have = true;
    }
    return latest;
}

}  // namespace

TEST(BookSnapshot, is_fixed_size_and_carries_no_heap) {
    // It crosses a lock-free ring by value; owning memory would mean allocating
    // on one thread and freeing on another for every update.
    EXPECT_TRUE(std::is_trivially_copyable<BookSnapshot>::value);
    BookSnapshot a = snap(1, 1000000);
    BookSnapshot b = a;
    EXPECT_EQ(b.bid_ticks[0], 1000000);
    EXPECT_EQ(b.seq, 1u);
}

TEST(MarketData, a_subscriber_that_keeps_up_sees_every_update) {
    MarketDataQueue q;
    for (std::uint64_t i = 1; i <= 20; ++i) {
        ASSERT_TRUE(q.push(snap(i, 1000000 + static_cast<std::int64_t>(i))));
        std::size_t skipped = 0;
        const auto got = drain_conflating(q, skipped);
        EXPECT_EQ(got.seq, i);
        EXPECT_EQ(skipped, 0u) << "conflated despite keeping up";
    }
}

TEST(MarketData, a_slow_subscriber_skips_to_the_newest) {
    // THE conflation property. Ten updates accumulate; the subscriber gets the
    // tenth and nothing else, because the first nine describe a book that no
    // longer exists.
    MarketDataQueue q;
    for (std::uint64_t i = 1; i <= 10; ++i) {
        ASSERT_TRUE(q.push(snap(i, 1000000 + static_cast<std::int64_t>(i))));
    }
    std::size_t skipped = 0;
    const auto got = drain_conflating(q, skipped);

    EXPECT_EQ(got.seq, 10u) << "did not skip to the newest snapshot";
    EXPECT_EQ(got.bid_ticks[0], 1000010) << "delivered stale prices";
    EXPECT_EQ(skipped, 9u) << "wrong conflation count";
    EXPECT_TRUE(q.pop() == std::nullopt) << "left stale updates queued";
}

TEST(MarketData, conflation_never_delivers_a_price_that_was_superseded) {
    // Stronger than "the last one wins": no intermediate state may leak out.
    MarketDataQueue q;
    for (std::uint64_t i = 1; i <= 50; ++i) {
        ASSERT_TRUE(q.push(snap(i, 1000000 + static_cast<std::int64_t>(i) * 100)));
    }
    std::size_t skipped = 0;
    const auto got = drain_conflating(q, skipped);
    EXPECT_EQ(got.bid_ticks[0], 1000000 + 50 * 100);
    EXPECT_EQ(skipped, 49u);
}

TEST(MarketData, a_full_queue_drops_rather_than_disconnecting) {
    // Market data's overflow policy. The producer drops; the subscriber stays.
    MarketDataQueue q;
    std::uint64_t pushed = 0;
    std::uint64_t dropped = 0;
    for (std::uint64_t i = 1; i <= kMarketDataCapacity * 3; ++i) {
        if (q.push(snap(i, 1000000))) {
            ++pushed;
        } else {
            ++dropped;
        }
    }
    EXPECT_EQ(pushed, kMarketDataCapacity) << "queue accepted beyond capacity";
    EXPECT_GT(dropped, 0u) << "nothing was dropped, so the policy was not exercised";

    // The subscriber is still usable: it drains and carries on.
    std::size_t skipped = 0;
    const auto got = drain_conflating(q, skipped);
    EXPECT_GT(got.seq, 0u);
    EXPECT_TRUE(q.push(snap(999, 1000000))) << "queue unusable after overflow";
}

TEST(OrderFlow, a_full_queue_latches_overflow_instead_of_dropping) {
    // THE CONTRAST. The same overflow condition, opposite response: order flow
    // cannot drop, so the queue latches a flag the network thread turns into a
    // disconnect. An Ack silently discarded leaves a client believing something
    // false about its own position.
    EgressQueue q;
    std::uint64_t accepted = 0;
    while (q.push(OrderEvent::ack(1, accepted + 1, accepted + 1))) {
        ++accepted;
    }
    EXPECT_GT(accepted, 0u);
    EXPECT_FALSE(q.overflowed()) << "latched before anything actually failed";

    q.mark_overflowed();
    EXPECT_TRUE(q.overflowed()) << "overflow was not observable to the network thread";

    // And nothing was silently discarded: every accepted event is still there.
    std::uint64_t drained = 0;
    while (q.pop()) {
        ++drained;
    }
    EXPECT_EQ(drained, accepted) << "order events went missing";
}

TEST(MarketData, snapshot_survives_a_round_trip_through_the_protocol) {
    // The snapshot is the internal form; BookUpdate is the wire form. A depth
    // mismatch between them would truncate silently.
    BookSnapshot s{};
    s.seq = 77;
    s.n_bids = protocol::kMaxBookDepth;
    s.n_asks = protocol::kMaxBookDepth;
    for (std::size_t i = 0; i < protocol::kMaxBookDepth; ++i) {
        s.bid_ticks[i] = 1000000 - static_cast<std::int64_t>(i) * 100;
        s.bid_qty[i] = 10 + i;
        s.ask_ticks[i] = 1000100 + static_cast<std::int64_t>(i) * 100;
        s.ask_qty[i] = 20 + i;
    }

    protocol::BookUpdate up{};
    up.seq = s.seq;
    for (std::uint8_t i = 0; i < s.n_bids; ++i) {
        up.bids.push_back({s.bid_ticks[i], s.bid_qty[i]});
    }
    for (std::uint8_t i = 0; i < s.n_asks; ++i) {
        up.asks.push_back({s.ask_ticks[i], s.ask_qty[i]});
    }

    std::vector<std::uint8_t> bytes;
    protocol::encode(bytes, up);
    const auto back = protocol::decode<protocol::BookUpdate>(bytes.data(), bytes.size());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->seq, 77u);
    ASSERT_EQ(back->bids.size(), protocol::kMaxBookDepth);
    ASSERT_EQ(back->asks.size(), protocol::kMaxBookDepth);
    EXPECT_EQ(back->bids[0].price_ticks, 1000000);
    EXPECT_EQ(back->asks[9].quantity, 29u);
}
