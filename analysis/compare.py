#!/usr/bin/env python3
"""Compare synthetic order flow against the real thing, across many seeds.

Runs tools/zi_sim for N seeds, measures each output with the SAME analysis code
that measures the real LOBSTER data — `stylized_facts.analyse`, imported rather
than reimplemented — and reports where the model does and does not reproduce the
market.

WHAT THE ERROR BARS DO AND DO NOT COVER

The synthetic side gets a real distribution: N independent seeds, reported as
mean ± standard deviation across them. That says how much of a difference is
just sampling noise in the model.

The real side is ONE observation. One session, one symbol, one day. It has no
error bar here and cannot have one from this data, so the comparison is
necessarily one-sided: it asks whether the real value falls inside the spread of
synthetic outcomes, not whether the two distributions differ. A real value far
outside the synthetic range is meaningful. A real value inside it means the
model is *not excluded*, which is weaker than the model being right.

Reported per statistic:
  real            the measured value from LOBSTER
  zi mean ± sd    across seeds
  rel             relative error, |real − zi_mean| / |real|
  z               (real − zi_mean) / zi_sd, when sd > 0

Both are needed. A precise model can be many sigma from the truth while being
only a few percent wrong (large z, small rel) — it captures the phenomenon and
misses the value. A noisy model can be an order of magnitude wrong and only one
sigma away (small z, large rel) — it does not capture anything and merely
cannot be excluded. Reading either column alone gets one of those backwards.

|z| under about 2 means the real value sits within the model's ordinary
variation. Large |z| means the model does not produce that behaviour.

Usage:
    python3 analysis/compare.py --symbol AMZN --seeds 30
"""

import argparse
import math
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stylized_facts as sf  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def mean(xs):
    return sum(xs) / len(xs) if xs else float("nan")


def stdev(xs):
    if len(xs) < 2:
        return 0.0
    m = mean(xs)
    return math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))


def run_seed(config, seed, duration, workdir):
    """One simulated session. Returns the analysis dict for its message log."""
    path = os.path.join(workdir, f"zi_{seed}_message_10.csv")
    r = subprocess.run(
        [os.path.join(ROOT, "build", "zi_sim"),
         "--config", config, "--seed", str(seed),
         "--duration", str(duration), "--messages", path],
        capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  seed {seed} failed: {r.stderr.strip()}", file=sys.stderr)
        return None
    return sf.analyse(f"ZI{seed}", path)


# (label, how to pull the number out of an analysis dict)
STATS = [
    ("cancels / new",            lambda r: r["cancel_ratio"] * 100),
    ("executions / new",         lambda r: r["exec_ratio"] * 100),
    ("effective spread (ticks)", lambda r: r["eff_spread"][0]),
    ("trade-level kurtosis",     lambda r: r["ret_kurtosis"]),
    ("sign ACF lag 1",           lambda r: dict(r["sign_acf"])[1]),
    ("sign ACF lag 2",           lambda r: dict(r["sign_acf"])[2]),
    ("sign ACF lag 5",           lambda r: dict(r["sign_acf"])[5]),
    ("sign ACF lag 10",          lambda r: dict(r["sign_acf"])[10]),
    ("sign ACF lag 50",          lambda r: dict(r["sign_acf"])[50]),
    ("|ret| ACF 10s lag 1",      lambda r: dict(r["absret_acf_fine"])[1]),
    ("|ret| ACF 10s lag 2",      lambda r: dict(r["absret_acf_fine"])[2]),
    ("|ret| ACF 10s lag 5",      lambda r: dict(r["absret_acf_fine"])[5]),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbol", default="AMZN", help="real symbol to compare against")
    ap.add_argument("--config", default="config/zi_amzn.conf")
    ap.add_argument("--seeds", type=int, default=30)
    ap.add_argument("--duration", type=float, default=23400.0)
    ap.add_argument("--markdown", action="store_true")
    args = ap.parse_args()

    msg, _ = None, None
    for d in sorted(os.listdir(sf.DATA_ROOT)):
        if args.symbol in d:
            import glob
            hits = glob.glob(os.path.join(sf.DATA_ROOT, d, "*_message_*.csv"))
            if hits:
                msg = hits[0]
                break
    if not msg:
        print(f"no LOBSTER data for {args.symbol}", file=sys.stderr)
        return 1

    print(f"measuring real {args.symbol} ...", file=sys.stderr)
    real = sf.analyse(args.symbol, msg)

    print(f"running {args.seeds} synthetic sessions of {args.duration:.0f}s ...", file=sys.stderr)
    runs = []
    with tempfile.TemporaryDirectory() as wd:
        for s in range(1, args.seeds + 1):
            r = run_seed(args.config, s, args.duration, wd)
            if r:
                runs.append(r)
            if s % 10 == 0:
                print(f"  {s}/{args.seeds}", file=sys.stderr)
    if len(runs) < 2:
        print("not enough successful runs", file=sys.stderr)
        return 1

    hdr = (f"{'statistic':<26} {'real':>10} {'zi mean':>10} {'zi sd':>8} "
           f"{'rel':>7} {'z':>7}   verdict")
    if args.markdown:
        print(f"| Statistic | Real {args.symbol} | ZI mean | ZI sd | rel | z | Verdict |")
        print("|---|---|---|---|---|---|---|")
    else:
        print()
        print(hdr)
        print("-" * len(hdr))

    for label, get in STATS:
        try:
            rv = get(real)
            vals = [get(r) for r in runs]
        except (KeyError, IndexError, TypeError):
            continue
        vals = [v for v in vals if isinstance(v, (int, float)) and not math.isnan(v)]
        if not vals:
            continue
        m, sd = mean(vals), stdev(vals)
        z = (rv - m) / sd if sd > 0 else float("inf")

        # z alone is not enough, in two directions.
        #
        # A model with huge seed variance produces a small z for a value that is
        # wildly wrong — trade-level kurtosis came out at 175 against a real 11.8
        # with an sd of 117, which is |z| = 1.4 and would read as "reproduced".
        # That is the model being too noisy to exclude, not the model being right.
        #
        # And two numbers that are both approximately zero agree trivially. An
        # autocorrelation of 0.01 matching one of 0.03 says nothing about whether
        # the mechanism is there.
        rel = abs(rv - m) / max(abs(rv), 1e-9)
        both_tiny = abs(rv) < 0.05 and abs(m) < 0.05

        if both_tiny:
            verdict = "both ~0"
        elif abs(z) >= 6:
            verdict = "NOT reproduced"
        elif rel > 0.5:
            verdict = "inconclusive (model too variable)"
        elif abs(z) >= 2:
            verdict = "off"
        else:
            verdict = "reproduced"
        if args.markdown:
            print(f"| {label} | {rv:.3f} | {m:.3f} | {sd:.3f} | {rel*100:.0f}% "
                  f"| {z:+.1f} | {verdict} |")
        else:
            print(f"{label:<26} {rv:>10.3f} {m:>10.3f} {sd:>8.3f} "
                  f"{rel*100:>6.0f}% {z:>+7.1f}   {verdict}")

    if not args.markdown:
        print()
        print(f"{len(runs)} seeds. The real column is a SINGLE session and has no error bar:")
        print("a real value inside the synthetic spread means the model is not excluded,")
        print("which is weaker than the model being right.")
        print()
        print("'inconclusive' means the seed-to-seed variance is large enough to swallow")
        print("a big relative error — the model is too noisy to exclude, not correct.")
        print("'both ~0' means the two values agree only because neither is far from zero.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
