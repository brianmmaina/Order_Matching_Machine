#!/usr/bin/env python3
"""Is the tick constraint a mechanism, or just a correlate?

analysis/farmer2005.py found that the Farmer/Patelli/Zovko spread law fails
across five LOBSTER stocks, and that the size of the failure is rank-ordered by
the model's own nondimensional tick size dp/p_c -- the parameter Equation 1
assumes away by taking dp -> 0.

That is a correlation on five points, and it has an obvious alternative
explanation. dp/p_c is large exactly for INTC and MSFT, which are also the two
cheapest, highest-volume, most heavily quoted names in the sample. Any of a
dozen things that distinguish a $30 stock from a $500 one would produce the
same ordering.

SIMULATION SEPARATES THOSE, because it can hold everything else fixed.

Run the paper's own model -- tools/zi_paper, the austere specification, not the
richer zi_sim -- at each stock's measured (alpha, mu, delta, sigma) and its real
tick size. Nothing about a $30 stock is present except four flow parameters and
dp. So if simulated INTC reproduces the inflated spread ratio that real INTC
shows, the tick is doing the work.

The prediction, made before running it:

    small dp/p_c   simulated ratio near 1        (the mean field result holds)
    large dp/p_c   simulated ratio near the EMPIRICAL ratio for that stock

The second half is the risky one. It is easy to get an inflated spread out of a
coarse grid; matching the size of the real inflation is not automatic, and a
mismatch would say the tick explains the direction but not the magnitude.

Parameters come from farmer2005.measure_all rather than being re-measured here,
so a difference in measurement cannot masquerade as a difference in the model.

Usage:
    python3 analysis/validate_scaling.py
    python3 analysis/validate_scaling.py --events 2000000 --seeds 5
"""

import argparse
import math
import os
import statistics
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import farmer2005 as f  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZI_PAPER = os.path.join(ROOT, "build", "zi_paper")


def simulate(row, events, seed, width_pc):
    """One run of the paper's model at this stock's measured parameters."""
    cmd = [ZI_PAPER,
           "--alpha", f"{row['alpha']:.10g}",
           "--mu", f"{row['mu']:.10g}",
           "--delta", f"{row['delta']:.10g}",
           "--sigma", f"{row['sigma']:.10g}",
           "--dp", f"{row['dp_log']:.10g}",
           "--events", str(events),
           "--width-pc", str(width_pc),
           "--seed", str(seed),
           "--no-header"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  {row['symbol']} seed {seed} failed: {r.stderr.strip()}", file=sys.stderr)
        return None
    parts = r.stdout.split()
    if len(parts) < 6:
        return None
    # eps dp/p_c half_tk s_hat s_sim ratio resting wide%
    return {"s_sim": float(parts[4]), "ratio": float(parts[5]),
            "resting": float(parts[6])}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbols", default="AAPL,AMZN,GOOG,INTC,MSFT")
    ap.add_argument("--events", type=int, default=1000000)
    ap.add_argument("--seeds", type=int, default=5)
    ap.add_argument("--width-pc", type=float, default=50.0)
    args = ap.parse_args()

    if not os.path.exists(ZI_PAPER):
        print(f"{ZI_PAPER} not built. cmake --build build --target zi_paper",
              file=sys.stderr)
        return 1

    syms = [s.strip().upper() for s in args.symbols.split(",") if s.strip()]
    rows = f.measure_all(syms)
    if not rows:
        return 1

    print(f"\nsimulating the paper's model at each stock's measured parameters")
    print(f"{args.seeds} seeds x {args.events:,} events, width = {args.width_pc} p_c\n")

    out = []
    for r in rows:
        sims = [simulate(r, args.events, s, args.width_pc)
                for s in range(1, args.seeds + 1)]
        sims = [s for s in sims if s]
        if not sims:
            continue
        ratios = [s["ratio"] for s in sims]
        emp = r["s_real"] / r["s_hat"] if r["s_hat"] else float("nan")
        out.append({"symbol": r["symbol"], "tick_ratio": r["tick_ratio"],
                    "sim_mean": statistics.mean(ratios),
                    "sim_sd": statistics.stdev(ratios) if len(ratios) > 1 else 0.0,
                    "emp": emp,
                    "s_sim": statistics.mean(s["s_sim"] for s in sims),
                    "s_real": r["s_real"], "s_hat": r["s_hat"]})
        print(f"  {r['symbol']} done", file=sys.stderr)

    hdr = (f"{'sym':<6} {'dp/p_c':>8} {'sim ratio':>16} {'real ratio':>11} "
           f"{'sim/real':>9}")
    print()
    print(hdr)
    print("-" * len(hdr))
    for o in out:
        agree = o["sim_mean"] / o["emp"] if o["emp"] else float("nan")
        print(f"{o['symbol']:<6} {o['tick_ratio']:>8.2f} "
              f"{o['sim_mean']:>10.2f} +/-{o['sim_sd']:>4.2f} {o['emp']:>11.2f} "
              f"{agree:>9.2f}")

    print("\n  sim ratio   simulated spread / s_hat, the paper's model at these parameters")
    print("  real ratio  actual LOBSTER spread / s_hat, from farmer2005.py")
    print("  sim/real    1.00 means the tick constraint alone accounts for the")
    print("              whole empirical departure from the scaling law")

    # Does the simulated inflation track the real one? With five points a rank
    # test is the honest statistic, for the same reason as in farmer2005.py.
    sp = f.spearman_exact([o["sim_mean"] for o in out], [o["emp"] for o in out])
    if sp:
        print(f"\n  simulated vs real inflation:  rho = {sp['rho']:+.3f}, "
              f"exact p = {sp['p']:.4f} (n = {sp['n']})")
    # The cleaner claim: inside the simulation, dp/p_c is the ONLY thing that
    # varies besides the four flow parameters, so if the simulated inflation is
    # ordered by it there is no confounder left to appeal to.
    sp2 = f.spearman_exact([o["tick_ratio"] for o in out],
                           [o["sim_mean"] for o in out])
    if sp2:
        print(f"  simulated inflation vs dp/p_c: rho = {sp2['rho']:+.3f}, "
              f"exact p = {sp2['p']:.4f} (n = {sp2['n']})")

    small = [o for o in out if o["tick_ratio"] < 1.0]
    large = [o for o in out if o["tick_ratio"] >= 1.0]
    if small:
        m = statistics.mean(o["sim_mean"] for o in small)
        print(f"\n  dp/p_c < 1  ({', '.join(o['symbol'] for o in small)}): "
              f"simulated ratio {m:.2f}")
        print("      near 1 means the mean field result describes the simulation,")
        print("      so the engine and the law agree where the law claims to apply.")
    if large:
        m = statistics.mean(o["sim_mean"] for o in large)
        me = statistics.mean(o["emp"] for o in large)
        print(f"\n  dp/p_c >= 1 ({', '.join(o['symbol'] for o in large)}): "
              f"simulated {m:.1f} vs real {me:.1f}")
        print("      the tick alone, with no other property of a cheap stock present,")
        print(f"      reproduces {100.0*m/me:.0f}% of the observed inflation.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
