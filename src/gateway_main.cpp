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

#include "ome/matching_thread.hpp"
#include "ome/risk_config.hpp"
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
    if (!wal_path.empty()) {
        const auto now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (!wal.open(wal_path, now, 0, wal_cfg)) {
            std::cerr << wal.error() << "\n";
            return 1;
        }
        std::cout << "write-ahead log: " << wal_path << "\n";
    }
    ome::MatchingThread matcher(inbound, waiter, cfg.risk, &egress_ready,
                                wal.is_open() ? &wal : nullptr);

    ome::TcpServer server(cfg, &inbound, &waiter, &egress_ready);
    if (!server.start()) {
        std::cerr << "failed to start: " << server.last_error() << "\n";
        return 1;
    }

    g_server = &server;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

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
