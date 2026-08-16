// Independent WAL verifier.
//
// Reads a write-ahead log (and optionally a snapshot), rebuilds the book with
// its OWN implementation, and prints the digest.
//
// WHY THIS EXISTS, AND WHY IT DUPLICATES THE ENGINE
//
// The recovery tests in tests/test_recovery.cpp compare a book the engine built
// against a book the engine rebuilt. That catches a broken replay, but it
// cannot catch a bug in the engine itself: if OrderBook mismatches on some edge
// case, it mismatches identically both times and the test is happy. The check
// is the engine agreeing with itself.
//
// So this file deliberately reimplements matching from scratch — std::map of
// price levels, straightforward crossing loop, no shared code with OrderBook at
// all. It shares only the WAL record format and the command decoder, because
// those are the input format rather than the logic under test.
//
// Two independent implementations agreeing on the digest is real evidence. Two
// copies of the same implementation agreeing is not.
//
// It is written for clarity over speed: std::map and std::deque, no cached
// aggregates, no tricks. If it is slower than the engine, that is fine and
// slightly reassuring — it means it is not accidentally the same code.
//
// The digest MUST match OrderBook::digest() exactly: FNV-1a over
// (price_ticks, total_qty, order_count) per level per side, bids best-first
// (descending) then asks best-first (ascending), with a side tag and level
// count mixed in.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "ome/commands.hpp"
#include "ome/risk_config.hpp"
#include "ome/snapshot.hpp"
#include "ome/wal.hpp"

namespace {

struct VOrder {
    std::uint64_t id{};
    std::uint64_t session{};
    std::uint64_t client_order_id{};
    std::uint32_t quantity{};
};

// A book built from scratch. std::map keeps levels sorted; the comparator gives
// each side "best first" iteration order directly.
class SimpleBook {
public:
    // bids: highest price first. asks: lowest price first.
    std::map<std::int64_t, std::deque<VOrder>, std::greater<std::int64_t>> bids;
    std::map<std::int64_t, std::deque<VOrder>> asks;

    void add(std::int64_t px, bool is_bid, const VOrder& o) {
        if (is_bid) {
            bids[px].push_back(o);
        } else {
            asks[px].push_back(o);
        }
        loc_[o.id] = {px, is_bid};
    }

    bool cancel(std::uint64_t id) {
        const auto it = loc_.find(id);
        if (it == loc_.end()) {
            return false;
        }
        const auto [px, is_bid] = it->second;
        bool removed = false;
        if (is_bid) {
            removed = erase_from(bids, px, id);
        } else {
            removed = erase_from(asks, px, id);
        }
        if (removed) {
            loc_.erase(it);
        }
        return removed;
    }

    // The engine rests an aggressive limit and THEN crosses, so this must too —
    // otherwise a self-crossing order behaves differently and the books diverge.
    void cross() {
        while (!bids.empty() && !asks.empty()) {
            auto bit = bids.begin();
            auto ait = asks.begin();
            if (bit->first < ait->first) {
                break;
            }
            VOrder& b = bit->second.front();
            VOrder& a = ait->second.front();
            const std::uint32_t qty = std::min(b.quantity, a.quantity);
            b.quantity -= qty;
            a.quantity -= qty;
            last_trade_ = ait->first;  // trade prints at the resting ask
            if (b.quantity == 0) {
                loc_.erase(b.id);
                bit->second.pop_front();
                if (bit->second.empty()) bids.erase(bit);
            }
            if (asks.empty()) break;
            ait = asks.begin();
            if (a.quantity == 0) {
                loc_.erase(a.id);
                ait->second.pop_front();
                if (ait->second.empty()) asks.erase(ait);
            }
        }
    }

    // Side of a resting order. Modify is cancel+replace and cannot change
    // side, so the replacement inherits it.
    [[nodiscard]] bool is_bid(std::uint64_t id, bool& out) const {
        const auto it = loc_.find(id);
        if (it == loc_.end()) return false;
        out = it->second.second;
        return true;
    }

    [[nodiscard]] std::uint64_t owner_of(std::uint64_t id) const {
        const auto it = owners_.find(id);
        return it == owners_.end() ? 0 : it->second;
    }
    void set_owner(std::uint64_t id, std::uint64_t session) { owners_[id] = session; }

    [[nodiscard]] bool best_bid(std::int64_t& out) const {
        if (bids.empty()) return false;
        out = bids.begin()->first;
        return true;
    }
    [[nodiscard]] bool best_ask(std::int64_t& out) const {
        if (asks.empty()) return false;
        out = asks.begin()->first;
        return true;
    }
    [[nodiscard]] std::int64_t last_trade() const { return last_trade_; }
    void set_last_trade(std::int64_t v) { last_trade_ = v; }

    [[nodiscard]] std::uint64_t find_by_client(std::uint64_t session, std::uint64_t coid) const {
        for (const auto& [id, s] : owners_) {
            if (s != session) continue;
            const auto c = client_ids_.find(id);
            if (c != client_ids_.end() && c->second == coid && loc_.count(id)) {
                return id;
            }
        }
        return 0;
    }
    void set_client_id(std::uint64_t id, std::uint64_t coid) { client_ids_[id] = coid; }

private:
    template <typename Map>
    bool erase_from(Map& m, std::int64_t px, std::uint64_t id) {
        const auto lvl = m.find(px);
        if (lvl == m.end()) return false;
        for (auto it = lvl->second.begin(); it != lvl->second.end(); ++it) {
            if (it->id == id) {
                lvl->second.erase(it);
                if (lvl->second.empty()) m.erase(lvl);
                return true;
            }
        }
        return false;
    }

    std::map<std::uint64_t, std::pair<std::int64_t, bool>> loc_;
    std::map<std::uint64_t, std::uint64_t> owners_;
    std::map<std::uint64_t, std::uint64_t> client_ids_;
    std::int64_t last_trade_{0};
};

// Must match OrderBook::digest() bit for bit. Written from the documented
// description rather than by calling the engine's version.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix(std::uint64_t& h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (8 * i)) & 0xFFu;
        h *= kFnvPrime;
    }
}

template <typename Map>
void mix_side(std::uint64_t& h, const Map& side, std::uint64_t tag) {
    mix(h, tag);
    mix(h, side.size());
    for (const auto& [px, orders] : side) {
        std::uint64_t total = 0;
        for (const auto& o : orders) total += o.quantity;
        mix(h, static_cast<std::uint64_t>(px));
        mix(h, total);
        mix(h, orders.size());
    }
}

std::uint64_t digest(const SimpleBook& b) {
    std::uint64_t h = kFnvOffset;
    mix_side(h, b.bids, 1);
    mix_side(h, b.asks, 2);
    return h;
}

// Mirrors MatchingThread::check_risk. Replicated rather than shared, for the
// same reason as the book: a risk check that wrongly rejects would otherwise
// wrongly reject in both places identically.
bool passes_risk(const SimpleBook& book, const ome::OrderCommand& c, const ome::RiskConfig& r) {
    if (c.quantity == 0 || c.quantity > r.max_order_qty) return false;
    if (c.order_type == ome::protocol::OrderType::Market) return true;
    if (c.price_ticks <= 0) return false;
    if (r.tick_size > 1 && (c.price_ticks % r.tick_size) != 0) return false;
    if (r.price_band_bp > 0) {
        std::int64_t ref = 0;
        if (book.last_trade() > 0) {
            ref = book.last_trade();
        } else {
            std::int64_t bb = 0, ba = 0;
            if (!book.best_bid(bb) || !book.best_ask(ba)) return true;
            ref = (bb + ba) / 2;
        }
        const std::int64_t d = c.price_ticks > ref ? c.price_ticks - ref : ref - c.price_ticks;
        if (d * 10000 > ref * r.price_band_bp) return false;
    }
    return true;
}

void apply(SimpleBook& book, const ome::OrderCommand& c, const ome::RiskConfig& risk,
           std::uint64_t& next_id) {
    switch (c.type) {
        case ome::CommandType::NewOrder: {
            if (!passes_risk(book, c, risk)) return;  // rejected: consumes no id
            const std::uint64_t id = next_id++;
            if (c.order_type == ome::protocol::OrderType::Market) {
                // Market orders take from the book and leave nothing resting;
                // the remainder is dropped, matching the engine.
                std::uint32_t remaining = c.quantity;
                const bool buy = (c.side == ome::protocol::Side::Bid);
                while (remaining > 0) {
                    if (buy ? book.asks.empty() : book.bids.empty()) break;
                    auto it = buy ? book.asks.begin() : book.bids.begin();
                    // note: iterators into different maps, handled separately
                    if (buy) {
                        auto& dq = book.asks.begin()->second;
                        VOrder& o = dq.front();
                        const std::uint32_t take = std::min(remaining, o.quantity);
                        o.quantity -= take;
                        remaining -= take;
                        book.set_last_trade(book.asks.begin()->first);
                        if (o.quantity == 0) {
                            const std::uint64_t rid = o.id;
                            dq.pop_front();
                            if (dq.empty()) book.asks.erase(book.asks.begin());
                            book.cancel(rid);
                        }
                    } else {
                        auto& dq = book.bids.begin()->second;
                        VOrder& o = dq.front();
                        const std::uint32_t take = std::min(remaining, o.quantity);
                        o.quantity -= take;
                        remaining -= take;
                        book.set_last_trade(book.bids.begin()->first);
                        if (o.quantity == 0) {
                            const std::uint64_t rid = o.id;
                            dq.pop_front();
                            if (dq.empty()) book.bids.erase(book.bids.begin());
                            book.cancel(rid);
                        }
                    }
                    static_cast<void>(it);
                }
                return;
            }
            VOrder o{};
            o.id = id;
            o.session = c.session;
            o.client_order_id = c.client_order_id;
            o.quantity = c.quantity;
            book.set_owner(id, c.session);
            book.set_client_id(id, c.client_order_id);
            book.add(c.price_ticks, c.side == ome::protocol::Side::Bid, o);
            book.cross();
            return;
        }
        case ome::CommandType::Cancel: {
            const std::uint64_t id = book.find_by_client(c.session, c.client_order_id);
            if (id != 0) book.cancel(id);
            return;
        }
        case ome::CommandType::Modify: {
            const std::uint64_t id = book.find_by_client(c.session, c.client_order_id);
            if (id == 0) return;
            bool was_bid = false;
            if (!book.is_bid(id, was_bid)) return;
            book.cancel(id);
            ome::OrderCommand rep = c;
            rep.type = ome::CommandType::NewOrder;
            rep.order_type = ome::protocol::OrderType::Limit;
            rep.side = was_bid ? ome::protocol::Side::Bid : ome::protocol::Side::Ask;
            apply(book, rep, risk, next_id);
            return;
        }
        case ome::CommandType::CancelAllForSession: {
            std::vector<std::uint64_t> ids;
            for (const auto& [px, dq] : book.bids)
                for (const auto& o : dq)
                    if (o.session == c.session) ids.push_back(o.id);
            for (const auto& [px, dq] : book.asks)
                for (const auto& o : dq)
                    if (o.session == c.session) ids.push_back(o.id);
            for (const std::uint64_t id : ids) book.cancel(id);
            return;
        }
        default:
            return;  // SessionOpened / Subscribe do not touch the book
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string wal_path;
    std::string snap_path;
    std::string risk_path;
    bool dump = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--wal" && i + 1 < argc) wal_path = argv[++i];
        else if (a == "--snapshot" && i + 1 < argc) snap_path = argv[++i];
        else if (a == "--risk" && i + 1 < argc) risk_path = argv[++i];
        else if (a == "--dump") dump = true;
        else {
            std::fprintf(stderr,
                         "usage: wal_verify --wal FILE [--snapshot FILE] [--risk FILE] [--dump]\n"
                         "\n"
                         "Rebuilds the book with an INDEPENDENT implementation and prints its\n"
                         "digest, so recovery can be checked against something other than the\n"
                         "engine agreeing with itself.\n");
            return 2;
        }
    }
    if (wal_path.empty()) {
        std::fprintf(stderr, "--wal is required\n");
        return 2;
    }

    ome::RiskConfig risk{};
    if (!risk_path.empty()) {
        std::string err;
        if (!ome::RiskConfig::load(risk_path, risk, err)) {
            std::fprintf(stderr, "risk config: %s\n", err.c_str());
            return 2;
        }
    }

    SimpleBook book;
    std::uint64_t next_id = 1;
    std::uint64_t from_seq = 0;

    if (!snap_path.empty()) {
        ome::SnapshotData d{};
        std::string err;
        if (ome::read_snapshot(snap_path, d, err)) {
            for (const auto& so : d.orders) {
                VOrder o{};
                o.id = so.exchange_id;
                o.session = so.session;
                o.client_order_id = so.client_order_id;
                o.quantity = so.quantity;
                book.set_owner(so.exchange_id, so.session);
                book.set_client_id(so.exchange_id, so.client_order_id);
                book.add(so.price_ticks, so.side == 0, o);
            }
            book.set_last_trade(d.last_trade_ticks);
            next_id = d.next_exchange_id;
            from_seq = d.last_seq;
        } else {
            std::fprintf(stderr, "snapshot unusable (%s), replaying the whole log\n", err.c_str());
        }
    }

    const auto rd = ome::read_wal(wal_path);
    if (!rd.error.empty()) {
        std::fprintf(stderr, "%s\n", rd.error.c_str());
        return 2;
    }
    if (rd.sequence_gap) {
        std::fprintf(stderr, "sequence gap after %llu\n",
                     static_cast<unsigned long long>(rd.gap_after));
        return 3;
    }

    std::uint64_t seq = 0;
    std::size_t applied = 0;
    for (const auto& c : rd.commands) {
        ++seq;
        if (seq <= from_seq) continue;  // already covered by the snapshot
        apply(book, c, risk, next_id);
        ++applied;
    }

    // Invariants the digest does not check. A digest only says two books are
    // the same; these say the book is sane. A crossed book that both
    // implementations agree on would pass a digest comparison and still be
    // wrong.
    {
        std::int64_t bb = 0, ba = 0;
        const bool has_bid = book.best_bid(bb);
        const bool has_ask = book.best_ask(ba);
        if (has_bid && has_ask && bb >= ba) {
            std::fprintf(stderr, "INVARIANT: book is crossed, best bid %lld >= best ask %lld\n",
                         static_cast<long long>(bb), static_cast<long long>(ba));
            return 4;
        }
        for (const auto* side : {&book.bids}) {
            for (const auto& [px, dq] : *side) {
                if (dq.empty()) {
                    std::fprintf(stderr, "INVARIANT: empty bid level at %lld\n",
                                 static_cast<long long>(px));
                    return 4;
                }
                for (const auto& o : dq) {
                    if (o.quantity == 0) {
                        std::fprintf(stderr, "INVARIANT: zero-quantity order %llu resting\n",
                                     static_cast<unsigned long long>(o.id));
                        return 4;
                    }
                }
            }
        }
        for (const auto& [px, dq] : book.asks) {
            if (dq.empty()) {
                std::fprintf(stderr, "INVARIANT: empty ask level at %lld\n",
                             static_cast<long long>(px));
                return 4;
            }
            for (const auto& o : dq) {
                if (o.quantity == 0) {
                    std::fprintf(stderr, "INVARIANT: zero-quantity order %llu resting\n",
                                 static_cast<unsigned long long>(o.id));
                    return 4;
                }
            }
        }
    }

    if (dump) {
        std::fprintf(stderr, "BIDS\n");
        for (const auto& [px, dq] : book.bids) {
            std::uint64_t t = 0;
            for (const auto& o : dq) t += o.quantity;
            std::fprintf(stderr, "  ticks=%lld qty=%llu orders=%zu\n",
                         static_cast<long long>(px), static_cast<unsigned long long>(t),
                         dq.size());
        }
        std::fprintf(stderr, "ASKS\n");
        for (const auto& [px, dq] : book.asks) {
            std::uint64_t t = 0;
            for (const auto& o : dq) t += o.quantity;
            std::fprintf(stderr, "  ticks=%lld qty=%llu orders=%zu\n",
                         static_cast<long long>(px), static_cast<unsigned long long>(t),
                         dq.size());
        }
    }

    std::fprintf(stderr, "verified %zu commands (torn tail: %zu bytes)\n", applied,
                 rd.truncated_bytes);
    std::printf("%llu\n", static_cast<unsigned long long>(digest(book)));
    return 0;
}
