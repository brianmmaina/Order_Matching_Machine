// Recovery: rebuild the book from the log and get the same book back.
//
// The 100-seed property test is the point of this file. A fixed scenario proves
// recovery works for that scenario; random command streams prove it works for
// streams nobody thought to write down, which is where replay bugs actually
// live — partial fills, cancels of already-filled orders, modifies that cross,
// levels emptying and reappearing.

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "ome/matching_thread.hpp"
#include "ome/wal.hpp"

using namespace ome;

namespace {

std::string temp_path() {
    std::vector<char> t(64);
    std::snprintf(t.data(), t.size(), "/tmp/ome_rec_XXXXXX");
    const int fd = ::mkstemp(t.data());
    EXPECT_GE(fd, 0);
    if (fd >= 0) ::close(fd);
    return std::string(t.data());
}

// A matching thread that is never started: recovery and direct drives both run
// on the calling thread, so these tests are fully deterministic.
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

// Random but VALID commands. Invalid ones are rejected before they reach the
// book, so they exercise the risk checks rather than replay.
std::vector<OrderCommand> random_stream(std::uint32_t seed, int n) {
    std::mt19937 rng(seed);
    std::vector<OrderCommand> out;
    std::vector<std::pair<SessionId, std::uint64_t>> live;
    std::uint64_t next_coid = 1;

    for (int i = 0; i < n; ++i) {
        const auto s = static_cast<SessionId>(1 + (rng() % 3));
        const int roll = static_cast<int>(rng() % 100);

        if (roll < 65 || live.empty()) {
            // Prices in a tight band so orders genuinely cross and partially
            // fill, rather than resting harmlessly at distinct levels.
            const std::int64_t px = 999500 + static_cast<std::int64_t>(rng() % 11) * 100;
            const auto side = (rng() % 2) ? protocol::Side::Bid : protocol::Side::Ask;
            const auto qty = static_cast<std::uint32_t>(1 + (rng() % 50));
            const std::uint64_t coid = next_coid++;
            out.push_back(order(s, coid, px, qty, side));
            live.emplace_back(s, coid);
        } else if (roll < 85) {
            const auto& t = live[static_cast<std::size_t>(rng()) % live.size()];
            out.push_back(OrderCommand::cancel(t.first, t.second));
        } else if (roll < 97) {
            const auto& t = live[static_cast<std::size_t>(rng()) % live.size()];
            protocol::Modify m{};
            m.client_order_id = t.second;
            m.new_price_ticks = 999500 + static_cast<std::int64_t>(rng() % 11) * 100;
            m.new_quantity = static_cast<std::uint32_t>(1 + (rng() % 50));
            out.push_back(OrderCommand::modify(t.first, m));
        } else {
            out.push_back(OrderCommand::cancel_all(static_cast<SessionId>(1 + (rng() % 3))));
        }
    }
    return out;
}

}  // namespace

// --- digest ----------------------------------------------------------------

TEST(Digest, empty_books_agree_and_differ_from_populated_ones) {
    Engine a, b;
    EXPECT_EQ(a.digest(), b.digest()) << "two empty books disagree";
    a.apply(order(1, 1, 1000000, 10, protocol::Side::Bid));
    EXPECT_NE(a.digest(), b.digest());
}

TEST(Digest, distinguishes_price_quantity_and_order_count) {
    // One order of 100 and two of 50 are the same aggregate depth but different
    // books: they behave differently on the next partial fill.
    Engine one, two;
    one.apply(order(1, 1, 1000000, 100, protocol::Side::Bid));
    two.apply(order(1, 1, 1000000, 50, protocol::Side::Bid));
    two.apply(order(1, 2, 1000000, 50, protocol::Side::Bid));
    EXPECT_NE(one.digest(), two.digest()) << "order count is not in the digest";

    Engine px_a, px_b;
    px_a.apply(order(1, 1, 1000000, 10, protocol::Side::Bid));
    px_b.apply(order(1, 1, 1000100, 10, protocol::Side::Bid));
    EXPECT_NE(px_a.digest(), px_b.digest()) << "price is not in the digest";

    Engine q_a, q_b;
    q_a.apply(order(1, 1, 1000000, 10, protocol::Side::Bid));
    q_b.apply(order(1, 1, 1000000, 11, protocol::Side::Bid));
    EXPECT_NE(q_a.digest(), q_b.digest()) << "quantity is not in the digest";
}

TEST(Digest, a_bid_book_does_not_collide_with_its_mirror_on_the_ask) {
    Engine bid, ask;
    bid.apply(order(1, 1, 1000000, 10, protocol::Side::Bid));
    ask.apply(order(1, 1, 1000000, 10, protocol::Side::Ask));
    EXPECT_NE(bid.digest(), ask.digest()) << "side is not in the digest";
}

TEST(Digest, is_independent_of_which_session_placed_the_orders) {
    // Sessions do not survive a restart, so a digest that depended on them
    // could never match after recovery.
    Engine a, b;
    a.apply(order(1, 1, 1000000, 10, protocol::Side::Bid));
    b.apply(order(99, 1, 1000000, 10, protocol::Side::Bid));
    EXPECT_EQ(a.digest(), b.digest()) << "the digest leaked session identity";
}

// --- recovery --------------------------------------------------------------

TEST(Recovery, a_fixed_scenario_rebuilds_an_identical_book) {
    const auto path = temp_path();
    std::uint64_t live_digest = 0;
    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0));
        Engine e;
        const std::vector<OrderCommand> cmds = {
            order(1, 1, 999000, 10, protocol::Side::Bid),
            order(1, 2, 999100, 20, protocol::Side::Bid),
            order(2, 1, 1001000, 15, protocol::Side::Ask),
            order(2, 2, 999100, 5, protocol::Side::Ask),  // crosses, partial fill
            OrderCommand::cancel(1, 1),
        };
        for (const auto& c : cmds) {
            ASSERT_TRUE(w.append(c, 0));
            e.apply(c);
        }
        live_digest = e.digest();
    }

    const auto rd = read_wal(path);
    ASSERT_TRUE(rd.error.empty()) << rd.error;
    ASSERT_FALSE(rd.sequence_gap);

    Engine recovered;
    EXPECT_EQ(recovered.mt.recover(rd.commands), rd.commands.size());
    EXPECT_EQ(recovered.digest(), live_digest)
        << "recovered book differs\n"
        << recovered.mt.engine().book().debug_dump();
    std::remove(path.c_str());
}

TEST(Recovery, replay_is_deterministic_across_100_seeds) {
    // THE property test. Each seed produces a different random stream of valid
    // commands — new orders that cross and partially fill, cancels, modifies
    // that reprice into the spread, cancel-alls — and for every one of them the
    // book rebuilt from the log must be byte-for-byte the book that existed.
    //
    // If this passes, "state is a deterministic fold over the log" is a tested
    // claim rather than a design intention.
    // A comparison of two empty books would pass forever while proving nothing,
    // so count how many seeds actually built state and require most of them to.
    Engine empty_ref;
    const std::uint64_t empty_digest = empty_ref.digest();
    int non_trivial = 0;

    for (std::uint32_t seed = 1; seed <= 100; ++seed) {
        const auto cmds = random_stream(seed, 300);

        Engine live;
        for (const auto& c : cmds) {
            live.apply(c);
        }

        Engine replayed;
        replayed.mt.recover(cmds);

        ASSERT_EQ(replayed.digest(), live.digest())
            << "seed " << seed << " diverged\nlive:\n"
            << live.mt.engine().book().debug_dump() << "replayed:\n"
            << replayed.mt.engine().book().debug_dump();
        ASSERT_TRUE(live.mt.engine().book().levels_consistent()) << "seed " << seed;
        ASSERT_TRUE(replayed.mt.engine().book().levels_consistent()) << "seed " << seed;
        if (live.digest() != empty_digest) {
            ++non_trivial;
        }
    }
    EXPECT_GE(non_trivial, 90) << "most seeds left an empty book, so this test "
                                  "was comparing nothing";
}

TEST(Recovery, survives_a_round_trip_through_the_log_file) {
    // The property test above replays in-memory commands. This one puts them
    // through the actual encoder, file, and decoder, so a field lost in
    // serialization cannot hide behind a shared in-memory struct.
    for (std::uint32_t seed = 1; seed <= 10; ++seed) {
        const auto path = temp_path();
        const auto cmds = random_stream(seed, 200);

        Engine live;
        {
            Wal w;
            ASSERT_TRUE(w.open(path, 0));
            for (const auto& c : cmds) {
                ASSERT_TRUE(w.append(c, 0));
                live.apply(c);
            }
        }

        const auto rd = read_wal(path);
        ASSERT_TRUE(rd.error.empty()) << rd.error;
        ASSERT_FALSE(rd.sequence_gap) << "seed " << seed;
        ASSERT_EQ(rd.commands.size(), cmds.size()) << "seed " << seed;

        Engine recovered;
        recovered.mt.recover(rd.commands);
        EXPECT_EQ(recovered.digest(), live.digest()) << "seed " << seed << " diverged on disk";
        std::remove(path.c_str());
    }
}

TEST(Recovery, a_torn_tail_recovers_the_book_as_of_the_last_intact_record) {
    // A crash mid-write must leave a usable book, not an unusable one: the
    // state as of the last complete record, with the partial one discarded.
    const auto path = temp_path();
    const auto cmds = random_stream(7, 60);

    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0));
        for (const auto& c : cmds) {
            ASSERT_TRUE(w.append(c, 0));
        }
    }

    // Expected state after every command except the last.
    Engine expected;
    for (std::size_t i = 0; i + 1 < cmds.size(); ++i) {
        expected.apply(cmds[i]);
    }

    // Chop the final record in half.
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::size_t rec = kWalHeaderSize + 31;
    ASSERT_GT(bytes.size(), rec);
    bytes.resize(bytes.size() - rec / 2);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    const auto rd = read_wal(path);
    EXPECT_GT(rd.truncated_bytes, 0u) << "the torn tail was not noticed";
    EXPECT_EQ(rd.commands.size(), cmds.size() - 1);

    Engine recovered;
    recovered.mt.recover(rd.commands);
    EXPECT_EQ(recovered.digest(), expected.digest())
        << "a torn tail did not recover to the last intact record";
    std::remove(path.c_str());
}

TEST(Recovery, replaying_an_empty_log_leaves_an_empty_book) {
    Engine fresh, recovered;
    EXPECT_EQ(recovered.mt.recover({}), 0u);
    EXPECT_EQ(recovered.digest(), fresh.digest());
}
