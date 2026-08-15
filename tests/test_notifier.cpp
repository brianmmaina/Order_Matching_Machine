// Cross-thread wake-up channel.
//
// The test that matters here is dead_channel: this class had a bug where the
// notification path stopped working permanently after one unlucky interleaving,
// and the symptom was a p99 in the tens of milliseconds while p50 stayed under
// 200us. It was found by a load generator, not by a unit test, which is why one
// exists now.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <poll.h>
#include <thread>

#include "ome/notifier.hpp"

using namespace ome;

namespace {
bool readable(int fd, int timeout_ms) {
    pollfd p{};
    p.fd = fd;
    p.events = POLLIN;
    return ::poll(&p, 1, timeout_ms) > 0;
}
}  // namespace

TEST(Notifier, constructs_with_a_valid_pollable_pipe) {
    Notifier n;
    ASSERT_TRUE(n.valid());
    EXPECT_GE(n.poll_fd(), 0);
    EXPECT_FALSE(readable(n.poll_fd(), 0)) << "readable before anything was notified";
}

TEST(Notifier, notify_makes_the_descriptor_readable) {
    Notifier n;
    ASSERT_TRUE(n.valid());
    n.notify();
    EXPECT_TRUE(readable(n.poll_fd(), 100));
    n.drain();
    EXPECT_FALSE(readable(n.poll_fd(), 0)) << "still readable after draining";
}

TEST(Notifier, coalesces_a_burst_into_one_wakeup) {
    // A thousand fills must not cost a thousand syscalls on the matching thread.
    Notifier n;
    ASSERT_TRUE(n.valid());
    for (int i = 0; i < 1000; ++i) {
        n.notify();
    }
    EXPECT_TRUE(readable(n.poll_fd(), 100));
    n.drain();
    EXPECT_FALSE(readable(n.poll_fd(), 0));
}

TEST(Notifier, the_channel_survives_a_notify_landing_inside_a_drain) {
    // REGRESSION. drain() used to clear its flag BEFORE reading the pipe. A
    // notify() arriving in that window wrote a byte, the read loop consumed it,
    // and the channel was left flagged-as-pending with an empty pipe — so every
    // later notify() skipped its write and the descriptor never became readable
    // again. Permanently dead, not a single missed wake-up.
    //
    // Interleaving it precisely is impractical, so this hammers the window from
    // another thread and asserts the channel still works afterwards.
    Notifier n;
    ASSERT_TRUE(n.valid());

    std::atomic<bool> stop{false};
    std::thread producer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            n.notify();
        }
    });

    for (int i = 0; i < 2000; ++i) {
        n.drain();
    }
    stop.store(true);
    producer.join();

    // Whatever state the hammering left, one more notify must wake a poller.
    n.drain();
    n.notify();
    EXPECT_TRUE(readable(n.poll_fd(), 1000))
        << "notification channel died after concurrent notify/drain";
}

TEST(Notifier, wakes_a_blocked_poller_from_another_thread) {
    Notifier n;
    ASSERT_TRUE(n.valid());
    std::thread waker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        n.notify();
    });
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(readable(n.poll_fd(), 2000)) << "poller was never woken";
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);
    waker.join();
}
