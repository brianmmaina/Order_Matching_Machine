#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>

// one LOBSTER orderbook row has 4*levels comma-separated fields per spec:
// AskPrice1, AskSize1, BidPrice1, BidSize1, AskPrice2, ...
// prices are integer ticks (same scale as message file). dummy / empty levels may use size 0 or non-positive prices.
// writes our LobsterValidator snapshot format: 10 lines bid (best first) then 10 lines ask (best first), each "price_ticks,size".
[[nodiscard]] bool write_validator_snapshot_from_lobster_orderbook_row(const std::string& csv_line,
                                                                       std::ostream& out, std::size_t levels = 10);

// reads orderbook file, returns the line at 0-based index (first data line is 0). skips empty lines and # comments.
[[nodiscard]] bool read_orderbook_line_at_index(const std::string& path, std::size_t line_index, std::string& out_line);
