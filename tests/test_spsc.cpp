// lock-free SPSC queue. currently single-threaded coverage only — a real
// producer/consumer test lands in session 0.3 alongside the generic element type.

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

#include "order.h"
#include "spsc_queue.h"

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
