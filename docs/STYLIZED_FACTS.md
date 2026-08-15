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

Autocorrelation of |returns|. Positive and slowly decaying means large
moves cluster in time. Zero-intelligence flow produces none.

The `band` column is ±2 standard errors under the null of no
autocorrelation (2/√n). Values inside it are not distinguishable from
noise, which is the whole story at the 1-minute horizon.

**1-minute returns** — conventional horizon, but only ~380 observations
from a single session:

| Symbol | n | band | lag 1 | lag 2 | lag 5 | lag 10 | lag 20 |
|---|---|---|---|---|---|---|---|
| AAPL | 389 | ±0.101 | 0.193 | 0.179 | 0.061 | 0.027 | 0.044 |
| AMZN | 384 | ±0.102 | 0.116 | 0.082 | -0.019 | 0.105 | 0.022 |
| GOOG | 382 | ±0.102 | 0.169 | 0.141 | -0.003 | 0.003 | -0.003 |
| INTC | 373 | ±0.104 | 0.162 | 0.071 | 0.124 | 0.025 | 0.155 |
| MSFT | 381 | ±0.102 | 0.029 | 0.144 | 0.077 | 0.059 | 0.051 |

**10-second returns** — 3-5x the observations, so the band tightens and
the effect separates from noise at every symbol:

| Symbol | n | band | lag 1 | lag 2 | lag 3 | lag 5 | lag 10 |
|---|---|---|---|---|---|---|---|
| AAPL | 2,007 | ±0.045 | 0.139 | 0.118 | 0.109 | 0.114 | 0.037 |
| AMZN | 1,366 | ±0.054 | 0.155 | 0.087 | 0.098 | 0.036 | 0.078 |
| GOOG | 1,333 | ±0.055 | 0.217 | 0.127 | 0.164 | 0.045 | 0.061 |
| INTC | 1,317 | ±0.055 | 0.015 | 0.040 | 0.054 | 0.027 | 0.042 |
| MSFT | 1,421 | ±0.053 | 0.067 | 0.029 | 0.078 | 0.003 | 0.024 |

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

R0 was the de-risking step: can a single session show these effects at all?

| Fact | Signal | Usable as a ZI comparison target? |
|---|---|---|
| Cancel-to-execution ratio | **Strong** — 91–96%, all five symbols | Yes |
| Trade-sign autocorrelation | **Strong** — ACF(1) = 0.72–0.91, decays to ~0 by lag 100 | Yes, and it is the sharpest test |
| Fat-tailed returns | **Strong** — trade-level excess kurtosis 12–24 | Yes |
| Volatility clustering | **Mixed** — significant in AAPL/AMZN/GOOG, absent in INTC/MSFT | Partially, and the split is itself a finding |
| Effective spread | **Strong** — clean, matches tick structure | Yes, but comparison needs the book |
| Price impact | **Not measurable** from trades alone | No — needs the book |

**The headline is trade-sign autocorrelation.** ACF(1) between 0.72 and 0.91,
decaying slowly over ~100 trades, consistent across five symbols spanning $27 to
$580 and 6 to 29 messages/second. A zero-intelligence model produces exactly
zero at every lag by construction. That is a clean, unambiguous falsification
target and it alone justifies the comparison.

### Volatility clustering splits by tick constraint

At the conventional 1-minute horizon a single session yields ~380 observations,
where ±2 standard errors is ±0.10 — wide enough to swallow most of the effect.
That is a power problem, not an absence.

Dropping to 10-second buckets triples the sample and tightens the band to
±0.05, and the result separates cleanly **but only for three of the five
symbols**:

- **AAPL, AMZN, GOOG** — ACF(1) of 0.139, 0.155, 0.217 against a ±0.045–0.055
  band, with positive values persisting to lag 3–5. Clearly present.
- **INTC, MSFT** — every value at every lag falls *inside* the band. Not
  detectable at any horizon tried.

The split is not random, and it is not a sample-size artifact: INTC and MSFT
have the *most* messages of the five. They are the two low-priced names, trading
near $27 and $30 against a 1-cent minimum tick — the effective spread table
above shows both pinned at exactly 100 ticks. When price can only move in steps
that are large relative to its volatility, short-horizon returns are dominated
by discrete bounce between two levels, and that mechanical component drowns the
volatility signal.

So the honest statement is: **volatility clustering is measurable here in
wide-spread names and not in tick-constrained ones, on one session.** That is
usable as a ZI target for three symbols, and the cross-sectional pattern is
worth reporting in its own right.

### Would more trading days help?

Only for the weakest fact, and not for the reason it first appeared.

The headline results — cancel ratio, sign autocorrelation, fat tails — are
already unambiguous and would not change. Volatility clustering in INTC and MSFT
is where more data would genuinely buy something: many sessions would allow a
daily-horizon measurement, which is where this effect is conventionally studied
and where tick constraint stops dominating.

That is a real but narrow gain. It does not block R1–R4, and the LOBSTER free
sample is one session by design, so obtaining more means either paid academic
access or parsing raw NASDAQ ITCH. Neither is justified by one marginal
statistic when the primary comparison target is already this clean.

### Price impact was reclassified, and that is R0 earning its cost

The plan listed impact as message-derived and therefore available immediately.
It is not. The trade-only proxy measures bid-ask bounce, and on the two
penny-spread symbols it confidently reports that buying pushes price *down*. A
correct measure needs the mid price, and the mid needs a reconstructed book.
Finding this now cost one sitting; finding it after building the simulator would
have produced a plausible-looking wrong answer.
