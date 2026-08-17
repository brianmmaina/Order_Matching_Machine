# Replicating Farmer, Patelli & Zovko (2005)

> J. D. Farmer, P. Patelli, I. I. Zovko, "The predictive power of zero
> intelligence in financial markets", *PNAS* **102**(6):2254–2259, 2005.
> [arXiv:cond-mat/0309233](https://arxiv.org/abs/cond-mat/0309233)

Run it: `python3 analysis/farmer2005.py`
Charts: `python3 analysis/plot_farmer2005.py` → [`farmer2005.html`](farmer2005.html)

---

## What went wrong first, because it is the point

The earlier work in this repo ([ZI_COMPARISON.md](ZI_COMPARISON.md)) built a
zero-intelligence order flow model, measured eight stylized facts against one
day of AMZN, found that none of them were reproduced, and reported that as a
result "in the Farmer/Patelli/Zovko spirit."

Then I read the paper.

**It makes none of those claims.** The model is Poisson with equal buy and sell
rates and uniform order deposition. It has no autocorrelated order flow by
construction, and the paper never suggests otherwise. Testing it on sign
autocorrelation and return kurtosis tests a claim nobody made. The earlier
result was not wrong, it was aimed at nothing.

The paper also explicitly warns off the *design* used there. §A6: testing a
single stock over time has "difficulties in getting a clean test," and "it is
not clear that this model should predict anything at all about longitudinal
variations."

What the paper actually does is **cross-sectional**: across stocks, does order
flow predict each stock's spread and price diffusion rate?

---

## The claims

Two scaling laws, with parameters measured in **event time** and **log price**:

| symbol | meaning | units |
|---|---|---|
| `μ` | effective market order rate | shares / event |
| `α` | effective limit order rate **density** | shares / log-price / event |
| `δ` | cancellation rate | 1 / event |
| `σ` | mean limit order size | shares |

```
ε      = δσ/μ                                    nondimensional order granularity
ŝ      = (μ/α) · f(ε),   f(ε) = 0.28 + 1.86·ε^¾  predicted mean spread
D̂      = k · μ^2.5 · δ^0.5 · α^-2 · σ^-0.5        predicted price diffusion rate
```

The test is a regression `log s = A·log ŝ + B`, with the model predicting
**A = 1**. `k` is common to every stock and lands entirely in `B`, so it never
needs to be known. `B` is not a test statistic either — the paper's own price
window makes it arbitrary. **A is the test.**

Paper's result on 11 LSE stocks over 434 trading days:
**A = 0.99 ± 0.10, B = 0.06 ± 0.29, R² = 0.96** for spread; R² = 0.76 for
diffusion.

### The parameter that decides everything

Buried in the supplementary material, and the thing I missed on the first pass:

> A non-dimensional scale parameter based on tick size is constructed by
> dividing the tick size dp by the characteristic price, i.e. `dp/p_c = dp·α/μ`
> … **the properties of the model only depend on the two non-dimensional
> parameters ε and dp/p_c.**

And Equation 1 is derived **in the limit dp → 0**.

So the model has a stated domain of validity, expressed in its own variables,
and `dp/p_c` measures how far each stock sits outside it. Any replication that
does not compute `dp/p_c` is not testing the paper's claim — it is testing a
claim the paper conditioned away.

---

## Results

5 LOBSTER stocks, 2012-06-21, US equities.

### The tick parameter splits the sample cleanly

| | price | `p_c` | `dp/p_c` | regime |
|---|---:|---:|---:|---|
| GOOG | $570.78 | 8.35e-05 | **0.21** | dp → 0 plausible |
| AAPL | $583.15 | 4.91e-05 | **0.35** | dp → 0 plausible |
| AMZN | $222.72 | 5.94e-05 | **0.76** | dp → 0 plausible |
| INTC | $27.05 | 2.14e-05 | **17.30** | tick-constrained |
| MSFT | $30.55 | 1.49e-05 | **22.03** | tick-constrained |

A penny on a $27 stock is seventeen times the characteristic price scale of its
own order flow. For INTC and MSFT the spread is pinned at one tick almost
always; there is no room for the continuous-price mean field result to describe
anything.

### The law fails on the full cross-section

| | `dp/p_c` | `ŝ` | `s` actual | ratio | `D̂` | `D` actual | ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| GOOG | 0.21 | 0.000119 | 0.000518 | **4.36** | 2.75e-10 | 3.19e-09 | **11.6** |
| AAPL | 0.35 | 0.000071 | 0.000263 | **3.70** | 9.47e-11 | 1.19e-09 | **12.6** |
| AMZN | 0.76 | 0.000104 | 0.000587 | **5.66** | 7.07e-11 | 6.59e-09 | **93.2** |
| INTC | 17.30 | 0.000011 | 0.000456 | **39.75** | 3.90e-12 | 3.66e-08 | **9,386** |
| MSFT | 22.03 | 0.000008 | 0.000403 | **50.35** | 1.72e-12 | 3.54e-08 | **20,629** |

A correct law gives a **constant** ratio, not a ratio of 1 — the prediction is
only determined up to `k` and the window choice. So read the ratio column for
*spread*, not for distance from unity.

```
spread    (all 5):  A = +0.038   95% CI −0.400 .. +0.475    R² = 0.024   REJECTED
diffusion (all 5):  A = −0.612   95% CI −1.181 .. −0.042    R² = 0.796   REJECTED
```

The diffusion regression is the interesting failure: R² = 0.80 with a
**negative** slope. That is not noise. It is a strong relationship pointing the
wrong way, which is what a missing control variable looks like.

> **The diffusion column above is measured on the paper's literal A4 clock, and
> that clock does not match `D̂`'s.** Part of this failure is a unit mismatch
> rather than a model failure. See *The two diffusion clocks* below — the
> corrected numbers are smaller and the corrected slope changes sign.

### The error is ordered by the model's own tick parameter

| | ρ | exact p | n |
|---|---:|---:|---:|
| diffusion error vs `dp/p_c` | **+1.000** | **0.0167** | 5 |
| spread error vs `dp/p_c` | +0.900 | 0.0833 | 5 |

The diffusion error is **perfectly rank-ordered** by `dp/p_c` — one of 5! = 120
equally likely orderings under the null, so p = 2/120 two-sided, by exact
enumeration rather than an asymptotic approximation that would not be valid at
n = 5.

The spread ordering is **not significant** (p = 0.083): GOOG and AAPL swap.
Stated as a null result, because it is one.

### The two diffusion clocks

Found by auditing my own strongest result before putting it in a paper.

**The paper measures diffusion on a different clock from the one its parameters
live on.** §A3 defines event time as the count of order placements and
cancellations, and μ, α, δ are all per event on that clock — so `D̂` is in
log-price² **per event**. §A4 then says *"here an event is anything that changes
the midpoint price m"* and measures `V(τ)` over that sequence — so `D` is in
log-price² **per midpoint change**.

The ratio `D/D̂` therefore carries a hidden factor of events-per-midpoint-change.
That is harmless when the factor is roughly constant across the sample, which is
presumably true of the paper's 11 LSE stocks. It is emphatically not true here:

| | dp/p_c | events / midpoint change | raw ratio | matched ratio |
|---|---:|---:|---:|---:|
| GOOG | 0.21 | 5.9 | 11.6 | **1.96** |
| AAPL | 0.35 | 6.0 | 12.6 | **2.11** |
| AMZN | 0.76 | 9.6 | 93.2 | **9.70** |
| INTC | 17.30 | 186.3 | 9,386 | **50.37** |
| MSFT | 22.03 | 158.5 | 20,629 | **130.15** |

**A spread pinned at one tick is a spread whose midpoint rarely moves.** So the
unit mismatch inflates exactly the stocks the tick already inflates, and a raw
comparison double counts the same effect.

What changes when the clocks are matched:

| | raw A4 clock | matched |
|---|---|---|
| spread of ratios | 1,777× | **66×** |
| regression slope A | −0.612 (R² = 0.80) | **+0.171** (R² = 0.39) |
| rank correlation with dp/p_c | ρ = 1.000, p = 0.0167 | **ρ = 1.000, p = 0.0167** |

Three consequences, and the first two are corrections to claims made earlier in
this repo:

1. **"Diffusion fails by four orders of magnitude" was wrong.** On a matched
   clock it is closer to two. The extra factor was bookkeeping.
2. **The dramatic negative slope is substantially an artifact.** Matched, A moves
   from −0.61 to +0.17. The narrative that a strong wrong-way relationship
   signalled a missing control variable still holds — the control was real and it
   was `dp/p_c` — but the specific statistic was inflated.
3. **The rank result is completely unaffected.** ρ = 1.000 at p = 0.0167 on both
   clocks.

Point 3 is why the argument was built on a rank test in the first place. The
statistic that survived a factor-of-27 error in its own input is the one worth
quoting.

### Intraday error bars

The paper averages parameters over 434 days per stock, which is what gives its
real side an error bar. LOBSTER's free sample is one day, so the real side above
is a single observation per stock.

`--blocks 13` cuts the session into 30-minute blocks and re-measures everything
inside each, in one pass:

| | dp/p_c | ratio (full day) | block mean ± sd | block range |
|---|---:|---:|---:|---:|
| GOOG | 0.21 | 4.36 | 4.39 ± 0.88 | 2.70 – 6.00 |
| AAPL | 0.35 | 3.70 | 3.60 ± 0.44 | 2.84 – 4.60 |
| AMZN | 0.76 | 5.66 | 6.13 ± 1.77 | 3.45 – 9.49 |
| INTC | 17.30 | 39.75 | 40.43 ± 5.54 | 28.48 – 49.89 |
| MSFT | 22.03 | 50.35 | 36.52 ± 10.96 | 25.32 – 55.25 |

**The two regimes do not overlap in any of the 65 block measurements.** The
small-tick group tops out at 9.49 and the tick-constrained group bottoms out at
25.32. That separation is a stronger statement than the full-day ratios alone,
because it survives being re-measured 13 times.

The ordering is also stable: the rank correlation between spread error and
`dp/p_c` is **positive in 13 of 13 blocks**, ρ = 0.9 in twelve of them and 1.0 in
one. The persistent ρ = 0.9 rather than 1.0 is the same GOOG/AAPL swap seen over
the full day, so that swap is a real feature of this sample rather than noise.

**What blocks do and do not buy.** They measure *sampling* variability — is this
stock's α the same in the second half hour as the ninth? They do **not** measure
day-to-day variation: overnight gaps, news and regime shifts are invisible inside
one session, so **these error bars are a lower bound on the true ones**. And
blocks from a single day are correlated, so "13 of 13" is a consistency check,
not thirteen independent replications. It answers *is the ordering an artifact of
one measurement window?* — not *how many sigma is the effect?*

One estimate does shift: MSFT's block mean (36.52) sits well below its full-day
ratio (50.35). Per-block δ is more heavily censored than the full-day δ, since an
order outliving its block is dropped rather than credited to a clock it never ran
on. **The blocks are for variability; the full-session row remains the headline
estimate.**

### Inside the stated domain

Restricted to the three stocks with `dp/p_c < 1`, the spread ratios are
**4.36, 3.70, 5.66** — constant within a factor of 1.5, which is what the law
predicts. The diffusion ratios are 11.6, 12.6, 93.2, so AMZN is already
degrading at `dp/p_c = 0.76`.

The corresponding regressions are reported by the script but should not be
quoted: three points give one degree of freedom and a 95% interval on A of
−6.6 to +9.6. That interval covers A = 1 and it covers nearly everything else,
so it is not evidence. **The ratio column and the rank test are the evidence.**

---

## Reading

The paper's claim, as conditioned by the paper itself, is **not refuted here**
— it is confirmed in the weak sense available from five stocks:

1. Across the full sample the laws fail badly — spread ratios spanning 3.7 to
   50, and diffusion ratios spanning 66× on a matched clock. (The raw A4 clock
   makes diffusion look like 1,777×, but most of that is the unit mismatch
   described above, not model failure.)
2. The stocks separate into **two non-overlapping groups** by `dp/p_c`, the
   model's own scope parameter, and the separation survives being re-measured
   in all 13 intraday blocks.
3. Where the scope condition holds, the spread ratio is roughly constant.

On the strength of the evidence: the **two-group separation** in point 2 is the
robust claim, with 65 non-overlapping block measurements behind it. The rank
correlation is weaker than it looks — significant for diffusion (ρ = 1.000,
p = 0.0167) but *not* for spread (ρ = 0.900, p = 0.0833), and at n = 5 the
smallest attainable p-value is 0.0167 regardless. The ordering *within* the
small-tick group is not real: GOOG and AAPL swap, persistently and in every
block. So the defensible statement is that tick-constrained stocks behave
categorically differently, not that the error is monotonic in `dp/p_c`.

Ignoring the tick parameter turns a scope condition into an apparent
refutation. That is the substantive lesson, and it is the same mistake in a new
costume as the one at the top of this document: **testing a model outside the
regime it was derived for and reporting the result as though the model had
lost.**

The strongest honest statement: on this sample the tick constraint dominates
the zero-intelligence prediction for low-priced stocks, and the model's own
nondimensional tick size predicts the size of its error.

---

---

## Is the tick a mechanism, or just a correlate?

Everything above is a correlation on five points, and it has an obvious
alternative explanation. `dp/p_c` is large for exactly INTC and MSFT, which are
also the two cheapest, highest-volume, most heavily quoted names in the sample.
A dozen things that distinguish a $30 stock from a $500 stock would produce the
same ordering.

Simulation separates them, because it can hold everything else fixed.
[`tools/zi_paper.cpp`](../tools/zi_paper.cpp) implements the paper's model —
the austere one: single order size, uniform deposition, equal rates, constant
cancellation — on this project's own matching engine. Run it at each stock's
measured `(α, μ, δ, σ)` and its **real tick size**, and nothing about a cheap
stock is present except four flow parameters and `dp`.

```bash
cmake --build build --target zi_paper
python3 analysis/validate_scaling.py --events 1000000 --seeds 5
```

The prediction, stated before running it: small `dp/p_c` should give a
simulated ratio near 1, and large `dp/p_c` should give a ratio near the
*empirical* ratio for that stock. The second half is the risky one — a coarse
grid inflating the spread is easy, matching the *size* of the real inflation is
not.

| | dp/p_c | sim ratio | real ratio | sim/real |
|---|---:|---:|---:|---:|
| GOOG | 0.21 | 0.66 ± 0.01 | 4.36 | 0.15 |
| AAPL | 0.35 | 0.72 ± 0.01 | 3.70 | 0.19 |
| AMZN | 0.76 | 0.83 ± 0.01 | 5.66 | 0.15 |
| INTC | 17.30 | **32.23** ± 0.00 | **39.75** | **0.81** |
| MSFT | 22.03 | **40.87** ± 0.00 | **50.35** | **0.81** |

**The tick alone reproduces 81% of the observed inflation** for both
tick-constrained stocks — the same fraction for each, which is not something
the run was tuned to produce.

The simulated inflation is **perfectly rank-ordered by `dp/p_c`** (ρ = 1.000,
exact p = 0.017). Inside the simulation that is a much stronger statement than
the same number was on real data: `dp/p_c` and the four flow parameters are the
*only* things that vary, so there is no confounder left to appeal to.

Two things this does **not** show:

* **The remaining 19% is real.** The tick is the dominant term, not the whole
  story. What is left is presumably the things the model deletes — strategic
  quoting, hidden liquidity, heterogeneous order sizes.
* **The small-tick ratio is 0.66–0.83, not 1.0.** The simulation runs about 25%
  *below* the mean field prediction, consistently. `f(ε)` is itself an
  approximation, so a systematic gap of this size is unremarkable, but it is a
  gap and it is not being rounded to "agreement".

The useful by-product: this is an independent exercise of the matching engine —
a continuous double auction driven for millions of events with an answer known
analytically — and it agrees with that answer to within a constant wherever the
analytic result claims to apply.

### The width scan is the negative control

The paper's deposition intervals are semi-infinite and a simulation must
truncate them. If the answer moves when the truncation moves, the boundary is
setting the spread rather than the order flow, and the whole result is an
artifact:

```bash
./build/zi_paper --alpha ... --dp ... --width-scan
```

GOOG returns 0.66, 0.68, 0.65, 0.69, 0.70 across a 16× range of widths; INTC
returns 32.23 at every width.

This was not a formality. The first implementation truncated to a **fixed price
box** centred at zero, and the scan produced 32.23, 32.23, 32.23, **0.00**,
32.23 — non-monotonic in the width, which is the signature of a bug rather than
a boundary effect. The book could pin its best bid against the top of the box,
at which point the sell interval `[b+1, box_top]` was empty and no sell order
could ever arrive again: an absorbing one-sided state that silently reported a
spread of zero. The same fixed box also made the buy and sell arrival rates
depend on where the price sat inside it, quietly breaking the model's
equal-rates assumption.

Anchoring each interval to the opposing best quote — buys on `[a(t)−W, a(t)−1]`,
sells on `[b(t)+1, b(t)+W]` — fixes both: constant equal widths, and prices free
to wander. Without the scan, the fixed-box version would have produced a
plausible-looking table.

---

## Limits — read before quoting any number here

* **5 stocks, not 11. One day, not 434.** The paper averages parameters over
  434 days per stock; every number here rests on a single day with no way to
  assess its stability. An R² on five points means little; that is why the rank
  test carries the argument instead.
* **The rank result is 5 points.** p = 0.017 is exact, but the smallest
  attainable two-sided p at n = 5 is 0.017. It cannot be stronger than this
  regardless of how real the effect is. It is suggestive, not established.
* **US equities in 2012, not the LSE in 1998–2000.** Different market, tick
  regime, and era. Reg NMS pins the increment at $0.01, which is what creates
  the `dp/p_c` spread being exploited here — the LSE sample had no comparable
  split, which is plausibly why the paper never needed the cut.
* **Hidden executions (LOBSTER type 5) are excluded.** The model has no hidden
  liquidity. ~2.4k of 270k messages for AMZN, so `μ` slightly understates true
  market order flow.
* **Order lifetimes are censored.** Orders resting at the close never cancel
  and never enter `δ`, biasing it upward. The paper shares this.
* **One deviation from the printed procedure**, marked `DEVIATION` in the
  source: the paper's formula for `α` omits an event-time normalisation its own
  definition of `μ` includes. `α` is a rate *density*, so it must be divided by
  the event count or `μ/α` is not a price. This cancels in the spread
  prediction but not in `ε` or the diffusion law, and event counts differ 4× in
  this sample, so it does not wash out.
* **Sweep grouping**: consecutive type-4 messages sharing timestamp and
  direction are counted as one market order *placement*. Counting each message
  would inflate the event clock precisely for stocks that trade in size, which
  is a cross-sectional bias in the quantity under test.

## Mapping to LOBSTER

The paper redefines orders by outcome — the transacting part of a marketable
limit order is an "effective market order," the resting part an "effective
limit order." LOBSTER's schema already does exactly this split, which is a
clean fit rather than an approximation: a type 4 message is one execution
against one resting order, a type 1 is an order joining the book, and a fully
marketable order never produces a type 1.
