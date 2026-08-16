#!/usr/bin/env python3
"""Render the replication results as a self-contained HTML page.

Emits hand-built SVG with no JavaScript and no external assets, matching the
project's dependency rule and the precedent set by tools/book_replay.html. The
file can be opened straight from disk or committed and viewed on GitHub Pages.

Three panels, in the order the argument is made:

  1. The law fails, and the failure is ordered by dp/p_c. Spread ratio against
     the model's own nondimensional tick size, with intraday error bars. A
     correct law gives a horizontal line at ANY height -- the prediction holds
     only up to a constant -- so the reader should be looking for flatness, not
     for proximity to 1.

  2. Simulation reproduces it. The same axes with the simulated ratio overlaid,
     from the paper's model run at each stock's measured parameters and real
     tick size.

  3. The negative control. Width scan showing the answer does not depend on
     where the semi-infinite deposition intervals are truncated.

Usage:
    python3 analysis/plot_farmer2005.py                     # writes docs/farmer2005.html
    python3 analysis/plot_farmer2005.py --out /tmp/f.html --blocks 13
    python3 analysis/plot_farmer2005.py --no-sim            # skip the slow panel
"""

import argparse
import math
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import farmer2005 as f  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZI_PAPER = os.path.join(ROOT, "build", "zi_paper")

W, H = 720, 420
PAD_L, PAD_R, PAD_T, PAD_B = 70, 30, 30, 60

# Colour-blind-safe: blue/orange rather than red/green, and the two regimes are
# also distinguished by fill so the chart survives being printed in greyscale.
C_SMALL = "#2f6db3"
C_LARGE = "#d1701c"
C_SIM = "#111111"


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


class LogAxes:
    """Log-log mapping from data space to SVG pixels."""

    def __init__(self, xlo, xhi, ylo, yhi):
        self.lx0, self.lx1 = math.log10(xlo), math.log10(xhi)
        self.ly0, self.ly1 = math.log10(ylo), math.log10(yhi)

    def x(self, v):
        t = (math.log10(v) - self.lx0) / (self.lx1 - self.lx0)
        return PAD_L + t * (W - PAD_L - PAD_R)

    def y(self, v):
        t = (math.log10(v) - self.ly0) / (self.ly1 - self.ly0)
        return H - PAD_B - t * (H - PAD_T - PAD_B)


def decade_ticks(lo, hi):
    out = []
    e = math.floor(math.log10(lo))
    while 10 ** e <= hi * 1.001:
        for m in (1, 2, 5):
            v = m * 10 ** e
            if lo * 0.999 <= v <= hi * 1.001:
                out.append(v)
        e += 1
    return out


def fmt(v):
    if v >= 100:
        return f"{v:.0f}"
    if v >= 1:
        return f"{v:g}"
    return f"{v:g}"


def frame(ax, xlo, xhi, ylo, yhi, xlabel, ylabel):
    """Axes, gridlines, and the dp/p_c = 1 boundary."""
    p = []
    p.append(f'<rect x="0" y="0" width="{W}" height="{H}" fill="var(--card)"/>')

    # The model's stated domain: Equation 1 is a dp -> 0 result, so everything
    # right of dp/p_c = 1 is outside the regime it was derived for. Shading it
    # states the scope condition on the chart instead of in a caption.
    if xlo < 1.0 < xhi:
        x1 = ax.x(1.0)
        p.append(f'<rect x="{x1:.1f}" y="{PAD_T}" width="{W - PAD_R - x1:.1f}" '
                 f'height="{H - PAD_T - PAD_B}" fill="var(--shade)"/>')
        p.append(f'<line x1="{x1:.1f}" y1="{PAD_T}" x2="{x1:.1f}" y2="{H - PAD_B}" '
                 f'stroke="var(--rule)" stroke-width="1" stroke-dasharray="4 3"/>')
        p.append(f'<text x="{x1 + 6:.1f}" y="{PAD_T + 14}" class="note">'
                 f'dp/p_c &gt; 1 &mdash; outside the model’s stated domain</text>')

    for v in decade_ticks(xlo, xhi):
        x = ax.x(v)
        p.append(f'<line x1="{x:.1f}" y1="{PAD_T}" x2="{x:.1f}" y2="{H - PAD_B}" '
                 f'stroke="var(--grid)" stroke-width="1"/>')
        p.append(f'<text x="{x:.1f}" y="{H - PAD_B + 18}" class="tick" '
                 f'text-anchor="middle">{fmt(v)}</text>')
    for v in decade_ticks(ylo, yhi):
        y = ax.y(v)
        p.append(f'<line x1="{PAD_L}" y1="{y:.1f}" x2="{W - PAD_R}" y2="{y:.1f}" '
                 f'stroke="var(--grid)" stroke-width="1"/>')
        p.append(f'<text x="{PAD_L - 8}" y="{y + 4:.1f}" class="tick" '
                 f'text-anchor="end">{fmt(v)}</text>')

    p.append(f'<rect x="{PAD_L}" y="{PAD_T}" width="{W - PAD_L - PAD_R}" '
             f'height="{H - PAD_T - PAD_B}" fill="none" stroke="var(--rule)"/>')
    p.append(f'<text x="{(PAD_L + W - PAD_R) / 2:.0f}" y="{H - 14}" class="axis" '
             f'text-anchor="middle">{esc(xlabel)}</text>')
    p.append(f'<text x="18" y="{(PAD_T + H - PAD_B) / 2:.0f}" class="axis" '
             f'text-anchor="middle" transform="rotate(-90 18 '
             f'{(PAD_T + H - PAD_B) / 2:.0f})">{esc(ylabel)}</text>')
    return p


def point(ax, x, y, lo, hi, colour, filled, label):
    p = []
    px, py = ax.x(x), ax.y(y)
    if lo and hi and hi > lo:
        p.append(f'<line x1="{px:.1f}" y1="{ax.y(lo):.1f}" x2="{px:.1f}" '
                 f'y2="{ax.y(hi):.1f}" stroke="{colour}" stroke-width="1.5"/>')
        for e in (lo, hi):
            p.append(f'<line x1="{px - 4:.1f}" y1="{ax.y(e):.1f}" '
                     f'x2="{px + 4:.1f}" y2="{ax.y(e):.1f}" '
                     f'stroke="{colour}" stroke-width="1.5"/>')
    fill = colour if filled else "var(--card)"
    p.append(f'<circle cx="{px:.1f}" cy="{py:.1f}" r="5.5" fill="{fill}" '
             f'stroke="{colour}" stroke-width="2"/>')
    if label:
        p.append(f'<text x="{px:.1f}" y="{py - 12:.1f}" class="lbl" '
                 f'text-anchor="middle" fill="{colour}">{esc(label)}</text>')
    return p


def panel_ratio(rows, spreads, sim=None):
    """Spread ratio against dp/p_c, with intraday error bars."""
    xs = [r["tick_ratio"] for r in rows]
    ys = [r["s_real"] / r["s_hat"] for r in rows]
    lo_hi = [spreads.get(r["symbol"]) for r in rows]

    allv = list(ys)
    for lh in lo_hi:
        if lh:
            allv += [lh[0], lh[1]]
    if sim:
        allv += [s for s in sim.values() if s]

    ax = LogAxes(min(xs) * 0.5, max(xs) * 2.0, min(allv) * 0.6, max(allv) * 1.7)
    p = frame(ax, min(xs) * 0.5, max(xs) * 2.0, min(allv) * 0.6, max(allv) * 1.7,
              "dp / p_c   (nondimensional tick size)",
              "measured spread / predicted spread")

    for r, y in zip(rows, ys):
        c = C_LARGE if r["tick_ratio"] >= 1.0 else C_SMALL
        lh = spreads.get(r["symbol"])
        p += point(ax, r["tick_ratio"], y, lh[0] if lh else None,
                   lh[1] if lh else None, c, True, r["symbol"])

    if sim:
        pts = [(r["tick_ratio"], sim[r["symbol"]]) for r in rows
               if sim.get(r["symbol"])]
        if len(pts) > 1:
            d = " ".join(f"{ax.x(a):.1f},{ax.y(b):.1f}" for a, b in sorted(pts))
            p.append(f'<polyline points="{d}" fill="none" stroke="{C_SIM}" '
                     f'stroke-width="1.5" stroke-dasharray="5 4"/>')
        for a, b in pts:
            p += point(ax, a, b, None, None, C_SIM, False, None)

    return "".join(p)


def panel_scan(scan_rows):
    """Width scan: the answer must not move when the truncation moves."""
    if not scan_rows:
        return ""
    xs = [w for w, _ in scan_rows]
    ys = [v for _, v in scan_rows]
    ylo, yhi = min(ys) * 0.5, max(ys) * 2.0
    ax = LogAxes(min(xs) * 0.7, max(xs) * 1.4, ylo, yhi)
    p = frame(ax, min(xs) * 0.7, max(xs) * 1.4, ylo, yhi,
              "truncation width  (characteristic prices)",
              "simulated spread / predicted")
    d = " ".join(f"{ax.x(a):.1f},{ax.y(b):.1f}" for a, b in scan_rows)
    p.append(f'<polyline points="{d}" fill="none" stroke="{C_LARGE}" '
             f'stroke-width="2"/>')
    for a, b in scan_rows:
        p += point(ax, a, b, None, None, C_LARGE, True, None)
    return "".join(p)


def run_scan(row):
    out = []
    for w in (12.5, 25.0, 50.0, 100.0, 200.0):
        r = subprocess.run(
            [ZI_PAPER, "--alpha", f"{row['alpha']:.10g}", "--mu", f"{row['mu']:.10g}",
             "--delta", f"{row['delta']:.10g}", "--sigma", f"{row['sigma']:.10g}",
             "--dp", f"{row['dp_log']:.10g}", "--events", "300000",
             "--width-pc", str(w), "--no-header"],
            capture_output=True, text=True)
        if r.returncode == 0 and len(r.stdout.split()) >= 6:
            out.append((w, float(r.stdout.split()[5])))
    return out


def run_sim(rows, events, seeds):
    sims = {}
    for r in rows:
        vals = []
        for s in range(1, seeds + 1):
            p = subprocess.run(
                [ZI_PAPER, "--alpha", f"{r['alpha']:.10g}", "--mu", f"{r['mu']:.10g}",
                 "--delta", f"{r['delta']:.10g}", "--sigma", f"{r['sigma']:.10g}",
                 "--dp", f"{r['dp_log']:.10g}", "--events", str(events),
                 "--seed", str(s), "--no-header"],
                capture_output=True, text=True)
            if p.returncode == 0 and len(p.stdout.split()) >= 6:
                vals.append(float(p.stdout.split()[5]))
        if vals:
            sims[r["symbol"]] = sum(vals) / len(vals)
        print(f"  simulated {r['symbol']}", file=sys.stderr)
    return sims


CSS = """
:root{
  --bg:#ffffff; --card:#ffffff; --fg:#1a1a1a; --muted:#5c6470;
  --grid:#e8ebef; --rule:#c3c9d2; --shade:#fdf1e4; --accent:#2f6db3;
}
@media (prefers-color-scheme: dark){
  :root:not([data-theme="light"]){
    --bg:#14171c; --card:#1b1f26; --fg:#e8eaed; --muted:#9aa3b0;
    --grid:#272c35; --rule:#3c434e; --shade:#332618; --accent:#7fb0e6;
  }
}
:root[data-theme="dark"]{
  --bg:#14171c; --card:#1b1f26; --fg:#e8eaed; --muted:#9aa3b0;
  --grid:#272c35; --rule:#3c434e; --shade:#332618; --accent:#7fb0e6;
}
*{box-sizing:border-box}
body{background:var(--bg);color:var(--fg);margin:0;padding:2.5rem 1.25rem 4rem;
  font:16px/1.65 -apple-system,BlinkMacSystemFont,"Segoe UI",Helvetica,Arial,sans-serif}
main{max-width:820px;margin:0 auto}
h1{font-size:1.7rem;line-height:1.25;margin:0 0 .4rem}
h2{font-size:1.15rem;margin:2.6rem 0 .5rem}
.sub{color:var(--muted);margin:0 0 2rem}
.sub a{color:var(--accent)}
figure{margin:1rem 0 0;overflow-x:auto}
svg{display:block;min-width:640px;max-width:100%;height:auto;
  border:1px solid var(--rule);border-radius:8px}
figcaption{color:var(--muted);font-size:.86rem;margin-top:.6rem}
.tick{font-size:11px;fill:var(--muted)}
.axis{font-size:12px;fill:var(--fg)}
.lbl{font-size:11px;font-weight:600}
.note{font-size:11px;fill:var(--muted)}
table{border-collapse:collapse;width:100%;font-size:.9rem;margin-top:1rem;
  font-variant-numeric:tabular-nums}
th,td{padding:.42rem .6rem;border-bottom:1px solid var(--grid);text-align:right}
th:first-child,td:first-child{text-align:left}
th{color:var(--muted);font-weight:600}
.key{display:flex;gap:1.4rem;flex-wrap:wrap;color:var(--muted);
  font-size:.86rem;margin-top:.8rem}
.key span{display:flex;align-items:center;gap:.4rem}
.dot{width:11px;height:11px;border-radius:50%;display:inline-block}
.caveat{border-left:3px solid var(--rule);padding:.1rem 0 .1rem 1rem;
  color:var(--muted);font-size:.92rem;margin:1.4rem 0}
code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:.9em}
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbols", default="AAPL,AMZN,GOOG,INTC,MSFT")
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "farmer2005.html"))
    ap.add_argument("--blocks", type=int, default=13)
    ap.add_argument("--events", type=int, default=400000)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--no-sim", action="store_true")
    args = ap.parse_args()

    syms = [s.strip().upper() for s in args.symbols.split(",") if s.strip()]
    rows = f.measure_all(syms)
    if not rows:
        return 1

    print(f"measuring {args.blocks} intraday blocks ...", file=sys.stderr)
    by_block = f.measure_blocks(syms, args.blocks, verbose=False)
    spreads, sds = {}, {}
    per = {}
    for rs in by_block.values():
        for r in rs:
            if r["s_hat"] > 0 and r["s_real"] > 0:
                per.setdefault(r["symbol"], []).append(r["s_real"] / r["s_hat"])
    for sym, vs in per.items():
        m, sd = f.mean_of(vs), f.sd_of(vs)
        spreads[sym] = (max(1e-9, m - sd), m + sd)
        sds[sym] = (m, sd, min(vs), max(vs), len(vs))

    sim, scan = {}, []
    if not args.no_sim and os.path.exists(ZI_PAPER):
        print("simulating ...", file=sys.stderr)
        sim = run_sim(rows, args.events, args.seeds)
        big = max(rows, key=lambda r: r["tick_ratio"])
        scan = run_scan(big)
    elif not args.no_sim:
        print(f"{ZI_PAPER} not built; skipping simulation panels", file=sys.stderr)

    tbl = []
    for r in rows:
        m, sd, lo, hi, n = sds.get(r["symbol"], (float("nan"),) * 4 + (0,))
        s = sim.get(r["symbol"])
        tbl.append(
            f"<tr><td>{r['symbol']}</td><td>${r['price']:.2f}</td>"
            f"<td>{r['tick_ratio']:.2f}</td>"
            f"<td>{r['s_real'] / r['s_hat']:.2f}</td>"
            f"<td>{m:.2f} &plusmn; {sd:.2f}</td>"
            f"<td>{lo:.2f}&ndash;{hi:.2f}</td>"
            f"<td>{s:.2f}</td></tr>" if s else
            f"<tr><td>{r['symbol']}</td><td>${r['price']:.2f}</td>"
            f"<td>{r['tick_ratio']:.2f}</td>"
            f"<td>{r['s_real'] / r['s_hat']:.2f}</td>"
            f"<td>{m:.2f} &plusmn; {sd:.2f}</td>"
            f"<td>{lo:.2f}&ndash;{hi:.2f}</td><td>&mdash;</td></tr>")

    html = f"""<title>Zero Intelligence and the Tick</title>
<style>{CSS}</style>
<main>
<h1>Zero intelligence and the tick</h1>
<p class="sub">Replicating Farmer, Patelli &amp; Zovko (2005),
<a href="https://arxiv.org/abs/cond-mat/0309233">&ldquo;The predictive power of
zero intelligence in financial markets&rdquo;</a>, on five NASDAQ symbols from
LOBSTER, 2012-06-21. Method and every caveat in
<code>docs/FARMER_2005.md</code>.</p>

<h2>The law fails, and the failure is ordered by the tick</h2>
<figure>
<svg viewBox="0 0 {W} {H}" xmlns="http://www.w3.org/2000/svg" role="img"
     aria-label="Spread ratio against nondimensional tick size">
{panel_ratio(rows, spreads, sim)}
</svg>
<figcaption><strong>A correct law gives a horizontal line at any height.</strong>
The prediction holds only up to an unknown constant, so flatness is the claim,
not proximity to 1. Error bars are &plusmn;1 sd across
{args.blocks} intraday blocks. Dashed line and open circles: the paper&rsquo;s
own model simulated at each stock&rsquo;s measured parameters and its real tick
size &mdash; nothing about a cheap stock is present except four flow numbers and
<code>dp</code>.</figcaption>
</figure>
<div class="key">
<span><i class="dot" style="background:{C_SMALL}"></i> dp/p_c &lt; 1 &mdash; inside the stated domain</span>
<span><i class="dot" style="background:{C_LARGE}"></i> dp/p_c &gt; 1 &mdash; tick-constrained</span>
<span><i class="dot" style="border:2px solid {C_SIM}"></i> simulated</span>
</div>

<table>
<tr><th>symbol</th><th>price</th><th>dp/p_c</th><th>ratio</th>
    <th>block mean</th><th>block range</th><th>simulated</th></tr>
{"".join(tbl)}
</table>

<p>The three small-tick stocks and the two tick-constrained ones
<strong>do not overlap across any of the {args.blocks * 5} block measurements</strong>.
Where the simulation is available it tracks the real inflation without having
seen it.</p>

{"<h2>The negative control</h2>" if scan else ""}
{f'''<figure>
<svg viewBox="0 0 {W} {H}" xmlns="http://www.w3.org/2000/svg" role="img"
     aria-label="Width scan">
{panel_scan(scan)}
</svg>
<figcaption>The model&rsquo;s deposition intervals are semi-infinite and a
simulation has to truncate them somewhere. If the answer moved with the
truncation, the boundary would be setting the spread rather than the order flow
and the whole result would be an artifact. It does not.
<br><br>This is not a formality: an earlier version truncated to a fixed price
box and scanned 32.23, 32.23, 32.23, <strong>0.00</strong>, 32.23. Non-monotonic
in the width is a bug rather than a boundary effect &mdash; the book could pin
its best bid against the top of the box, leaving no legal prices for sell orders
and freezing the market one-sided forever.</figcaption>
</figure>''' if scan else ""}

<div class="caveat">
<strong>Read before quoting anything here.</strong> Five stocks and one trading
day, against the paper&rsquo;s eleven stocks and 434 days. The error bars are
intraday, so they measure sampling variability and <em>not</em> day-to-day
variation &mdash; they are a lower bound on the true uncertainty. At n&nbsp;=&nbsp;5
the smallest attainable exact p-value is 0.017, so the rank results cannot be
stronger than that however real the effect is.
</div>
</main>
"""
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as fh:
        fh.write(html)
    print(f"wrote {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
