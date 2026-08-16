// Write-ahead log: record fidelity, corruption detection, torn tails.
//
// The torn-tail cases are the ones that matter. A crash leaves the last record
// half-written, and recovery has to stop cleanly at the last intact record
// rather than replaying garbage into the book or refusing to start. Every
// truncation length is tested, not one arbitrary cut, for the same reason the
// framing tests split at every byte: an off-by-one in the "is this record
// complete" check hides from a single case.

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "ome/commands.hpp"
#include "ome/crc32.hpp"
#include "ome/wal.hpp"

using namespace ome;

namespace {

std::string temp_path() {
    std::vector<char> tmpl(64);
    std::snprintf(tmpl.data(), tmpl.size(), "/tmp/ome_wal_XXXXXX");
    const int fd = ::mkstemp(tmpl.data());
    EXPECT_GE(fd, 0);
    if (fd >= 0) ::close(fd);
    return std::string(tmpl.data());
}

OrderCommand new_order(SessionId s, std::uint64_t coid, std::int64_t px, std::uint32_t qty) {
    protocol::NewOrder m{};
    m.client_order_id = coid;
    m.price_ticks = px;
    m.quantity = qty;
    m.side = protocol::Side::Bid;
    m.order_type = protocol::OrderType::Limit;
    return OrderCommand::new_order(s, m);
}

std::vector<std::uint8_t> read_file(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

void write_file(const std::string& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

// --- checksum --------------------------------------------------------------

TEST(Crc32, matches_known_vectors) {
    // Pinned against the standard IEEE CRC-32 so a "clever" rewrite of the
    // table cannot quietly change the format of every record ever written.
    const std::string check = "123456789";
    EXPECT_EQ(crc32(reinterpret_cast<const std::uint8_t*>(check.data()), check.size()),
              0xCBF43926u);
    EXPECT_EQ(crc32(nullptr, 0), 0u);
}

TEST(Crc32, leading_zeros_change_the_result) {
    // The pre/post inversion exists for this. Without it a run of zero bytes
    // checksums identically to an empty buffer — exactly what a torn write
    // leaves behind.
    const std::vector<std::uint8_t> none;
    const std::vector<std::uint8_t> zeros(8, 0);
    EXPECT_NE(crc32(zeros.data(), zeros.size()), crc32(none.data(), none.size()));
}

TEST(Crc32, detects_a_single_bit_flip) {
    std::vector<std::uint8_t> data(64);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i);
    const std::uint32_t good = crc32(data.data(), data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] ^= 0x01;
        EXPECT_NE(crc32(data.data(), data.size()), good) << "bit flip at byte " << i << " missed";
        data[i] ^= 0x01;
    }
}

// --- round trip ------------------------------------------------------------

TEST(Wal, appends_and_reads_back_every_field) {
    const auto path = temp_path();
    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0));
        auto c = new_order(7, 42, -1234500, 99);
        c.order_type = protocol::OrderType::Market;
        c.side = protocol::Side::Ask;
        ASSERT_TRUE(w.append(c, 0));
        ASSERT_TRUE(w.append(OrderCommand::cancel(7, 42), 0));
        ASSERT_TRUE(w.append(OrderCommand::cancel_all(7), 0));
    }

    const auto r = read_wal(path);
    ASSERT_TRUE(r.error.empty()) << r.error;
    ASSERT_EQ(r.commands.size(), 3u);
    EXPECT_EQ(r.truncated_bytes, 0u);
    EXPECT_FALSE(r.sequence_gap);
    EXPECT_EQ(r.last_seq, 3u);

    EXPECT_EQ(r.commands[0].type, CommandType::NewOrder);
    EXPECT_EQ(r.commands[0].session, 7u);
    EXPECT_EQ(r.commands[0].client_order_id, 42u);
    EXPECT_EQ(r.commands[0].price_ticks, -1234500) << "negative price did not survive";
    EXPECT_EQ(r.commands[0].quantity, 99u);
    EXPECT_EQ(r.commands[0].side, protocol::Side::Ask);
    EXPECT_EQ(r.commands[0].order_type, protocol::OrderType::Market);
    EXPECT_EQ(r.commands[1].type, CommandType::Cancel);
    EXPECT_EQ(r.commands[2].type, CommandType::CancelAllForSession);
    std::remove(path.c_str());
}

TEST(Wal, sequence_numbers_strictly_increase_from_one) {
    const auto path = temp_path();
    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0));
        for (std::uint64_t i = 1; i <= 50; ++i) {
            ASSERT_TRUE(w.append(new_order(1, i, 1000000, 1), 0));
        }
        EXPECT_EQ(w.last_seq(), 50u);
    }
    const auto r = read_wal(path);
    EXPECT_EQ(r.commands.size(), 50u);
    EXPECT_EQ(r.last_seq, 50u);
    std::remove(path.c_str());
}

TEST(Wal, never_writes_the_in_memory_pointers) {
    // OrderCommand carries raw egress pointers. They are per-process, and a
    // recovered command resurrecting one would be a use-after-free waiting to
    // happen — hence field-by-field encoding rather than a memcpy of a
    // trivially copyable struct.
    const auto path = temp_path();
    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0));
        auto c = new_order(1, 1, 1000000, 5);
        c.egress = reinterpret_cast<EgressQueue*>(0xDEADBEEF);
        c.md_queue = reinterpret_cast<void*>(0xFEEDFACE);
        ASSERT_TRUE(w.append(c, 0));
    }
    const auto r = read_wal(path);
    ASSERT_EQ(r.commands.size(), 1u);
    EXPECT_EQ(r.commands[0].egress, nullptr);
    EXPECT_EQ(r.commands[0].md_queue, nullptr);
    std::remove(path.c_str());
}

TEST(Wal, only_state_mutating_commands_are_loggable) {
    // A SessionOpened or Subscribe describes a connection that will not exist
    // after a restart; replaying one would emit events for a dead session.
    EXPECT_TRUE(is_loggable(CommandType::NewOrder));
    EXPECT_TRUE(is_loggable(CommandType::Cancel));
    EXPECT_TRUE(is_loggable(CommandType::Modify));
    EXPECT_TRUE(is_loggable(CommandType::CancelAllForSession));
    EXPECT_FALSE(is_loggable(CommandType::SessionOpened));
    EXPECT_FALSE(is_loggable(CommandType::Subscribe));
}

// --- corruption ------------------------------------------------------------

TEST(Wal, a_corrupted_payload_byte_stops_the_replay_there) {
    const auto path = temp_path();
    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0));
        for (std::uint64_t i = 1; i <= 5; ++i) {
            ASSERT_TRUE(w.append(new_order(1, i, 1000000, 1), 0));
        }
    }
    auto bytes = read_file(path);
    ASSERT_GT(bytes.size(), 60u);
    // Corrupt a byte inside the THIRD record's payload.
    const std::size_t rec = kWalHeaderSize + 31;  // one whole record
    bytes[2 * rec + kWalHeaderSize + 4] ^= 0xFF;
    write_file(path, bytes);

    const auto r = read_wal(path);
    EXPECT_EQ(r.commands.size(), 2u) << "replayed past a record that failed its checksum";
    EXPECT_EQ(r.last_seq, 2u);
    EXPECT_GT(r.truncated_bytes, 0u);
    std::remove(path.c_str());
}

TEST(Wal, every_truncation_of_the_tail_is_handled_cleanly) {
    // A crash can cut the file anywhere. For EVERY possible truncation length,
    // recovery must return the intact prefix and report the rest as torn —
    // never a parse error, never a partial command, never a crash.
    const auto path = temp_path();
    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0));
        for (std::uint64_t i = 1; i <= 4; ++i) {
            ASSERT_TRUE(w.append(new_order(1, i, 1000000 + static_cast<std::int64_t>(i), 1), 0));
        }
    }
    const auto full = read_file(path);
    const std::size_t rec = kWalHeaderSize + 31;
    ASSERT_EQ(full.size(), 4 * rec);

    for (std::size_t cut = 0; cut <= full.size(); ++cut) {
        write_file(path, std::vector<std::uint8_t>(full.begin(), full.begin() +
                                                   static_cast<std::ptrdiff_t>(cut)));
        const auto r = read_wal(path);
        const std::size_t expect_complete = cut / rec;
        EXPECT_EQ(r.commands.size(), expect_complete) << "wrong command count at cut " << cut;
        EXPECT_EQ(r.last_seq, expect_complete) << "wrong last_seq at cut " << cut;
        EXPECT_EQ(r.truncated_bytes, cut % rec) << "wrong torn byte count at cut " << cut;
        EXPECT_FALSE(r.sequence_gap) << "a torn tail was misreported as a gap at cut " << cut;
        // whatever survived must be intact and in order
        for (std::size_t i = 0; i < r.commands.size(); ++i) {
            EXPECT_EQ(r.commands[i].client_order_id, i + 1) << "at cut " << cut;
        }
    }
    std::remove(path.c_str());
}

TEST(Wal, a_sequence_gap_is_reported_and_is_not_a_torn_tail) {
    // A torn tail means an interrupted write, which is normal after a crash.
    // A GAP means a record that was written is now missing, which implies a
    // lost or truncated file — a different and much worse condition, and one
    // startup should refuse rather than rebuild a book with a hole in it.
    const auto path = temp_path();
    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0));
        for (std::uint64_t i = 1; i <= 4; ++i) {
            ASSERT_TRUE(w.append(new_order(1, i, 1000000, 1), 0));
        }
    }
    auto bytes = read_file(path);
    const std::size_t rec = kWalHeaderSize + 31;
    // Excise the second record entirely, leaving seq 1, 3, 4.
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(rec),
                bytes.begin() + static_cast<std::ptrdiff_t>(2 * rec));
    write_file(path, bytes);

    const auto r = read_wal(path);
    EXPECT_TRUE(r.sequence_gap) << "a missing record was not detected";
    EXPECT_EQ(r.gap_after, 1u);
    EXPECT_EQ(r.commands.size(), 1u) << "replayed across a gap";
    std::remove(path.c_str());
}

TEST(Wal, an_absurd_length_field_is_bounded_not_trusted) {
    // A corrupt length must not become an allocation, and must not be believed.
    const auto path = temp_path();
    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0));
        ASSERT_TRUE(w.append(new_order(1, 1, 1000000, 1), 0));
    }
    auto bytes = read_file(path);
    bytes.resize(kWalHeaderSize + 31);
    // second record with a preposterous length
    for (int i = 0; i < 4; ++i) bytes.push_back(0xFF);
    for (int i = 0; i < 12; ++i) bytes.push_back(0x00);
    write_file(path, bytes);

    const auto r = read_wal(path);
    EXPECT_EQ(r.commands.size(), 1u) << "trusted a corrupt length field";
    EXPECT_EQ(r.truncated_bytes, kWalHeaderSize);
    std::remove(path.c_str());
}

TEST(Wal, an_empty_or_missing_log_is_not_an_error) {
    const auto path = temp_path();
    const auto r = read_wal(path);  // exists, zero length
    EXPECT_TRUE(r.error.empty());
    EXPECT_TRUE(r.commands.empty());
    EXPECT_EQ(r.last_seq, 0u);
    std::remove(path.c_str());

    const auto missing = read_wal("/nonexistent/dir/x.wal");
    EXPECT_FALSE(missing.error.empty()) << "a missing log should say so";
}

// --- group commit ----------------------------------------------------------

TEST(Wal, group_commit_syncs_on_the_record_count) {
    const auto path = temp_path();
    Wal w;
    WalConfig cfg{};
    cfg.sync_every_n = 10;
    cfg.sync_interval_ns = 1000ULL * 1000 * 1000 * 3600;  // effectively never
    ASSERT_TRUE(w.open(path, 0, 0, cfg));

    for (std::uint64_t i = 1; i <= 9; ++i) {
        ASSERT_TRUE(w.append(new_order(1, i, 1000000, 1), 0));
    }
    EXPECT_EQ(w.syncs(), 0u) << "synced before reaching the batch size";
    ASSERT_TRUE(w.append(new_order(1, 10, 1000000, 1), 0));
    EXPECT_EQ(w.syncs(), 1u) << "did not sync at the batch size";
    std::remove(path.c_str());
}

TEST(Wal, group_commit_syncs_on_the_time_interval) {
    const auto path = temp_path();
    Wal w;
    WalConfig cfg{};
    cfg.sync_every_n = 1000000;  // effectively never
    cfg.sync_interval_ns = 10ULL * 1000 * 1000;  // 10ms
    ASSERT_TRUE(w.open(path, 0, 0, cfg));

    ASSERT_TRUE(w.append(new_order(1, 1, 1000000, 1), 0));
    EXPECT_EQ(w.syncs(), 0u);
    // A quiet writer must still sync: without the time bound, a burst that
    // stops short of the batch size would sit unsynced indefinitely.
    w.poll_sync(20ULL * 1000 * 1000);
    EXPECT_EQ(w.syncs(), 1u) << "the interval did not trigger a sync";
    std::remove(path.c_str());
}

TEST(Wal, closing_syncs_so_a_clean_shutdown_has_no_loss_window) {
    const auto path = temp_path();
    {
        Wal w;
        WalConfig cfg{};
        cfg.sync_every_n = 1000000;
        cfg.sync_interval_ns = 1000ULL * 1000 * 1000 * 3600;
        ASSERT_TRUE(w.open(path, 0, 0, cfg));
        ASSERT_TRUE(w.append(new_order(1, 1, 1000000, 1), 0));
        EXPECT_EQ(w.syncs(), 0u);
    }  // destructor -> close() -> sync()
    const auto r = read_wal(path);
    EXPECT_EQ(r.commands.size(), 1u);
    std::remove(path.c_str());
}

TEST(Wal, reopening_continues_the_sequence) {
    // Recovery reopens an existing log; restarting numbering at 1 would create
    // a spurious gap on the next read.
    const auto path = temp_path();
    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0));
        ASSERT_TRUE(w.append(new_order(1, 1, 1000000, 1), 0));
        ASSERT_TRUE(w.append(new_order(1, 2, 1000000, 1), 0));
    }
    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0, /*start_seq=*/2));
        ASSERT_TRUE(w.append(new_order(1, 3, 1000000, 1), 0));
    }
    const auto r = read_wal(path);
    EXPECT_FALSE(r.sequence_gap) << "reopening created a gap";
    EXPECT_EQ(r.commands.size(), 3u);
    EXPECT_EQ(r.last_seq, 3u);
    std::remove(path.c_str());
}

// --- truncation ------------------------------------------------------------

TEST(Wal, truncation_drops_covered_records_and_keeps_the_rest) {
    const auto path = temp_path();
    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0));
        for (std::uint64_t i = 1; i <= 20; ++i) {
            ASSERT_TRUE(w.append(new_order(1, i, 1000000, 1), 0));
        }
        std::string err;
        ASSERT_TRUE(w.truncate_before(12, err)) << err;
        EXPECT_EQ(w.truncated_records(), 12u);
        // appends continue from where the sequence left off
        ASSERT_TRUE(w.append(new_order(1, 21, 1000000, 1), 0));
    }

    const auto r = read_wal(path);
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_FALSE(r.sequence_gap) << "truncation created a gap";
    EXPECT_EQ(r.commands.size(), 9u) << "kept the wrong number of records";
    EXPECT_EQ(r.last_seq, 21u);
    std::remove(path.c_str());
}

TEST(Wal, truncation_preserves_sequence_numbers) {
    // REGRESSION. Truncation keeps the original numbering rather than
    // renumbering from 1, because recovery compares those numbers against a
    // snapshot's last_seq to decide what it has already covered.
    //
    // Both the gateway and the independent verifier used to derive the sequence
    // by counting records from 1. After a truncation the first surviving record
    // is not number 1, so every remaining command looked like one the snapshot
    // already contained and the entire tail was skipped — silently, producing a
    // stale book with no error anywhere.
    const auto path = temp_path();
    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0));
        for (std::uint64_t i = 1; i <= 30; ++i) {
            ASSERT_TRUE(w.append(new_order(1, i, 1000000, 1), 0));
        }
        std::string err;
        ASSERT_TRUE(w.truncate_before(25, err)) << err;
    }

    const auto r = read_wal(path);
    ASSERT_EQ(r.commands.size(), 5u);
    EXPECT_EQ(r.first_seq, 26u) << "first_seq must be the surviving record's own number";
    EXPECT_EQ(r.last_seq, 30u);
    // and the payloads are the ones we expect, not renumbered stand-ins
    EXPECT_EQ(r.commands.front().client_order_id, 26u);
    EXPECT_EQ(r.commands.back().client_order_id, 30u);
    std::remove(path.c_str());
}

TEST(Wal, first_seq_is_one_for_an_untruncated_log) {
    const auto path = temp_path();
    {
        Wal w;
        ASSERT_TRUE(w.open(path, 0));
        for (std::uint64_t i = 1; i <= 4; ++i) {
            ASSERT_TRUE(w.append(new_order(1, i, 1000000, 1), 0));
        }
    }
    const auto r = read_wal(path);
    EXPECT_EQ(r.first_seq, 1u);
    EXPECT_EQ(r.last_seq, 4u);
    std::remove(path.c_str());
}

TEST(Wal, truncating_everything_leaves_an_empty_but_usable_log) {
    const auto path = temp_path();
    Wal w;
    ASSERT_TRUE(w.open(path, 0));
    for (std::uint64_t i = 1; i <= 5; ++i) {
        ASSERT_TRUE(w.append(new_order(1, i, 1000000, 1), 0));
    }
    std::string err;
    ASSERT_TRUE(w.truncate_before(5, err)) << err;
    ASSERT_TRUE(w.append(new_order(1, 6, 1000000, 1), 0));

    const auto r = read_wal(path);
    ASSERT_EQ(r.commands.size(), 1u);
    EXPECT_EQ(r.first_seq, 6u);
    std::remove(path.c_str());
}

TEST(Wal, repeated_truncation_does_not_renumber_records) {
    // REGRESSION, found by the kill harness rather than by a unit test.
    //
    // truncate_before() derived the sequence by counting from 1. That is right
    // for a log that still starts at 1, and wrong for one already truncated:
    // the second truncation renumbered every surviving record, producing a real
    // sequence gap and a gateway that refused to start.
    //
    // A single truncation cannot catch this. It takes two.
    const auto path = temp_path();
    Wal w;
    ASSERT_TRUE(w.open(path, 0));
    for (std::uint64_t i = 1; i <= 40; ++i) {
        ASSERT_TRUE(w.append(new_order(1, i, 1000000, 1), 0));
    }

    std::string err;
    ASSERT_TRUE(w.truncate_before(10, err)) << err;
    {
        const auto r = read_wal(path);
        ASSERT_FALSE(r.sequence_gap) << "gap after the first truncation";
        EXPECT_EQ(r.first_seq, 11u);
        EXPECT_EQ(r.last_seq, 40u);
    }

    ASSERT_TRUE(w.truncate_before(25, err)) << err;
    {
        const auto r = read_wal(path);
        ASSERT_FALSE(r.sequence_gap) << "gap after the SECOND truncation";
        EXPECT_EQ(r.first_seq, 26u) << "records were renumbered";
        EXPECT_EQ(r.last_seq, 40u);
        EXPECT_EQ(r.commands.size(), 15u);
        EXPECT_EQ(r.commands.front().client_order_id, 26u);
    }

    // and appends still continue from the right place
    ASSERT_TRUE(w.append(new_order(1, 41, 1000000, 1), 0));
    const auto r = read_wal(path);
    EXPECT_FALSE(r.sequence_gap);
    EXPECT_EQ(r.last_seq, 41u);
    std::remove(path.c_str());
}
