// Order gateway entry point.
//
// Session 1.2: network layer only. A valid NewOrder gets a hardcoded Ack.
// The matching engine is wired in session 1.4.

#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>

#include "ome/tcp_server.hpp"

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
              << "\n"
              << "Session 1.2: responds to a valid NewOrder with a hardcoded Ack.\n"
              << "Protocol: docs/PROTOCOL.md. Smoke test: tools/smoke_client.py\n";
}

}  // namespace

int main(int argc, char** argv) {
    ome::TcpServerConfig cfg{};

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            usage();
            return 0;
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

    ome::TcpServer server(cfg);
    if (!server.start()) {
        std::cerr << "failed to start: " << server.last_error() << "\n";
        return 1;
    }

    g_server = &server;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::cout << "gateway listening on 127.0.0.1:" << server.bound_port() << "\n";
    std::cout.flush();

    server.run();

    if (!server.last_error().empty()) {
        std::cerr << "event loop error: " << server.last_error() << "\n";
        return 1;
    }
    std::cout << "shutdown clean\n";
    return 0;
}
