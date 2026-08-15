# Order book & matching engine (C++17)

Limit-order **book**, **matching engine**, and **LOBSTER** (NASDAQ-style) CSV replay/validation. **Command-line only** — no web or desktop UI.

**First C++ project**: I’m still learning idioms, tooling, and the tradeoffs below so the code is a learning sandbox, not battle-tested infra.

I thought it would be good for showing **systems-style C++**, **CMake**, **tests**, and **real market data** ingestion — not a production exchange stack.

---

## Tech stack

- **C++17** · **CMake** · **GoogleTest** (103 tests)
- Sorted **vector** of price levels + **deque** FIFO per level · **hash map** for cancels by order id
- **Lock-free SPSC queue** (single producer / single consumer) for feed vs matcher
- Optional **Benchmarker** harness (latency percentiles, throughput)

---

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Needs **network once** on configure (GoogleTest via FetchContent). Build type
defaults to **Release** if you don't pass one. Builds with `-Wall -Wextra -Werror`.

Sanitizer builds — `address`, `undefined`, `address,undefined`, or `thread`
(thread and address are mutually exclusive):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DOME_SANITIZE=address,undefined
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

---

## LOBSTER data (optional)

Sample CSVs are **not** in git. Download a message + orderbook pair from [LOBSTER samples](https://data.lobsterdata.com/info/DataSamples.php), place under `data/lobster/`, then see [`data/lobster/README.md`](data/lobster/README.md).

```bash
./build/lobster_replay --help
```

### Visualizing a replay

`--jsonl` writes one top-of-book snapshot per applied message; open
[`tools/book_replay.html`](tools/book_replay.html) and load the file to scrub
through the book. The page is self-contained — no server, no CDN, no build step.

```bash
./build/lobster_replay --messages <message.csv> --orderbook <orderbook.csv> \
  --events 1000 --jsonl /tmp/replay.jsonl
open tools/book_replay.html
```

---

## Highlights (example numbers)

Measured on a **Release** build using the **AMZN** LOBSTER depth-10 sample; your machine may differ.

| What | About |
|------|--------|
| **Depth check** | After *N* messages, compares your top **10 bid + 10 ask** levels to the LOBSTER snapshot (**accuracy** = how many of **20** slots match). |
| **Sample accuracy** | **~70%** at 1k events · **~40–50%** at 5k–100k events (same AMZN file, default replay). |
| **Replay speed** | **~270k** messages parsed + applied + checked in **~0.5 s** on a typical laptop (**order 500k+ events/s** for that path). |

---

## What’s in the repo

| Piece | Role |
|-------|------|
| `OrderBook` | Bids best-first (high → low), asks best-first (low → high); limit orders, market sweep, cross, cancel |
| `MatchingEngine` | Routes orders and records `Trade`s |
| `LobsterParser` / validator | Read message CSV, replay events, diff against orderbook snapshot |
| `lobster_replay` | CLI to run validation on your local files |

**Size:** ~2k lines of project code (`include/`, `src/`, `tests/`).

---

## Design (short)

- **Vector + binary search** for price levels instead of `std::map` — locality over pointer-chasing.
- **SPSC queue** uses atomics with **64-byte aligned** head/tail to limit false sharing between cores.

LOBSTER replay treats executions as **reducing the passive side** at the event price (see validator), not as arbitrary market orders; see source for message-type details.

---

## What I want to improve next

Ideas I’m curious to tackle, open to any feedback.

- **LOBSTER / realism** — Tighter parity with the official reconstruction (order-level state across the file window, partial cancels when IDs aren’t in the book, cross/auction messages, hidden liquidity edge cases). Maybe golden checks **per event** instead of only an end snapshot.
- **Hardening** — `clang-tidy` / `clang-format`, **ASan/UBSan** in CI, `-Werror` cleanup, stricter parsing error reporting.
- **Performance** — Systematic **benchmarks** (different book depths and match patterns), profiling, and writing down what actually dominated (allocations, book walks, etc.).
- **Modern C++** — Move toward **C++20** where it helps (`std::span`, concepts, `std::format`), smarter ownership and error types (`expected`-style) instead of “log and continue.”
- **Ergonomics** — Richer **CLI** output (JSON / CSV diff), optional **verbose replay trace** for the first divergence event.

---
