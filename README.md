# Order Matching Machine

A limit order book and matching engine in **C++17**, wrapped in a **networked
order gateway** with crash recovery — then used to replicate a
[2005 PNAS paper](https://arxiv.org/abs/cond-mat/0309233) on market
microstructure.

Clients connect over TCP, submit orders, and receive acks, fills and market
data. A single matching thread owns the book; a write-ahead log makes the state
survive being killed. No dependencies beyond GoogleTest.

- **45 µs** median order-to-ack latency, **250,000 orders/sec** sustained
- **50/50** randomized `kill -9` recovery tests, checked against an independent
  reimplementation of the book
- Matching validated by differential replay against **269K NASDAQ LOBSTER
  messages**
- **191 tests**, green on clang and gcc, under ASan/UBSan and ThreadSanitizer

> **First C++ project.** I'm still learning the idioms and the tradeoffs below;
> this is a learning sandbox, not battle-tested infrastructure. Feedback welcome.

---

## Architecture

```
clients --TCP--> [network thread] --SPSC--> [matching thread] --> WAL
                        ^                          |            snapshots
                        |                          v
                        +---- per-session SPSC ----+
                             order events + book snapshots
```

**The book is single-writer.** One thread owns every mutation, so there is no
lock anywhere near it. Concurrency lives entirely at the edges, and the two SPSC
queues are the seam. The rest follows from it:

- The network thread **never touches the book** — not to check a price band, not
  to answer a cancel. Anything needing book state crosses the queue.
- **Cancel-on-disconnect** travels the same queue as every other command, so a
  dropped connection needs no special path and no lock.
- **Market data conflates; order flow never does.** A newer book snapshot
  supersedes an older one, so a slow subscriber skips ahead. An ack or fill is a
  fact a client cannot reconstruct, so a slow consumer is disconnected instead.

---

## Try it

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure          # 191 tests

./build/gateway --wal /tmp/ome.wal --port 9001 &
python3 tools/smoke_client.py --port 9001            # send an order, print the ack
```

Watch a live book in the browser:

```bash
python3 tools/live_feed.py --port 9001 --out /tmp/live.jsonl &
./build/loadgen --port 9001 --clients 4 --rate 500 --duration 20 --cross
open tools/book_replay.html                          # load /tmp/live.jsonl
```

Kill it and watch it come back:

```bash
kill -9 $(pgrep -f 'build/gateway')
./build/gateway --wal /tmp/ome.wal --recover --port 9001
# recovered N commands, last seq N
```

Needs network once on first configure (GoogleTest via FetchContent). Build type
defaults to Release. Builds with `-Wall -Wextra -Werror`.

---

## Measured results

Apple M4 Pro, Release, median of repeated runs. Methodology and boundaries in
[`docs/BENCHMARK.md`](docs/BENCHMARK.md).

| | |
|---|---|
| Order-to-ack p50 | **45 µs** at 5,000 orders/sec, 1 client |
| Order-to-ack p50 / p99 | 89 µs / 149 µs at 50,000 orders/sec, 10 clients |
| Sustained throughput | **250,000 orders/sec** at 50 clients, nothing rejected |
| WAL cost (`fsync`) | +11 µs p50, +52 µs p99 |
| Snapshot pause | 28 ms at 100,000 resting orders |
| Crash recovery | **50/50** WAL replay, **25/25** snapshot + tail |

Two results worth stating outright:

**Latency falls as load rises** — 185 µs at 100 orders/sec versus 45 µs at
5,000. Under load the matching thread's spin finds work and never parks; when
nearly idle every order pays a park-and-wake round trip. "45 µs" is only
meaningful with its offered rate attached.

**Media-level durability is expensive.** On macOS `fsync` returns once data
reaches the drive, which may still hold it in a volatile cache. `F_FULLFSYNC`
waits for stable media and is the only call that survives a power cut — and it
runs on the matching thread, so p99 goes from 222 µs to ~4,400 µs. It is the
default anyway: a log whose purpose is surviving machine failure should not
quietly pick the weaker guarantee to look faster.

---

## Crash safety

`tools/kill_test.sh` runs the gateway under load, `kill -9`s it at a random
moment, restarts with recovery, and checks the rebuilt book.

```bash
tools/kill_test.sh 50                          # WAL replay      50/50
SNAPSHOT_EVERY=1500 tools/kill_test.sh 25      # snapshot + tail 25/25
```

What matters is not that recovery succeeds but what it is checked *against*.
`tools/wal_verify` rebuilds the book from the same log using a separate
implementation — `std::map` price levels, its own matching loop, no shared code
with `OrderBook`. Both must produce the same digest. Otherwise the test is the
engine agreeing with itself, and a matching bug would produce the same wrong
book on both sides and pass.

The harness was weaker than it looked. A deliberately broken verifier passed
1/1, because the load generator quoted bids below asks and nothing ever matched
— both implementations agreed only that they could accumulate a book. It now
quotes crossing prices with varying sizes, and the broken verifier is caught
immediately.

The snapshot path found a bug the unit tests could not: `truncate_before()`
derived sequence numbers by counting from 1, which is right for a log that still
starts at 1 and wrong for one already truncated. It takes *two* truncations to
see it.

**Scope, stated honestly.** `kill -9` does not destroy the page cache, so
everything already handed to `write()` survives. This proves the recovery path,
torn-tail handling and append-before-apply ordering. It does **not** prove
behavior under power loss.

---

## Research: replicating Farmer, Patelli & Zovko (2005)

Full writeup and every caveat in [`docs/FARMER_2005.md`](docs/FARMER_2005.md).

[The paper](https://arxiv.org/abs/cond-mat/0309233) (PNAS 102(6):2254–2259)
predicts a stock's mean spread from its order flow alone, `ŝ = (μ/α)·f(ε)`, and
tests it **cross-sectionally** — across stocks, regressing `log s = A·log ŝ + B`
and asking whether A = 1. On 11 LSE stocks it gets A = 0.99 ± 0.10, R² = 0.96.

[`analysis/farmer2005.py`](analysis/farmer2005.py) reruns that test on the five
LOBSTER symbols. **Both laws fail**, diffusion by four orders of magnitude — but
the failure is ordered by `dp/p_c`, the model's own nondimensional tick size,
which Equation 1 assumes away by taking `dp → 0`. A penny on a $27 stock is
seventeen times the characteristic price scale of its own order flow.

That is a correlation on five points, and `dp/p_c` is large for exactly the two
cheapest stocks. [`tools/zi_paper.cpp`](tools/zi_paper.cpp) separates cause from
coincidence by simulating the paper's model on this project's matching engine at
each stock's measured parameters and its **real tick**, where nothing about a
cheap stock is present except four flow numbers and `dp`:

| | dp/p_c | simulated ratio | real ratio | sim/real |
|---|---:|---:|---:|---:|
| GOOG | 0.21 | 0.66 | 4.36 | 0.15 |
| AAPL | 0.35 | 0.72 | 3.70 | 0.19 |
| AMZN | 0.76 | 0.83 | 5.66 | 0.15 |
| INTC | 17.30 | **32.23** | **39.75** | **0.81** |
| MSFT | 22.03 | **40.87** | **50.35** | **0.81** |

**The tick alone reproduces 81% of the observed departure from the law** — the
same fraction for both constrained stocks — and the simulated inflation is
perfectly rank-ordered by `dp/p_c` (ρ = 1.000, exact p = 0.017). So the paper is
not refuted; ignoring its scope condition is what makes it look refuted.

Two things this does not show, both kept in the writeup: the remaining 19% is
real, and the small-tick simulation runs ~25% *below* the mean-field prediction
rather than matching it.

Earlier and narrower: [`docs/STYLIZED_FACTS.md`](docs/STYLIZED_FACTS.md)
measures cancel ratios, trade-sign autocorrelation and volatility clustering
straight from the message stream, and
[`docs/ZI_COMPARISON.md`](docs/ZI_COMPARISON.md) is a calibrated ZI baseline
that tests properties this paper never claims — and says so at the top.

---

## Design decisions

**Wire protocol** ([`docs/PROTOCOL.md`](docs/PROTOCOL.md)) — length-prefixed
binary, little-endian, `int64` tick prices, no floating point anywhere.
Serialization is field by field, never a struct `memcpy`: `sizeof(NewOrder)` is
24 on this build while its wire encoding is 22, and those two padding bytes are
uninitialized memory a struct copy would put on the wire.

**Prices are integer ticks.** Durability asserts that a recovered book's digest
*equals* the original's, and a digest over doubles would make that a
float-equality claim dressed up as a guarantee.

**The WAL record is written before the command touches the book.** That ordering
picks which failure mode a mid-command crash produces: append-first leaves a
record for a command that never reached the book, and recovery replays it.
Apply-first mutates the book for a command whose record never landed, and
nothing afterwards can detect it.

**Recovery uses the live apply path**, not a separate replay path. Otherwise
"the recovered book is identical" would only mean two implementations happen to
agree.

**Risk checks live on the thread that owns the state they need.** Rate limiting
is on the network thread, so an over-limit client is refused before consuming
queue capacity. The price band is on the matching thread, because it is relative
to the last trade or the mid and only that thread may read the book.

---

## Limitations

Specific rather than hedged. Most are deliberate; all are real.

**Single symbol.** No instrument identifier in any message.

**No authentication, no TLS.** Anyone who can reach the port can trade. Binds
loopback only.

**One matching thread by design.** A real ceiling, not a bug. A second symbol
would get a second thread and a second book; a second thread on *one* book would
need the locking this architecture exists to avoid.

**Benchmarks are loopback on one machine**, and the workload does not cross — it
measures the accept-and-acknowledge path, not matching under contention. At 100
clients × 5,000 orders/sec the load generator, not the gateway, stops making
progress, so that cell is blank rather than guessed.

**No self-trade prevention.** A session holding both sides will match itself.

**`Modify` is cancel-and-replace.** Time priority is lost on a reprice or a size
increase, retained on a size decrease.

**Ack precedes Fill.** An aggressive limit rests before it matches, so clients
must not read an ack as "did not trade".

**`BookUpdate` sequence numbers are not contiguous.** Conflation skips values by
design; a client treating a gap as loss would flag healthy behavior as an error.
No delta encoding either — every update is a full top-N snapshot.

**LOBSTER replay accuracy is 70% at 1K events and 50% beyond.** Faithful
reconstruction needs order-level state across the file window, and 21% of
executions in this sample are against hidden liquidity that never appears in the
book at all.

**Snapshots pause the matching thread** — 28 ms at 100K resting orders. The fix
is measured and known (the copy is 2.2 ms while the write is 26 ms) and not yet
implemented. Snapshots are off unless `--snapshot` is given.

**Cancel-on-disconnect is O(orders held)**, and **`poll()` is O(connections)** —
p50 goes 185 µs → 1,010 µs from 1 to 100 clients. `epoll`/`kqueue` are O(ready),
not yet worth the portability cost.

**The replication is 5 stocks and 1 day**, against the paper's 11 stocks and 434
days. At n = 5 the smallest attainable exact p-value *is* 0.017, so that result
is suggestive rather than established.

---

## Repo layout

| Path | What |
|---|---|
| `include/order_book/`, `src/order_book.cpp` | the book: sorted vector of levels, deque FIFO per level |
| `include/matching_engine/` | order routing and the trade log |
| `include/ome/` | protocol, framing, sessions, WAL, snapshots, queues |
| `src/net/tcp_server.cpp` | `poll()` event loop |
| `bench/` | load generator and latency sweep |
| `tools/` | visualizer, live feed, WAL verifier, kill-test harness, ZI simulators |
| `analysis/` | LOBSTER measurement, calibration, the paper replication |
| `docs/` | protocol spec, benchmarks, market data facts, replication writeup |

Sanitizer builds — `address`, `undefined`, `address,undefined`, or `thread`
(thread and address are mutually exclusive):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DOME_SANITIZE=address,undefined
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

---

## LOBSTER data (optional)

Sample CSVs are **not** in git. Download message + orderbook pairs from
[LOBSTER samples](https://data.lobsterdata.com/info/DataSamples.php), place under
`data/lobster/`, then see [`data/lobster/README.md`](data/lobster/README.md).
The replication needs all five symbols; everything else works with one.

```bash
./build/lobster_replay --messages <message.csv> --orderbook <orderbook.csv> \
  --events 1000 --jsonl /tmp/replay.jsonl
open tools/book_replay.html          # load /tmp/replay.jsonl
```

---

## What I want to improve next

- **LOBSTER parity** — order-level state across the file window, hidden
  liquidity, cross and auction messages.
- **Copy-then-write snapshots**, to remove the matching-thread pause.
- **A crossing benchmark**, to measure matching under contention rather than the
  accept path alone.
- **More trading days**, so the replication has error bars on the real side
  rather than a single observation per stock.
