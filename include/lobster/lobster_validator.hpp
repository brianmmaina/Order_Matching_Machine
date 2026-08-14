#pragma once

#include <cstddef>
#include <istream>
#include <optional>
#include <ostream>
#include <string>

// replays lobster messages through MatchingEngine, then compares aggregated top-10 bid/ask
// price levels to a reference snapshot csv (20 rows: 10 bids best-first, 10 asks best-first).
class LobsterValidator {
public:
    struct Result {
        std::size_t total_events{};
        // matched_levels / total_levels * 100, total_levels = 20 (10 bid + 10 ask slots).
        double accuracy_percent{};
        // combined index: 0-9 = bid ranks (best first), 10-19 = ask ranks (best first).
        std::optional<std::size_t> first_mismatch_level_index;
        // human-readable diff lines; also notes if snapshot/messages were truncated or io failed.
        std::string mismatch_log;
        // single-line summary: events, accuracy, first mismatch level (if any).
        [[nodiscard]] std::string summary() const;
    };

    // reads up to n_events rows from messages (same csv as LobsterParser). snapshot: 20 lines of
    // price_ticks,size (comments with # skipped). empty/malformed snapshot rows treated as 0 size.
    // If snapshot_initial is set, it must match LOBSTER orderbook row 0 (state after message 0);
    // replay applies messages 1..n_events-1 on that seed (LOBSTER row indexing).
    // If jsonl_out is set, one book snapshot record (see include/ome/book_jsonl.hpp) is written
    // per applied message. Emitting from inside the replay loop rather than re-running it keeps
    // the visualized book and the validated book the same book.
    [[nodiscard]] static Result validate(std::istream& messages, std::istream& snapshot, std::size_t n_events,
                                        std::istream* snapshot_initial = nullptr,
                                        std::ostream* jsonl_out = nullptr);
};
