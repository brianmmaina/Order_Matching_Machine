# Zero-intelligence order flow vs. real order flow

Synthetic order flow, calibrated to AMZN (`docs/CALIBRATION.md`) and matched by
the same `MatchingEngine` the gateway uses, compared against the real LOBSTER
stream.

Both sides are measured by the same code — `analysis/compare.py` imports
`stylized_facts.analyse` rather than reimplementing it — because a difference in
the analysis would be indistinguishable from a difference in the market.

**30 seeds**, one 6.5-hour session each.

```bash
python3 analysis/compare.py --symbol AMZN --seeds 30
```

## Results

| Statistic | Real AMZN | ZI mean | ZI sd | rel | z | Verdict |
|---|---|---|---|---|---|---|
| cancels / new | 95.772 | 87.847 | 0.457 | 8% | +17.3 | NOT reproduced |
| executions / new | 6.801 | 14.321 | 0.149 | 111% | -50.3 | NOT reproduced |
| effective spread (ticks) | 300.000 | 693.433 | 8.928 | 131% | -44.1 | NOT reproduced |
| trade-level kurtosis | 11.756 | 174.993 | 117.047 | 1389% | -1.4 | inconclusive (model too variable) |
| sign ACF lag 1 | 0.720 | 0.468 | 0.006 | 35% | +42.5 | NOT reproduced |
| sign ACF lag 2 | 0.575 | 0.174 | 0.009 | 70% | +46.6 | NOT reproduced |
| sign ACF lag 5 | 0.340 | 0.015 | 0.008 | 96% | +40.7 | NOT reproduced |
| sign ACF lag 10 | 0.205 | 0.007 | 0.009 | 97% | +22.9 | NOT reproduced |
| sign ACF lag 50 | 0.085 | -0.001 | 0.009 | 101% | +9.4 | NOT reproduced |
| |ret| ACF 10s lag 1 | 0.155 | 0.376 | 0.043 | 143% | -5.1 | inconclusive (model too variable) |
| |ret| ACF 10s lag 2 | 0.087 | 0.015 | 0.029 | 82% | +2.5 | inconclusive (model too variable) |
| |ret| ACF 10s lag 5 | 0.036 | 0.010 | 0.031 | 74% | +0.9 | both ~0 |

`rel` is relative error; `z` is `(real − zi_mean) / zi_sd`.

**Both columns are necessary, and either alone misleads.** A precise model can be
many sigma from the truth while only a few percent wrong — it captures the
phenomenon and misses the value. A noisy model can be an order of magnitude
wrong and one sigma away — it captures nothing and merely cannot be excluded.
Trade-level kurtosis is the second case: 175 against a real 11.8, which is 1,389%
wrong and |z| = 1.4.

## What this actually says

**Nothing here is reproduced.** With 30 seeds the model's own variance is small
enough that every real value sits far outside it.

That is a sharper and more negative result than a single seed suggested. The
first version of this document, written from one run, called the structural
properties "broadly reproduced" on the strength of an 88% cancel ratio against a
real 96%. With error bars that gap is **17 standard deviations**. The model is
internally consistent — seed to seed it lands on 87.8% ± 0.5% — and consistently
in the wrong place. Being precise is not the same as being right, and one draw
could not tell the difference.

**The cancel ratio is the model's best showing anyway.** 8% relative error, and
qualitatively it gets the fact that matters: a book made overwhelmingly of orders
that will be withdrawn. That falls out of calibrated arrival and cancellation
rates with nothing intending it.

**Trade-sign autocorrelation is where the model fails completely, and it fails in
a specific shape.**

| Lag | 1 | 2 | 5 | 10 | 50 |
|---|---|---|---|---|---|
| Real | 0.720 | 0.575 | 0.340 | 0.205 | 0.085 |
| ZI mean | 0.468 | 0.174 | 0.015 | 0.007 | −0.001 |
| relative error | 35% | 70% | **96%** | **97%** | **101%** |

At lag 1 the model is only 35% low. By lag 5 it is 96% low, and past that it has
nothing at all. The lag-1 value is not memory — it is mechanical, one market
order walking several price levels and printing several same-signed executions
in the same instant. Any model with multi-level fills produces it.

Real order flow decays slowly over a hundred trades. **The question is not
whether correlation exists but how far it reaches**, and only that separates the
two. Deleting strategic behaviour deletes the reach entirely while leaving the
mechanical artifact intact — which is exactly the distinction the experiment was
built to draw.

## The failure that shaped the model

The first generator placed limit orders relative to the **instantaneous**
half-spread, which destroyed the market:

| | Real | Adaptive scale | Fixed scale |
|---|---|---|---|
| Cancels / new | 95.8% | 8.5% | 87.8% |
| Executions / new | 6.8% | 96.7% | 14.3% |
| Effective spread | 300 ticks | **1 tick** | 693 |
| Trade-level kurtosis | 11.8 | **9,833** | 175 |

Orders placed inside the spread narrow it; a narrower spread makes the next
order's offset smaller in ticks; within seconds the book collapses to one tick
and every arrival crosses. The market executes everything and cancels nothing —
the exact inverse of the real one.

Using the calibrated median spread as the placement scale breaks the loop: it is
exogenous and does not move with the book. The instantaneous spread still sets
the reference price; it no longer sets the scale.

Preserved behind `--adaptive-scale` so the failure stays reproducible.

## Limits of this comparison

- **The real side is a single session** and has no error bar. A real value
  outside the synthetic spread is meaningful; one inside would mean the model is
  *not excluded*, which is weaker than the model being right.
- **One symbol, one day, 2012.** Nothing here generalises to markets.
- **The synthetic book is thinner than the real one**, so market orders walk too
  deep. That is the likely common cause of the excess kurtosis, the excess
  execution rate, and the inflated lag-1 correlation.
- **Hidden liquidity is not modelled** — 21% of real executions.
- **The calibrated cancel rate is an overestimate by construction**
  (`docs/CALIBRATION.md`).
- **Not a faithful reimplementation of any published model.** It is a
  zero-intelligence model in the Farmer/Patelli/Zovko spirit, calibrated to this
  data, not a reproduction of their specification.
