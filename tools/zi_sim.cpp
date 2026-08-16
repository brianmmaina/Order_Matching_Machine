// Zero-intelligence order flow, driven through the real matching engine.
//
// Generates synthetic orders from parameters calibrated to LOBSTER data
// (analysis/calibrate.py) and matches them with the SAME MatchingEngine and
// OrderBook the gateway uses — same price-time priority, same tick arithmetic,
// same partial-fill semantics.
//
// WHAT IS ACTUALLY HELD CONSTANT, PRECISELY
//
// The real stream is not run through the engine at all: LOBSTER messages are
// analysed directly, because reconstructing them would inherit this project's
// replay accuracy gap. So the engine is not a shared stage both streams pass
// through.
//
// What IS shared is the ANALYSIS: this writes its message log in LOBSTER's
// schema, so analysis/stylized_facts.py runs on both unchanged. If the
// synthetic stream needed its own analysis code, any difference in the results
// could be a difference in the analysis rather than in the market.
//
// And the synthetic side is matched by a real engine rather than a toy, so the
// emergent dynamics come from genuine price-time priority rather than from a
// simplification chosen to make the model work.
//
// MatchingEngine directly rather than MatchingThread: the threaded wrapper
// clears the trade log inside its own apply path, so trades are gone before a
// caller can read them. The book and the matching are identical either way.
//
// WHAT "ZERO INTELLIGENCE" MEANS HERE
//
// Order arrivals are Poisson at the calibrated rates. Sides are coin flips.
// Sizes are drawn from the empirical distribution. Placement is drawn from the
// calibrated histogram of half-spreads from the mid. Cancels pick a resting
// order uniformly at random.
//
// Nothing reacts to anything. No agent has a view, an inventory, a strategy, or
// a memory. The only structure is the book itself and the calibrated marginal
// distributions — which is the whole hypothesis: how much of what markets look
// like falls out of structure rather than intent.
//
// OUTPUT IS LOBSTER FORMAT, ON PURPOSE
//
// The message log is written in exactly the schema LOBSTER uses, so
// analysis/stylized_facts.py runs on it unchanged. One analysis path, two
// inputs. If the synthetic stream needed its own analysis code, any difference
// in results could be a difference in the analysis rather than in the market.
//
// Seeded and reproducible: the same seed produces a byte-identical stream, for
// the same reason WAL replay has to.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cmath>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "ome/book_jsonl.hpp"
#include "matching_engine/matching_engine.hpp"
#include "ome/protocol.hpp"

namespace {

struct Params {
    double limit_rate = 5.0;
    double cancel_rate = 5.0;
    double market_rate = 0.4;
    std::int64_t spread_p50 = 100;
    double size_mean = 100.0;
    std::int64_t size_p50 = 100;
    std::int64_t size_p90 = 200;
    double round_lot_share = 0.7;
    double crossed_mid_share = 0.01;
    std::vector<double> placement_edges;
    std::vector<double> placement_weights;
    // Where the book starts. Arbitrary; only relative moves matter.
    std::int64_t initial_mid = 1000000;
};

bool load_params(const std::string& path, Params& p, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            const auto b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) { s.clear(); return; }
            const auto e = s.find_last_not_of(" \t\r\n");
            s = s.substr(b, e - b + 1);
        };
        trim(k);
        trim(v);
        if (k.empty() || v.empty()) continue;

        auto csv_doubles = [&](std::vector<double>& out) {
            out.clear();
            std::stringstream ss(v);
            std::string tok;
            while (std::getline(ss, tok, ',')) out.push_back(std::atof(tok.c_str()));
        };

        if (k == "limit_rate") p.limit_rate = std::atof(v.c_str());
        else if (k == "cancel_rate") p.cancel_rate = std::atof(v.c_str());
        else if (k == "market_rate") p.market_rate = std::atof(v.c_str());
        else if (k == "spread_p50") p.spread_p50 = std::atoll(v.c_str());
        else if (k == "size_mean") p.size_mean = std::atof(v.c_str());
        else if (k == "size_p50") p.size_p50 = std::atoll(v.c_str());
        else if (k == "size_p90") p.size_p90 = std::atoll(v.c_str());
        else if (k == "round_lot_share") p.round_lot_share = std::atof(v.c_str());
        else if (k == "crossed_mid_share") p.crossed_mid_share = std::atof(v.c_str());
        else if (k == "placement_edges") csv_doubles(p.placement_edges);
        else if (k == "placement_weights") csv_doubles(p.placement_weights);
        // symbol and cancel_rate_per_depth are informational here
    }
    if (p.placement_edges.empty() || p.placement_weights.empty()) {
        error = "placement_edges and placement_weights are required";
        return false;
    }
    if (p.placement_weights.size() != p.placement_edges.size() + 1) {
        error = "placement_weights must have one more entry than placement_edges";
        return false;
    }
    return true;
}

class Sim {
public:
    Sim(const Params& p, std::uint32_t seed) : p_(p), rng_(seed) {
        double acc = 0;
        for (const double w : p_.placement_weights) {
            acc += w;
            cum_.push_back(acc);
        }
        total_weight_ = acc > 0 ? acc : 1.0;
    }

    // Half-spreads from the mid, sampled from the calibrated histogram.
    double sample_placement() {
        const double u = uni_(rng_) * total_weight_;
        std::size_t b = 0;
        while (b + 1 < cum_.size() && u > cum_[b]) ++b;
        const double lo = (b == 0) ? 0.0 : p_.placement_edges[b - 1];
        const double hi = (b < p_.placement_edges.size()) ? p_.placement_edges[b]
                                                          : p_.placement_edges.back() * 2.0;
        return lo + uni_(rng_) * (hi - lo);
    }

    // Round lots dominate real order sizes, so the distribution is reproduced
    // as a mixture rather than smoothed: a round-lot draw, else a small odd lot.
    std::uint32_t sample_size() {
        if (uni_(rng_) < p_.round_lot_share) {
            const int lots = 1 + static_cast<int>(-std::log(1.0 - uni_(rng_) * 0.86) * 1.2);
            return static_cast<std::uint32_t>(std::min(lots, 30) * 100);
        }
        return static_cast<std::uint32_t>(1 + static_cast<int>(uni_(rng_) * 99));
    }

    bool coin() { return uni_(rng_) < 0.5; }
    double uniform() { return uni_(rng_); }

    // Time to the next event of a Poisson process.
    double exponential(double rate) {
        if (rate <= 0) return 1e18;
        return -std::log(1.0 - uni_(rng_)) / rate;
    }

private:
    Params p_;
    std::mt19937 rng_;
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
    std::vector<double> cum_;
    double total_weight_ = 1.0;
};

}  // namespace

int main(int argc, char** argv) {
    std::string cfg_path;
    std::string msg_out;
    std::string jsonl_out;
    std::uint32_t seed = 1;
    double duration = 3600.0;
    bool use_fixed_scale = true;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--config") cfg_path = next();
        else if (a == "--messages") msg_out = next();
        else if (a == "--jsonl") jsonl_out = next();
        else if (a == "--seed") seed = static_cast<std::uint32_t>(std::atoi(next().c_str()));
        else if (a == "--duration") duration = std::atof(next().c_str());
        else if (a == "--adaptive-scale") use_fixed_scale = false;
        else {
            std::fprintf(stderr,
                "usage: zi_sim --config F [--messages F] [--jsonl F] [--seed N] [--duration S]\n"
                "\n"
                "Generates zero-intelligence order flow from a calibration produced by\n"
                "analysis/calibrate.py and runs it through the real matching engine.\n"
                "\n"
                "  --messages  LOBSTER-format message log, so analysis/stylized_facts.py\n"
                "              runs on the output unchanged\n"
                "  --jsonl     book snapshots for tools/book_replay.html\n"
                "  --adaptive-scale  place relative to the LIVE spread instead of the\n"
                "                    calibrated one. Collapses the book; kept so the\n"
                "                    failure is reproducible.\n");
            return 2;
        }
    }
    if (cfg_path.empty()) {
        std::fprintf(stderr, "--config is required\n");
        return 2;
    }

    Params p{};
    std::string err;
    if (!load_params(cfg_path, p, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }

    Sim sim(p, seed);
    MatchingEngine engine;
    // client_order_id -> exchange order id, so cancels can name a resting order.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> live;
    std::uint64_t next_oid = 1;

    std::ofstream msgf;
    if (!msg_out.empty()) msgf.open(msg_out);
    std::ofstream jf;
    if (!jsonl_out.empty()) jf.open(jsonl_out);

    // The generator's own view of what it has resting, so cancels can pick one.
    // The engine owns the book; this is just a bag of ids to sample from.
    std::uint64_t next_coid = 1;
    std::int64_t last_mid = p.initial_mid;
    std::uint64_t logical_ts = 0;

    double t = 0.0;
    std::uint64_t n_new = 0, n_cancel = 0, n_exec = 0, n_market = 0;

    // Seed a starting book: a ZI model with an empty book has no mid to place
    // against, and the first orders would all be arbitrary. Ten levels a side
    // around the initial mid at the calibrated spread.
    auto place = [&](std::int64_t px, std::uint32_t qty, bool bid, bool market) -> std::uint64_t {
        Order o{};
        o.id = next_oid++;
        o.price_ticks = px;
        o.quantity = qty;
        o.side = bid ? Order::BID : Order::ASK;
        o.type = market ? Order::MARKET : Order::LIMIT;
        o.timestamp = ++logical_ts;
        static_cast<void>(engine.processOrder(o));
        return o.id;
    };

    for (int i = 0; i < 10; ++i) {
        for (int side = 0; side < 2; ++side) {
            const std::int64_t off = p.spread_p50 / 2 + static_cast<std::int64_t>(i) * p.spread_p50;
            const std::int64_t px = (side == 0) ? (p.initial_mid - off) : (p.initial_mid + off);
            const std::uint64_t oid = place(px, 100, side == 0, false);
            live.emplace_back(next_coid++, oid);
        }
    }

    const auto& book = engine.book();
    std::uint64_t seq = 0;

    while (t < duration) {
        // Competing Poisson clocks: whichever fires first is the next event.
        const double dt_limit = sim.exponential(p.limit_rate);
        const double dt_cancel = sim.exponential(live.empty() ? 0.0 : p.cancel_rate);
        const double dt_market = sim.exponential(p.market_rate);
        const double dt = std::min({dt_limit, dt_cancel, dt_market});
        t += dt;
        if (t >= duration) break;

        std::int64_t bb = 0, ba = 0;
        const bool have_bid = book.best_bid_ticks(bb);
        const bool have_ask = book.best_ask_ticks(ba);
        const std::int64_t mid = (have_bid && have_ask) ? (bb + ba) / 2 : last_mid;
        // Placement scale.
        //
        // Using the INSTANTANEOUS half-spread creates a feedback loop that
        // destroys the market: orders placed inside the spread narrow it, a
        // narrower spread makes the next order's offset smaller in ticks, and
        // within seconds the book collapses to a one-tick spread where every
        // arrival crosses. Measured on the first version: spread 1 tick,
        // 97% of orders executed, 8% cancelled, against a real market that
        // cancels 96% and executes 7%.
        //
        // The calibrated median spread is exogenous and does not move with the
        // book, which breaks the loop. The instantaneous spread still sets the
        // reference PRICE (the mid); it just no longer sets the SCALE.
        const std::int64_t half = use_fixed_scale
                                      ? p.spread_p50 / 2
                                      : ((have_bid && have_ask && ba > bb) ? (ba - bb) / 2
                                                                          : p.spread_p50 / 2);
        last_mid = mid;

        engine.clear_trade_log();
        bool incoming_was_buy = false;

        if (dt == dt_limit) {
            const bool bid = sim.coin();
            const double r = (sim.uniform() < p.crossed_mid_share) ? -sim.sample_placement()
                                                                   : sim.sample_placement();
            auto off = static_cast<std::int64_t>(r * static_cast<double>(half));
            std::int64_t px = bid ? (mid - off) : (mid + off);
            if (px <= 0) px = 1;
            const std::uint32_t qty = sim.sample_size();
            incoming_was_buy = bid;
            const std::uint64_t coid = next_coid++;
            const std::uint64_t oid = place(px, qty, bid, false);
            live.emplace_back(coid, oid);
            ++n_new;
            if (msgf) {
                msgf << t << ",1," << coid << "," << qty << "," << px << ","
                     << (bid ? 1 : -1) << "\n";
            }
        } else if (dt == dt_cancel && !live.empty()) {
            const auto idx = std::min(static_cast<std::size_t>(sim.uniform() *
                                          static_cast<double>(live.size())),
                                      live.size() - 1);
            const auto [coid, oid] = live[idx];
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(idx));
            Order c{};
            c.id = oid;
            c.type = Order::CANCEL;
            // Only record a cancel that actually removed something. An order
            // already consumed by a trade is gone from the book, and emitting a
            // cancel for it would put an event in the log that never happened —
            // inflating the cancel-to-execution ratio, which is one of the
            // statistics being compared.
            if (engine.processOrder(c).accepted) {
                ++n_cancel;
                if (msgf) {
                    msgf << t << ",3," << coid << ",0," << mid << ",1\n";
                }
            }
        } else {
            const bool buy = sim.coin();
            incoming_was_buy = buy;
            static_cast<void>(place(0, sim.sample_size(), buy, true));
            ++n_market;
        }

        // Trades this event produced, in LOBSTER's execution form: direction is
        // the side of the RESTING order, so a buy-initiated trade hits an ask
        // and prints -1. Getting this backwards would flip every trade-sign
        // statistic, which is the headline of the comparison.
        for (const auto& tr : engine.trade_log()) {
            ++n_exec;
            if (msgf) {
                const int direction = incoming_was_buy ? -1 : 1;
                const std::uint64_t resting_id = incoming_was_buy ? tr.seller_id : tr.buyer_id;
                msgf << t << ",4," << resting_id << "," << tr.quantity << ","
                     << tr.price_ticks << "," << direction << "\n";
            }
        }
        if (jf && (++seq % 10 == 0)) {
            ome::write_book_record(jf, static_cast<std::uint64_t>(t * 1e9), seq,
                                   book.bid_levels_ticks(10), book.ask_levels_ticks(10));
        }
    }

    std::fprintf(stderr,
                 "seed %u, %.0fs: %llu new, %llu cancels, %llu market orders, %llu executions\n",
                 seed, duration, static_cast<unsigned long long>(n_new),
                 static_cast<unsigned long long>(n_cancel),
                 static_cast<unsigned long long>(n_market),
                 static_cast<unsigned long long>(n_exec));
    {
        std::int64_t b = 0, a = 0;
        std::fprintf(stderr, "final book: bid=%lld ask=%lld  bid levels=%zu ask levels=%zu\n",
                     book.best_bid_ticks(b) ? static_cast<long long>(b) : -1,
                     book.best_ask_ticks(a) ? static_cast<long long>(a) : -1,
                     book.bid_levels_ticks(100).size(), book.ask_levels_ticks(100).size());
    }
    std::fprintf(stderr, "final book digest %llu\n",
                 static_cast<unsigned long long>(book.digest()));
    return 0;
}
