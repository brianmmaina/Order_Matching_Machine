// Order gateway entry point.
//
// Session 1.2: network layer only. A valid NewOrder gets a hardcoded Ack.
// The matching engine is wired in session 1.4.

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "ome/matching_thread.hpp"
#include "ome/risk_config.hpp"
#include "ome/snapshot.hpp"
#include "ome/wal.hpp"
#include "ome/tcp_server.hpp"
#include "ome/waiter.hpp"

namespace {

ome::TcpServer* g_server = nullptr;

// Signal handlers may only touch async-signal-safe state. stop() sets a flag
// and nothing else, which is why the shutdown path is a flag rather than any
// direct teardown here.
extern "C" void on_signal(int) {
    if (g_server != nullptr) {
        g_server->stop();
    }
}

void usage() {
    std::cerr << "usage: gateway [--port N]\n"
              << "  --port N   listen port (default 9001; 0 = pick any free port)\n"
              << "  --risk F   risk limits from a key=value file (see config/risk.conf)\n"
              << "  --wal F    append commands to a write-ahead log at F\n"
              << "  --wal-no-fullsync  weaker durability, much lower tail latency\n"
              << "  --recover  replay the log at startup before accepting clients\n"
              << "  --snapshot F        periodic book snapshots at F (bounds recovery time)\n"
              << "  --snapshot-every N  commands between snapshots (default 100000)\n"
              << "\n"
              << "Runs a network thread and a single matching thread joined by a\n"
              << "lock-free queue. The book is touched only by the matching thread.\n"
              << "Protocol: docs/PROTOCOL.md. Smoke test: tools/smoke_client.py\n";
}

}  // namespace

int main(int argc, char** argv) {
    ome::TcpServerConfig cfg{};
    std::string wal_path;
    ome::WalConfig wal_cfg{};
    bool recover = false;
    std::string snap_path;
    std::uint64_t snap_every = 100000;
    std::vector<ome::OrderCommand> pending_recovery;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            usage();
            return 0;
        }
        if (arg == "--wal" && i + 1 < argc) {
            wal_path = argv[++i];
            continue;
        }
        if (arg == "--snapshot" && i + 1 < argc) {
            snap_path = argv[++i];
            continue;
        }
        if (arg == "--snapshot-every" && i + 1 < argc) {
            snap_every = std::strtoull(argv[++i], nullptr, 10);
            continue;
        }
        if (arg == "--recover") {
            recover = true;
            continue;
        }
        if (arg == "--wal-no-fullsync") {
            wal_cfg.full_sync = false;
            continue;
        }
        if (arg == "--risk" && i + 1 < argc) {
            std::string err;
            if (!ome::RiskConfig::load(argv[++i], cfg.risk, err)) {
                std::cerr << "risk config: " << err << "\n";
                return 1;
            }
            continue;
        }
        if (arg == "--port" && i + 1 < argc) {
            // strtol with full validation, not atoi: atoi cannot distinguish
            // "0" from unparseable input, so `--port abc` would silently bind
            // an ephemeral port, and an unchecked narrowing would turn
            // `--port 70000` into 4464. Both fail in the confusing direction —
            // the server starts, on the wrong port.
            const std::string raw = argv[++i];
            char* end = nullptr;
            errno = 0;
            const long v = std::strtol(raw.c_str(), &end, 10);
            if (raw.empty() || end == raw.c_str() || *end != '\0' || errno == ERANGE ||
                v < 0 || v > 65535) {
                std::cerr << "invalid --port: " << raw << " (expected 0-65535)\n";
                return 1;
            }
            cfg.port = static_cast<std::uint16_t>(v);
            continue;
        }
        std::cerr << "unknown argument: " << arg << "\n";
        usage();
        return 1;
    }

    ome::InboundQueue inbound;
    ome::Waiter waiter;
    if (!waiter.valid()) {
        std::cerr << "failed to create wake-up pipe\n";
        return 1;
    }
    ome::Notifier egress_ready;
    if (!egress_ready.valid()) {
        std::cerr << "failed to create egress wake-up pipe\n";
        return 1;
    }
    ome::Wal wal;
    std::uint64_t resume_seq = 0;
    ome::SnapshotData snap_data{};
    bool have_snapshot = false;

    // Newest snapshot first, then the one kept behind it. A snapshot that fails
    // its checksum is not fatal — falling back to full replay is slower but
    // correct, and refusing to start would turn a recoverable situation into an
    // outage.
    if (!snap_path.empty() && recover) {
        std::string err;
        if (ome::read_snapshot(snap_path, snap_data, err)) {
            have_snapshot = true;
        } else {
            std::cout << "snapshot unusable (" << err << "), trying previous\n";
            if (ome::read_snapshot(snap_path + ".prev", snap_data, err)) {
                have_snapshot = true;
                std::cout << "using the previous snapshot\n";
            } else {
                std::cout << "no usable snapshot, replaying the whole log\n";
            }
        }
    }

    if (!wal_path.empty() && recover) {
        const auto rd = ome::read_wal(wal_path);
        if (!rd.error.empty()) {
            // A missing log on first start is fine; anything else is not.
            std::cout << "no log to recover from (" << rd.error << ")\n";
        } else {
            if (rd.sequence_gap) {
                // A torn tail means an interrupted write. A GAP means a record
                // that was written is missing, so the book we would rebuild is
                // not the book that existed. Refuse rather than serve it.
                std::cerr << "refusing to start: sequence gap after seq " << rd.gap_after
                          << " in " << wal_path << "\n";
                return 1;
            }
            // Skip what the snapshot already covers. Sequence numbers are
            // preserved across truncation precisely so this comparison works.
            std::uint64_t seq = rd.first_seq;
            for (const auto& c : rd.commands) {
                if (!have_snapshot || seq > snap_data.last_seq) {
                    pending_recovery.push_back(c);
                }
                ++seq;
            }
            resume_seq = rd.last_seq > snap_data.last_seq ? rd.last_seq : snap_data.last_seq;
            const std::size_t n = pending_recovery.size();
            std::cout << "recovered " << n << " commands, last seq " << rd.last_seq;
            if (rd.truncated_bytes > 0) {
                std::cout << ", discarded " << rd.truncated_bytes << " bytes of torn tail";
            }
            std::cout << "\n";
        }
    }
    if (!wal_path.empty()) {
        const auto now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (!wal.open(wal_path, now, resume_seq, wal_cfg)) {
            std::cerr << wal.error() << "\n";
            return 1;
        }
        std::cout << "write-ahead log: " << wal_path << "\n";
    }
    ome::SnapshotPolicy snap_policy{};
    snap_policy.path = snap_path;
    snap_policy.every_n = snap_every;
    ome::MatchingThread matcher(inbound, waiter, cfg.risk, &egress_ready,
                                wal.is_open() ? &wal : nullptr, snap_policy);

    ome::TcpServer server(cfg, &inbound, &waiter, &egress_ready);
    if (!server.start()) {
        std::cerr << "failed to start: " << server.last_error() << "\n";
        return 1;
    }

    g_server = &server;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (have_snapshot) {
        matcher.restore(snap_data);
        matcher.set_snapshot_baseline(snap_data.last_seq);
        std::cout << "restored " << snap_data.orders.size() << " orders from snapshot at seq "
                  << snap_data.last_seq << "\n";
    }
    if (!pending_recovery.empty()) {
        // Before start(): recovery runs on this thread, so nothing races the
        // book while it is being rebuilt.
        matcher.recover(pending_recovery);
    }
    if (recover) {
        // Printed whenever recovery ran, including when the snapshot covered
        // everything and the tail was empty.
        std::cout << "book digest after recovery: " << matcher.digest() << "\n";
    }
    matcher.start();

    std::cout << "gateway listening on 127.0.0.1:" << server.bound_port() << "\n";
    std::cout.flush();

    server.run();

    // Network thread first, then the matching thread: stopping the producer
    // before the consumer means nothing is enqueued that will never be applied.
    matcher.stop();

    if (!server.last_error().empty()) {
        std::cerr << "event loop error: " << server.last_error() << "\n";
        return 1;
    }
    std::cout << "shutdown clean\n";
    return 0;
}
