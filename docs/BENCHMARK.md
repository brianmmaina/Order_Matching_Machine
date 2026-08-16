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

**Baseline recorded 2026-08-13**, before the Session 0.2 integer-tick refactor,
and **re-verified unchanged after it** on 2026-08-14. The refactor was a
representation change and had to move nothing.

Two checks, the second much stronger than the first:
1. Accuracy and first-mismatch index identical at all three event counts.
2. The full `--jsonl` book snapshot stream at 1,000 events — every level of
   both sides after every applied message — is **byte-identical** (md5
   `8aaff3cb99aa790862336d2253702562` before and after). The end-state check
   compares one snapshot; this compares a thousand.

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

Order-to-ack latency under concurrent TCP clients. Measured 2026-08-15.

- Hardware: Apple M4 Pro (8 performance + 4 efficiency cores), 24 GB, macOS 26.4.1, otherwise idle
- Build: Apple clang 21.0.0, `CMAKE_BUILD_TYPE=Release`
- Risk limits: `config/bench.conf` — deliberately far above anything offered, so the rate limiter is not what is measured
- **5 runs per cell**, 4s measured after a 2s warm-up excluded by *send* time
- Reported figure is the **median of the per-run statistic**, not a statistic over pooled samples
- **A fresh gateway per run** — see the note on book accumulation below

Generated by `bench/run_sweep.sh`; raw data in `bench/sweep.csv`.

| Clients | Orders/s/client | Offered/s | Achieved/s | p50 µs | p90 µs | p99 µs | p999 µs | max µs |
|---|---|---|---|---|---|---|---|---|
| 1 | 100 | 100 | 100 | 185.4 | 204.8 | 320.2 | 681.0 | 1391.3 |
| 1 | 1,000 | 1,000 | 1,000 | 98.8 | 112.2 | 128.5 | 191.1 | 306.4 |
| 1 | 5,000 | 5,000 | 5,000 | **45.4** | 49.5 | 69.2 | 144.0 | 734.1 |
| 10 | 100 | 1,000 | 1,000 | 199.4 | 290.9 | 487.2 | 1412.3 | 2738.2 |
| 10 | 1,000 | 10,000 | 10,000 | 110.0 | 143.4 | 169.2 | 211.7 | 1213.8 |
| 10 | 5,000 | 50,000 | 50,000 | 88.7 | 119.9 | 149.3 | 3284.5 | 6088.4 |
| 50 | 100 | 5,000 | 5,000 | 630.2 | 863.8 | 1663.3 | 4159.7 | 4633.8 |
| 50 | 1,000 | 50,000 | 50,000 | 441.7 | 550.0 | 658.3 | 4453.0 | 6800.8 |
| 50 | 5,000 | 250,000 | 249,959 | 764.2 | 1043.5 | 5857.0 | 22048.3 | 26257.4 |
| 100 | 100 | 10,000 | 10,000 | 1010.4 | 1590.9 | 2550.7 | 7591.7 | 9941.3 |
| 100 | 1,000 | 100,000 | 100,000 | 689.7 | 926.8 | 1372.6 | 9532.1 | 13654.2 |
| 100 | 5,000 | 500,000 | — | — | — | — | — | — |

Every cell offered was fully served except the last: **no orders were rejected
and no sends failed anywhere in the table.** The highest sustained rate measured
is 250,000 orders/sec at 50 clients, where the offered load was met within 41
orders over four seconds.

**100 clients x 5,000/s is not a gateway result.** At 500,000 orders/sec offered
the *load generator* stops making progress — four worker threads each pacing
125,000 sends/sec while draining acks is beyond what this generator does. The
cell is left blank rather than filled with a number describing the instrument.
Raising it would mean a generator on separate hardware, which a single-machine
setup cannot provide.

### Latency falls as offered load rises, and that is the wake-up policy

The most counter-intuitive result here is real and reproducible. At one client:

| Offered | p50 |
|---|---|
| 100/s | 185 µs |
| 1,000/s | 99 µs |
| 5,000/s | **45 µs** |

Four times the load, a quarter of the latency. The cause is the matching
thread's wake-up policy (`include/ome/waiter.hpp`): it spins briefly before
parking on a self-pipe. Under load the queue is rarely empty, the spin finds
work immediately, and no syscall is made at all. When nearly idle the spin
expires, the thread parks, and every order then pays a park-and-wake round trip.

That is the CPU-versus-latency trade being paid exactly where it was designed to
be paid. It also means the headline p50 should be quoted with its offered rate
attached — "45 µs" without "at 5,000 orders/sec" is not a meaningful number.

### Latency rises with client count, and that is `poll()`

At a fixed 100 orders/sec per client, p50 goes 185 → 199 → 630 → 1010 µs across
1, 10, 50 and 100 clients. The network thread rebuilds and scans its whole
`pollfd` set every iteration, which is O(connections) regardless of how many
are actually ready. `epoll`/`kqueue` are O(ready) instead, and this is the
measurement that would justify the portability cost of switching. It is not
justified yet: the p50 at 100 clients is stillroughly 1 ms.

### Known measurement boundaries

Beyond the standing honesty requirements above:

- **The generator shares the machine.** Four worker threads plus the gateway's
  two threads on 12 cores. At 50 and 100 clients some of the p999 and max
  figures are generator scheduling, not gateway behavior.
- **Nothing crosses.** The workload quotes a bid below and an ask above a fixed
  price, so every order rests and none ever match. This measures the accept and
  acknowledge path, *not* the matching path under contention. A crossing
  workload is a separate benchmark and would produce fills, changing both the
  work done and the number of messages returned.
- **The book grows without bound during a run**, for the same reason. A 4-second
  run at 250,000/s leaves a million resting orders on two price levels. This is
  why the sweep starts a fresh gateway for every run: sharing one made cells
  later in the sweep report *zero* acks, because each cell ended with fifty or a
  hundred sessions disconnecting and each disconnect cancels every order that
  session left resting. Run standalone, those same cells are healthy.
- **Single runs are not trustworthy at the tail.** One early run reported a p99
  of 54 ms for a cell that five repeats placed at 440–570 µs. That is what the
  repetition requirement is for.
- **The price band was disabled and nobody was subscribed.** `config/bench.conf`
  sets `price_band_bp = 0`, and the load generator does not subscribe to market
  data. Both of those paths consult the top of book, so this table measures the
  order path with them switched off. They were made cheap after this run — the
  level accessors now read a cached per-level total instead of summing a deque,
  and the band check reads the best price without touching quantities at all —
  but these numbers predate that and do not include either cost. A sweep with
  the band enabled and a live subscriber is a separate measurement, and it has
  not been taken.

---

## Durability cost

The WAL delta **is** the price of durability. It gets stated, not hidden.

Measured 2026-08-16. 10 clients x 1,000 orders/sec (10,000/s offered), 4s after
a 1s warm-up, three runs each, same machine and build as the table above.
Group commit at 100 records or 10 ms, whichever comes first.

| Configuration | p50 µs | p90 µs | p99 µs |
|---|---|---|---|
| No WAL | 110 | 143 | 170 |
| WAL, `fsync` | 121 | 167 | 222 |
| WAL, `F_FULLFSYNC` (default) | 136–203 | ~3,400 | ~4,400 |

**Ordinary durability is cheap. Media-level durability is not.**

`fsync` costs **+11 µs at p50 and +52 µs at p99** — about 10% and 30%. That is
the honest price of an append-before-apply log at this rate, and it is small
because the append itself is a `write()` into the page cache; only the periodic
flush costs anything.

`F_FULLFSYNC` costs **25x at the tail**. On macOS, `fsync` returns once the data
has reached the drive; the drive may still hold it in a volatile cache.
`F_FULLFSYNC` waits for the drive to commit to stable media, and that is the
only call that survives a power cut. It takes milliseconds, and it runs **on the
matching thread**, so order processing stalls for its duration — which is why
p90 and p99 blow up while p50 moves comparatively little. Most orders are
unaffected; the ones that land during a flush wait for it.

The gateway defaults to `F_FULLFSYNC`. A log whose entire purpose is surviving
machine failure should not quietly pick the weaker guarantee to look faster.
`--wal-no-fullsync` trades it back, and anyone using it should say so.

### What is actually lost, and when

Worth being precise, because "durable" is doing a lot of work in most writeups:

- **Process death — `kill -9`, a segfault, an assertion — loses nothing.** The
  records were handed to the kernel with `write()`, and the page cache outlives
  the process. Session 2.4's kill testing exercises exactly this case, and it is
  fully recoverable.
- **Machine death — power cut, kernel panic — loses at most the group-commit
  window**: 100 records or 10 ms of orders, whichever came first. Those orders
  may have been acknowledged to their clients.
- With `--wal-no-fullsync` on macOS, a power cut can additionally lose whatever
  the drive was holding in its own cache, which the OS considers written.

_Recovery time with and without snapshots: Sessions 2.2 and 2.3._

---

## Known bottlenecks

Named, not hand-waved. Each entry says what degrades, under what conditions, and
what was actually observed.

**1. `poll()` scan is O(connections).** p50 rises 185 → 1010 µs from 1 to 100
clients at a fixed per-client rate. The network thread rebuilds and scans the
whole descriptor set every iteration whether or not anything is ready.
`epoll`/`kqueue` are O(ready). Not yet worth the portability cost — 1 ms at 100
connections is acceptable — but this is the measurement that would justify it.

**2. Park-and-wake dominates at low load.** p50 is 4x worse at 100 orders/sec
than at 5,000, because an idle matching thread parks on its self-pipe and each
order then pays a wake-up. Deliberate: the alternative is burning a core
continuously at zero load. The knob is the spin duration in
`include/ome/waiter.hpp`.

**3. Cancel-on-disconnect is O(orders held by the session).** A session
disconnecting after resting a hundred thousand orders blocks the matching thread
for the whole sweep of cancels, and the inbound queue backs up behind it. Found
by the sweep rather than by reasoning: a shared gateway made every later cell
report zero acks. Not a problem at realistic order counts, and the fix if it
ever became one is to bound the work per drain iteration rather than cancelling
the whole set at once.

## Fixed during benchmarking

Two defects the load generator caught that no test had:

**Egress delivery waited for the next poll wake-up.** The event loop drained
egress queues *before* reading sockets, so an ack produced by this iteration's
orders was not written until the following pass — and that pass only ran when
another client happened to send something or the 100 ms timeout expired.
Measured as p50 in the milliseconds against a true cost of tens of microseconds.
Fixed with a wake-up pipe from the matching thread (`include/ome/notifier.hpp`)
and by draining egress after dispatch.

**The wake-up channel could die permanently.** `Notifier::drain()` cleared its
coalescing flag *before* reading the pipe. A `notify()` landing in that window
wrote a byte, the read loop consumed it, and the channel was left flagged as
pending with an empty pipe — after which every `notify()` skipped its write
forever and the network thread only ever woke on its timeout. Symptom was a p99
in the tens of milliseconds with a healthy p50. Fixed, regression-tested in
`tests/test_notifier.cpp`, and the event loop no longer depends on the
notification for correctness: it refuses to block whenever any egress queue is
non-empty.
