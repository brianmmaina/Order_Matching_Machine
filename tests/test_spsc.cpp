// lock-free SPSC queue.
//
// The concurrent test below is the point of this file. Until it existed the
// queue was only ever driven by one thread, which exercises none of the memory
// ordering it is built out of — a single-threaded pass proves the ring
// arithmetic and nothing about the handoff.

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "order.h"
#include "spsc_queue.h"

TEST(SpscQueue, push_pop_roundtrip_single_thread) {
    // one thread acting as both roles is allowed for testing; real spsc uses one thread per role.
    SpscQueue<Order, 4> q;
    Order o = Order::make(10000, 2, Order::BID, Order::LIMIT, 9);
    const uint64_t id = o.id;
    EXPECT_TRUE(q.push(std::move(o)));
    auto out = q.pop();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->id, id);
    EXPECT_FALSE(q.pop().has_value());
}

TEST(SpscQueue, full_rejects_push) {
    SpscQueue<Order, 2> q;
    EXPECT_TRUE(q.push(Order::make(10000, 1, Order::BID, Order::LIMIT, 1)));
    EXPECT_TRUE(q.push(Order::make(20000, 1, Order::BID, Order::LIMIT, 2)));
    EXPECT_FALSE(q.push(Order::make(30000, 1, Order::BID, Order::LIMIT, 3)));
}

TEST(SpscQueue, generic_over_element_type) {
    // the gateway will carry a command struct, not a bare Order.
    SpscQueue<std::uint64_t, 8> q;
    EXPECT_TRUE(q.push(42));
    auto out = q.pop();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, 42u);
}

// One producer thread, one consumer thread, a queue far smaller than the
// message count so both the full and empty paths are hit repeatedly.
//
// Asserts the three properties the matching thread will depend on:
//   no loss, no duplication, and FIFO order.
//
// Run this under -DOME_SANITIZE=thread. Without TSan it is a liveness test that
// happens to pass; with TSan it is a check on the release/acquire pairing.
TEST(SpscQueue, concurrent_producer_consumer_preserves_order) {
    // capacity 64 against 100k messages: the ring wraps ~1500 times and both
    // sides genuinely block on each other rather than one racing ahead.
    constexpr std::size_t kCapacity = 64;
    constexpr std::uint64_t kMessages = 100000;

    SpscQueue<std::uint64_t, kCapacity> q;
    std::vector<std::uint64_t> received;
    received.reserve(kMessages);

    std::thread consumer([&] {
        std::uint64_t seen = 0;
        while (seen < kMessages) {
            if (auto v = q.pop()) {
                received.push_back(*v);
                ++seen;
            } else {
                // spin rather than sleep: this is the "how does the consumer
                // wait" question session 1.4 answers properly. here we only
                // need the handoff exercised, not a good idle policy.
                std::this_thread::yield();
            }
        }
    });

    for (std::uint64_t i = 0; i < kMessages; ++i) {
        while (!q.push(i)) {
            std::this_thread::yield();
        }
    }
    consumer.join();

    // no loss and no duplication
    ASSERT_EQ(received.size(), kMessages);
    // FIFO: values were pushed in ascending order, so receipt order must match
    // exactly. a single reordered or repeated element fails here.
    for (std::uint64_t i = 0; i < kMessages; ++i) {
        ASSERT_EQ(received[i], i) << "first divergence at index " << i;
    }
}
