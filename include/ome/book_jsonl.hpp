#pragma once

// ---------------------------------------------------------------------------
// Book snapshot JSONL record — the single wire format between anything that
// produces book state and tools/book_replay.html.
//
//   {"t":<u64 ns>,"seq":<u64>,"bids":[[<i64 ticks>,<u64 qty>],...],
//                             "asks":[[<i64 ticks>,<u64 qty>],...]}
//
// One object per line, no trailing comma, no newlines inside a record.
//   t     monotonic-ish event time in NANOSECONDS. For file replay this is the
//         source message timestamp; for the live gateway it is the clock at
//         publish. Consumers use it for pacing only, never for ordering.
//   seq   strictly increasing record counter. THIS is the ordering key.
//   bids  descending by price. asks ascending by price. Both capped at 10.
//
// Prices are integer ticks, never decimals. That is the whole reason this
// format exists in ticks: a visualizer that renders floats will eventually
// render 100.30000000000001, and a format that carries floats invites a
// producer that computes them.
//
// This header is deliberately dependency-free and stream-based so the file
// replay path and the live gateway path emit byte-identical records. If they
// ever diverge, the visualizer silently becomes two visualizers.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <ostream>
#include <utility>
#include <vector>

namespace ome {

using BookLevels = std::vector<std::pair<std::int64_t, std::uint64_t>>;

namespace detail {

inline void write_levels(std::ostream& out, const BookLevels& levels) {
    out << '[';
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << '[' << levels[i].first << ',' << levels[i].second << ']';
    }
    out << ']';
}

}  // namespace detail

// Writes exactly one record plus a newline. No flush — the caller decides,
// because flushing per record dominates the cost of a 270K-message replay.
inline void write_book_record(std::ostream& out, std::uint64_t t_ns, std::uint64_t seq,
                              const BookLevels& bids, const BookLevels& asks) {
    out << R"({"t":)" << t_ns << R"(,"seq":)" << seq << R"(,"bids":)";
    detail::write_levels(out, bids);
    out << R"(,"asks":)";
    detail::write_levels(out, asks);
    out << "}\n";
}

}  // namespace ome
