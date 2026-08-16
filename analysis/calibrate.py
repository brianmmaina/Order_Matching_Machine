#!/usr/bin/env python3
"""Calibrate a zero-intelligence order flow model to LOBSTER data.

Produces the parameters a synthetic order-flow generator needs: how often orders
arrive, how big they are, where they are placed relative to the mid, and how
quickly they are cancelled.

WHY THE ORDERBOOK FILE AND NOT A RECONSTRUCTED BOOK

Placement distance needs a mid price, and a mid needs a book. Reconstructing one
from the messages would inherit the replay accuracy gap (50-70%), which would
put a systematic error straight into the calibration.

LOBSTER ships a second file giving the true depth-10 book state after every
message. Using it means the mid is ground truth rather than something this
project computed. The two files are row-aligned, so the book state *before*
message i is row i-1.

WHAT IS APPROXIMATED, AND SAY SO

The cancel rate is expressed per resting order per second, which needs to know
how many orders are resting. The orderbook file only shows the top ten levels,
so the denominator is top-10 depth rather than the whole book. That
underestimates the resting quantity and therefore OVERestimates the per-order
cancel rate. It is the right shape and the wrong constant; a generator using it
will cancel somewhat too eagerly.

Sizes are reported as an empirical distribution rather than a fitted one. Order
sizes are strongly clustered on round lots — 100 especially — and any smooth
distribution fitted to that would misrepresent it.

Pure standard library, matching the project's dependency rule.

Usage:
    python3 analysis/calibrate.py --symbol AMZN
    python3 analysis/calibrate.py --symbol AMZN --out config/zi_amzn.conf
"""

import argparse
import csv
import glob
import math
import os
import sys

DATA_ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                         "data", "lobster")

# Placement is measured in HALF-SPREADS from the mid, not in ticks.
#
# Absolute ticks are not comparable across instruments and are misleading even
# within one. AMZN's median spread here is 1,300 ticks; INTC's is 100. Bucketing
# both by "0-1 ticks, 1-2 ticks..." puts every AMZN order in the tail and tells
# you nothing, which is exactly what the first version of this did.
#
# In half-spread units the scale is intrinsic:
#   r <  1.0  inside the spread (price improvement)
#   r == 1.0  at the touch
#   r >  1.0  behind the touch, r-1 half-spreads back
# and a value below 0 means the order crossed the mid entirely.
PLACEMENT_EDGES = [0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 5.0, 10.0, 20.0]


def quantile(sorted_xs, q):
    if not sorted_xs:
        return 0
    return sorted_xs[min(int(q * (len(sorted_xs) - 1)), len(sorted_xs) - 1)]


def find_pair(symbol):
    msg = glob.glob(os.path.join(DATA_ROOT, f"*{symbol}*", f"{symbol}_*_message_*.csv"))
    book = glob.glob(os.path.join(DATA_ROOT, f"*{symbol}*", f"{symbol}_*_orderbook_*.csv"))
    if not msg or not book:
        return None, None
    return msg[0], book[0]


def calibrate(symbol, msg_path, book_path):
    types = {}
    new_sizes = []
    placements = []          # signed ticks from mid, positive = away from mid
    spreads = []
    depths = []              # top-10 total resting quantity, both sides
    first_t = last_t = None
    crossing_limits = 0

    with open(msg_path, newline="") as mf, open(book_path, newline="") as bf:
        mr, br = csv.reader(mf), csv.reader(bf)
        prev_book = None
        for row, brow in zip(mr, br):
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

            if first_t is None:
                first_t = t
            last_t = t
            types[mtype] = types.get(mtype, 0) + 1

            # Book state BEFORE this message is the previous row.
            if mtype == 1 and prev_book is not None:
                ask, bid = prev_book
                half = (ask - bid) / 2.0
                if ask > 0 and bid > 0 and half > 0:
                    mid = (ask + bid) / 2.0
                    # Positive is always "away from the mid", whichever side the
                    # order is on. Divided by the half-spread so the unit is
                    # intrinsic to the instrument.
                    d = (mid - price) if direction == 1 else (price - mid)
                    placements.append(d / half)
                    if d < 0:
                        crossing_limits += 1
                new_sizes.append(size)
            elif mtype == 1:
                new_sizes.append(size)

            # Parse this row's book for the next iteration and for the aggregates.
            try:
                vals = [int(x) for x in brow]
            except ValueError:
                prev_book = None
                continue
            ask1, bid1 = vals[0], vals[2]
            if ask1 > 0 and bid1 > 0 and ask1 > bid1:
                spreads.append(ask1 - bid1)
                total = 0
                for i in range(0, min(len(vals), 40), 4):
                    total += vals[i + 1] + vals[i + 3]
                depths.append(total)
                prev_book = (ask1, bid1)
            else:
                prev_book = None

    duration = (last_t - first_t) if (first_t is not None and last_t is not None) else 1.0
    duration = duration or 1.0

    new = types.get(1, 0)
    cancels = types.get(2, 0) + types.get(3, 0)
    exec_visible = types.get(4, 0)
    exec_hidden = types.get(5, 0)

    sizes_sorted = sorted(new_sizes)
    depth_sorted = sorted(depths)
    mean_depth = sum(depths) / len(depths) if depths else 0.0

    hist = [0] * (len(PLACEMENT_EDGES) + 1)
    inside = 0
    for d in placements:
        if d < 0:
            inside += 1
            continue
        placed = False
        for i, e in enumerate(PLACEMENT_EDGES):
            if d <= e:
                hist[i] += 1
                placed = True
                break
        if not placed:
            hist[-1] += 1

    return {
        "symbol": symbol,
        "duration_s": duration,
        "messages": sum(types.values()),
        "new": new,
        "cancels": cancels,
        "exec_visible": exec_visible,
        "exec_hidden": exec_hidden,
        "limit_rate": new / duration,
        "cancel_rate": cancels / duration,
        "market_rate": exec_visible / duration,
        "hidden_rate": exec_hidden / duration,
        "size_mean": sum(new_sizes) / len(new_sizes) if new_sizes else 0,
        "size_p50": quantile(sizes_sorted, 0.50),
        "size_p90": quantile(sizes_sorted, 0.90),
        "size_p99": quantile(sizes_sorted, 0.99),
        "size_round_lot_share": (sum(1 for s in new_sizes if s % 100 == 0) / len(new_sizes)
                                 if new_sizes else 0.0),
        "spread_p50": quantile(sorted(spreads), 0.50),
        "depth_mean_top10": mean_depth,
        "depth_p50_top10": quantile(depth_sorted, 0.50),
        # Per resting order per second. See the module docstring: the
        # denominator is top-10 depth, so this is an overestimate.
        "cancel_rate_per_unit_depth": (cancels / duration / mean_depth) if mean_depth else 0.0,
        "placement_hist": hist,
        "placement_inside_spread": inside,
        "placement_n": len(placements),
        "crossing_limits": crossing_limits,
    }


def report(r, markdown=False):
    h = "## " if markdown else "=== "
    print(f"{h}{r['symbol']}\n")
    print(f"  session {r['duration_s']:.0f}s, {r['messages']:,} messages\n")

    print("  arrival rates (per second)")
    print(f"    limit orders   {r['limit_rate']:8.2f}")
    print(f"    cancels        {r['cancel_rate']:8.2f}")
    print(f"    market orders  {r['market_rate']:8.2f}   (visible executions)")
    print(f"    hidden execs   {r['hidden_rate']:8.2f}   (not modelled)")
    print()
    print("  order size")
    print(f"    mean {r['size_mean']:.1f}   p50 {r['size_p50']}   p90 {r['size_p90']}   "
          f"p99 {r['size_p99']}")
    print(f"    round lots (multiple of 100): {r['size_round_lot_share']*100:.1f}%")
    print()
    print("  book")
    print(f"    median spread      {r['spread_p50']} ticks")
    print(f"    mean top-10 depth  {r['depth_mean_top10']:.0f}")
    print(f"    cancel rate per unit of resting depth: "
          f"{r['cancel_rate_per_unit_depth']:.6f}/s  (overestimate — top-10 denominator)")
    print()
    print("  placement, half-spreads from mid  (1.0 = at the touch)")
    total = r["placement_n"] or 1
    print(f"    crossed the mid: {r['placement_inside_spread']:>8,} "
          f"({r['placement_inside_spread']/total*100:5.2f}%)")
    lo = 0.0
    for i, e in enumerate(PLACEMENT_EDGES):
        c = r["placement_hist"][i]
        tag = ""
        if e <= 1.0:
            tag = "  inside the spread" if e < 1.0 else "  at the touch"
        print(f"    {lo:>5.2f}-{e:<5.2f} {c:>8,} ({c/total*100:5.1f}%){tag}")
        lo = e
    c = r["placement_hist"][-1]
    print(f"    >{PLACEMENT_EDGES[-1]:<10.2f} {c:>8,} ({c/total*100:5.1f}%)")
    print()


def write_config(r, path):
    total = r["placement_n"] or 1
    with open(path, "w") as f:
        f.write(f"# Zero-intelligence order flow, calibrated to {r['symbol']}.\n")
        f.write(f"# Generated by analysis/calibrate.py from a {r['duration_s']:.0f}s session.\n")
        f.write("#\n")
        f.write("# Rates are per second. Placement is a histogram over HALF-SPREADS from\n")
        f.write("# the mid (1.0 = at the touch); the generator samples a bucket then a\n")
        f.write("# uniform offset inside it, and multiplies by the current half-spread.\n")
        f.write("#\n")
        f.write("# cancel_rate_per_depth uses a top-10 denominator and is therefore an\n")
        f.write("# OVERESTIMATE — the real book is deeper than the ten levels LOBSTER shows.\n\n")
        f.write(f"symbol = {r['symbol']}\n")
        f.write(f"limit_rate = {r['limit_rate']:.6f}\n")
        f.write(f"cancel_rate = {r['cancel_rate']:.6f}\n")
        f.write(f"market_rate = {r['market_rate']:.6f}\n")
        f.write(f"cancel_rate_per_depth = {r['cancel_rate_per_unit_depth']:.9f}\n")
        f.write(f"size_p50 = {r['size_p50']}\n")
        f.write(f"size_p90 = {r['size_p90']}\n")
        f.write(f"size_mean = {r['size_mean']:.2f}\n")
        f.write(f"round_lot_share = {r['size_round_lot_share']:.4f}\n")
        f.write(f"spread_p50 = {r['spread_p50']}\n")
        f.write(f"crossed_mid_share = {r['placement_inside_spread']/total:.6f}\n")
        edges = ",".join(f"{e:g}" for e in PLACEMENT_EDGES)
        weights = ",".join(f"{c/total:.6f}" for c in r["placement_hist"])
        f.write(f"placement_edges = {edges}\n")
        f.write(f"placement_weights = {weights}\n")
    print(f"wrote {path}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbol", help="restrict to one symbol")
    ap.add_argument("--out", help="write a generator config to this path")
    args = ap.parse_args()

    syms = []
    for d in sorted(glob.glob(os.path.join(DATA_ROOT, "LOBSTER_SampleFile_*"))):
        s = os.path.basename(d).split("_")[2]
        if args.symbol and s.upper() != args.symbol.upper():
            continue
        syms.append(s)

    if not syms:
        print(f"no LOBSTER data under {DATA_ROOT}", file=sys.stderr)
        return 1

    results = []
    for s in syms:
        m, b = find_pair(s)
        if not m:
            continue
        r = calibrate(s, m, b)
        results.append(r)
        report(r)

    if args.out:
        if len(results) != 1:
            print("--out needs exactly one symbol", file=sys.stderr)
            return 1
        write_config(results[0], args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
