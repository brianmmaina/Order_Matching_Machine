// Order gateway entry point.
//
// Session 1.2: network layer only. A valid NewOrder gets a hardcoded Ack.
// The matching engine is wired in session 1.4.

#include <csignal>
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
            cfg.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
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
