// The Farmer/Patelli/Zovko (2005) zero-intelligence model, simulated exactly as
// specified, on the project's real matching engine.
//
//   J. D. Farmer, P. Patelli, I. I. Zovko, "The predictive power of zero
//   intelligence in financial markets", PNAS 102(6):2254-2259, 2005.
//
// WHY THIS EXISTS SEPARATELY FROM zi_sim
//
// tools/zi_sim.cpp is a RICHER model: empirical order sizes, an empirical
// placement histogram fitted to LOBSTER. That makes it a better imitation of a
// real market and a worse test of this paper, whose scaling laws are derived
// for a specific and much more austere specification:
//
//   * every order is the same size, sigma
//   * buy limit orders deposit UNIFORMLY on (-inf, a(t)), sells on (b(t), +inf)
//   * equal rates for buying and selling
//   * every process Poisson and independent except through the boundaries
//   * resting orders cancel at a constant rate delta, regardless of position
//
// Mixing the two would let a difference in the model masquerade as a difference
// in the law. So this file implements the paper's model and nothing else.
//
// WHAT IT TESTS
//
// analysis/farmer2005.py found that the spread law fails across five LOBSTER
// stocks, and that the error is rank-ordered by the model's own nondimensional
// tick size dp/p_c -- the parameter Equation 1 assumes away by taking dp -> 0.
// That is a correlation on five points. It is consistent with the tick
// constraint causing the failure, and equally consistent with dp/p_c being a
// proxy for something else that separates $30 stocks from $500 stocks.
//
// Simulation separates those. Run the paper's own model at a stock's measured
// (alpha, mu, delta, sigma) and its REAL tick size, and the only thing that can
// inflate the spread is the tick, because nothing else about a $30 stock is
// present. If simulated INTC reproduces the inflated ratio that real INTC
// shows, the tick constraint is a mechanism rather than a correlate.
//
// This is also an independent exercise of the matching engine: a continuous
// double auction driven for millions of events, where the answer is known
// analytically for small dp.
//
// Prices are log-price tick indices: index i is a log price of i*dp. The mean
// field result is in log price, and this keeps the engine's exact integer
// comparisons intact.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "matching_engine/matching_engine.hpp"

namespace {

struct Config {
    // Measured per stock by analysis/farmer2005.py. Units are shares,
    // log-price and events; see that script for how each is estimated.
    double alpha = 0.0;    // shares per unit log-price per unit time
    double mu = 0.0;       // shares per unit time
    double delta = 0.0;    // 1/time, per resting order
    double sigma = 0.0;    // shares
    double dp = 0.0;       // tick size, in log price

    std::uint64_t events = 2000000;
    double warmup_frac = 0.2;
    // Depth of the simulated deposition interval, in characteristic prices p_c.
    //
    // The paper's intervals are semi-infinite and a simulation needs them
    // finite, so each is truncated at this distance BEHIND the opposing best
    // quote -- buys on [a(t)-W, a(t)-1], sells on [b(t)+1, b(t)+W].
    //
    // Anchoring to the opposing quote rather than to a fixed price box matters
    // for two reasons, both learned the hard way. A fixed box lets the book
    // pin its best bid against the top wall, at which point the sell interval
    // [b+1, box_top] is empty and no sell can ever arrive again: an absorbing
    // one-sided state that silently produced a spread of zero. A fixed box
    // also makes the buy and sell arrival rates depend on where the price sits
    // inside it, which breaks the model's equal-rates-for-buying-and-selling
    // assumption. Anchored intervals have constant, equal width by
    // construction, and prices are free to wander.
    //
    // --width-scan checks the answer does not depend on W rather than assuming
    // it. If it does, the truncation is setting the spread instead of the
    // order flow.
    double width_pc = 50.0;
    std::uint32_t seed = 1;
};

// f(eps) = 0.28 + 1.86 eps^(3/4), Equation 1.
double f_eps(double eps) { return 0.28 + 1.86 * std::pow(eps, 0.75); }

struct Result {
    double mean_spread_log = 0.0;   // measured, in log price
    double mean_spread_ticks = 0.0;
    std::uint64_t measured = 0;
    std::uint64_t trades = 0;
    std::uint64_t boundary_hits = 0;  // events where the range bound was reached
    double mean_resting = 0.0;
};

class PaperSim {
public:
    PaperSim(const Config& c, std::int64_t half_ticks)
        : c_(c), half_(half_ticks), rng_(c.seed),
          size_(static_cast<std::uint32_t>(std::max<long long>(1, std::llround(c.sigma)))) {}

    Result run() {
        Result r;
        const std::uint64_t warm =
            static_cast<std::uint64_t>(static_cast<double>(c_.events) * c_.warmup_frac);
        double spread_sum = 0.0;
        double resting_sum = 0.0;
        std::uint64_t ts = 0;

        for (std::uint64_t ev = 0; ev < c_.events; ++ev) {
            std::int64_t bid = 0, ask = 0;
            const bool has_bid = eng_.book().best_bid_ticks(bid);
            const bool has_ask = eng_.book().best_ask_ticks(ask);

            // Deposition intervals, per the model: buy limit orders arrive
            // with constant density below the best ask, sells above the best
            // bid. Each is truncated half_ ticks behind the quote it is
            // anchored to, so both are the same constant width and neither can
            // be squeezed to nothing.
            //
            // With one side empty there is no opposing quote to anchor to, so
            // the other side's best stands in and the book is one tick wide.
            const std::int64_t ref_ask = has_ask ? ask : (has_bid ? bid + 1 : 1);
            const std::int64_t ref_bid = has_bid ? bid : (has_ask ? ask - 1 : 0);
            const std::int64_t buy_hi = ref_ask - 1;
            const std::int64_t buy_lo = buy_hi - half_ + 1;
            const std::int64_t sell_lo = ref_bid + 1;
            const std::int64_t sell_hi = sell_lo + half_ - 1;
            if (has_bid && has_ask && (ask - bid) >= half_) ++r.boundary_hits;

            // Rates. alpha is a density in shares per unit log price, so the
            // arrival rate over an interval is alpha * width, and the ORDER
            // rate is that divided by the order size. Both intervals are
            // half_ ticks wide, which is the model's equal rates for buying
            // and selling.
            const double w = static_cast<double>(half_) * c_.dp;
            const double r_lb = c_.alpha * w / c_.sigma;
            const double r_ls = r_lb;
            const double r_mkt = c_.mu / c_.sigma;
            const double n_live = static_cast<double>(live_.size());
            const double r_can = c_.delta * n_live;
            const double total = r_lb + r_ls + r_mkt + r_can;
            if (total <= 0.0) break;

            double u = uni_(rng_) * total;
            ++ts;

            if (u < r_lb) {
                place(Order::BID, pick(buy_lo, buy_hi), ts);
            } else if ((u -= r_lb) < r_ls) {
                place(Order::ASK, pick(sell_lo, sell_hi), ts);
            } else if ((u -= r_ls) < r_mkt) {
                r.trades += market(uni_(rng_) < 0.5 ? Order::BID : Order::ASK, ts);
            } else {
                cancel();
            }

            if (ev >= warm) {
                std::int64_t b2 = 0, a2 = 0;
                if (eng_.book().best_bid_ticks(b2) && eng_.book().best_ask_ticks(a2) && a2 > b2) {
                    // The paper measures the spread after every event, each
                    // with equal weight.
                    spread_sum += static_cast<double>(a2 - b2);
                    ++r.measured;
                }
                resting_sum += n_live;
            }
        }

        if (r.measured > 0) {
            r.mean_spread_ticks = spread_sum / static_cast<double>(r.measured);
            r.mean_spread_log = r.mean_spread_ticks * c_.dp;
        }
        const auto denom = static_cast<double>(c_.events) * (1.0 - c_.warmup_frac);
        r.mean_resting = denom > 0 ? resting_sum / denom : 0.0;
        return r;
    }

private:
    std::int64_t pick(std::int64_t lo, std::int64_t hi) {
        if (hi < lo) return lo;
        const auto span = static_cast<std::uint64_t>(hi - lo);
        std::uniform_int_distribution<std::uint64_t> d(0, span);
        return lo + static_cast<std::int64_t>(d(rng_));
    }

    void place(Order::Side side, std::int64_t px, std::uint64_t ts) {
        Order o = Order::make(px, size_, side, Order::LIMIT, ts);
        const ApplyResult res = eng_.processOrder(o);
        if (res.accepted) live_.push_back(o.id);
    }

    std::uint64_t market(Order::Side side, std::uint64_t ts) {
        const std::size_t before = eng_.trade_log_size();
        Order o = Order::make(0, size_, side, Order::MARKET, ts);
        (void)eng_.processOrder(o);
        const std::size_t after = eng_.trade_log_size();

        // Every order in this model is the same size, so a market order
        // consumes exactly one resting order in full. The passive side of each
        // new trade is therefore gone from the book, and dropping it keeps the
        // cancellation rate delta*N honest -- N must be the number of orders
        // actually resting, not the number ever placed.
        for (std::size_t i = before; i < after; ++i) {
            const Trade& t = eng_.trade_log()[i];
            const std::uint64_t passive = (t.buyer_id == o.id) ? t.seller_id : t.buyer_id;
            forget(passive);
        }
        eng_.clear_trade_log();
        return static_cast<std::uint64_t>(after - before);
    }

    void cancel() {
        // "Queued limit orders are canceled at a constant rate", independent
        // of price and of age, so the victim is drawn uniformly at random.
        while (!live_.empty()) {
            std::uniform_int_distribution<std::size_t> d(0, live_.size() - 1);
            const std::size_t i = d(rng_);
            const std::uint64_t id = live_[i];
            live_[i] = live_.back();
            live_.pop_back();
            if (eng_.book().cancelOrder(id)) return;
            // Stale entry: already executed. Not a cancellation event, so keep
            // drawing rather than consuming the event on a no-op.
        }
    }

    void forget(std::uint64_t id) {
        const auto it = std::find(live_.begin(), live_.end(), id);
        if (it != live_.end()) {
            *it = live_.back();
            live_.pop_back();
        }
    }

    Config c_;
    std::int64_t half_;
    MatchingEngine eng_;
    std::mt19937 rng_;
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
    std::vector<std::uint64_t> live_;
    std::uint32_t size_;
};

int run_one(const Config& c, bool header) {
    if (c.alpha <= 0 || c.mu <= 0 || c.delta <= 0 || c.sigma <= 0 || c.dp <= 0) {
        std::fprintf(stderr, "alpha, mu, delta, sigma and dp must all be positive\n");
        return 2;
    }
    const double eps = c.delta * c.sigma / c.mu;
    const double p_c = c.mu / c.alpha;
    const double s_hat = p_c * f_eps(eps);
    const double tick_ratio = c.dp / p_c;

    // The range is expressed in characteristic prices so it means the same
    // thing for a large-tick and a small-tick stock. At least a few ticks
    // either way, or there is no room for a book at all.
    const auto half = std::max<std::int64_t>(
        3, static_cast<std::int64_t>(std::llround(c.width_pc * p_c / c.dp)));

    PaperSim sim(c, half);
    const Result r = sim.run();

    if (header) {
        std::printf("%9s %9s %9s %12s %12s %8s %9s %8s\n",
                    "eps", "dp/p_c", "half_tk", "s_hat", "s_sim", "ratio",
                    "resting", "wide%");
    }
    const double ratio = s_hat > 0 ? r.mean_spread_log / s_hat : 0.0;
    const double bnd = c.events ? 100.0 * static_cast<double>(r.boundary_hits) /
                                      static_cast<double>(c.events)
                                : 0.0;
    std::printf("%9.4f %9.3f %9lld %12.3e %12.3e %8.2f %9.1f %8.2f\n",
                eps, tick_ratio, static_cast<long long>(half), s_hat,
                r.mean_spread_log, ratio, r.mean_resting, bnd);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config c;
    bool scan = false;
    bool header = true;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--alpha") c.alpha = std::atof(next().c_str());
        else if (a == "--mu") c.mu = std::atof(next().c_str());
        else if (a == "--delta") c.delta = std::atof(next().c_str());
        else if (a == "--sigma") c.sigma = std::atof(next().c_str());
        else if (a == "--dp") c.dp = std::atof(next().c_str());
        else if (a == "--events") c.events = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--width-pc") c.width_pc = std::atof(next().c_str());
        else if (a == "--seed") c.seed = static_cast<std::uint32_t>(std::atoi(next().c_str()));
        else if (a == "--no-header") header = false;
        else if (a == "--width-scan") scan = true;
        else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: zi_paper --alpha A --mu M --delta D --sigma S --dp T [options]\n"
                "\n"
                "Simulates the Farmer/Patelli/Zovko (2005) zero-intelligence model on the\n"
                "project's matching engine and compares the realised spread to Equation 1,\n"
                "  s_hat = (mu/alpha) * (0.28 + 1.86*eps^0.75),  eps = delta*sigma/mu\n"
                "\n"
                "Parameters come from analysis/farmer2005.py, which measures them from\n"
                "LOBSTER the way the paper measures them from the LSE. All of alpha, mu and\n"
                "delta are per unit time; the spread prediction is invariant to the time\n"
                "unit, since scaling all three leaves both eps and mu/alpha unchanged.\n"
                "\n"
                "  --dp          tick size IN LOG PRICE. This is the parameter Equation 1\n"
                "                assumes away by taking dp -> 0, so it is the point of the\n"
                "                whole exercise; dp/p_c is reported.\n"
                "  --events      simulated events (default 2000000)\n"
                "  --width-pc    half-width of the price range, in characteristic prices\n"
                "  --width-scan  rerun at several widths. The semi-infinite intervals have\n"
                "                to be truncated somewhere, and if the answer moves with\n"
                "                the truncation then the boundary is setting the spread\n"
                "                rather than the order flow.\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    if (!scan) return run_one(c, header);

    std::printf("width scan -- the answer must not depend on where the interval is cut\n");
    bool first = true;
    for (const double w : {12.5, 25.0, 50.0, 100.0, 200.0}) {
        Config c2 = c;
        c2.width_pc = w;
        std::printf("width_pc = %6.1f   ", w);
        if (run_one(c2, first) != 0) return 2;
        first = false;
    }
    return 0;
}
