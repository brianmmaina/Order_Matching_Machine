# Stylized facts from LOBSTER message data

Measured by `analysis/stylized_facts.py` directly from the message
stream — no book reconstruction, so none of these numbers are affected
by the replay accuracy gap. See `OME_RESEARCH_PLAN.md` (R0).

Sample: NASDAQ, 2012-06-21, one full session per symbol.

## Sample composition

| Symbol | Messages | New | Cancels | Exec (visible) | Exec (hidden) | msg/s |
|---|---|---|---|---|---|---|
| AAPL | 400,391 | 191,015 | 174,386 | 23,658 | 11,332 | 17.1 |
| AMZN | 269,748 | 131,954 | 126,375 | 8,974 | 2,445 | 11.5 |
| GOOG | 147,916 | 71,258 | 64,980 | 7,765 | 3,913 | 6.3 |
| INTC | 624,040 | 304,790 | 286,767 | 28,924 | 3,559 | 26.7 |
| MSFT | 668,765 | 329,566 | 305,785 | 29,798 | 3,616 | 28.6 |

## Fact 1 — almost every order is cancelled, not executed

| Symbol | Cancels / new | Executions / new | Hidden share of executions |
|---|---|---|---|
| AAPL | 91.3% | 12.4% | 32.4% |
| AMZN | 95.8% | 6.8% | 21.4% |
| GOOG | 91.2% | 10.9% | 33.5% |
| INTC | 94.1% | 9.5% | 11.0% |
| MSFT | 92.8% | 9.0% | 10.8% |

## Fact 2 — trade signs are strongly autocorrelated (long memory)

Order flow is not independent: a buy is far more likely to be followed
by another buy. A zero-intelligence model has an ACF of exactly zero at
every lag by construction, so this is the sharpest test of it.

| Symbol | lag 1 | lag 2 | lag 5 | lag 10 | lag 20 | lag 50 | lag 100 |
|---|---|---|---|---|---|---|---|
| AAPL | 0.715 | 0.555 | 0.334 | 0.200 | 0.115 | 0.058 | 0.039 |
| AMZN | 0.720 | 0.575 | 0.340 | 0.205 | 0.132 | 0.085 | 0.009 |
| GOOG | 0.720 | 0.562 | 0.304 | 0.141 | 0.057 | 0.005 | 0.034 |
| INTC | 0.907 | 0.842 | 0.696 | 0.519 | 0.318 | 0.117 | 0.005 |
| MSFT | 0.898 | 0.830 | 0.675 | 0.471 | 0.242 | 0.103 | 0.019 |

## Fact 3 — returns are fat-tailed

Excess kurtosis; a normal distribution scores 0.

| Symbol | Trades | Trade-level | 1-minute | 1-min obs |
|---|---|---|---|---|
| AAPL | 23,658 | 12.5 | 2.0 | 389 |
| AMZN | 8,974 | 11.8 | 2.7 | 384 |
| GOOG | 7,765 | 16.2 | 1.1 | 382 |
| INTC | 28,924 | 23.5 | 4.9 | 373 |
| MSFT | 29,798 | 19.0 | 1.0 | 381 |

## Fact 4 — volatility clustering

Autocorrelation of |1-minute returns|. Positive and slowly decaying means
large moves cluster in time. Zero-intelligence flow produces none.

| Symbol | lag 1 | lag 2 | lag 5 | lag 10 | lag 20 |
|---|---|---|---|---|---|
| AAPL | 0.193 | 0.179 | 0.061 | 0.027 | 0.044 |
| AMZN | 0.116 | 0.082 | -0.019 | 0.105 | 0.022 |
| GOOG | 0.169 | 0.141 | -0.003 | 0.003 | -0.003 |
| INTC | 0.162 | 0.071 | 0.124 | 0.025 | 0.155 |
| MSFT | 0.029 | 0.144 | 0.077 | 0.059 | 0.051 |

## Fact 5 — effective spread, and the sign-convention check

Median (buy price − sell price) over consecutive opposite-sign trades.
They straddle the spread, so this is immune to both drift and bounce.
A positive value confirms the aggressor-sign convention; 100 ticks = 1 cent.

| Symbol | Pairs | Median buy − sell (ticks) | Sign convention |
|---|---|---|---|
| AAPL | 3,366 | 400 | correct |
| AMZN | 1,250 | 300 | correct |
| GOOG | 1,084 | 600 | correct |
| INTC | 1,311 | 100 | correct |
| MSFT | 1,525 | 100 | correct |

## Not measured: price impact

The trade-only proxy for impact — signed price change to the next print —
measures bid-ask bounce rather than impact, and the contamination is worst
where the spread is tightest. On this sample it reports *negative* impact
for INTC and MSFT, i.e. that buying pushes price down.

A sound measure needs the mid price before and after the trade, and the mid
needs a reconstructed book. Impact therefore belongs to the book-dependent
half of the battery and waits on reconstruction fidelity.

## What this means for the research plan

R0 was the de-risking step: can 269K–669K messages of one session show these
effects at all? Verdict per fact:

| Fact | Signal | Usable as a ZI comparison target? |
|---|---|---|
| Cancel-to-execution ratio | **Strong** — 91–96% across all five symbols | Yes |
| Trade-sign autocorrelation | **Strong** — ACF(1) = 0.72–0.91, decaying to ~0 by lag 100 | Yes, and it is the sharpest test |
| Fat-tailed returns | **Strong** — trade-level excess kurtosis 12–24 | Yes |
| Volatility clustering | **Weak** — ACF(1) = 0.03–0.19, noisy | Marginal; see below |
| Effective spread | **Strong** — clean, and matches tick structure | Yes, but needs the book to compare properly |
| Price impact | **Not measurable** from trades alone | No — needs the book |

**The headline is trade-sign autocorrelation.** ACF(1) between 0.72 and 0.91,
decaying slowly over ~100 trades, consistent across five symbols spanning very
different price levels and activity rates. A zero-intelligence model produces
exactly zero at every lag by construction. That is a clean, unambiguous
falsification target, and it alone justifies the comparison.

**Volatility clustering is the weak one, and the cause is sample size.** One
session yields only ~380 one-minute return observations. That is too few to
resolve an autocorrelation reliably, which is why the numbers wander in sign at
higher lags. Options: use finer buckets (more observations, more microstructure
noise), obtain more trading days, or report the fact as inconclusive on this
sample. The third is honest and cheap.

**Price impact was reclassified, and that is R0 earning its cost.** The plan
listed impact as message-derived and therefore available immediately. It is not.
The trade-only proxy measures bid-ask bounce, and on the two penny-spread
symbols it confidently reports that buying pushes price *down*. A correct
measure needs the mid price, and the mid needs a reconstructed book. Finding
this now cost one sitting; finding it after building the simulator would have
cost considerably more and produced a plausible-looking wrong answer.
