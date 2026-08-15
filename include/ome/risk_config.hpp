#pragma once

// ---------------------------------------------------------------------------
// Risk limits, and where each one is enforced.
//
// THE DESIGN POINT OF THIS SESSION: a check lives on the thread that has the
// state it needs, and nowhere else.
//
//   NETWORK THREAD  — checks needing only per-session state
//                     malformed frames, unknown message types, duplicate
//                     client order ids, rate limiting
//                     Rejecting here keeps garbage out of the queue entirely,
//                     so a misbehaving client cannot consume matching-thread
//                     time or queue capacity that well-behaved sessions need.
//
//   MATCHING THREAD — checks needing BOOK state
//                     price bands, which are relative to the last trade or the
//                     mid. Only the matching thread may read the book, so this
//                     check cannot live anywhere else. It is not a preference.
//
// Quantity and absolute price limits could go on either thread — they need no
// state at all. They are on the matching thread so that every rejection whose
// reason is "risk" comes from one place and is applied to the same book
// snapshot as the order it concerns. Splitting them across threads would mean
// two code paths producing RISK_* rejections with different guarantees about
// when they ran.
//
// Config is a flat key=value file. No YAML, no JSON, no dependency — the whole
// parser is thirty lines and the format is one a human can edit under pressure.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

namespace ome {

struct RiskConfig {
    // --- network thread ---
    // Sustained orders per second per session, and the burst allowance.
    // Capacity above rate is deliberate: real clients re-quote a whole ladder
    // at once and then go quiet.
    double rate_per_sec{1000.0};
    double rate_burst{2000.0};

    // --- matching thread ---
    std::uint32_t max_order_qty{1000000};

    // Minimum price increment, in ticks. 1 means every tick is valid, which is
    // the default and makes the alignment check a no-op.
    //
    // NOTE: with int64 tick prices there is no such thing as a "misaligned"
    // price unless a coarser increment is imposed on top of the tick — a price
    // IS a tick count by construction. This knob exists so the check is
    // meaningful when it is wanted, rather than pretending to validate
    // something that cannot be invalid. It is not a floating-point epsilon,
    // and it must never become one.
    std::int64_t tick_size{1};

    // Reject a limit order priced further than this from the reference price,
    // in basis points. 1000 bp = 10%.
    //
    // The reference is the last trade price if there has been one, else the
    // mid, else nothing — an empty book with no trades has no opinion about
    // what a reasonable price is, and inventing one would reject the first
    // order ever placed.
    std::int64_t price_band_bp{1000};

    // Loads key=value lines. Unknown keys are an error rather than a warning:
    // a typo'd risk limit that silently keeps the default is exactly the
    // failure you do not want to discover from a fill.
    [[nodiscard]] static bool load(const std::string& path, RiskConfig& out, std::string& error) {
        std::ifstream in(path);
        if (!in) {
            error = "cannot open " + path;
            return false;
        }
        std::string line;
        int lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            const auto hash = line.find('#');
            if (hash != std::string::npos) {
                line.erase(hash);
            }
            trim(line);
            if (line.empty()) {
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                error = at(path, lineno) + "expected key=value";
                return false;
            }
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            trim(key);
            trim(val);

            if (!assign(key, val, out)) {
                error = at(path, lineno) + "unknown or invalid setting '" + key + "'";
                return false;
            }
        }
        return out.validate(error);
    }

    // Rejects configurations that are internally inconsistent. A limiter with
    // capacity below its rate, or a non-positive tick size, is a mistake that
    // should stop startup rather than silently misbehave later.
    [[nodiscard]] bool validate(std::string& error) const {
        if (rate_per_sec <= 0.0) {
            error = "rate_per_sec must be > 0";
            return false;
        }
        if (rate_burst < 1.0) {
            error = "rate_burst must be >= 1 (a bucket that cannot hold one token rejects everything)";
            return false;
        }
        if (max_order_qty == 0) {
            error = "max_order_qty must be > 0";
            return false;
        }
        if (tick_size <= 0) {
            error = "tick_size must be > 0";
            return false;
        }
        if (price_band_bp < 0) {
            error = "price_band_bp must be >= 0";
            return false;
        }
        return true;
    }

private:
    static void trim(std::string& s) {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) {
            s.clear();
            return;
        }
        const auto e = s.find_last_not_of(" \t\r\n");
        s = s.substr(b, e - b + 1);
    }

    static std::string at(const std::string& path, int line) {
        return path + ":" + std::to_string(line) + ": ";
    }

    template <typename T>
    static bool parse(const std::string& s, T& out) {
        std::istringstream is(s);
        is >> out;
        // The trailing check rejects "100abc", which >> alone accepts as 100.
        return !is.fail() && is.eof();
    }

    static bool assign(const std::string& key, const std::string& val, RiskConfig& c) {
        if (key == "rate_per_sec") return parse(val, c.rate_per_sec);
        if (key == "rate_burst") return parse(val, c.rate_burst);
        if (key == "max_order_qty") return parse(val, c.max_order_qty);
        if (key == "tick_size") return parse(val, c.tick_size);
        if (key == "price_band_bp") return parse(val, c.price_band_bp);
        return false;
    }
};

}  // namespace ome
