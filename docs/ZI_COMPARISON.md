# Zero-intelligence order flow vs. real order flow

Synthetic order flow, calibrated to AMZN (`docs/CALIBRATION.md`) and matched by
the same `MatchingEngine` the gateway uses, compared against the real LOBSTER
stream. Both are analysed by `analysis/stylized_facts.py` — the same code,
unchanged, because the simulator writes its message log in LOBSTER's schema.

Single seed, one 6.5-hour session. Error bars across seeds are still to come.

## Structural properties: broadly reproduced

| | Real AMZN | ZI |
|---|---|---|
| Cancels / new orders | 95.8% | **88.2%** |
| Executions / new orders | 6.8% | 14.4% |
| Median effective spread | 300 ticks | 691 |
| Messages | 269,748 | 269,893 |

Order flow that is cancelled rather than executed, at roughly nine in ten, falls
straight out of the calibrated arrival and cancellation rates. Nothing in the
model intends it.

## Dynamic properties: reproduced at lag 1, absent thereafter

**Trade-sign autocorrelation**

| Lag | 1 | 2 | 5 | 10 | 20 | 50 | 100 |
|---|---|---|---|---|---|---|---|
| Real | 0.720 | 0.575 | 0.340 | 0.205 | 0.132 | 0.085 | 0.009 |
| ZI | 0.474 | 0.183 | 0.016 | 0.013 | −0.003 | −0.007 | −0.005 |

**Volatility clustering, |10-second returns|**

| Lag | 1 | 2 | 3 | 5 | 10 |
|---|---|---|---|---|---|
| Real | 0.155 | 0.087 | 0.098 | 0.036 | 0.078 |
| ZI | 0.378 | 0.014 | 0.010 | −0.010 | 0.002 |

This is the result. **ZI produces correlation at lag 1 and none beyond it.**

The lag-1 value is not evidence of memory — it is mechanical. One market order
walks several price levels and prints several executions, all with the same
sign, within the same instant. Any model with multi-level fills produces that.
It is gone by lag 5.

Real order flow decays slowly over a hundred trades. That persistence is what
strategic behaviour looks like from the outside — order splitting, participants
reacting to each other — and deleting the strategy deletes it entirely. The
distinction is not "does correlation exist" but "how far does it reach", and
only the second one separates the two.

## A failure worth recording

The first version placed limit orders relative to the **instantaneous**
half-spread, which destroyed the market:

| | Real | Adaptive scale | Fixed scale |
|---|---|---|---|
| Cancels / new | 95.8% | 8.5% | 88.2% |
| Executions / new | 6.8% | 96.7% | 14.4% |
| Effective spread | 300 | **1 tick** | 691 |
| Trade-level kurtosis | 11.8 | **9,833** | 155 |

Orders placed inside the spread narrow it; a narrower spread makes the next
order's offset smaller in ticks; within seconds the book collapses to one tick
and every arrival crosses. The market executes everything and cancels nothing —
the exact inverse of the real one.

Using the calibrated median spread as the placement scale breaks the loop: it is
exogenous and does not move with the book. The instantaneous spread still sets
the reference price; it no longer sets the scale.

The broken behaviour is preserved behind `--adaptive-scale` so it stays
reproducible rather than becoming a story about something that used to happen.

## Known gaps

- **Kurtosis is 155 against a real 11.8.** Returns are far too heavy-tailed.
  Likely the same mechanism as the lag-1 sign correlation: market orders walk
  too deep because the synthetic book is thinner than the real one.
- **Executions per order are 2× too high** (14.4% vs 6.8%), for the same reason.
- **Single seed.** Every number here is one draw. Multi-seed runs with error
  bars are the next step; nothing above should be treated as settled.
- **Hidden liquidity is not modelled** — 21% of real executions.
- **The cancel rate is an overestimate by construction** (`docs/CALIBRATION.md`).
