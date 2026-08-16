#!/usr/bin/env python3
"""Test the Farmer/Patelli/Zovko (2005) scaling laws against LOBSTER data.

    J. D. Farmer, P. Patelli, I. I. Zovko,
    "The predictive power of zero intelligence in financial markets",
    PNAS 102(6):2254-2259, 2005.  arXiv:cond-mat/0309233

WHAT THIS TESTS, AND WHY IT REPLACED THE EARLIER COMPARISON

The paper does NOT claim that a zero-intelligence market reproduces the
stylized facts. Its model is Poisson with equal buy and sell rates and uniform
order deposition, so it has no autocorrelated order flow by construction and
never claimed to. Measuring it against sign autocorrelation or return kurtosis
-- which is what analysis/compare.py does -- tests a claim nobody made.

What the paper actually claims is two scaling laws relating order flow to
prices, tested CROSS-SECTIONALLY: across stocks, does order flow predict each
stock's spread and price diffusion rate?

    predicted spread     s_hat = (mu/alpha) * f(eps)
                         f(eps) = 0.28 + 1.86 * eps**0.75
                         eps    = delta*sigma/mu

    predicted diffusion  D_hat = k * mu**2.5 * delta**0.5 * alpha**-2 * sigma**-0.5

Both are verified dimensionally below. The test is a regression of the measured
value on the predicted one,

    log s = A log s_hat + B

with the model predicting A = 1. On 11 LSE stocks over 434 trading days the
paper reports A = 0.99 +/- 0.10, B = 0.06 +/- 0.29, R^2 = 0.96 for the spread
and R^2 = 0.76 for the diffusion rate.

The constant k is not needed: it is common to every stock, so it lands entirely
in the intercept B. B is in any case not a meaningful test statistic here, since
the paper's own choice of price window (below) sets it arbitrarily. A is the
test.

HOW THIS REPLICATION DIFFERS FROM THE PAPER -- READ BEFORE QUOTING THE R^2

  * FIVE stocks, not eleven, and ONE day, not 434. That is the LOBSTER sample.
    An R^2 on five points is not worth much on its own and is reported here
    with a confidence interval on A precisely so it cannot be quoted alone.
    The paper averages parameters over 434 days per stock; every number here
    rests on a single day's estimate with no way to measure its stability.

  * US equities in 2012, not the LSE in 1998-2000. Different market, different
    tick regime, fourteen years later, and the paper's model is fitted to
    neither.

  * Hidden executions (LOBSTER type 5) are excluded. The model has no hidden
    liquidity. They are 2.4k of 270k messages for AMZN, and excluding them
    means mu slightly understates true market order flow.

Deviations forced by the data are marked DEVIATION at the point they occur.

Pure standard library, matching the project's dependency rule.

Usage:
    python3 analysis/farmer2005.py
    python3 analysis/farmer2005.py --markdown
"""

import argparse
import csv
import glob
import math
import os
import sys

DATA_ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                         "data", "lobster")

# LOBSTER message types.
NEW_LIMIT = 1
PARTIAL_CANCEL = 2
FULL_DELETE = 3
EXEC_VISIBLE = 4
EXEC_HIDDEN = 5

# Section A3: "we somewhat arbitrarily choose Q_lower as the 2 percentile ...
# and Q_upper as the 60 percentile". This window is the paper's ONE free
# parameter, and the paper says its choice makes the intercept B arbitrary.
Q_LOWER_PCT = 0.02
Q_UPPER_PCT = 0.60


def quantile(sorted_xs, q):
    if not sorted_xs:
        return 0.0
    return sorted_xs[min(int(q * (len(sorted_xs) - 1)), len(sorted_xs) - 1)]


def find_pair(symbol):
    d = glob.glob(os.path.join(DATA_ROOT, f"*{symbol}*"))
    if not d:
        return None, None
    msg = glob.glob(os.path.join(d[0], f"{symbol}_*_message_*.csv"))
    book = glob.glob(os.path.join(d[0], f"{symbol}_*_orderbook_*.csv"))
    return (msg[0] if msg else None), (book[0] if book else None)


SESSION_START = 34200.0   # 09:30:00, in seconds past midnight
SESSION_END = 57600.0     # 16:00:00


def _new_block():
    return {
        "mo_shares": 0, "lo_sizes": [], "n_events": 0,
        "zeta_all": [], "lifetimes_by_zeta": [], "placed": [],
        "spreads": [], "mids": [], "mid_sum": 0.0, "mid_n": 0,
        "last_mid": None,
    }


def scan(symbol, msg_path, book_path, skip_open_s=0.0, n_blocks=1):
    """One pass over the message and orderbook files.

    Collects everything both halves of the test need: the raw material for the
    four model parameters, the realised spread, and the midpoint path.

    With n_blocks > 1 the session is cut into equal-duration intraday blocks and
    every quantity is accumulated per block, in ONE pass. Returns a single dict
    when n_blocks == 1, a list of dicts otherwise.

    WHY BLOCKS EXIST

    The paper measures each parameter per day and averages over 434 days, which
    is what gives its real side an error bar. LOBSTER's free sample is a single
    day, so the real side here has exactly one observation per stock and no way
    to say whether a measurement is stable or a fluke.

    Intraday blocks are a partial substitute and it is important to be clear
    about what they do and do not buy. They measure SAMPLING variability -- is
    this stock's alpha the same in the second half hour as the ninth? They do
    NOT measure day-to-day variability, so they will UNDERSTATE the true error:
    overnight gaps, news, and regime changes are all invisible inside one
    session. An error bar from blocks is a lower bound on the real one.

    What they do buy, which is worth more than the error bar: the cross-sectional
    ordering can be re-tested independently in each block. One ordering of five
    stocks is one draw. The same ordering holding in block after block is a
    different and much better-supported claim -- with the caveat that blocks
    from one day are not independent of each other.
    """
    # --- effective market / effective limit ------------------------------
    #
    # Section A3 redefines order types by outcome rather than by label: an
    # order is an "effective market order" to the extent it transacts
    # immediately, and an "effective limit order" to the extent it rests. A
    # marketable limit order is split between the two.
    #
    # LOBSTER's schema already does exactly this split, which is a genuinely
    # clean fit rather than an approximation. A type 4 message is one execution
    # against one resting order, so type 4 shares ARE the transacted part. A
    # type 1 message is an order joining the book, so type 1 shares ARE the
    # resting part. A fully marketable order never produces a type 1 at all.
    blocks = [_new_block() for _ in range(max(1, n_blocks))]
    t0 = SESSION_START + skip_open_s
    span = max(1e-9, (SESSION_END - t0) / len(blocks))

    live = {}                # order_id -> (zeta, event_index, block) for resting orders
    last_mo_key = None       # (timestamp, direction) for grouping a sweep

    with open(msg_path, newline="") as mf, open(book_path, newline="") as bf:
        prev_mid = None
        for row, brow in zip(csv.reader(mf), csv.reader(bf)):
            if len(row) < 6:
                continue
            try:
                t = float(row[0])
                mtype = int(row[1])
                oid = int(row[2])
                size = int(row[3])
                price = int(row[4])
                direction = int(row[5])
            except ValueError:
                continue
            if t < t0:
                continue
            bi = min(len(blocks) - 1, max(0, int((t - t0) / span)))
            B = blocks[bi]

            # ---- event-time clock -------------------------------------
            #
            # A3: "the time elapsed during a given period is just the total
            # number of events, including effective market order placements,
            # effective limit order placements, and cancellations."
            #
            # DEVIATION: one aggressive order sweeping several price levels
            # emits several type 4 messages. Those are one market order
            # PLACEMENT, so consecutive type 4s sharing a timestamp and
            # direction are counted as a single event. Counting each message
            # would inflate the clock for exactly the stocks that trade in
            # size, which is a cross-sectional bias -- the thing under test.
            if mtype == EXEC_VISIBLE:
                key = (t, direction)
                if key != last_mo_key:
                    B["n_events"] += 1
                    last_mo_key = key
                B["mo_shares"] += size
            elif mtype in (NEW_LIMIT, PARTIAL_CANCEL, FULL_DELETE):
                B["n_events"] += 1
                last_mo_key = None
            else:
                # Hidden executions and anything else: not in the model.
                last_mo_key = None

            # ---- relative price of a new limit order -------------------
            #
            # A3: "For buy orders we define the relative price as zeta = m - p,
            # where p is the logarithm of the limit price and m is the
            # logarithm of the midquote price. Similarly for sell orders,
            # zeta = p - m."
            #
            # The midquote is the one prevailing BEFORE the order, which is the
            # previous row of the orderbook file. Using the paper's sign
            # convention, zeta > 0 means the order rests away from the mid and
            # zeta < 0 means it crossed.
            if mtype == NEW_LIMIT:
                B["lo_sizes"].append(size)
                if prev_mid is not None and price > 0:
                    p = math.log(price)
                    z = (prev_mid - p) if direction == 1 else (p - prev_mid)
                    B["zeta_all"].append(z)
                    B["placed"].append((z, size))
                    live[oid] = (z, B["n_events"], bi)
            elif mtype == FULL_DELETE:
                # A3 measures delta from cancelled orders only, and lifetime
                # "in terms of number of events happening between the
                # introduction of the order and its subsequent cancellation".
                # A partial cancel (type 2) leaves the order alive, so only a
                # full delete ends a lifetime.
                rec = live.pop(oid, None)
                if rec is not None:
                    # The lifetime belongs to the block the order was PLACED in,
                    # and is measured on that block's event clock. An order that
                    # outlives its block is dropped rather than being credited
                    # to a clock it never ran on -- see the censoring note in
                    # the limits section.
                    if rec[2] == bi:
                        B["lifetimes_by_zeta"].append((rec[0], B["n_events"] - rec[1]))

            # ---- realised spread and midpoint path --------------------
            try:
                ask1, bid1 = int(brow[0]), int(brow[2])
            except (ValueError, IndexError):
                prev_mid = None
                continue
            if ask1 > 0 and bid1 > 0 and ask1 > bid1:
                # "Spread is measured as the daily average of log b(t) -
                # log a(t)", measured after each event with equal weight.
                B["spreads"].append(math.log(ask1) - math.log(bid1))
                B["mid_sum"] += (ask1 + bid1) / 2.0
                B["mid_n"] += 1
                m = math.log((ask1 + bid1) / 2.0)
                # A4: "an event is anything that changes the midpoint price m".
                if B["last_mid"] is None or m != B["last_mid"]:
                    B["mids"].append(m)
                    B["last_mid"] = m
                prev_mid = m
            else:
                prev_mid = None

    out = []
    for i, B in enumerate(blocks):
        out.append({
            "symbol": symbol,
            "block": i,
            "n_events": B["n_events"],
            "mo_shares": B["mo_shares"],
            "lo_sizes": B["lo_sizes"],
            "zeta_all": B["zeta_all"],
            "placed": B["placed"],
            "lifetimes_by_zeta": B["lifetimes_by_zeta"],
            "spreads": B["spreads"],
            "mids": B["mids"],
            "mean_mid": (B["mid_sum"] / B["mid_n"]) if B["mid_n"] else 0.0,
        })
    return out[0] if len(blocks) == 1 else out


def parameters(s):
    """The four model parameters, per Section A3.

    Units are shares, log-price, and events:
        mu     shares per event
        sigma  shares
        alpha  shares per unit log-price per event
        delta  1 / event
    """
    n = s["n_events"] or 1

    # "It is just the ratio of the number of shares of effective market orders
    # (for both buy and sell orders) to the number of events during the
    # trading day."
    mu = s["mo_shares"] / n

    # "sigma_t is the average limit order size in shares for that day." The
    # paper uses the limit order size rather than the market order size, for
    # the stated reason that it matters more theoretically.
    sigma = (sum(s["lo_sizes"]) / len(s["lo_sizes"])) if s["lo_sizes"] else 0.0

    z_sorted = sorted(s["zeta_all"])
    q_lo = quantile(z_sorted, Q_LOWER_PCT)
    q_hi = quantile(z_sorted, Q_UPPER_PCT)
    width = q_hi - q_lo

    # "alpha_t = L/(Q_upper_t - Q_lower_t) where L is the total number of
    # shares of effective limit orders within the price interval."
    #
    # DEVIATION: the paper's printed formula omits an event-time
    # normalisation that its own definition of mu includes. alpha is a rate
    # DENSITY -- shares per price per time -- so with time measured in events
    # it must be divided by the event count, exactly as mu is. Left
    # unnormalised, alpha would carry units of shares per price and the ratio
    # mu/alpha would not be a price at all.
    #
    # This matters beyond bookkeeping. The n cancels in mu/alpha, so the
    # predicted spread is unaffected either way, but eps = delta*sigma/mu
    # depends on mu alone and the diffusion law depends on both. Since stocks
    # here differ in event count by a factor of four, the normalisation does
    # not cancel across the cross-section.
    L = sum(sz for z, sz in s["placed"] if q_lo <= z <= q_hi)
    alpha = (L / width / n) if width > 0 else 0.0

    # "we base our estimate for delta only on canceled limit orders within the
    # range of the same relative price boundaries ... delta_t is the inverse of
    # the average lifetime of a canceled limit order in the above price range."
    lifes = [lt for z, lt in s["lifetimes_by_zeta"] if q_lo <= z <= q_hi and lt > 0]
    mean_life = (sum(lifes) / len(lifes)) if lifes else 0.0
    delta = (1.0 / mean_life) if mean_life > 0 else 0.0

    return {
        "mu": mu, "sigma": sigma, "alpha": alpha, "delta": delta,
        "q_lo": q_lo, "q_hi": q_hi, "window_shares": L,
        "n_cancels_in_window": len(lifes), "mean_life_events": mean_life,
        "n_events": n,
    }


def diffusion_rate(mids, max_lag=512):
    """Section A4: regress V(tau) against tau assuming V(tau) = D*tau.

    "We measure the intraday price diffusion by computing the variance V(tau)
    of m(i+tau) - m(i), averaged over different intraday events i ... We use an
    ordinary least squares regression to estimate D_t, weighting each value of
    tau by the square root of the number of independent observations."

    Independent observations at lag tau number M/tau, not M: overlapping
    increments share data. That weighting is what stops long lags, which have
    the fewest independent samples and the most scatter, from dominating.
    """
    M = len(mids)
    if M < 64:
        return float("nan")

    lags = []
    lag = 1
    while lag <= min(max_lag, M // 8):
        lags.append(lag)
        lag = max(lag + 1, int(lag * 1.5))

    num = den = 0.0
    for tau in lags:
        diffs = [mids[i + tau] - mids[i] for i in range(0, M - tau)]
        if len(diffs) < 2:
            continue
        mean = sum(diffs) / len(diffs)
        var = sum((d - mean) ** 2 for d in diffs) / (len(diffs) - 1)
        w = math.sqrt(max(1.0, len(diffs) / tau))   # sqrt(independent obs)
        num += w * tau * var
        den += w * tau * tau
    return (num / den) if den > 0 else float("nan")


def predict(p):
    """The two scaling laws, Equations 1 and 2.

    Dimensional check, with [mu]=sh/ev, [alpha]=sh/(lp*ev), [delta]=1/ev,
    [sigma]=sh:

        eps   = delta*sigma/mu = (1/ev)(sh)/(sh/ev)                  = 1     OK
        s_hat = mu/alpha       = (sh/ev)/(sh/(lp*ev))                = lp    OK
        D_hat = mu^2.5 delta^0.5 alpha^-2 sigma^-0.5                 = lp^2/ev

    The last is the one worth checking by hand, since the paper's exponents
    survived text extraction without their signs:
        sh^2.5 ev^-2.5 . ev^-0.5 . lp^2 ev^2 sh^-2 . sh^-0.5 = lp^2 ev^-1   OK
    """
    mu, alpha, delta, sigma = p["mu"], p["alpha"], p["delta"], p["sigma"]
    if min(mu, alpha, delta, sigma) <= 0:
        return float("nan"), float("nan"), float("nan"), float("nan")
    eps = delta * sigma / mu
    p_c = mu / alpha                      # characteristic price scale
    s_hat = p_c * (0.28 + 1.86 * eps ** 0.75)
    # k is common to all stocks and lands in the regression intercept.
    d_hat = mu ** 2.5 * delta ** 0.5 * alpha ** -2.0 * sigma ** -0.5
    return eps, p_c, s_hat, d_hat


def regress(xs, ys):
    """OLS y = A x + B, with standard errors and R^2."""
    n = len(xs)
    if n < 3:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    if sxx == 0:
        return None
    A = sxy / sxx
    B = my - A * mx
    resid = [y - (A * x + B) for x, y in zip(xs, ys)]
    sse = sum(r * r for r in resid)
    sst = sum((y - my) ** 2 for y in ys)
    dof = n - 2
    s2 = sse / dof if dof > 0 else float("nan")
    se_A = math.sqrt(s2 / sxx) if dof > 0 else float("nan")
    se_B = math.sqrt(s2 * (1.0 / n + mx * mx / sxx)) if dof > 0 else float("nan")
    r2 = 1.0 - sse / sst if sst > 0 else float("nan")
    return {"A": A, "B": B, "se_A": se_A, "se_B": se_B, "r2": r2, "dof": dof, "n": n}


# Two-sided 95% t critical values. n = 5 stocks gives dof = 3, where the
# normal approximation is badly wrong: 3.18 against 1.96.
T95 = {1: 12.71, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447,
       7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228}


def spearman_exact(xs, ys):
    """Rank correlation with an EXACT permutation p-value.

    With five stocks, a least-squares regression has three degrees of freedom
    and a confidence interval on the slope wide enough to cover almost any
    hypothesis -- it cannot reject anything, so it cannot support anything
    either. A rank statistic can. If the model's error is perfectly ordered by
    dp/p_c, that ordering is one of 5! = 120 equally likely arrangements under
    the null, which is a real p-value rather than an appeal to asymptotics.

    Exact enumeration, so it is valid at n = 5 where a t or z approximation is
    not.
    """
    from itertools import permutations
    n = len(xs)
    if n < 3 or n > 8:
        return None

    def ranks(vs):
        order = sorted(range(len(vs)), key=lambda i: vs[i])
        rk = [0.0] * len(vs)
        for pos, i in enumerate(order):
            rk[i] = float(pos)
        return rk

    rx, ry = ranks(xs), ranks(ys)

    def rho(a, b):
        n_ = len(a)
        ma, mb = sum(a) / n_, sum(b) / n_
        num = sum((p - ma) * (q - mb) for p, q in zip(a, b))
        da = math.sqrt(sum((p - ma) ** 2 for p in a))
        db = math.sqrt(sum((q - mb) ** 2 for q in b))
        return num / (da * db) if da and db else float("nan")

    obs = rho(rx, ry)
    hits = tot = 0
    for perm in permutations(ry):
        tot += 1
        if abs(rho(rx, list(perm))) >= abs(obs) - 1e-12:
            hits += 1
    return {"rho": obs, "p": hits / tot, "n": n, "perms": tot}


def report_regression(name, r, markdown=False):
    if r is None:
        print(f"{name}: too few stocks to regress")
        return
    t = T95.get(r["dof"], 1.96)
    loA, hiA = r["A"] - t * r["se_A"], r["A"] + t * r["se_A"]
    covers = loA <= 1.0 <= hiA
    print(f"\n{name}")
    print(f"  A  = {r['A']:+.3f}  (95% CI {loA:+.3f} .. {hiA:+.3f})   "
          f"model predicts A = 1: {'CONSISTENT' if covers else 'REJECTED'}")
    print(f"  B  = {r['B']:+.3f}  (arbitrary -- set by the price window)")
    print(f"  R^2 = {r['r2']:.3f}   n = {r['n']} stocks, dof = {r['dof']}")
    if not covers:
        print("  the interval excludes 1, so the law does not hold on this sample")
    elif hiA - loA > 1.0:
        print("  NOTE: the interval is wider than 1.0. It covers A=1, but it would")
        print("  cover almost anything. This is 5 points; it is weak evidence, not")
        print("  confirmation.")


def _row_from_scan(s):
    """Turn one scan result into parameters, predictions and measured values."""
    p = parameters(s)
    eps, p_c, s_hat, d_hat = predict(p)
    s_real = (sum(s["spreads"]) / len(s["spreads"])) if s["spreads"] else float("nan")
    d_real = diffusion_rate(s["mids"])

    # Nondimensional tick size, the model's SECOND control parameter:
    # "A non-dimensional scale parameter based on tick size is constructed by
    # dividing the tick size dp by the characteristic price, i.e.
    # dp/p_c = dp*alpha/mu ... the properties of the model only depend on the
    # two non-dimensional parameters eps and dp/p_c."
    #
    # In log-price terms one cent on a share priced P is log(P+1c) - log(P).
    # LOBSTER quotes in units of 1/10000 dollar, so the US Reg NMS minimum
    # increment of $0.01 is 100 of those units.
    mm = s["mean_mid"]
    dp_log = math.log(mm + 100.0) - math.log(mm) if mm > 0 else float("nan")
    tick_ratio = (dp_log / p_c) if p_c > 0 else float("nan")

    # ---- the two diffusion clocks -------------------------------------
    #
    # The paper measures diffusion on a DIFFERENT clock from the one its
    # parameters live on, and in this sample that matters enormously.
    #
    # A3 defines event time as the count of order placements and cancellations,
    # and mu, alpha, delta are all per event on that clock. So D_hat is in
    # log-price^2 per event.
    #
    # A4 then says "here an event is anything that changes the midpoint price
    # m", and measures V(tau) over that sequence. So D_real is in log-price^2
    # per MIDPOINT CHANGE.
    #
    # The ratio D_real/D_hat therefore carries a hidden factor of events per
    # midpoint change. That is harmless if the factor is roughly constant across
    # the sample, which is presumably true of the paper's 11 LSE stocks. It is
    # emphatically not true here: the factor is ~6 for GOOG and ~186 for INTC,
    # because a spread pinned at one tick is a spread whose midpoint rarely
    # moves. The tick constraint suppresses midpoint changes directly, so the
    # unit mismatch inflates exactly the stocks the tick already inflates, and
    # a raw comparison double counts it.
    #
    # Both are reported. d_real is the literal A4 quantity; d_real_ev converts
    # it to the same per-event clock as D_hat, which is the apples-to-apples
    # comparison.
    n_mid = len(s["mids"])
    ev_per_midchg = (p["n_events"] / n_mid) if n_mid else float("nan")
    d_real_ev = (d_real / ev_per_midchg) if ev_per_midchg > 0 else float("nan")

    return {"symbol": s["symbol"], "block": s.get("block", 0), **p,
            "eps": eps, "p_c": p_c, "s_hat": s_hat, "d_hat": d_hat,
            "s_real": s_real, "d_real": d_real, "n_mid": n_mid,
            "ev_per_midchg": ev_per_midchg, "d_real_ev": d_real_ev,
            "price": mm / 10000.0, "dp_log": dp_log, "tick_ratio": tick_ratio}


def measure_blocks(symbols, n_blocks, skip_open=0.0, verbose=True):
    """Per-symbol, per-intraday-block measurements.

    Returns {block_index: [row, ...]}, each inner list sorted by dp/p_c, so the
    cross-sectional test can be re-run independently inside every block.
    """
    by_block = {i: [] for i in range(n_blocks)}
    for sym in symbols:
        m, b = find_pair(sym)
        if not m or not b:
            print(f"skipping {sym}: no message/orderbook pair", file=sys.stderr)
            continue
        if verbose:
            print(f"scanning {sym} in {n_blocks} blocks ...", file=sys.stderr)
        for s in scan(sym, m, b, skip_open, n_blocks):
            if s["n_events"] < 500 or not s["spreads"]:
                continue          # too thin to estimate anything from
            by_block[s["block"]].append(_row_from_scan(s))
    for i in by_block:
        by_block[i].sort(key=lambda r: r["tick_ratio"])
    return {i: rs for i, rs in by_block.items() if len(rs) >= 3}


def measure_all(symbols, skip_open=0.0, verbose=True):
    """Measure every symbol over the whole session.

    Shared with analysis/validate_scaling.py: the simulator must run on exactly
    the parameters the empirical test used, or a difference in the measurement
    would look like a difference in the model. Same reason analysis/compare.py
    imports stylized_facts rather than reimplementing it.
    """
    rows = []
    for sym in symbols:
        m, b = find_pair(sym)
        if not m or not b:
            print(f"skipping {sym}: no message/orderbook pair", file=sys.stderr)
            continue
        if verbose:
            print(f"scanning {sym} ...", file=sys.stderr)
        rows.append(_row_from_scan(scan(sym, m, b, skip_open)))
    rows.sort(key=lambda r: r["tick_ratio"])
    return rows


def report_blocks(symbols, n_blocks, skip_open=0.0):
    """Re-run the cross-sectional test independently inside each intraday block.

    This is the closest available substitute for the paper's 434 trading days,
    and it is a substitute for only one of the two things those days buy.

    It DOES give the real side a spread: each stock's ratio now has a mean and a
    standard deviation instead of a single number, so "GOOG is at 4.36" becomes
    a claim with a width.

    It does NOT give day-to-day variation. Overnight gaps, news and regime
    shifts are invisible inside one session, so these error bars are a LOWER
    BOUND on the true ones. Blocks from one day are also not independent of each
    other, so "held in 12 of 13 blocks" is a description, not a p-value with 13
    degrees of freedom behind it.
    """
    by_block = measure_blocks(symbols, n_blocks, skip_open)
    if not by_block:
        print("\nnot enough data per block", file=sys.stderr)
        return

    print("\n" + "=" * 72)
    print(f"INTRADAY BLOCKS -- {len(by_block)} usable blocks of "
          f"{(SESSION_END - SESSION_START - skip_open) / n_blocks / 60:.0f} min")
    print("=" * 72)

    # Per stock, across blocks.
    per_sym = {}
    for rs in by_block.values():
        for r in rs:
            if r["s_hat"] > 0 and r["s_real"] > 0:
                per_sym.setdefault(r["symbol"], []).append(
                    (r["tick_ratio"], r["s_real"] / r["s_hat"]))

    hdr = (f"{'sym':<6} {'blocks':>7} {'dp/p_c mean':>12} {'ratio mean':>11} "
           f"{'ratio sd':>9} {'min':>8} {'max':>8}")
    print("\nspread ratio, measured independently in each block")
    print(hdr)
    print("-" * len(hdr))
    for sym in sorted(per_sym, key=lambda s: mean_of([t for t, _ in per_sym[s]])):
        ts = [t for t, _ in per_sym[sym]]
        rs = [v for _, v in per_sym[sym]]
        print(f"{sym:<6} {len(rs):>7} {mean_of(ts):>12.2f} {mean_of(rs):>11.2f} "
              f"{sd_of(rs):>9.2f} {min(rs):>8.2f} {max(rs):>8.2f}")

    # Does the ordering survive block by block?
    print("\ncross-sectional ordering, re-tested inside each block")
    print(f"{'block':>6} {'n':>3} {'rho(spread err, dp/p_c)':>25} {'p':>8}")
    print("-" * 46)
    perfect = 0
    positive = 0
    total = 0
    for i in sorted(by_block):
        rs = [r for r in by_block[i] if r["s_hat"] > 0 and r["s_real"] > 0]
        if len(rs) < 3:
            continue
        sp = spearman_exact([r["tick_ratio"] for r in rs],
                            [r["s_real"] / r["s_hat"] for r in rs])
        if not sp:
            continue
        total += 1
        if sp["rho"] > 0.999:
            perfect += 1
        if sp["rho"] > 0:
            positive += 1
        print(f"{i:>6} {len(rs):>3} {sp['rho']:>25.3f} {sp['p']:>8.4f}")

    if total:
        print(f"\n  positive in {positive}/{total} blocks, "
              f"perfectly ordered in {perfect}/{total}")
        print("  Blocks from a single session are correlated, so this is a")
        print("  consistency check, not 13 independent replications. It answers")
        print("  'is the ordering an artifact of one measurement window?' and")
        print("  not 'how many sigma is the effect?'")


def mean_of(xs):
    return sum(xs) / len(xs) if xs else float("nan")


def sd_of(xs):
    if len(xs) < 2:
        return 0.0
    m = mean_of(xs)
    return math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbols", default="AAPL,AMZN,GOOG,INTC,MSFT")
    ap.add_argument("--skip-open", type=float, default=0.0,
                    help="seconds of the session to skip; the paper excludes the "
                         "opening auction, which LOBSTER samples already omit")
    ap.add_argument("--markdown", action="store_true")
    ap.add_argument("--blocks", type=int, default=0,
                    help="also split the session into N intraday blocks and "
                         "re-run the cross-sectional test inside each")
    args = ap.parse_args()

    rows = measure_all([x.strip().upper() for x in args.symbols.split(",") if x.strip()],
                       args.skip_open)

    if not rows:
        print(f"no LOBSTER data under {DATA_ROOT}", file=sys.stderr)
        return 1

    print("\nmeasured model parameters (event time, log price, shares)")
    hdr = (f"{'sym':<6} {'events':>8} {'mu':>9} {'sigma':>8} {'alpha':>11} "
           f"{'delta':>9} {'eps':>8}")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        print(f"{r['symbol']:<6} {r['n_events']:>8,} {r['mu']:>9.4f} {r['sigma']:>8.1f} "
              f"{r['alpha']:>11.1f} {r['delta']:>9.6f} {r['eps']:>8.3f}")

    # The model has TWO control parameters and Equation 1 is derived in the
    # limit dp -> 0. dp/p_c says how badly each stock violates that limit, so
    # it is the first thing to look at, not a diagnostic tacked on afterwards.
    print("\nnondimensional tick size -- the model's second control parameter")
    hdr3 = f"{'sym':<6} {'price':>9} {'p_c':>11} {'dp/p_c':>9}   regime"
    print(hdr3)
    print("-" * len(hdr3))
    for r in rows:
        regime = "tick-constrained" if r["tick_ratio"] >= 1.0 else "dp -> 0 plausible"
        print(f"{r['symbol']:<6} {r['price']:>8.2f}  {r['p_c']:>11.3e} "
              f"{r['tick_ratio']:>9.2f}   {regime}")

    print("\npredicted vs actual")
    hdr2 = (f"{'sym':<6} {'dp/p_c':>8} {'s_hat':>11} {'s_real':>11} {'ratio':>7} "
            f"{'D_hat':>12} {'D_real':>12} {'ratio':>9}")
    print(hdr2)
    print("-" * len(hdr2))
    for r in rows:
        ratio = (r["s_real"] / r["s_hat"]) if r["s_hat"] else float("nan")
        dratio = (r["d_real"] / r["d_hat"]) if r["d_hat"] else float("nan")
        print(f"{r['symbol']:<6} {r['tick_ratio']:>8.2f} {r['s_hat']:>11.6f} "
              f"{r['s_real']:>11.6f} {ratio:>7.2f} {r['d_hat']:>12.3e} "
              f"{r['d_real']:>12.3e} {dratio:>9.1f}")
    print("\n  A perfect law gives a CONSTANT ratio, not a ratio of 1: the")
    print("  prediction is up to an overall constant (k, and the price window).")
    print("  So read the ratio column for spread, not scatter.")

    # The two diffusion clocks. See _row_from_scan for why these differ.
    print("\ndiffusion on a matched clock")
    hdr3 = (f"{'sym':<6} {'dp/p_c':>8} {'ev/midchg':>10} {'raw ratio':>12} "
            f"{'matched':>10}")
    print(hdr3)
    print("-" * len(hdr3))
    for r in rows:
        dratio = (r["d_real"] / r["d_hat"]) if r["d_hat"] else float("nan")
        dev = (r["d_real_ev"] / r["d_hat"]) if r["d_hat"] else float("nan")
        print(f"{r['symbol']:<6} {r['tick_ratio']:>8.2f} "
              f"{r['ev_per_midchg']:>10.1f} {dratio:>12.1f} {dev:>10.2f}")
    raws = [r["d_real"] / r["d_hat"] for r in rows if r["d_hat"] > 0]
    matched = [r["d_real_ev"] / r["d_hat"] for r in rows if r["d_hat"] > 0]
    if raws and matched:
        print(f"\n  spread of raw ratios      {max(raws) / min(raws):>8.0f}x")
        print(f"  spread of matched ratios  {max(matched) / min(matched):>8.0f}x")
    print("  D_hat is per event; the raw D_real is per MIDPOINT CHANGE, and this")
    print("  sample's events-per-midpoint-change runs from 6 to 186. A pinned")
    print("  spread is a spread whose midpoint rarely moves, so the mismatch")
    print("  inflates exactly the stocks the tick already inflates. The matched")
    print("  column is the apples-to-apples comparison; the raw one double counts.")

    def ok(rs, key_hat, key_real):
        return [r for r in rs if r[key_hat] > 0 and r[key_real] > 0
                and not math.isnan(r[key_hat]) and not math.isnan(r[key_real])]

    def both(rs, label):
        report_regression(f"spread {label}:    log s = A log s_hat + B",
                          regress([math.log(r["s_hat"]) for r in ok(rs, "s_hat", "s_real")],
                                  [math.log(r["s_real"]) for r in ok(rs, "s_hat", "s_real")]))
        report_regression(f"diffusion {label}, raw A4 clock:  log D = A log D_hat + B",
                          regress([math.log(r["d_hat"]) for r in ok(rs, "d_hat", "d_real")],
                                  [math.log(r["d_real"]) for r in ok(rs, "d_hat", "d_real")]))
        report_regression(f"diffusion {label}, matched clock: log D = A log D_hat + B",
                          regress([math.log(r["d_hat"]) for r in ok(rs, "d_hat", "d_real_ev")],
                                  [math.log(r["d_real_ev"]) for r in ok(rs, "d_hat", "d_real_ev")]))

    print("\n" + "=" * 72)
    print("ALL STOCKS")
    print("=" * 72)
    both(rows, "(all)")

    # Restricting to dp/p_c < 1 is not cherry-picking: it is the model's own
    # stated domain. Equation 1 is a dp -> 0 result, and dp/p_c is the paper's
    # own measure of how far a stock is from that limit. The cut is stated in
    # the model's variables and fixed before seeing which stocks it drops.
    #
    # It is still only three points, which is not enough to regress. Both
    # regressions are printed regardless of which looks better.
    small = [r for r in rows if r["tick_ratio"] < 1.0]
    if small and len(small) < len(rows):
        print("\n" + "=" * 72)
        print(f"dp/p_c < 1 ONLY -- the model's stated domain "
              f"({', '.join(r['symbol'] for r in small)})")
        print("=" * 72)
        both(small, "(small tick)")

    # The headline result. Does the model's own tick parameter order its error?
    print("\n" + "=" * 72)
    print("DOES dp/p_c EXPLAIN THE ERROR?")
    print("=" * 72)
    tr = [r["tick_ratio"] for r in rows]
    for label, num, den in (("spread", "s_real", "s_hat"),
                            ("diffusion (raw A4 clock)", "d_real", "d_hat"),
                            ("diffusion (matched clock)", "d_real_ev", "d_hat")):
        err = [r[num] / r[den] for r in rows]
        sp = spearman_exact(tr, err)
        if sp is None:
            continue
        print(f"\n  {label} error vs dp/p_c:  rho = {sp['rho']:+.3f}, "
              f"exact p = {sp['p']:.4f}  ({sp['perms']} permutations, n = {sp['n']})")
        if abs(sp["rho"]) > 0.999:
            print(f"    perfectly rank-ordered: the {label} error is monotonic in the")
            print("    model's own nondimensional tick size, which is the scope")
            print("    condition Equation 1 was derived under (the dp -> 0 limit).")

    if args.blocks > 1:
        report_blocks([r["symbol"] for r in rows], args.blocks, args.skip_open)

    print("\npaper, for reference: 11 LSE stocks over 434 trading days,")
    print("  spread     A = 0.99 +/- 0.10, B = 0.06 +/- 0.29, R^2 = 0.96")
    print("  diffusion  R^2 = 0.76")
    print("This replication has 5 stocks and 1 day. It is a much weaker test, and")
    print("agreement here is far less informative than agreement there.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
