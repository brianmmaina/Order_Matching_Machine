// Session lifecycle: heartbeats, timeout, order ownership, cancel-on-disconnect.
//
// Time is injected rather than read from a clock, so a 15-second timeout is
// tested in microseconds. Sleeping for real would make the suite slow enough
// that people skip it and flaky on a loaded CI runner — and a flaky liveness
// test is worse than none, because it trains you to ignore it.

#include <gtest/gtest.h>

#include <cstdint>

#include "ome/session.hpp"

using namespace ome;

namespace {
constexpr Nanos kSec = 1000ULL * 1000 * 1000;
}

TEST(Session, starts_connected_with_no_orders) {
    Session s(1, 0);
    EXPECT_EQ(s.id(), 1u);
    EXPECT_TRUE(s.alive());
    EXPECT_EQ(s.state(), SessionState::Connected);
    EXPECT_EQ(s.live_order_count(), 0u);
}

// --- heartbeats ------------------------------------------------------------

TEST(Session, heartbeat_becomes_due_at_the_interval) {
    Session s(1, 0);  // default: send every 5s
    EXPECT_FALSE(s.heartbeat_due(1 * kSec));
    EXPECT_FALSE(s.heartbeat_due(4 * kSec));
    EXPECT_TRUE(s.heartbeat_due(5 * kSec)) << "boundary is inclusive";
    EXPECT_TRUE(s.heartbeat_due(9 * kSec));
}

TEST(Session, sending_a_heartbeat_resets_the_interval) {
    Session s(1, 0);
    ASSERT_TRUE(s.heartbeat_due(5 * kSec));
    s.on_heartbeat_sent(5 * kSec);
    EXPECT_FALSE(s.heartbeat_due(9 * kSec));
    EXPECT_TRUE(s.heartbeat_due(10 * kSec));
}

TEST(Session, dead_sessions_are_never_due_a_heartbeat) {
    Session s(1, 0);
    static_cast<void>(s.mark_dead());
    EXPECT_FALSE(s.heartbeat_due(1000 * kSec));
}

// --- timeout ---------------------------------------------------------------

TEST(Session, times_out_after_three_missed_intervals) {
    Session s(1, 0);  // default: 15s
    EXPECT_FALSE(s.timed_out(14 * kSec));
    EXPECT_TRUE(s.timed_out(15 * kSec)) << "boundary is inclusive";
}

TEST(Session, any_inbound_traffic_defers_the_timeout) {
    // A client streaming orders is obviously alive; requiring it to also send
    // heartbeats would be a protocol that times out its busiest clients.
    Session s(1, 0);
    s.on_inbound(10 * kSec);
    EXPECT_FALSE(s.timed_out(20 * kSec)) << "activity at 10s did not defer the timeout";
    EXPECT_TRUE(s.timed_out(25 * kSec));
}

TEST(Session, heartbeat_sends_do_not_defer_the_timeout) {
    // The critical asymmetry. Timeout must be driven by what we RECEIVE. If our
    // own outbound heartbeats reset the timer, a session whose peer has gone
    // silent would be kept alive forever by the server talking to itself.
    Session s(1, 0);
    for (Nanos t = 5 * kSec; t <= 15 * kSec; t += 5 * kSec) {
        s.on_heartbeat_sent(t);
    }
    EXPECT_TRUE(s.timed_out(15 * kSec)) << "outbound traffic wrongly counted as liveness";
}

TEST(Session, timed_out_sessions_report_false_once_dead) {
    // Prevents a swept session from being swept again on the next loop.
    Session s(1, 0);
    ASSERT_TRUE(s.timed_out(20 * kSec));
    static_cast<void>(s.mark_dead());
    EXPECT_FALSE(s.timed_out(20 * kSec));
}

TEST(Session, custom_timing_config_is_honored) {
    SessionConfig cfg{};
    cfg.heartbeat_interval_ns = 100;
    cfg.timeout_ns = 300;
    Session s(1, 0, cfg);
    EXPECT_TRUE(s.heartbeat_due(100));
    EXPECT_FALSE(s.timed_out(299));
    EXPECT_TRUE(s.timed_out(300));
}

// --- cancel-on-disconnect fires exactly once -------------------------------

TEST(Session, mark_dead_returns_true_only_on_the_transition) {
    // THE property cancel-on-disconnect depends on. Several paths can notice a
    // death in the same loop iteration — poll timeout, peer close, framing
    // error, write overflow — and every one of them calls mark_dead(). Only the
    // first may emit a CancelAllForSession.
    Session s(1, 0);
    EXPECT_TRUE(s.mark_dead()) << "first transition must report true";
    EXPECT_FALSE(s.mark_dead()) << "second call must not re-fire the cancel-all";
    EXPECT_FALSE(s.mark_dead());
    EXPECT_FALSE(s.alive());
}

TEST(Session, counts_live_orders_for_the_cancel_all) {
    Session s(1, 0);
    ASSERT_TRUE(s.register_order(1));
    ASSERT_TRUE(s.register_order(2));
    ASSERT_TRUE(s.register_order(3));
    EXPECT_EQ(s.live_order_count(), 3u);

    EXPECT_TRUE(s.forget_order(2));
    EXPECT_EQ(s.live_order_count(), 2u)
        << "a filled or cancelled order must not be cancelled again on disconnect";
}

// --- order ownership -------------------------------------------------------

TEST(Session, duplicate_client_order_id_is_refused) {
    Session s(1, 0);
    EXPECT_TRUE(s.register_order(100));
    EXPECT_FALSE(s.register_order(100)) << "duplicate accepted";
    EXPECT_EQ(s.live_order_count(), 1u);
}

TEST(Session, client_order_ids_are_scoped_per_session) {
    // Clients number their own orders from 1 and cannot coordinate with each
    // other. Global uniqueness is the exchange_order_id's job.
    Session a(1, 0);
    Session b(2, 0);
    EXPECT_TRUE(a.register_order(1));
    EXPECT_TRUE(b.register_order(1)) << "another session's id must not collide";
}

TEST(Session, an_id_is_reusable_once_the_order_leaves_the_book) {
    // Deliberate: a long-lived session would otherwise accumulate ids forever.
    // The consequence — client_order_id is unique among LIVE orders rather than
    // for all time — is stated in docs/PROTOCOL.md.
    Session s(1, 0);
    ASSERT_TRUE(s.register_order(5));
    EXPECT_FALSE(s.register_order(5));

    ASSERT_TRUE(s.forget_order(5));
    EXPECT_TRUE(s.register_order(5)) << "id not reusable after the order left the book";
}

TEST(Session, forgetting_an_unknown_order_reports_false) {
    Session s(1, 0);
    EXPECT_FALSE(s.forget_order(999));
    ASSERT_TRUE(s.register_order(1));
    EXPECT_FALSE(s.forget_order(999));
    EXPECT_EQ(s.live_order_count(), 1u);
}

TEST(Session, has_order_tracks_registration) {
    Session s(1, 0);
    EXPECT_FALSE(s.has_order(7));
    ASSERT_TRUE(s.register_order(7));
    EXPECT_TRUE(s.has_order(7));
    ASSERT_TRUE(s.forget_order(7));
    EXPECT_FALSE(s.has_order(7));
}
