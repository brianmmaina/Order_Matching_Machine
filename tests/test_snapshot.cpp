// Snapshots: bounded recovery time, without changing what recovery produces.
//
// The load-bearing test is snapshot_plus_tail_equals_full_replay. A snapshot is
// only useful if taking one is invisible in the result — the book you get from
// "load snapshot, replay the tail" must be the book you get from replaying
// everything. Anything else means the snapshot is an approximation, and an
// approximate exchange is not an exchange.

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "ome/matching_thread.hpp"
#include "ome/snapshot.hpp"
#include "ome/wal.hpp"

using namespace ome;

namespace {

std::string temp_path() {
    std::vector<char> t(64);
    std::snprintf(t.data(), t.size(), "/tmp/ome_snap_XXXXXX");
    const int fd = ::mkstemp(t.data());
    EXPECT_GE(fd, 0);
    if (fd >= 0) ::close(fd);
    return std::string(t.data());
}

struct Engine {
    InboundQueue queue;
    Waiter waiter;
    MatchingThread mt{queue, waiter};

    void apply(const OrderCommand& c) {
        ASSERT_TRUE(queue.push(c));
        mt.drain_once();
    }
    [[nodiscard]] std::uint64_t digest() const { return mt.digest(); }
};

OrderCommand order(SessionId s, std::uint64_t coid, std::int64_t px, std::uint32_t qty,
                   protocol::Side side) {
    protocol::NewOrder m{};
    m.client_order_id = coid;
    m.price_ticks = px;
    m.quantity = qty;
    m.side = side;
    m.order_type = protocol::OrderType::Limit;
    return OrderCommand::new_order(s, m);
}

std::vector<OrderCommand> random_stream(std::uint32_t seed, int n, std::uint64_t& next_coid) {
    std::mt19937 rng(seed);
    std::vector<OrderCommand> out;
    std::vector<std::pair<SessionId, std::uint64_t>> live;
    for (int i = 0; i < n; ++i) {
        const auto s = static_cast<SessionId>(1 + (rng() % 3));
        const int roll = static_cast<int>(rng() % 100);
        if (roll < 70 || live.empty()) {
            const std::int64_t px = 999500 + static_cast<std::int64_t>(rng() % 11) * 100;
            const auto side = (rng() % 2) ? protocol::Side::Bid : protocol::Side::Ask;
            const auto qty = static_cast<std::uint32_t>(1 + (rng() % 50));
            const std::uint64_t coid = next_coid++;
            out.push_back(order(s, coid, px, qty, side));
            live.emplace_back(s, coid);
        } else if (roll < 90) {
            const auto& t = live[static_cast<std::size_t>(rng()) % live.size()];
            out.push_back(OrderCommand::cancel(t.first, t.second));
        } else {
            const auto& t = live[static_cast<std::size_t>(rng()) % live.size()];
            protocol::Modify m{};
            m.client_order_id = t.second;
            m.new_price_ticks = 999500 + static_cast<std::int64_t>(rng() % 11) * 100;
            m.new_quantity = static_cast<std::uint32_t>(1 + (rng() % 50));
            out.push_back(OrderCommand::modify(t.first, m));
        }
    }
    return out;
}

}  // namespace

TEST(Snapshot, round_trips_an_empty_book) {
    const auto path = temp_path();
    Engine e;
    std::string err;
    ASSERT_TRUE(write_snapshot(path, e.mt.export_snapshot(0), err)) << err;

    SnapshotData d{};
    ASSERT_TRUE(read_snapshot(path, d, err)) << err;
    EXPECT_TRUE(d.orders.empty());

    Engine restored;
    restored.mt.restore(d);
    EXPECT_EQ(restored.digest(), e.digest());
    std::remove(path.c_str());
}

TEST(Snapshot, restores_an_identical_book) {
    const auto path = temp_path();
    Engine live;
    std::uint64_t coid = 1;
    for (const auto& c : random_stream(11, 400, coid)) {
        live.apply(c);
    }
    ASSERT_TRUE(live.mt.engine().book().levels_consistent());

    std::string err;
    ASSERT_TRUE(write_snapshot(path, live.mt.export_snapshot(400), err)) << err;
    SnapshotData d{};
    ASSERT_TRUE(read_snapshot(path, d, err)) << err;
    EXPECT_EQ(d.last_seq, 400u);

    Engine restored;
    restored.mt.restore(d);
    EXPECT_EQ(restored.digest(), live.digest())
        << "restored book differs\nlive:\n"
        << live.mt.engine().book().debug_dump() << "restored:\n"
        << restored.mt.engine().book().debug_dump();
    EXPECT_TRUE(restored.mt.engine().book().levels_consistent());
    std::remove(path.c_str());
}

TEST(Snapshot, snapshot_plus_tail_equals_full_replay) {
    // THE test. Taking a snapshot must not change what recovery produces.
    const auto path = temp_path();
    std::uint64_t coid = 1;
    const auto first_half = random_stream(21, 250, coid);
    const auto second_half = random_stream(22, 250, coid);

    // Path A: replay everything from scratch.
    Engine full;
    for (const auto& c : first_half) full.apply(c);
    for (const auto& c : second_half) full.apply(c);

    // Path B: snapshot after the first half, then replay only the tail.
    Engine partial;
    for (const auto& c : first_half) partial.apply(c);
    std::string err;
    ASSERT_TRUE(write_snapshot(path, partial.mt.export_snapshot(first_half.size()), err)) << err;

    SnapshotData d{};
    ASSERT_TRUE(read_snapshot(path, d, err)) << err;
    Engine from_snapshot;
    from_snapshot.mt.restore(d);
    from_snapshot.mt.recover(second_half);

    EXPECT_EQ(from_snapshot.digest(), full.digest())
        << "snapshot+tail diverged from full replay\nfull:\n"
        << full.mt.engine().book().debug_dump() << "snapshot+tail:\n"
        << from_snapshot.mt.engine().book().debug_dump();
    std::remove(path.c_str());
}

TEST(Snapshot, cancels_still_work_against_a_restored_book) {
    // The ownership maps must come back too. Without them a cancel arriving
    // after recovery finds nothing and is rejected, even though the order is
    // sitting in the book.
    const auto path = temp_path();
    Engine live;
    live.apply(order(7, 100, 999000, 10, protocol::Side::Bid));
    live.apply(order(7, 101, 999100, 20, protocol::Side::Bid));

    std::string err;
    ASSERT_TRUE(write_snapshot(path, live.mt.export_snapshot(2), err)) << err;
    SnapshotData d{};
    ASSERT_TRUE(read_snapshot(path, d, err)) << err;

    Engine restored;
    restored.mt.restore(d);
    restored.mt.recover({OrderCommand::cancel(7, 100)});

    live.apply(OrderCommand::cancel(7, 100));
    EXPECT_EQ(restored.digest(), live.digest())
        << "a cancel after restore did not find its order";
    std::remove(path.c_str());
}

TEST(Snapshot, new_orders_after_restore_do_not_reuse_exchange_ids) {
    // next_exchange_id must survive, or a fresh order would collide with a
    // restored one and the ownership map would point at the wrong order.
    const auto path = temp_path();
    Engine live;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        live.apply(order(1, i, 999000, 10, protocol::Side::Bid));
    }
    std::string err;
    ASSERT_TRUE(write_snapshot(path, live.mt.export_snapshot(5), err)) << err;
    SnapshotData d{};
    ASSERT_TRUE(read_snapshot(path, d, err)) << err;
    EXPECT_GT(d.next_exchange_id, 5u);

    Engine restored;
    restored.mt.restore(d);
    restored.mt.recover({order(1, 99, 999000, 7, protocol::Side::Bid)});
    live.apply(order(1, 99, 999000, 7, protocol::Side::Bid));
    EXPECT_EQ(restored.digest(), live.digest());
    std::remove(path.c_str());
}

// --- corruption ------------------------------------------------------------

TEST(Snapshot, a_corrupted_body_fails_its_checksum) {
    const auto path = temp_path();
    Engine e;
    std::uint64_t coid = 1;
    for (const auto& c : random_stream(31, 50, coid)) e.apply(c);
    std::string err;
    ASSERT_TRUE(write_snapshot(path, e.mt.export_snapshot(50), err)) << err;

    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    ASSERT_GT(bytes.size(), kSnapshotHeaderSize + 8);
    bytes[kSnapshotHeaderSize + 4] ^= 0xFF;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    SnapshotData d{};
    EXPECT_FALSE(read_snapshot(path, d, err)) << "accepted a corrupted snapshot";
    EXPECT_NE(err.find("checksum"), std::string::npos) << err;
    std::remove(path.c_str());
}

TEST(Snapshot, a_truncated_file_is_rejected_rather_than_partially_loaded) {
    const auto path = temp_path();
    Engine e;
    std::uint64_t coid = 1;
    for (const auto& c : random_stream(41, 80, coid)) e.apply(c);
    std::string err;
    ASSERT_TRUE(write_snapshot(path, e.mt.export_snapshot(80), err)) << err;

    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    bytes.resize(bytes.size() / 2);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    SnapshotData d{};
    EXPECT_FALSE(read_snapshot(path, d, err)) << "loaded a truncated snapshot";
    std::remove(path.c_str());
}

TEST(Snapshot, a_foreign_or_missing_file_is_rejected_cleanly) {
    const auto path = temp_path();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "this is not a snapshot at all, not even close";
    }
    SnapshotData d{};
    std::string err;
    EXPECT_FALSE(read_snapshot(path, d, err));
    std::remove(path.c_str());

    EXPECT_FALSE(read_snapshot("/nonexistent/x.snap", d, err));
    EXPECT_FALSE(err.empty());
}

TEST(Snapshot, writing_leaves_no_temp_file_behind) {
    const auto path = temp_path();
    Engine e;
    std::string err;
    ASSERT_TRUE(write_snapshot(path, e.mt.export_snapshot(1), err)) << err;
    const std::string tmp = path + ".tmp";
    EXPECT_NE(::access(path.c_str(), F_OK), -1) << "target missing after write";
    EXPECT_EQ(::access(tmp.c_str(), F_OK), -1) << "temp file left behind";
    std::remove(path.c_str());
}
