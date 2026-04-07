#include "lobster/lobster_validator.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

#include "lobster/lobster_message.hpp"
#include "lobster/lobster_parser.hpp"
#include "matching_engine/matching_engine.hpp"
#include "order.h"

namespace {

constexpr double kTickScale = 10000.0;

struct SnapshotLevel {
    int64_t price_ticks{};
    std::uint64_t size{};
};

bool parse_snapshot_line(const std::string& line, SnapshotLevel& out) {
    if (line.empty()) {
        return false;
    }
    const auto hash = line.find('#');
    const std::string body = (hash == std::string::npos) ? line : line.substr(0, hash);
    std::string s = body;
    // trim minimal
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    if (s.empty()) {
        return false;
    }
    const auto comma = s.find(',');
    if (comma == std::string::npos) {
        return false;
    }
    std::string ps = s.substr(0, comma);
    std::string sz = s.substr(comma + 1);
    try {
        out.price_ticks = std::stoll(ps);
        out.size = std::stoull(sz);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool load_snapshot(std::istream& in, std::array<SnapshotLevel, 10>& bids,
                   std::array<SnapshotLevel, 10>& asks, std::ostringstream& log) {
    std::string line;
    std::size_t filled = 0;
    while (filled < 20 && std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        if (!line.empty() && line[0] == '#') {
            continue;
        }
        SnapshotLevel lvl{};
        if (!parse_snapshot_line(line, lvl)) {
            log << "snapshot: skipped malformed line\n";
            continue;
        }
        if (filled < 10) {
            bids[filled] = lvl;
        } else {
            asks[filled - 10] = lvl;
        }
        ++filled;
    }
    if (filled < 20) {
        log << "snapshot: fewer than 20 data rows; padded remaining with zeros\n";
    }
    while (filled < 20) {
        if (filled < 10) {
            bids[filled] = {};
        } else {
            asks[filled - 10] = {};
        }
        ++filled;
    }
    return true;
}

void apply_message(MatchingEngine& eng, const LobsterMessage& m) {
    switch (m.type) {
        case 1: {
            Order o{};
            o.id = m.order_id;
            o.price = static_cast<double>(m.price_ticks) / kTickScale;
            o.quantity = m.size;
            o.side = (m.direction == 1) ? Order::BID : Order::ASK;
            o.type = Order::LIMIT;
            o.timestamp = m.timestamp_us;
            eng.processOrder(std::move(o));
            break;
        }
        case 2:
            // LOBSTER: size = new remaining quantity on the order.
            static_cast<void>(eng.book().replace_remaining_quantity(m.order_id, m.size));
            break;
        case 3: {
            if (eng.book().cancelOrder(m.order_id)) {
                break;
            }
            // LOBSTER row may reference liquidity from before the file; drop size at price on that side.
            const Order::Side side = (m.direction == 1) ? Order::BID : Order::ASK;
            const double px = static_cast<double>(m.price_ticks) / kTickScale;
            static_cast<void>(eng.book().reduce_level_after_lobster_execution(side, px, m.size, true));
            break;
        }
        case 4:
        case 5: {
            // LOBSTER: direction is the resting limit's side (1 = buy/bid, -1 = sell/ask).
            const Order::Side passive = (m.direction == 1) ? Order::BID : Order::ASK;
            const double px = static_cast<double>(m.price_ticks) / kTickScale;
            static_cast<void>(eng.book().reduce_level_after_lobster_execution(passive, px, m.size, true));
            break;
        }
        case 7:
            // trading halt: message row exists but book state is duplicated — no book change here.
            break;
        default:
            break;
    }
}

bool level_equal(bool side_is_bid, std::size_t rank, const SnapshotLevel& exp,
                 const std::vector<std::pair<std::int64_t, std::uint64_t>>& our_levels,
                 std::ostringstream& verbose) {
    const bool have = rank < our_levels.size();
    if (exp.size == 0) {
        if (!have) {
            return true;
        }
        verbose << (side_is_bid ? "bid" : "ask") << " rank " << rank << ": expected empty level, got tick "
                << our_levels[rank].first << " size " << our_levels[rank].second << '\n';
        return false;
    }
    if (!have) {
        verbose << (side_is_bid ? "bid" : "ask") << " rank " << rank << ": missing level, expected tick "
                << exp.price_ticks << " size " << exp.size << '\n';
        return false;
    }
    if (our_levels[rank].first != exp.price_ticks || our_levels[rank].second != exp.size) {
        verbose << (side_is_bid ? "bid" : "ask") << " rank " << rank << ": expected tick " << exp.price_ticks
                << " size " << exp.size << " got tick " << our_levels[rank].first << " size "
                << our_levels[rank].second << '\n';
        return false;
    }
    return true;
}

bool seed_book_from_snapshot(std::istream& in, MatchingEngine& eng, std::ostringstream& log) {
    std::array<SnapshotLevel, 10> snap_bids{};
    std::array<SnapshotLevel, 10> snap_asks{};
    if (!load_snapshot(in, snap_bids, snap_asks, log)) {
        return false;
    }
    uint64_t synth = std::numeric_limits<uint64_t>::max();
    for (std::size_t i = 0; i < 10; ++i) {
        if (snap_bids[i].size == 0 || snap_bids[i].price_ticks <= 0) {
            continue;
        }
        const std::uint64_t sz64 = snap_bids[i].size;
        if (sz64 > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            log << "seed: bid level " << i << " size overflow\n";
            return false;
        }
        Order o{};
        o.id = synth--;
        o.price = static_cast<double>(snap_bids[i].price_ticks) / kTickScale;
        o.quantity = static_cast<std::uint32_t>(sz64);
        o.side = Order::BID;
        o.type = Order::LIMIT;
        o.timestamp = 0;
        eng.book().addOrder(std::move(o));
    }
    for (std::size_t i = 0; i < 10; ++i) {
        if (snap_asks[i].size == 0 || snap_asks[i].price_ticks <= 0) {
            continue;
        }
        const std::uint64_t sz64 = snap_asks[i].size;
        if (sz64 > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            log << "seed: ask level " << i << " size overflow\n";
            return false;
        }
        Order o{};
        o.id = synth--;
        o.price = static_cast<double>(snap_asks[i].price_ticks) / kTickScale;
        o.quantity = static_cast<std::uint32_t>(sz64);
        o.side = Order::ASK;
        o.type = Order::LIMIT;
        o.timestamp = 0;
        eng.book().addOrder(std::move(o));
    }
    return true;
}

}  // namespace

std::string LobsterValidator::Result::summary() const {
    std::ostringstream os;
    os << "total_events=" << total_events << " accuracy_percent=" << accuracy_percent;
    if (first_mismatch_level_index.has_value()) {
        os << " first_mismatch_level_index=" << *first_mismatch_level_index;
    } else {
        os << " first_mismatch_level_index=none";
    }
    return os.str();
}

LobsterValidator::Result LobsterValidator::validate(std::istream& messages, std::istream& snapshot,
                                                    std::size_t n_events, std::istream* snapshot_initial) {
    Result r{};
    std::ostringstream log_stream;

    std::vector<LobsterMessage> all = LobsterParser::parse_messages(messages);
    const std::size_t use_n = std::min(n_events, all.size());
    r.total_events = use_n;

    if (use_n < n_events) {
        log_stream << "messages: requested " << n_events << " events but only " << all.size()
                   << " valid rows\n";
    }

    std::array<SnapshotLevel, 10> snap_bids{};
    std::array<SnapshotLevel, 10> snap_asks{};
    if (!load_snapshot(snapshot, snap_bids, snap_asks, log_stream)) {
        r.accuracy_percent = 0.0;
        r.mismatch_log = log_stream.str();
        return r;
    }

    MatchingEngine engine{};
    std::size_t first_msg = 0;
    if (snapshot_initial != nullptr) {
        if (!seed_book_from_snapshot(*snapshot_initial, engine, log_stream)) {
            r.accuracy_percent = 0.0;
            r.mismatch_log = log_stream.str();
            return r;
        }
        first_msg = 1;
    }
    for (std::size_t i = first_msg; i < use_n; ++i) {
        apply_message(engine, all[i]);
    }

    const auto our_bids = engine.book().bid_levels_ticks(10);
    const auto our_asks = engine.book().ask_levels_ticks(10);

    std::size_t matched = 0;
    constexpr std::size_t kTotalLevels = 20;
    std::optional<std::size_t> first_bad;
    std::ostringstream detail;

    for (std::size_t i = 0; i < 10; ++i) {
        if (level_equal(true, i, snap_bids[i], our_bids, detail)) {
            ++matched;
        } else if (!first_bad.has_value()) {
            first_bad = i;
        }
    }
    for (std::size_t i = 0; i < 10; ++i) {
        if (level_equal(false, i, snap_asks[i], our_asks, detail)) {
            ++matched;
        } else if (!first_bad.has_value()) {
            first_bad = i + 10;
        }
    }

    r.first_mismatch_level_index = first_bad;
    r.accuracy_percent = (static_cast<double>(matched) * 100.0) / static_cast<double>(kTotalLevels);

    if (first_bad.has_value()) {
        log_stream << "first mismatch at combined level index " << *first_bad
                   << " (0-9 bids best-first, 10-19 asks best-first); divergence observed after replaying "
                   << use_n << " events\n";
        log_stream << "note: locating the exact culprit event needs per-event golden snapshots or a diff trace;\n";
        log_stream << "this tool compares end state after n events to the provided snapshot only.\n";
    }

    r.mismatch_log = log_stream.str() + detail.str();
    return r;
}
