// CLI: replay LOBSTER message csv and compare book to a row from LOBSTER orderbook file.
// Example:
//   ./build/lobster_replay --messages data/lobster/AMZN_..._message_10.csv \
//     --orderbook data/lobster/AMZN_..._orderbook_10.csv --events 10000

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "lobster/lobster_orderbook_converter.hpp"
#include "lobster/lobster_validator.hpp"

namespace {

void usage() {
    std::cerr
        << "usage: lobster_replay --messages <message.csv> --orderbook <orderbook.csv> --events N \\\n"
        << "          [--orderbook-line L] [--seed-orderbook-zero]\n"
        << "  replays the first N events from the message file, builds a reference snapshot from\n"
        << "  orderbook line L (0-based over non-empty non-comment lines). default L = N-1.\n"
        << "  --seed-orderbook-zero: load LOBSTER orderbook row 0 then apply messages 1..N-1 (LOBSTER row sync).\n";
}

bool has_flag(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == key) {
            return true;
        }
    }
    return false;
}

bool get_str(int argc, char** argv, const char* key, std::string& out) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == key) {
            out = argv[i + 1];
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--help") {
        usage();
        return 0;
    }

    std::string msg_path;
    std::string book_path;
    std::string events_str;
    if (!get_str(argc, argv, "--messages", msg_path) || !get_str(argc, argv, "--orderbook", book_path) ||
        !get_str(argc, argv, "--events", events_str)) {
        usage();
        return 1;
    }

    std::size_t n_events = 0;
    try {
        n_events = static_cast<std::size_t>(std::stoull(events_str));
    } catch (const std::exception&) {
        std::cerr << "bad --events\n";
        return 1;
    }
    if (n_events == 0) {
        std::cerr << "--events must be > 0\n";
        return 1;
    }

    std::size_t orderbook_line = n_events - 1;
    std::string line_override;
    if (get_str(argc, argv, "--orderbook-line", line_override)) {
        try {
            orderbook_line = static_cast<std::size_t>(std::stoull(line_override));
        } catch (const std::exception&) {
            std::cerr << "bad --orderbook-line\n";
            return 1;
        }
    }

    std::string order_row;
    if (!read_orderbook_line_at_index(book_path, orderbook_line, order_row)) {
        std::cerr << "failed to read orderbook line " << orderbook_line << " from " << book_path << '\n';
        return 1;
    }

    std::ostringstream snap;
    if (!write_validator_snapshot_from_lobster_orderbook_row(order_row, snap)) {
        std::cerr << "failed to convert orderbook row to snapshot (need 4*10 columns for level-10 book)\n";
        return 1;
    }

    std::string order_row0;
    std::ostringstream snap0;
    std::istringstream snapshot0_in;
    std::istream* seed_in = nullptr;
    if (has_flag(argc, argv, "--seed-orderbook-zero") &&
        read_orderbook_line_at_index(book_path, 0, order_row0) &&
        write_validator_snapshot_from_lobster_orderbook_row(order_row0, snap0)) {
        snapshot0_in.str(snap0.str());
        snapshot0_in.clear();
        seed_in = &snapshot0_in;
    }

    std::ifstream msg_in(msg_path);
    if (!msg_in) {
        std::cerr << "cannot open messages: " << msg_path << '\n';
        return 1;
    }

    std::istringstream snapshot_in(snap.str());
    const auto result = LobsterValidator::validate(msg_in, snapshot_in, n_events, seed_in);

    std::cout << result.summary() << '\n';
    if (!result.mismatch_log.empty()) {
        std::cout << "--- mismatch log ---\n" << result.mismatch_log;
    }
    return result.first_mismatch_level_index.has_value() ? 1 : 0;
}
