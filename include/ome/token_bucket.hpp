#pragma once

// ---------------------------------------------------------------------------
// Per-session rate limiter.
//
// TOKEN BUCKET RATHER THAN FIXED WINDOW, and the difference is about burst
// behavior at the boundary:
//
//   fixed window   a counter reset every second. A client sending N orders at
//                  t=0.999 and N more at t=1.001 passes both windows and has
//                  delivered 2N orders in two milliseconds. The limit is
//                  nominally N/sec and the achievable instantaneous rate is
//                  twice that — the classic boundary-doubling problem.
//
//   token bucket   tokens accrue continuously at `rate` and accumulate up to
//                  `capacity`. Burst is bounded by capacity at every instant,
//                  not just at arbitrary window edges, and the long-run average
//                  is exactly `rate`.
//
// Capacity is a deliberate allowance for burstiness, not a flaw. Real clients
// send in bursts — a strategy re-quoting a whole ladder emits twenty orders in
// a microsecond and then nothing for a second. A limiter that refuses that is
// wrong about the workload. Capacity says how much burst is tolerable; rate
// says what is sustainable.
//
// TIME IS INJECTED, never read from a clock in here. A rate limiter tested by
// sleeping is slow and flaky, and flaky tests get deleted. Callers pass a
// monotonic timestamp; tests pass whatever they like.
//
// Lives on the NETWORK thread: it needs no book state, so it can reject before
// anything reaches the queue. That is the whole point of putting it there.
// ---------------------------------------------------------------------------

#include <cstdint>

#include "ome/session.hpp"  // Nanos

namespace ome {

class TokenBucket {
public:
    // rate_per_sec: sustained allowance. capacity: maximum burst.
    TokenBucket(double rate_per_sec, double capacity, Nanos now)
        : rate_(rate_per_sec), capacity_(capacity), tokens_(capacity), last_ns_(now) {}

    // Consumes one token if available. Returns false when the caller is over
    // its limit and the message should be rejected.
    [[nodiscard]] bool allow(Nanos now) {
        refill(now);
        if (tokens_ < 1.0) {
            return false;
        }
        tokens_ -= 1.0;
        return true;
    }

    [[nodiscard]] double tokens(Nanos now) {
        refill(now);
        return tokens_;
    }

private:
    void refill(Nanos now) {
        // Monotonic clock, but guard anyway: a caller passing a stale timestamp
        // would otherwise compute a negative elapsed time, and on unsigned Nanos
        // that subtraction wraps to an enormous positive value and hands out
        // effectively unlimited tokens. Failing closed is the safe direction for
        // anything named "limit".
        if (now <= last_ns_) {
            return;
        }
        const double elapsed_s = static_cast<double>(now - last_ns_) / 1e9;
        last_ns_ = now;
        tokens_ += elapsed_s * rate_;
        if (tokens_ > capacity_) {
            tokens_ = capacity_;
        }
    }

    double rate_;
    double capacity_;
    double tokens_;
    Nanos last_ns_;
};

}  // namespace ome
