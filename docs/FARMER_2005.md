# Replicating Farmer, Patelli & Zovko (2005)

> J. D. Farmer, P. Patelli, I. I. Zovko, "The predictive power of zero
> intelligence in financial markets", *PNAS* **102**(6):2254–2259, 2005.
> [arXiv:cond-mat/0309233](https://arxiv.org/abs/cond-mat/0309233)

Run it: `python3 analysis/farmer2005.py`

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

1. Across the full sample the laws fail, badly, by up to four orders of
   magnitude on diffusion.
2. The failure is **monotonic in `dp/p_c`**, the model's own scope parameter
   (p = 0.017 for diffusion).
3. Where the scope condition holds, the spread ratio is roughly constant.

Ignoring the tick parameter turns a scope condition into an apparent
refutation. That is the substantive lesson, and it is the same mistake in a new
costume as the one at the top of this document: **testing a model outside the
regime it was derived for and reporting the result as though the model had
lost.**

The strongest honest statement: on this sample the tick constraint dominates
the zero-intelligence prediction for low-priced stocks, and the model's own
nondimensional tick size predicts the size of its error.

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
