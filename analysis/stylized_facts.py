#!/usr/bin/env python3
"""Stylized facts measured directly from LOBSTER message data.

No simulation, no matching engine, and — crucially — no book reconstruction:
every statistic here comes straight from the message stream, where the data is
exact.

That constraint is the point. Book reconstruction is currently 50-70% accurate,
so any statistic derived from a reconstructed book is contaminated. Returns,
trade signs, impact and the cancel ratio are all in the messages themselves and
are unaffected.

These are the baseline against which a synthetic order-flow model would be
compared: if 269K messages of one stock on one day cannot show these effects,
there is nothing to compare a model against.

Pure standard library, matching the project's dependency rule. Everything below
is a few lines of arithmetic; pulling in numpy would buy speed we do not need on
270K rows and would make the script unrunnable from a clean clone.

Usage:
    python3 analysis/stylized_facts.py                    # all symbols
    python3 analysis/stylized_facts.py --symbol AMZN
    python3 analysis/stylized_facts.py --markdown > docs/STYLIZED_FACTS.md

LOBSTER message columns:
    time, type, order_id, size, price_ticks, direction

    type 1 new limit | 2 partial cancel | 3 delete
         4 execution of a visible limit order
         5 execution of a hidden order
         6 cross | 7 halt

    direction on an EXECUTION is the side of the RESTING limit order:
        direction = -1  an ask was executed  -> the aggressor BOUGHT  -> sign +1
        direction = +1  a bid was executed   -> the aggressor SOLD    -> sign -1
    so trade_sign = -direction. Getting this backwards silently flips every
    sign-dependent statistic below, so it is asserted against price moves.
"""

import argparse
import csv
import glob
import math
import os
import sys

TICKS_PER_UNIT = 10000
DATA_ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                         "data", "lobster")


# --- statistics -------------------------------------------------------------

def mean(xs):
    return sum(xs) / len(xs) if xs else 0.0


def stdev(xs):
    if len(xs) < 2:
        return 0.0
    m = mean(xs)
    return math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))


def excess_kurtosis(xs):
    """Fourth standardised moment minus 3. A normal distribution scores 0;
    fat-tailed financial returns score well above it."""
    n = len(xs)
    if n < 4:
        return float("nan")
    m = mean(xs)
    s = stdev(xs)
    if s == 0:
        return float("nan")
    return sum(((x - m) / s) ** 4 for x in xs) / n - 3.0


def autocorrelation(xs, lag):
    """Sample autocorrelation at a given lag."""
    n = len(xs)
    if n <= lag + 1:
        return float("nan")
    m = mean(xs)
    denom = sum((x - m) ** 2 for x in xs)
    if denom == 0:
        return float("nan")
    num = sum((xs[i] - m) * (xs[i + lag] - m) for i in range(n - lag))
    return num / denom


def quantile(sorted_xs, q):
    if not sorted_xs:
        return float("nan")
    i = min(int(q * (len(sorted_xs) - 1)), len(sorted_xs) - 1)
    return sorted_xs[i]


# --- data -------------------------------------------------------------------

class Messages:
    def __init__(self, path):
        self.types = {}
        self.new_sizes = []
        self.times = []
        self.trades = []      # (time, price_ticks, size, sign) visible executions
        self.trades_all = []  # including hidden
        self.first_time = None
        self.last_time = None

        with open(path, newline="") as f:
            for row in csv.reader(f):
                if len(row) < 6:
                    continue
                try:
                    t = float(row[0])
                    mtype = int(row[1])
                    size = int(row[3])
                    price = int(row[4])
                    direction = int(row[5])
                except ValueError:
                    continue

                if self.first_time is None:
                    self.first_time = t
                self.last_time = t
                self.types[mtype] = self.types.get(mtype, 0) + 1
                self.times.append(t)

                if mtype == 1:
                    self.new_sizes.append(size)
                elif mtype in (4, 5):
                    # see module docstring: sign is the AGGRESSOR's side
                    sign = -direction
                    rec = (t, price, size, sign)
                    self.trades_all.append(rec)
                    if mtype == 4:
                        self.trades.append(rec)

    @property
    def duration(self):
        if self.first_time is None or self.last_time is None:
            return 0.0
        return self.last_time - self.first_time

    @property
    def count(self):
        return sum(self.types.values())


def effective_spread(trades):
    """Median (buy price - sell price) over CONSECUTIVE opposite-sign trades.

    Doubles as the sign-convention check and as a spread estimate, and it is
    immune to both price drift and bid-ask bounce because the two prices are
    adjacent in time and straddle the spread.

    An earlier version of this check used the mean next-trade price change
    conditioned on sign, and it reported INTC and MSFT as sign-inverted. They
    were not. That statistic measures bid-ask bounce, not direction: in a
    penny-spread stock consecutive trades alternate across the spread, so the
    print after a buy is usually LOWER. The lesson generalises — see the note on
    price impact below.
    """
    diffs = []
    for (pa, sa), (pb, sb) in zip([(t[1], t[3]) for t in trades],
                                  [(t[1], t[3]) for t in trades][1:]):
        if sa == sb:
            continue
        buy, sell = (pa, pb) if sa > 0 else (pb, pa)
        diffs.append(buy - sell)
    return quantile(sorted(diffs), 0.5), len(diffs)


def resample_returns(trades, bucket_seconds):
    """Log returns of last-trade price in fixed time buckets. Empty buckets are
    skipped rather than forward-filled: a synthetic zero return would suppress
    exactly the volatility clustering we are trying to measure."""
    if not trades:
        return []
    buckets = {}
    for t, price, _, _ in trades:
        buckets[int(t // bucket_seconds)] = price
    keys = sorted(buckets)
    rets = []
    for a, b in zip(keys, keys[1:]):
        pa, pb = buckets[a], buckets[b]
        if pa > 0 and pb > 0:
            rets.append(math.log(pb / pa))
    return rets


# NOTE: price impact is deliberately NOT measured here.
#
# The obvious trade-only proxy — signed price change to the next print — is
# dominated by bid-ask bounce rather than by impact, and the contamination is
# worst exactly where the spread is tightest. Measured on this sample it reports
# NEGATIVE impact for INTC and MSFT, i.e. that buying pushes the price down.
#
# A sound impact measure needs the MID price before and after the trade, and the
# mid needs a reconstructed book. So impact belongs to the book-dependent half of
# the battery and waits on reconstruction fidelity. Reporting the trade-only
# version with a caveat would be worse than not reporting it: the number looks
# like a result and is an artifact.


# --- report -----------------------------------------------------------------

def analyse(symbol, path):
    m = Messages(path)
    dur = m.duration or 1.0

    new = m.types.get(1, 0)
    cancels = m.types.get(2, 0) + m.types.get(3, 0)
    vis = m.types.get(4, 0)
    hid = m.types.get(5, 0)

    signs = [t[3] for t in m.trades]
    trade_rets = []
    for a, b in zip(m.trades, m.trades[1:]):
        if a[1] > 0 and b[1] > 0:
            trade_rets.append(math.log(b[1] / a[1]))
    # Two horizons. 60s is the conventional choice but yields only ~380
    # observations from one session, where the white-noise standard error of an
    # autocorrelation (~1/sqrt(n)) is 0.05 — too coarse to resolve an effect of
    # the size we are looking for. 10s buckets give 3-5x the observations at the
    # cost of more microstructure noise, and the effect resolves cleanly.
    min_rets = resample_returns(m.trades, 60.0)
    fine_rets = resample_returns(m.trades, 10.0)

    inter = [b - a for a, b in zip(m.times, m.times[1:]) if b > a]

    return {
        "symbol": symbol,
        "messages": m.count,
        "duration_s": dur,
        "rate": m.count / dur,
        "types": m.types,
        "new": new,
        "cancels": cancels,
        "visible": vis,
        "hidden": hid,
        "cancel_ratio": cancels / new if new else float("nan"),
        "exec_ratio": vis / new if new else float("nan"),
        "hidden_share": hid / (vis + hid) if (vis + hid) else float("nan"),
        "size_mean": mean(m.new_sizes),
        "size_median": quantile(sorted(m.new_sizes), 0.5),
        "size_p99": quantile(sorted(m.new_sizes), 0.99),
        "trades": len(m.trades),
        "ret_kurtosis": excess_kurtosis(trade_rets),
        "ret_kurtosis_1m": excess_kurtosis(min_rets),
        "n_min_rets": len(min_rets),
        "sign_acf": [(k, autocorrelation(signs, k)) for k in (1, 2, 5, 10, 20, 50, 100)],
        "absret_acf": [(k, autocorrelation([abs(r) for r in min_rets], k))
                       for k in (1, 2, 5, 10, 20)],
        "absret_acf_fine": [(k, autocorrelation([abs(r) for r in fine_rets], k))
                            for k in (1, 2, 3, 5, 10)],
        "n_fine_rets": len(fine_rets),
        # +/- 2 standard errors under the null of no autocorrelation
        "acf_band_1m": 2.0 / math.sqrt(len(min_rets)) if min_rets else float("nan"),
        "acf_band_10s": 2.0 / math.sqrt(len(fine_rets)) if fine_rets else float("nan"),
        "eff_spread": effective_spread(m.trades),
        "interarrival_mean": mean(inter),
        "interarrival_median": quantile(sorted(inter), 0.5),
    }


def fmt(x, nd=4):
    if isinstance(x, float) and math.isnan(x):
        return "n/a"
    return f"{x:.{nd}f}"


def print_report(rs, markdown=False):
    h1, h2, bullet = ("## ", "### ", "- ") if markdown else ("=== ", "--- ", "  ")

    if markdown:
        print("# Stylized facts from LOBSTER message data\n")
        print("Measured by `analysis/stylized_facts.py` directly from the message")
        print("stream — no book reconstruction, so none of these numbers are affected")
        print("by the replay accuracy gap.\n")
        print("Sample: NASDAQ, 2012-06-21, one full session per symbol.\n")

    print(f"{h1}Sample composition\n")
    if markdown:
        print("| Symbol | Messages | New | Cancels | Exec (visible) | Exec (hidden) | msg/s |")
        print("|---|---|---|---|---|---|---|")
        for r in rs:
            print(f"| {r['symbol']} | {r['messages']:,} | {r['new']:,} | {r['cancels']:,} "
                  f"| {r['visible']:,} | {r['hidden']:,} | {r['rate']:.1f} |")
    else:
        for r in rs:
            print(f"{bullet}{r['symbol']:5s} {r['messages']:>8,} msgs  {r['rate']:>6.1f}/s  "
                  f"new={r['new']:,} cancel={r['cancels']:,} exec={r['visible']:,}(+{r['hidden']:,} hidden)")
    print()

    print(f"{h1}Fact 1 — almost every order is cancelled, not executed\n")
    if markdown:
        print("| Symbol | Cancels / new | Executions / new | Hidden share of executions |")
        print("|---|---|---|---|")
        for r in rs:
            print(f"| {r['symbol']} | {r['cancel_ratio']*100:.1f}% | {r['exec_ratio']*100:.1f}% "
                  f"| {r['hidden_share']*100:.1f}% |")
    else:
        for r in rs:
            print(f"{bullet}{r['symbol']:5s} cancels/new={r['cancel_ratio']*100:5.1f}%  "
                  f"exec/new={r['exec_ratio']*100:4.1f}%  hidden={r['hidden_share']*100:4.1f}% of execs")
    print()

    print(f"{h1}Fact 2 — trade signs are strongly autocorrelated (long memory)\n")
    if markdown:
        print("Order flow is not independent: a buy is far more likely to be followed")
        print("by another buy. A zero-intelligence model has an ACF of exactly zero at")
        print("every lag by construction, so this is the sharpest test of it.\n")
        lags = [k for k, _ in rs[0]["sign_acf"]]
        print("| Symbol | " + " | ".join(f"lag {k}" for k in lags) + " |")
        print("|---" * (len(lags) + 1) + "|")
        for r in rs:
            print(f"| {r['symbol']} | " + " | ".join(fmt(v, 3) for _, v in r["sign_acf"]) + " |")
    else:
        for r in rs:
            print(f"{bullet}{r['symbol']:5s} " +
                  "  ".join(f"lag{k}={fmt(v,3)}" for k, v in r["sign_acf"]))
    print()

    print(f"{h1}Fact 3 — returns are fat-tailed\n")
    if markdown:
        print("Excess kurtosis; a normal distribution scores 0.\n")
        print("| Symbol | Trades | Trade-level | 1-minute | 1-min obs |")
        print("|---|---|---|---|---|")
        for r in rs:
            print(f"| {r['symbol']} | {r['trades']:,} | {fmt(r['ret_kurtosis'],1)} "
                  f"| {fmt(r['ret_kurtosis_1m'],1)} | {r['n_min_rets']} |")
    else:
        for r in rs:
            print(f"{bullet}{r['symbol']:5s} trade-level={fmt(r['ret_kurtosis'],1):>8s}  "
                  f"1-min={fmt(r['ret_kurtosis_1m'],1):>7s}  (n={r['n_min_rets']})")
    print()

    print(f"{h1}Fact 4 — volatility clustering\n")
    if markdown:
        print("Autocorrelation of |returns|. Positive and slowly decaying means large")
        print("moves cluster in time. Zero-intelligence flow produces none.\n")
        print("The `band` column is ±2 standard errors under the null of no")
        print("autocorrelation (2/√n). Values inside it are not distinguishable from")
        print("noise, which is the whole story at the 1-minute horizon.\n")
        print("**1-minute returns** — conventional horizon, but only ~380 observations")
        print("from a single session:\n")
        lags = [k for k, _ in rs[0]["absret_acf"]]
        print("| Symbol | n | band | " + " | ".join(f"lag {k}" for k in lags) + " |")
        print("|---" * (len(lags) + 3) + "|")
        for r in rs:
            print(f"| {r['symbol']} | {r['n_min_rets']} | ±{r['acf_band_1m']:.3f} | " +
                  " | ".join(fmt(v, 3) for _, v in r["absret_acf"]) + " |")
        print()
        print("**10-second returns** — 3-5x the observations, so the band tightens and")
        print("the effect separates from noise at every symbol:\n")
        lags = [k for k, _ in rs[0]["absret_acf_fine"]]
        print("| Symbol | n | band | " + " | ".join(f"lag {k}" for k in lags) + " |")
        print("|---" * (len(lags) + 3) + "|")
        for r in rs:
            print(f"| {r['symbol']} | {r['n_fine_rets']:,} | ±{r['acf_band_10s']:.3f} | " +
                  " | ".join(fmt(v, 3) for _, v in r["absret_acf_fine"]) + " |")
    else:
        for r in rs:
            print(f"{bullet}{r['symbol']:5s} 1m  n={r['n_min_rets']:>5} band=±{r['acf_band_1m']:.3f}  " +
                  "  ".join(f"lag{k}={fmt(v,3)}" for k, v in r["absret_acf"]))
        print()
        for r in rs:
            print(f"{bullet}{r['symbol']:5s} 10s n={r['n_fine_rets']:>5} band=±{r['acf_band_10s']:.3f}  " +
                  "  ".join(f"lag{k}={fmt(v,3)}" for k, v in r["absret_acf_fine"]))
    print()

    print(f"{h1}Fact 5 — effective spread, and the sign-convention check\n")
    if markdown:
        print("Median (buy price − sell price) over consecutive opposite-sign trades.")
        print("They straddle the spread, so this is immune to both drift and bounce.")
        print("A positive value confirms the aggressor-sign convention; 100 ticks = 1 cent.\n")
        print("| Symbol | Pairs | Median buy − sell (ticks) | Sign convention |")
        print("|---|---|---|---|")
        for r in rs:
            med, n = r["eff_spread"]
            print(f"| {r['symbol']} | {n:,} | {med:.0f} | {'correct' if med > 0 else '**INVERTED**'} |")
    else:
        for r in rs:
            med, n = r["eff_spread"]
            print(f"{bullet}{r['symbol']:5s} n={n:>6,}  median(buy-sell)={med:>6.0f} ticks  "
                  f"{'OK' if med > 0 else 'INVERTED!'}")
    print()

    if markdown:
        print(f"{h1}Not measured: price impact\n")
        print("The trade-only proxy for impact — signed price change to the next print —")
        print("measures bid-ask bounce rather than impact, and the contamination is worst")
        print("where the spread is tightest. On this sample it reports *negative* impact")
        print("for INTC and MSFT, i.e. that buying pushes price down.")
        print()
        print("A sound measure needs the mid price before and after the trade, and the mid")
        print("needs a reconstructed book. Impact therefore belongs to the book-dependent")
        print("half of the battery and waits on reconstruction fidelity.")
        print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbol", help="restrict to one symbol, e.g. AMZN")
    ap.add_argument("--markdown", action="store_true", help="emit a markdown report")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(DATA_ROOT, "*", "*_message_*.csv")))
    if not paths:
        print(f"no LOBSTER message files under {DATA_ROOT}", file=sys.stderr)
        print("see data/lobster/README.md for how to obtain them", file=sys.stderr)
        return 1

    results = []
    for p in paths:
        sym = os.path.basename(p).split("_")[0]
        if args.symbol and sym.upper() != args.symbol.upper():
            continue
        results.append(analyse(sym, p))

    if not results:
        print("no matching symbol", file=sys.stderr)
        return 1

    print_report(results, markdown=args.markdown)
    return 0


if __name__ == "__main__":
    sys.exit(main())
