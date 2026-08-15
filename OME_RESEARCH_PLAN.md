# Research extension — zero-intelligence order flow

**What this is:** a second project that reuses the matching engine as an instrument. It is not part of the service build plan and does not start until Phase 4 of that plan has shipped.

**Status:** R0 is **done** — see `docs/STYLIZED_FACTS.md`. It was pulled forward deliberately because it is one sitting, touches no gateway code, and de-risks everything after it.

**Prerequisite for R1 onward:** `OME_SERVICE_BUILD_PLAN.md` complete through Phase 2, with Phase 3 (observability) cut. Do not interleave. A half-finished gateway plus a half-finished research project is two things you cannot show anyone.

---

## The thesis

> **Same engine, two order flows.** Feed the matching engine (a) real LOBSTER messages and (b) synthetic zero-intelligence flow calibrated to that same data. Ask which market properties survive when all the strategic behavior is deleted.

The reference result is Farmer, Patelli & Zovko, *The predictive power of zero intelligence in financial markets* (PNAS, 2005): random order flow, constrained only by placement and budget rules, reproduces a surprising amount of real market structure.

**Why this framing rather than a plain reimplementation.** The engine is the controlled variable. Both streams pass through identical matching code, identical tick arithmetic, identical book. Any difference in the output is attributable to the order flow and not to the implementation — a claim most reproductions cannot make, because they simulate and observe with different machinery.

**Why this repo can do it.** Almost every reproduction of this literature is done in pandas against a toy book. There is a real matching engine here, real message data, and a visualizer that can show both books side by side. The instrument already exists; only the experiment is missing.

---

## What the data actually supports

Measured from `LOBSTER_SampleFile_AMZN_2012-06-21_10`, 269,748 messages over a 6.5-hour session (23,400s), before any of this was planned:

| Type | Meaning | Count | Share | Rate |
|---|---|---|---|---|
| 1 | New limit order | 131,954 | 48.92% | 5.64/s |
| 3 | Delete | 123,458 | 45.77% | 5.28/s |
| 2 | Partial cancel | 2,917 | 1.08% | 0.12/s |
| 4 | Execution, visible | 8,974 | 3.33% | 0.38/s |
| 5 | Execution, **hidden** | 2,445 | 0.91% | 0.10/s |
| 6, 7 | Cross, halt | 0 | — | — |

Three consequences, all good:

1. **Calibration is nearly free.** The zero-intelligence model needs a limit-order arrival rate, a cancel rate, and a market-order rate. All three are counts divided by session length. No estimation machinery, no likelihood fitting.
2. **The awkward message types are absent.** No crosses, no halts in this sample. The two classes hardest to model correctly simply do not occur.
3. **95.8% of submitted orders are cancelled rather than executed** (126,375 cancels against 131,954 new orders). That is a real stylized fact, measurable from this data alone, and it is the single most important input to a ZI model — the book is overwhelmingly made of orders that will never trade.

And one that is not good, addressed below: **21% of executions are against hidden liquidity** (2,445 of 11,419). Hidden orders never appear in the book but consume real volume.

---

## The measurement battery

Both streams, same analysis code, error bars across seeds.

| Statistic | ZI expected to reproduce? | Needs book reconstruction? |
|---|---|---|
| Cancel-to-execution ratio | Yes, by construction | No |
| Spread distribution | Yes | **Yes** |
| Depth profile by level | Approximately | **Yes** |
| Return distribution, kurtosis | Partially | No |
| Volatility clustering (ACF of \|r\|) | **No** | No — but only usable for the three wide-spread symbols |
| Order-sign autocorrelation | **No** — zero by construction | No |
| Price impact vs volume (~√) | Partially | **Yes** — reclassified by R0 |

**Price impact moved columns after R0.** The trade-only proxy for impact measures bid-ask bounce, not impact, and on the two penny-spread symbols it reports that buying pushes price down. A correct measure needs the mid price, and the mid needs a reconstructed book. This is exactly what R0 existed to find.

**The failures are the result.** A finding that ZI reproduces structural properties but not dynamic ones is the interesting outcome, and it is the honest one. If everything matched, the experiment would be uninformative.

---

## The contamination risk, and the way around it

**Book reconstruction is currently 70% accurate at 1k events and 50% at 5k+.** Statistics computed from a book that is half wrong are worthless, and a comparison between a wrong real book and a correct simulated book measures nothing.

The mitigation is visible in the table above: **most of the battery does not need the book at all.** Returns, order signs, impact, and the cancel ratio are computed directly from the message stream, where the data is exact. Only spread and depth require a reconstructed book.

So the project splits cleanly:

- **Message-derived statistics are available today**, at full fidelity, with no prerequisite work.
- **Book-derived statistics require the reconstruction work** already listed in the README's "what I want to improve next" — order-level state across the file window, partial cancels for unknown ids, and hidden liquidity.

That the research project's prerequisite is a thing already wanted for its own sake is a point in its favor.

---

## Phases

**R0 — Stylized facts from the raw messages.** ✅ **DONE** — `analysis/stylized_facts.py`, results in `docs/STYLIZED_FACTS.md`

Outcome: trade-sign autocorrelation is the headline (ACF(1) = 0.72–0.91 across five symbols, decaying to ~0 by lag 100) and is a clean falsification target, since a ZI model produces exactly zero by construction. Cancel ratio and fat tails are also strong. Volatility clustering is significant at a 10-second horizon for AAPL/AMZN/GOOG but undetectable for INTC/MSFT at any horizon — those two are tick-constrained (1-cent tick on a $27-30 stock), so bounce dominates. Price impact is not measurable without the book.

*(original scope below)*

No simulation. No engine. Read the LOBSTER message CSVs and measure every statistic in the battery that does not need a book: cancel ratio, return distribution and kurtosis, ACF of absolute returns, ACF of order signs, impact curve.

**Do this before committing to anything else.** It answers, cheaply, the only question that matters: is 269K messages of one stock on one day rich enough to show these effects at all? If the autocorrelation functions are noise, the comparison has nothing to compare and the project stops here having cost one sitting.

It is also independently useful — those numbers belong in the README regardless.

**R1 — Calibration.** *(1 sitting)*
Extract arrival, cancel, and execution rates per symbol. Order size distribution. Placement distribution: how far from the mid do new orders arrive? That last one is the ZI model's key input and the one thing requiring more than counting.

**R2 — The generator.** *(2 sittings)*
Synthetic order flow through the real engine, emitting the same JSONL the visualizer already consumes. Seeded and reproducible — a fixed seed must produce a byte-identical stream, for the same reason the WAL replay must.

**R3 — Comparison harness.** *(2–3 sittings)*
One analysis path, two inputs. Statistics over ≥30 seeds with error bars. Figures: spread distribution, depth profile, return QQ plot, ACF of |returns|, ACF of order signs, impact curve. Real and simulated overlaid on every one.

**R4 — Writeup and demo.** *(2 sittings)*
`docs/RESEARCH.md`. Side-by-side visualization: real book left, synthetic right, same engine, same clock.

**Total: 8–9 sittings**, plus the reconstruction work if the book-derived statistics are wanted.

---

## Honest limitations

To be stated as prominently as any result:

- **One day, one venue, five stocks, 2012.** Adequate for "here is a reproduction on a specific sample and here is where it breaks." Not adequate for any claim about markets in general.
- **NASDAQ 2012 is not the market the original paper studied.** Farmer et al. used London Stock Exchange data from around 1998–2000. Different venue, different era, different microstructure. A mismatch in results is not automatically a failed reproduction.
- **Hidden liquidity is not modelled.** 21% of executions in this sample are against it.
- **A ZI model is a null hypothesis, not a market model.** The point is what it fails to explain.

---

## What it buys

A defensible sentence: *"I built a matching engine, calibrated a zero-intelligence order flow model to real NASDAQ message data, ran both through the same engine, and measured which stylized facts survive."*

That is a different claim from "I implemented a matching engine," and it is checkable. It also changes who the project speaks to — this is a quant-research story, whereas the service build plan is a backend story. Both can live in one repo, but the README has to decide which one leads.

---

## Cut lines

1. Drop book-derived statistics; ship message-derived only. Removes the reconstruction prerequisite entirely.
2. Drop the side-by-side visualization; static figures carry the result.
3. Drop to one symbol. Five is breadth, not rigor, at one day each.
4. **Floor: R0 alone.** A measured battery of stylized facts on real data, in the README, with no simulation at all. Still worth having.
