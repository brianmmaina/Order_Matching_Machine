# Order flow calibration

Parameters for a synthetic order-flow model, measured from LOBSTER data by
`analysis/calibrate.py`. NASDAQ, 2012-06-21, one full session per symbol.

## Where the mid comes from

Placement distance needs a mid price, and a mid needs a book. Reconstructing one
from the messages would inherit this project's replay accuracy gap (50–70%) and
push a systematic error straight into the calibration.

LOBSTER ships a second file giving the true depth-10 book state after every
message, row-aligned with the messages. The book *before* message *i* is row
*i*−1. So the mid here is ground truth rather than something this project
computed.

## Arrival rates

Per second, over the full session.

| Symbol | Limit | Cancel | Market | Hidden exec | Median spread |
|---|---|---|---|---|---|
| AAPL | 8.16 | 7.45 | 1.01 | 0.48 | 1,500 ticks |
| AMZN | 5.64 | 5.40 | 0.38 | 0.10 | 1,300 |
| GOOG | 3.05 | 2.78 | 0.33 | 0.17 | 2,800 |
| INTC | 13.03 | 12.26 | 1.24 | 0.15 | 100 |
| MSFT | 14.08 | 13.07 | 1.27 | 0.15 | 100 |

Cancels track limit arrivals closely everywhere — the ratio never leaves
0.91–0.94. That is the same fact as R0's cancel-to-execution ratio seen from the
other side: the book is overwhelmingly made of orders that will be withdrawn.

## Placement, in half-spreads from the mid

Ticks are not a usable unit here. AMZN's median spread is 1,300 ticks and INTC's
is 100, so a histogram in absolute ticks puts every AMZN order in the tail and
tells you nothing. In half-spread units the scale is intrinsic: **1.0 is the
touch**, below 1.0 is price improvement inside the spread, above is behind it.

| Symbol | Crossed the mid | Inside the spread | At the touch (0.75–1.0) | Behind |
|---|---|---|---|---|
| AAPL | 1.41% | 5.3% | **19.5%** | 73.8% |
| AMZN | 0.83% | 4.7% | **15.6%** | 78.9% |
| GOOG | 1.48% | 6.1% | **25.9%** | 66.5% |
| INTC | 0.00% | 0.3% | **65.8%** | 33.9% |
| MSFT | 0.00% | 0.4% | **61.8%** | 37.8% |

### Tick constraint again

The split is the same one R0 found in volatility clustering, and it has the same
cause. INTC and MSFT trade near \$27–30 against a one-cent minimum tick, so
their median spread is exactly one tick. **You cannot improve on a one-tick
spread** — there is no price between the bid and the ask. Placement therefore
collapses onto the touch: 62–66% of orders, versus 16–26% for the wide-spread
names, and essentially nothing inside.

Two different measurements, on the same five symbols, separating the same two
groups for the same structural reason. That is worth more than either result
alone.

## What is approximated

**The cancel rate per resting order** uses top-10 depth as its denominator,
because that is all the orderbook file shows. The real book is deeper, so the
denominator is too small and the per-order rate is an **overestimate**. Right
shape, wrong constant: a generator using it will cancel too eagerly.

**Hidden executions are not modelled.** They are 10–32% of executions depending
on the symbol, and they consume real liquidity that never appears in the book.

**Order sizes are reported empirically, not fitted.** They cluster hard on round
lots — 70% of AMZN's are multiples of 100 — and any smooth distribution fitted
to that would misrepresent it.

**One session, one venue, five symbols, 2012.** Adequate for calibrating a model
to compare against this data. Not a claim about markets in general.
