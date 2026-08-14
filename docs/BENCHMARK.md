# Benchmarks

Methodology first, numbers second. A number without its boundary conditions is
not a result — it is a claim, and claims do not survive scrutiny.

This file is written before any measurement exists so that the methodology is
decided while nothing is riding on the outcome.

---

## Methodology

Every table in this document states the following. If a row cannot state them,
it does not belong here.

**Hardware.** CPU model, core count, whether performance and efficiency cores
are present (they are on Apple silicon, and the scheduler will move a hot thread
between them), RAM, and whether the machine was otherwise idle.

**Build.** Compiler and version, `CMAKE_BUILD_TYPE`, and the resulting
optimization flags. Every number here comes from a **Release** build. A default
build with no `CMAKE_BUILD_TYPE` gets no optimization flags at all — the
`CMakeLists.txt` now defaults to Release specifically so an untyped build cannot
silently produce a number that ends up quoted somewhere.

**Warm-up.** The first N operations are excluded from statistics. Cold caches,
lazy page faults, and branch predictors that have not seen the loop yet all
inflate early samples. State the warm-up size and exclude it explicitly rather
than hoping it averages out.

**Run count.** At least **10 runs**. Report the **median of the per-run
statistic**, not the statistic over pooled samples from all runs — pooling hides
run-to-run variance, which is often the more interesting number. Report the
spread across runs alongside the median.

**Percentiles.** Computed from the full sorted sample vector, not a histogram
approximation, at the sample sizes used here. p50 / p90 / p99 / p999 / max.
The max is reported because it is the number a real operator gets paged about.

**What is inside the timed path.** Stated explicitly per benchmark, including
what is deliberately outside it. A latency figure that quietly excludes
serialization, or the queue hop, or the syscall, is not comparable to one that
does not.

---

## Honesty requirements

These are stated in every benchmark section, not assumed to be understood:

- **Loopback TCP is not a network.** It skips the NIC, the driver, the wire, and
  every switch. Real-network latency is a different measurement and will be
  larger by more than the error bars here.
- **Single machine.** Client and server contend for the same cores. At high
  client counts the load generator itself becomes part of what is being
  measured.
- **Client-side measurement includes client-side scheduling noise.** When the OS
  deschedules the measuring thread between send and receive, that delay is
  attributed to the server. It is not separable without hardware timestamping.
- **These are not exchange-grade numbers** and are not presented as such. They
  are a measurement of this implementation on this hardware under this load.

---

## Engine benchmarks

Single-threaded, in-process. No network, no serialization, no queue.

_No results recorded yet. Populated by the engine benchmark runs._

| Scenario | Runs | p50 (ns) | p99 (ns) | p999 (ns) | max (ns) | Notes |
|---|---|---|---|---|---|---|
| _pending_ | | | | | | |

---

## LOBSTER replay

Throughput of parse + apply + validate over the sample files.

**Baseline recorded 2026-08-13**, before the Session 0.2 integer-tick refactor.
Its purpose is regression detection: 0.2 is a representation change that must
not move accuracy at all. If any accuracy figure below changes after 0.2, the
refactor altered behavior and needs investigating before anything is built on
top of it.

- Hardware: Apple M4 Pro (8 performance + 4 efficiency cores), 24 GB, macOS 26.4.1
- Build: Apple clang 21.0.0, `CMAKE_BUILD_TYPE=Release`
- File: `LOBSTER_SampleFile_AMZN_2012-06-21_10`, depth 10
- Single run each — these are correctness baselines, not timing claims. The wall
  times are recorded only to show they are dominated by file parsing rather than
  by the replay, and are **not** the ≥10-run measurements the methodology above
  requires. Do not quote them as throughput.

| Events | Accuracy | First mismatch (combined index) | Wall (s) |
|---|---|---|---|
| 1,000 | 70% | 9 (bid rank 9) | 0.13 |
| 5,000 | 50% | 0 (bid rank 0) | 0.13 |
| 100,000 | 50% | 10 (ask rank 0) | 0.16 |

Accuracy is the fraction of 20 top-of-book slots (10 bids + 10 asks) matching
the LOBSTER reference snapshot. It is well below 100% because the replay does
not yet reconstruct order-level state across the full file window — see the
README's "what I want to improve next".

---

## Service benchmarks

Order-to-ack latency under concurrent TCP clients. Populated in Session 1.7.

_No results recorded yet._

| Clients | Orders/s/client | Achieved thr. | p50 (µs) | p99 (µs) | p999 (µs) | Notes |
|---|---|---|---|---|---|---|
| _pending_ | | | | | | |

---

## Durability cost

The latency delta introduced by the write-ahead log, and recovery time with and
without snapshots. Populated in Sessions 2.1 and 2.3.

The WAL delta **is** the price of durability. It gets stated, not hidden.

_No results recorded yet._

---

## Known bottlenecks

Named, not hand-waved. Each entry says what degrades, under what conditions, and
what the profile actually showed.

_None identified yet._
