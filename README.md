# Order Matching Machine

A limit order book and matching engine in **C++17**, wrapped in a **networked
order gateway** with crash recovery.

Clients connect over TCP, submit orders, and receive acks, fills and market
data. A single matching thread owns the book; a write-ahead log makes the state
survive being killed. No dependencies beyond GoogleTest.

- **45 µs** median order-to-ack latency, **250,000 orders/sec** sustained
- **50/50** randomized `kill -9` recovery tests, checked against an independent
  reimplementation of the book
- Matching validated by differential replay against **269K NASDAQ LOBSTER
  messages**
- **186 tests**, green on clang and gcc, under ASan/UBSan and ThreadSanitizer

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
lock anywhere near it. Concurrency lives entirely at the edges — network I/O and
market-data fanout — and the two SPSC queues are the seam. That is the whole
design in one sentence, and the rest follows from it:

- The network thread **never touches the book**. Not to check a price band, not
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
ctest --test-dir build --output-on-failure          # 186 tests

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
# book digest after recovery: ...
```

With periodic snapshots, recovery reads state rather than history:

```bash
./build/gateway --wal /tmp/ome.wal --snapshot /tmp/ome.snap \
                --snapshot-every 100000 --port 9001
# ... after a kill:
./build/gateway --wal /tmp/ome.wal --snapshot /tmp/ome.snap --recover --port 9001
# restored 8020 orders from snapshot at seq 8020
# recovered 1580 commands, last seq 9600
```

The WAL is compacted behind each snapshot, so it holds the tail rather than the
whole history — 1,580 records instead of 9,600 above.

Needs network once on first configure (GoogleTest via FetchContent). Build type
defaults to Release. Builds with `-Wall -Wextra -Werror`.

---

## Measured results

Every number here comes from [`docs/BENCHMARK.md`](docs/BENCHMARK.md), which
states its own methodology and boundaries. Apple M4 Pro, Release, median of
repeated runs.

| | |
|---|---|
| Order-to-ack p50 | **45 µs** at 5,000 orders/sec, 1 client |
| Order-to-ack p50 / p99 | 89 µs / 149 µs at 50,000 orders/sec, 10 clients |
| Sustained throughput | **250,000 orders/sec** at 50 clients, nothing rejected |
| WAL cost (`fsync`) | +11 µs p50, +52 µs p99 |
| WAL cost (`F_FULLFSYNC`) | 25× at the tail — see below |
| Snapshot pause | 28 ms at 100,000 resting orders |
| Crash recovery | **50/50** `kill -9` iterations, independent verifier |
| LOBSTER replay | ~270K messages parsed, applied and checked in ~0.5 s |

**Latency falls as load rises** — 185 µs at 100 orders/sec versus 45 µs at
5,000. Under load the matching thread's spin finds work and never parks; when
nearly idle every order pays a park-and-wake round trip. So "45 µs" is only
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
moment, restarts with recovery, and checks the rebuilt book. **50/50 passing.**

The check that matters is not that recovery succeeds — it is what it is checked
*against*. `tools/wal_verify` rebuilds the book from the same log using a
separate implementation: `std::map` price levels, its own matching loop, no
shared code with `OrderBook`. Both must produce the same digest. Without that,
the test would be the engine agreeing with itself — a matching bug would produce
the same wrong book on both sides and pass.

The harness was weaker than it looked at first. A deliberately broken verifier
passed 1/1, because the load generator quoted bids below asks and nothing ever
matched: both implementations agreed only that they could accumulate a book. It
now quotes crossing prices with varying sizes so orders partially fill and leave
real depth. With that fixed the broken verifier is caught immediately.

```bash
tools/kill_test.sh 50
```

**Scope, stated honestly.** `kill -9` destroys the process without letting it
flush, but it does not destroy the page cache, so everything already handed to
`write()` survives. This proves the recovery path, torn-tail handling, and the
append-before-apply ordering. It does **not** prove behavior under power loss,
which would additionally lose the group-commit window.

---

## How it works

**Wire protocol** ([`docs/PROTOCOL.md`](docs/PROTOCOL.md)) — length-prefixed
binary, little-endian, `int64` tick prices, no floating point anywhere.
Serialization is field by field, never a struct `memcpy`: `sizeof(NewOrder)` is
24 on this build while its wire encoding is 22, and those two padding bytes are
uninitialized memory a struct copy would put on the wire.

**Prices are integer ticks.** Not a style choice — durability asserts that a
recovered book's digest *equals* the original's, and a digest over doubles would
make that a float-equality claim dressed up as a guarantee.

**Write-ahead log** — the record is written before the command touches the book.
That ordering picks which failure mode a mid-command crash produces:
append-first leaves a record for a command that never reached the book, and
recovery replays it. Apply-first mutates the book for a command whose record
never landed, and nothing afterwards can detect it.

**Recovery uses the live apply path** — the same function live traffic uses, not
a separate replay path. Otherwise "the recovered book is identical" would only
mean two implementations happen to agree. Verified by a 100-seed property test
over random command streams.

**Risk checks live on the thread that owns the state they need.** Rate limiting
is on the network thread, so an over-limit client is refused before consuming
queue capacity. The price band is on the matching thread, because it is relative
to the last trade or the mid and only that thread may read the book.

---

## Market data analysis

[`analysis/stylized_facts.py`](analysis/stylized_facts.py) measures classic
microstructure facts directly from the LOBSTER message stream — no book
reconstruction, so none of it is affected by the replay accuracy gap. Results in
[`docs/STYLIZED_FACTS.md`](docs/STYLIZED_FACTS.md).

Across five symbols: **91–96% of submitted orders are cancelled rather than
executed**, trade-sign autocorrelation runs **0.72–0.91 at lag 1** and decays
slowly, and trade-level returns show excess kurtosis of 12–24.

Volatility clustering splits by tick constraint: clearly present in AAPL, AMZN
and GOOG at a 10-second horizon, and undetectable in INTC and MSFT — the two
low-priced names, pinned at a one-cent spread, where discrete bounce drowns the
signal.

---

## Limitations

Specific rather than hedged. Most are deliberate; all are real.

**Single symbol.** No instrument identifier in any message.

**No authentication, no TLS.** Anyone who can reach the port can trade. Binds
loopback only.

**One matching thread by design.** A real ceiling, not a bug. A second symbol
would get a second thread and a second book; a second thread on *one* book would
need the locking this architecture exists to avoid.

**Benchmarks are loopback on one machine.** Not a network. The load generator
shares cores with the gateway, and at high client counts some of the tail is its
own scheduling. At 100 clients × 5,000 orders/sec the generator — not the
gateway — stops making progress, so that cell is blank rather than guessed.

**The benchmark workload does not cross.** It measures the accept-and-acknowledge
path, not matching under contention. Published numbers were also taken with the
price band disabled and no subscriber attached.

**No self-trade prevention.** A session holding both sides will match itself.

**`Modify` is cancel-and-replace.** Time priority is lost on a reprice or a size
increase, retained on a size decrease.

**Ack precedes Fill.** An aggressive limit rests before it matches, so clients
must not read an ack as "did not trade".

**`BookUpdate` sequence numbers are not contiguous.** Conflation skips values by
design; a client treating a gap as loss would flag healthy behavior as an error.

**No delta encoding for market data** — every update is a full top-N snapshot.

**LOBSTER replay accuracy is 70% at 1K events and 50% beyond.** Faithful
reconstruction needs order-level state across the file window, and 21% of
executions in this sample are against hidden liquidity that never appears in the
book at all.

**Snapshots pause the matching thread** — 28 ms at 100K resting orders. The fix
is measured and known (the copy is 2.2 ms while the write is 26 ms, so
serializing a copy on a background thread would remove most of it) and not yet
implemented. Snapshots are off unless `--snapshot` is given.

**Cancel-on-disconnect is O(orders held).** A session disconnecting with a very
large book blocks the matching thread for the sweep.

**`poll()` is O(connections).** p50 goes 185 µs → 1,010 µs from 1 to 100
clients. `epoll`/`kqueue` are O(ready); not yet worth the portability cost.

---

## Repo layout

| Path | What |
|---|---|
| `include/order_book/`, `src/order_book.cpp` | the book: sorted vector of levels, deque FIFO per level |
| `include/matching_engine/` | order routing and the trade log |
| `include/ome/` | protocol, framing, sessions, WAL, snapshots, queues |
| `src/net/tcp_server.cpp` | `poll()` event loop |
| `bench/` | load generator and latency sweep |
| `tools/` | visualizer, live feed, WAL verifier, kill-test harness |
| `analysis/` | stylized facts from LOBSTER messages |
| `docs/` | protocol spec, benchmarks, measured market data facts |

Sanitizer builds — `address`, `undefined`, `address,undefined`, or `thread`
(thread and address are mutually exclusive):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DOME_SANITIZE=address,undefined
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

---

## LOBSTER data (optional)

Sample CSVs are **not** in git. Download a message + orderbook pair from
[LOBSTER samples](https://data.lobsterdata.com/info/DataSamples.php), place under
`data/lobster/`, then see [`data/lobster/README.md`](data/lobster/README.md).

```bash
./build/lobster_replay --messages <message.csv> --orderbook <orderbook.csv> \
  --events 1000 --jsonl /tmp/replay.jsonl
open tools/book_replay.html          # load /tmp/replay.jsonl
```

---

## What I want to improve next

- **LOBSTER parity** — order-level state across the file window, hidden
  liquidity, cross and auction messages. Per-event golden checks rather than an
  end snapshot.
- **Copy-then-write snapshots**, to remove the matching-thread pause.
- **A crossing benchmark**, to measure matching under contention rather than the
  accept path alone.
- **Synthetic order flow** calibrated to the LOBSTER data and run through the
  same engine, to see which stylized facts survive when strategic behavior is
  removed.
