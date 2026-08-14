# Order Gateway — Service Build Plan

**What this is:** the pivot plan that turns the C++17 matching engine from a file-replay library into a networked order gateway with crash recovery and observability. Written to be executed with Claude Code, session by session.

**Prerequisite:** Phase 0 below. Do not build a service on a core whose correctness number you can't explain — and right now the repo has no CI, no warnings-as-errors, no sanitizer builds, one test file, and prices stored as `double`. Phase 0 is three sittings that fix exactly the things Phases 1-4 assume are already true. It is not optional and it is not cuttable.

**The thesis of this build:** backend interviewers think in services — requests, concurrency, durability, latency percentiles, recovery. This plan wraps the engine in that shape. The end state is a project described in backend vocabulary: sessions, p99 latency, write-ahead log, deterministic recovery, metrics.

---

## Contents

- [How to work this plan](#how-to-work-this-plan)
- [Commit cadence](#commit-cadence)
- [Standing Context](#standing-context)
- [Architecture target](#architecture-target)
- [Session map and time estimates](#session-map-and-time-estimates)
- [Phase 0 — Groundwork](#phase-0--groundwork)
  - [Session 0.1 — Toolchain, CI, and the replay visualizer](#session-01--toolchain-ci-and-the-replay-visualizer)
  - [Session 0.2 — Integer ticks](#session-02--integer-ticks)
  - [Session 0.3 — Engine service surface](#session-03--engine-service-surface)
- [Phase 1 — Order Gateway](#phase-1--order-gateway)
  - [Session 1.1 — Protocol design](#session-11--protocol-design)
  - [Session 1.2 — TCP server skeleton](#session-12--tcp-server-skeleton)
  - [Session 1.3 — Session management](#session-13--session-management)
  - [Session 1.4 — Wiring the queue for real](#session-14--wiring-the-queue-for-real)
  - [Session 1.5 — Risk checks and validation](#session-15--risk-checks-and-validation)
  - [Session 1.6 — Market data broadcast](#session-16--market-data-broadcast)
  - [Session 1.7 — Load generator and latency benchmark](#session-17--load-generator-and-latency-benchmark)
- [Phase 2 — Durability](#phase-2--durability)
  - [Session 2.1 — Write-ahead log](#session-21--write-ahead-log)
  - [Session 2.2 — Recovery](#session-22--recovery)
  - [Session 2.3 — Snapshots](#session-23--snapshots)
  - [Session 2.4 — Kill testing](#session-24--kill-testing)
- [Phase 3 — Observability](#phase-3--observability)
  - [Session 3.1 — Metrics](#session-31--metrics)
  - [Session 3.2 — Structured logging](#session-32--structured-logging)
- [Phase 4 — Ship it](#phase-4--ship-it)
  - [Session 4.1 — README, diagram, demo](#session-41--readme-diagram-demo)
  - [Session 4.2 — Resume bullets and interview story](#session-42--resume-bullets-and-interview-story)
- [Full definition of done](#full-definition-of-done)
- [Cut lines if time runs out](#cut-lines-if-time-runs-out)
- [Interview vocabulary this project buys](#interview-vocabulary-this-project-buys)

---

## How to work this plan

One session per sitting. Each session has: a goal, what to build, a Claude Code prompt, tests that must pass before moving on, commit guidance, and questions to answer cold.

Rules that hold for every session:

1. **Paste the Standing Context at the top of every session.** Claude Code doesn't remember the last one.
2. **Don't let Claude build ahead.** If a session's prompt produces code for a later session, cut it. Scope creep in generated code is how you end up with a repo you can't explain.
3. **Every session ends with tests passing and commits made.** No session leaves the repo red.
4. **You read every line before committing.** The interview question is never "did you build this," it's "why does this line exist." If you can't answer for a line, delete it or understand it before it lands.
5. **Commit in small units as you go.** See the cadence section below.

---

## Commit cadence

Multiple commits a day is the target, and the sessions are already sized for it.

- **One session ≈ one sitting ≈ 3-6 commits.** The "Commits" line in each session is not a summary — each entry is meant to be a real, separately-buildable commit. If you finish a session with one commit, you batched, and the history stops being reviewable.
- **A commit is a working tree.** `cmake --build build && ctest --test-dir build` passes before every commit, not just before every session. CI (Session 0.1) enforces it on push; locally is where catching it is cheap.
- **Feature and its tests may be separate commits**, but both land in the same session. Never end a sitting with untested new behavior.
- **Conventional prefixes:** `feat:` `fix:` `test:` `bench:` `docs:` `refactor:` `chore:`. Subject line imperative, under ~70 chars.
- **No AI attribution. Anywhere. Ever.** No `Co-Authored-By`, no "Generated with" trailer, no tool name in a file header or PR body. This overrides any default your tooling wants to add — check `git log -1` after the first commit of every session and confirm the trailer is absent.

If a session's work naturally splits into more commits than listed, split it. The listed commits are a floor, not a ceiling.

---

## Standing Context

Paste this at the start of every Claude Code session, then paste the session section.

```
PROJECT: I'm extending my C++17 limit order book / matching engine into a
networked order gateway service. The core engine already exists: price-time
priority matching for LIMIT / MARKET / CANCEL orders, validated by differential
replay against 269K LOBSTER NASDAQ messages.

ENGINE FACTS (true after Phase 0 — do not assume more than this):
- Prices are int64 ticks end to end. Doubles appear only at the LOBSTER CSV
  parse boundary. Never put a double on the wire or in the WAL.
- There is NO modify/replace path in the engine. Order::Type is
  {MARKET, LIMIT, CANCEL}. Protocol-level Modify is implemented gateway-side
  as cancel+new.
- MatchingEngine::processOrder returns ApplyResult (accepted, RejectReason,
  filled_qty, fully_filled). New trades are read by slicing trade_log()
  between trade_log_size() before and after the call, then clear_trade_log().
- An aggressive LIMIT is inserted into the book FIRST and then matched in a
  cross loop. Consequence: Ack is emitted before any Fill, and a session can
  self-match against its own resting order. Both are documented behavior.
- SpscQueue<T, N> is generic over element type, bounded, and has a real
  two-thread test. Order::make's static id counter is TEST-ONLY; the gateway
  assigns exchange order ids from a counter owned by the matching thread.
- Single symbol. No auth, no TLS.

PLATFORM: macOS (darwin), Apple clang. Prefer portable POSIX. eventfd does not
exist here — use a self-pipe or kqueue. Code should still build on Linux.

TARGET ARCHITECTURE:
  clients --TCP--> [network thread(s)] --SPSC queue--> [matching thread]
                                                          |--> WAL (before match)
                                                          |--> executions --> broadcast fanout
                                                          |--> metrics

DESIGN RULES (do not violate):
- Single matching thread. All book mutations happen on it. No locks on the book.
- The SPSC queue is the only path from network to matching. Network threads
  never touch the book.
- Standard library + minimal deps only. No Boost, no gRPC, no framework.
  Sockets are raw POSIX (or a single small header lib if I approve it first).
- C++17, CMake, GoogleTest. Same toolchain as the existing repo. Builds clean
  under -Wall -Wextra -Werror.
- Every public behavior gets a test in the same session it's built. One test
  file per subject, named tests/test_<subject>.cpp.
- Errors are explicit: no silent drops. Every rejected order gets a reject
  message with a reason code from include/ome/reject_reason.hpp — that enum is
  the single source of truth shared by protocol, engine, metrics, and logs.
  Never invent a parallel reason enum.

WORKING RULES:
- Build ONLY what this session's scope says. If you think something later is
  needed now, tell me, don't build it.
- Explain any concurrency decision (memory ordering, ownership, lifetime)
  in comments at the decision site.
- After each logical unit, stop and give me one conventional-format commit
  message. Small commits, no batching.
- No AI attribution in commits, PR text, or file headers. Ever.
- I will read every line. Prefer boring, readable code over clever code.
```

---

## Architecture target

The picture to keep in your head (and eventually in the README):

```
                                   ┌─────────────────────────────────────────┐
                                   │              GATEWAY PROCESS             │
                                   │                                          │
 client A ──TCP──┐                 │  ┌──────────┐   SPSC    ┌────────────┐  │
 client B ──TCP──┼──> accept ──────┼─>│ network  │──queue───>│  matching  │  │
 client C ──TCP──┘   (listener)    │  │ thread   │           │  thread    │  │
                                   │  └────┬─────┘           └─────┬──────┘  │
                                   │       │ acks/rejects/fills    │         │
                                   │       │<───── egress queues ──┤         │
                                   │       │                       │         │
                                   │       │                 ┌─────▼─────┐   │
                                   │       │                 │    WAL    │   │
                                   │       │                 │ (fsync'd) │   │
                                   │       │                 └─────┬─────┘   │
                                   │       │                       │         │
                                   │  ┌────▼─────┐           ┌─────▼─────┐   │
                                   │  │broadcast │<──────────│ snapshots │   │
                                   │  │  fanout  │  book Δs  └───────────┘   │
                                   │  └──────────┘                           │
                                   │  /metrics endpoint (HTTP, read-only)    │
                                   └─────────────────────────────────────────┘
```

Key property to be able to say out loud: **the book is single-writer.** One thread owns all mutations; concurrency lives at the edges (network I/O, fanout, logging), and the queue is the seam. This is the design conversation in every interview this project generates.

---

## Session map and time estimates

A "sitting" is one focused block, roughly 2-4 hours. Estimates assume you read every line, which is the slow part and the whole point.

| Session | Sittings | Produces |
|---|---|---|
| 0.1 Toolchain, CI, visualizer | 1-2 | CI, `-Werror`, sanitizer builds, `book_replay.html`, JSONL format, split test files |
| 0.2 Integer ticks | 1 | `int64` prices throughout; LOBSTER accuracy unchanged |
| 0.3 Engine service surface | 1 | `RejectReason`, `ApplyResult`, trade drain, `SpscQueue<T,N>` + two-thread test |
| 1.1 Protocol design | 1 | `PROTOCOL.md`, `protocol.hpp`, round-trip tests |
| 1.2 TCP server skeleton | 1-2 | listener, `FrameReader`, write buffering, smoke client |
| 1.3 Session management | 1 | heartbeats, cancel-on-disconnect stub, dup id rejection |
| 1.4 Wiring the queue | 2 | the architectural core; TSan-clean pipeline test |
| 1.5 Risk and validation | 1 | two-layer checks, token bucket, config file |
| 1.6 Market data broadcast | 1 | subscribe, conflation, `live_feed.py` |
| 1.7 Load generator | 1-2 | p50/p99 sweep, `BENCHMARK.md` results |
| 2.1 Write-ahead log | 1 | append path, group commit, CRC |
| 2.2 Recovery | 1-2 | replay, `digest()`, 100-seed property test |
| 2.3 Snapshots | 1 | atomic snapshots, bounded recovery |
| 2.4 Kill testing | 1-2 | independent verifier, 50-iteration harness |
| 3.1 Metrics | 1 | registry, `/metrics` endpoint |
| 3.2 Structured logging | 1 | async JSON-lines logging |
| 4.1 README, diagram, demo | 1-2 | restructured README, SVG, GIFs |
| 4.2 Interview prep | 1 | bullets, walkthroughs, adversarial drill |

**Total: 20-27 sittings.** Phase 0 is 3-4 of them and removes blockers that would otherwise stall 1.1, 1.4, 1.6, 2.2, and 4.1 mid-session.

---

# Phase 0 — Groundwork

**Outcome:** the repo actually matches what the rest of this plan assumes. CI green, warnings-as-errors clean, sanitizer builds available, prices are integers, and the engine exposes the surface a service needs.

**Why this phase exists:** this plan was drafted against an idealized version of the repo. The real one has one test file, no CI, `double` prices, an SPSC queue hardcoded to `Order`, a `processOrder` that returns `void`, and no visualizer. Each of those blocks a specific later session. Fixing them costs three sittings now and progressively more later — Session 0.2 in particular becomes a wire-format-and-WAL rewrite if deferred past Phase 2.

**Time estimate:** 3-4 sittings.

---

## Session 0.1 — Toolchain, CI, and the replay visualizer

**Goal:** a build that fails loudly, a CI that runs on every push, sanitizer configurations ready before the concurrency work needs them, and the file-replay visualizer that Sessions 1.6 and 4.1 both assume exists.

**Build**

- `CMakeLists.txt`: default `CMAKE_BUILD_TYPE=Release` when the user didn't set one; `-Wall -Wextra -Werror` on the project targets (not on the fetched GoogleTest); an `OME_SANITIZE` option taking `address`, `undefined`, or `thread`, applied to **both** compile and link flags.
- `.clang-format` — LLVM base, 100 columns, 4-space indent to match the existing style.
- `.github/workflows/ci.yml` — configure, build, `ctest`; a second job building with `OME_SANITIZE=address,undefined` and running the tests under it.
- `docs/BENCHMARK.md` — skeleton only: a methodology section stating hardware, compiler and flags, warm-up policy, run count and that the median of runs is reported. Later sessions fill in tables; nobody invents methodology under time pressure at the end.
- `tools/book_replay.html` — self-contained page, no CDN, that reads a JSONL file and animates the book. **Define the record format here, once:**
  ```
  {"t":<u64 ns>,"seq":<u64>,"bids":[[<i64 ticks>,<u64 qty>],...],"asks":[[...]]}
  ```
  One JSON object per line, depth capped at 10 per side, `bids` descending by price, `asks` ascending.
- `--jsonl <path>` flag on `lobster_replay` emitting exactly that format. Feed it from the existing `bid_levels_ticks()` / `ask_levels_ticks()` accessors on `OrderBook` — they already return `(int64 ticks, uint64 qty)` pairs, which is precisely why the format uses ticks.
- Split `tests/order_book_test.cpp` (302 lines, 17 tests, four unrelated subjects) into `test_order_book.cpp`, `test_matching.cpp`, `test_lobster.cpp`, `test_spsc.cpp`. Every later session adds a test file — establish the convention now, not at ten files.

**The `-Werror` fallout is the real work here.** Fix warnings properly; do not suppress them. If something needs a suppression, it needs a comment saying why.

**Prompt**

```
[Standing Context above]

SESSION SCOPE: build hygiene, CI, and the replay visualizer. No networking,
no protocol, no engine behavior changes.

1. CMakeLists.txt:
   - default CMAKE_BUILD_TYPE to Release if the user set none
   - -Wall -Wextra -Werror on order_book/main/lobster_replay/tests only,
     NOT on the FetchContent'd googletest targets
   - option OME_SANITIZE (empty|address|undefined|thread) applied to both
     compile and link flags. Verify all three configure and build.
   Then fix every warning the new flags surface. Show me each fix and why —
   do not blanket-suppress, do not cast warnings away.

2. .clang-format: LLVM base, ColumnLimit 100, IndentWidth 4. Do NOT reformat
   the whole repo in this session — add the config and a format check only.

3. .github/workflows/ci.yml: job 1 = configure/build/ctest on Release.
   job 2 = OME_SANITIZE=address,undefined build + ctest.

4. docs/BENCHMARK.md: methodology skeleton only (hardware, compiler+flags,
   warm-up excluded, >=10 runs, median of runs reported, what's in and out of
   the timed path). No numbers yet.

5. tools/book_replay.html: single self-contained file, no external requests.
   Loads a JSONL file the user picks, animates top-10 book levels over time,
   play/pause/scrub. Record format exactly:
   {"t":<u64 ns>,"seq":<u64>,"bids":[[<i64 ticks>,<u64 qty>],...],"asks":[...]}
   Document that format in a comment at the top of the file AND in
   docs/PROTOCOL.md's future home — for now, a header comment is enough.

6. Add --jsonl <path> to lobster_replay, emitting that format from
   bid_levels_ticks()/ask_levels_ticks(). One record per applied message.

7. Split tests/order_book_test.cpp into test_order_book.cpp, test_matching.cpp,
   test_lobster.cpp, test_spsc.cpp. Pure move — no test logic changes. Update
   CMakeLists to build them all into the tests target.

Note: README says 16 tests, there are 17. Fix the README count.
```

**Must pass:** `ctest` green on Release; green under `OME_SANITIZE=address,undefined`; `OME_SANITIZE=thread` at minimum *configures and builds*; CI green on push; `lobster_replay --jsonl` output loads in `book_replay.html` and animates.

**Commits:** `build: warnings-as-errors and sanitizer configurations` · `fix: resolve -Wall -Wextra warnings` · `chore: clang-format config` · `ci: build, test, and sanitizer workflow` · `docs: benchmark methodology skeleton` · `feat: JSONL book snapshot output for lobster_replay` · `chore: standalone book replay visualizer` · `test: split test file by subject` · `docs: correct test count in README`

**Answer cold**

- What did `-Werror` catch, and was any of it a real bug?
- Why does the TSan configuration exist three sessions before anything is threaded?

---

## Session 0.2 — Integer ticks

**Goal:** prices become `int64_t` ticks everywhere inside the engine. `double` survives only where LOBSTER CSVs are parsed.

**Why now, and why this is the highest-leverage hour in Phase 0:** Phase 2 stakes its entire claim on `digest()` comparing **equal** across a crash and recovery. A digest over `double` prices is a float-equality claim dressed up as a durability guarantee. Session 1.5's "price tick-aligned" risk check is also undefinable on doubles without an epsilon nobody can justify. And the wire format and WAL record layout are both built on top of this decision — changing it after Session 2.1 means rewriting both.

**Build**

- `include/order.h`: `double price` → `int64_t price_ticks`.
- `include/order_book/order_book.hpp`: `PriceLevel::price` → `price_ticks`; `order_loc_` becomes `unordered_map<uint64_t, pair<Order::Side, int64_t>>`.
- `src/order_book.cpp`: delete every `std::llround` / `kTickScale` conversion *inside* comparisons and level lookup. Price comparison becomes plain integer comparison.
- New `include/ome/ticks.hpp`: `constexpr int64_t kTicksPerUnit = 10000;` plus `ticks_from_price(double)` and `price_from_ticks(int64_t)`. Used **only** by the LOBSTER parser and by CLI/display output.
- `bid_levels_ticks()` / `ask_levels_ticks()` become trivial pass-throughs rather than converting.
- `lobster_parser.cpp` converts at parse; `lobster_validator.cpp` compares in ticks.
- Update the existing tests to construct orders in ticks.

**The regression check that proves this was mechanical:** run `lobster_replay` on the AMZN sample at 1k, 5k, and 100k events before and after. The accuracy numbers must be **identical**. Record both runs in the commit message. If they differ, the refactor changed behavior and you need to find out why before moving on.

**Prompt**

```
[Standing Context]

SESSION SCOPE: convert prices from double to int64 ticks. Behavior-preserving
refactor only. No new features, no networking.

BEFORE YOU START: run lobster_replay on the AMZN sample at --events 1000,
5000, and 100000 and record the accuracy numbers. These must be unchanged
at the end. If they move, we stop and find out why.

1. include/ome/ticks.hpp: constexpr int64_t kTicksPerUnit = 10000;
   ticks_from_price(double) -> int64_t (llround), price_from_ticks(int64_t)
   -> double. Comment that these are boundary-only helpers: LOBSTER parsing
   and human-readable output. Anything in the matching path that calls them
   is a bug.

2. Order::price (double) -> Order::price_ticks (int64_t). Update Order::make.

3. order_book.hpp/.cpp: PriceLevel::price -> price_ticks; order_loc_ value
   type to (Side, int64_t); remove every llround/kTickScale use inside
   comparisons, binary search, and level lookup — integer compare directly.
   bid_levels_ticks/ask_levels_ticks become pass-throughs.

4. lobster_parser.cpp converts at parse time via ticks_from_price.
   lobster_validator.cpp compares in ticks.
   lobster_replay.cpp converts back only for printed output.

5. Update all existing tests to construct orders in ticks. No test logic
   changes beyond the units.

6. Re-run the three lobster_replay measurements. Show me before/after
   side by side.

Tell me if you find any place where the double->tick conversion was
previously lossy or inconsistent — that's a latent bug and I want to know.
```

**Must pass:** all tests green; LOBSTER accuracy at 1k/5k/100k events byte-identical to the pre-refactor run; no `double` remaining in `order.h`, `order_book.hpp`, or the matching path (`grep double` on those files returns nothing but comments).

**Commits:** `feat: tick conversion helpers at the data boundary` · `refactor!: represent prices as int64 ticks in the book and engine` · `refactor: convert LOBSTER parse and validation to ticks` · `test: update order construction to ticks`

**Answer cold**

- Why don't exchanges use floating point for price?
- What specifically would have gone wrong in the recovery digest if prices stayed `double`?
- Where does a `double` still legitimately appear, and why is that safe?

---

## Session 0.3 — Engine service surface

**Goal:** close the four API gaps that make Session 1.4 unwritable as the plan currently describes it.

**Build**

- `include/ome/reject_reason.hpp` — `enum class RejectReason : uint16_t` with **ten** codes and a `const char* to_string(RejectReason)`:
  `NONE`, `UNKNOWN_ORDER`, `INVALID_PRICE`, `INVALID_QTY`, `RISK_MAX_ORDER_SIZE`, `RISK_PRICE_BAND`, `MALFORMED`, `RATE_LIMITED`, `NOT_SUBSCRIBED`, `DUPLICATE_ORDER_ID`, `UNKNOWN_MESSAGE_TYPE`.
  (The plan's original list of 8 grows by `DUPLICATE_ORDER_ID`, which Session 1.3 needs, and `UNKNOWN_MESSAGE_TYPE`, which Session 1.1's own tests require. Numeric values are explicit and never reordered — they go on the wire.)
- `MatchingEngine::processOrder` returns `ApplyResult{ bool accepted; RejectReason reason; uint32_t filled_qty; bool fully_filled; }` instead of `void`. An unknown-id cancel now reports `UNKNOWN_ORDER` rather than a `static_cast<void>`-discarded `bool`.
- Trade drain: `size_t trade_log_size() const` and `void clear_trade_log()`. The matching thread records the size before apply, slices `trade_log()` after to get the trades *this command* produced, dispatches them, then clears. This also bounds a vector that currently grows for the process lifetime. The LOBSTER replay path never calls `clear_trade_log()`, so its behavior is unchanged.
- `SpscQueue<T, N>` — templated on element type. Same memory ordering, same 64-byte alignment, keep the existing comments (they're the best-explained code in the repo). Existing uses become `SpscQueue<Order, N>`.
- **The first real two-thread test:** producer thread pushes N items, consumer thread pops until it has all N, assert every item arrives exactly once and in order. Must run clean under `OME_SANITIZE=thread`. Today the queue is only exercised single-threaded, which is a thin foundation for this project's headline concurrency claim.
- Comment on `Order::make` marking the static counter test-only, pointing at the matching-thread-owned counter the gateway will use.

**Prompt**

```
[Standing Context]

SESSION SCOPE: engine API surface for service use. No networking, no protocol,
no threads beyond the queue test.

1. include/ome/reject_reason.hpp: enum class RejectReason : uint16_t with
   explicit numeric values — NONE=0, UNKNOWN_ORDER=1, INVALID_PRICE=2,
   INVALID_QTY=3, RISK_MAX_ORDER_SIZE=4, RISK_PRICE_BAND=5, MALFORMED=6,
   RATE_LIMITED=7, NOT_SUBSCRIBED=8, DUPLICATE_ORDER_ID=9,
   UNKNOWN_MESSAGE_TYPE=10. Plus to_string(). Comment: these values go on the
   wire, so they are append-only forever — never reorder, never reuse.

2. MatchingEngine::processOrder returns ApplyResult{accepted, reason,
   filled_qty, fully_filled} instead of void. Unknown-id CANCEL ->
   accepted=false, reason=UNKNOWN_ORDER. Update all call sites and tests.

3. Add trade_log_size() and clear_trade_log(). Comment the intended usage
   pattern: record size, apply, slice trade_log() from that index, clear.
   Do not change the lobster replay path's behavior.

4. Make SpscQueue generic: SpscQueue<T, N>. Keep every existing comment about
   memory ordering and false sharing — they are correct and I want them
   preserved verbatim where still accurate. Update existing uses to
   SpscQueue<Order, N>.

5. tests/test_spsc.cpp: add a real two-thread test. Producer pushes N=100000
   sequence-numbered items (spinning on a full queue), consumer pops until it
   has N (spinning on empty), assert exact ordering and no loss/duplication.
   Keep it under a few seconds. This must pass under OME_SANITIZE=thread.

6. Comment on Order::make: its static next_id is TEST-ONLY. Note that the
   gateway assigns exchange order ids from a counter owned exclusively by the
   matching thread, and that a non-atomic static shared across network threads
   would be a data race.

7. Add the thread sanitizer job to CI now that there's something to check.
```

**Must pass:** all tests green; the two-thread SPSC test green under TSan; the LOBSTER replay path's output unchanged.

**Commits:** `feat: shared reject reason codes` · `feat: ApplyResult return from processOrder` · `feat: incremental trade log drain` · `refactor: generic SpscQueue element type` · `test: two-thread SPSC ordering under ThreadSanitizer` · `ci: thread sanitizer job`

**Answer cold**

- Why are the reject code numeric values append-only?
- What could go wrong with `Order::make`'s counter once network threads exist? Is it a race in C++'s formal sense?
- What does the two-thread SPSC test prove, and what does it still not prove?

**PHASE 0 COMPLETE.** The Standing Context is now true. Everything below can be executed as written.

---

# Phase 1 — Order Gateway

**Outcome:** clients connect over TCP, submit orders, receive acks/rejects/fills. Book updates broadcast to subscribers. Load generator measures order-to-ack latency under concurrency.

**Time estimate:** 6-8 working sessions.

---

## Session 1.1 — Protocol design

**Goal:** a written protocol spec before any socket code. Protocol-first prevents the ad-hoc JSON soup that makes student networking projects unreviewable.

**Build**

- `docs/PROTOCOL.md` defining every message in both directions
- `include/ome/protocol.hpp` with the message structs and serialization functions
- Round-trip unit tests: struct → bytes → struct, byte-identical

**Decisions to make in this session (with Claude presenting tradeoffs, you choosing):**

1. **Wire format.** Two respectable options:
   - Length-prefixed JSON: trivially debuggable with `nc`, slower, fine for this project's claims
   - Length-prefixed binary (fixed-layout structs): faster, teaches serialization discipline, harder to debug
   - Recommended: **binary with a tiny JSON debug mode**, because the serialization conversation ("how do you handle endianness? struct padding? versioning?") is interview gold. But JSON-only is acceptable if timeline is tight.
2. **Framing.** 4-byte little-endian length prefix, then payload. Non-negotiable — no delimiter-based framing.
3. **Message set (minimum):**
   - Client → server: `NewOrder{client_order_id, side, type, price_ticks, qty}`, `Cancel{client_order_id}`, `Modify{client_order_id, new_price_ticks, new_qty}`, `Subscribe{depth}`
   - Server → client: `Ack{client_order_id, exchange_order_id}`, `Reject{client_order_id, reason_code}`, `Fill{exchange_order_id, price_ticks, qty, remaining}`, `BookUpdate{...}`, `Heartbeat`
4. **Prices are `int64` ticks on the wire. There are no floating-point fields in this protocol at all.** Phase 0 made this true in the engine; the wire format inherits it. Say so explicitly in `PROTOCOL.md` — a reviewer looking for the one thing student exchange protocols get wrong will look here first.
5. **`Modify` semantics — decide and document now, because it constrains 1.4.** The engine has no modify path (`Order::Type` is `{MARKET, LIMIT, CANCEL}`). Modify is implemented **gateway-side as cancel + new order**, which means: **time priority is lost on any price change and on any size increase, and retained only on a size decrease.** That is what real exchanges do, it needs no engine surgery, and it is a good answer when asked. Specify it in `PROTOCOL.md` as protocol-level behavior, not as an implementation detail — clients need to know their queue position changed.
6. **Reason codes come from `include/ome/reject_reason.hpp`** (Session 0.3), which already has all ten. Do **not** define a second enum in the protocol layer — serialize the shared one directly. Its numeric values are append-only forever because they go on the wire.
7. **Document the two engine behaviors clients can observe** (from Phase 0's Standing Context): a marketable limit produces `Ack` **before** its `Fill`s, because the engine rests then matches; and a session holding both sides can self-match. Neither is a bug to hide — both are protocol-visible and belong in the spec.

**Prompt**

```
[Standing Context above]

SESSION SCOPE: protocol design only. No sockets, no threads, no engine changes.

1. Draft docs/PROTOCOL.md for a length-prefixed binary protocol over TCP.
   Present me the tradeoff between binary fixed-layout and JSON payloads first,
   with 3 bullets each, and wait for my choice before writing the spec.

2. After I choose: write include/ome/protocol.hpp with
   - message structs for: NewOrder, Cancel, Modify, Subscribe (client->server)
     and Ack, Reject, Fill, BookUpdate, Heartbeat (server->client)
   - explicit serialize/deserialize functions per message (no reflection magic,
     no memcpy of whole structs across the wire — field by field, explicit
     endianness)
   - a MessageHeader{u32 length, u16 type, u16 version}
   - prices as int64 ticks. NO floating point fields anywhere in this
     protocol. Assert that in a comment.
   - reason codes: serialize the existing RejectReason from
     include/ome/reject_reason.hpp. Do NOT define a second enum here.

3. In docs/PROTOCOL.md, specify explicitly:
   - Modify = cancel + new order. Time priority is LOST on a price change or
     a size increase, RETAINED on a size decrease. This is client-visible
     behavior, so it is spec, not an implementation note.
   - A marketable limit order receives its Ack BEFORE its Fills.
   - A client holding both sides of the book can self-match.
   - Reject code values are append-only and never reordered or reused.

4. Write tests/test_protocol.cpp: round-trip every message type, truncated
   buffer handling, unknown message type handling (-> UNKNOWN_MESSAGE_TYPE),
   version mismatch handling.

Explain in comments why field-by-field serialization instead of memcpy
(padding, endianness, versioning).
```

**Must pass before moving on:** all round-trip tests green, including the malformed-input cases.

**Commits (roughly):** `docs: protocol specification v1` · `feat: protocol message types and serialization` · `test: protocol round-trip and malformed input coverage`

**Answer cold**

- Why field-by-field serialization instead of memcpy'ing the struct?
- Why length-prefix framing instead of a delimiter?
- What happens when the server receives a message type it doesn't know?
- A client modifies an order's size downward. Does it keep its place in the queue? What if it modifies upward, and why is that rule the fair one?

---

## Session 1.2 — TCP server skeleton

**Goal:** a listener that accepts connections, reads framed messages, echoes acks. No engine wiring yet — the network layer proven in isolation.

**Build**

- `src/net/tcp_server.cpp` — POSIX sockets: `socket/bind/listen/accept`, non-blocking I/O
- Event loop with `poll()` (or `epoll` if Linux-only, `kqueue` if macOS-only — `poll()` is portable and fine at this scale; make the choice explicit and defensible)
- Read-side framing: accumulate bytes per connection, extract complete frames, handle partial reads
- Write-side buffering: handle partial writes, per-connection outbound buffer with a cap

**The two bugs this session exists to force you through:** partial reads (a frame arriving in 3 TCP segments) and partial writes (kernel buffer full mid-message). Every backend engineer has fought these; having fought them is the point.

**Prompt**

```
[Standing Context]

SESSION SCOPE: TCP server skeleton. No engine integration, no matching thread.
When a complete, valid NewOrder frame arrives, respond with a hardcoded Ack.

1. Build src/net/tcp_server.cpp:
   - POSIX sockets, non-blocking, single network thread, poll()-based loop
   - per-connection read buffer that correctly reassembles frames across
     partial reads — write this as a standalone FrameReader class I can
     unit test without sockets
   - per-connection write buffer that handles partial writes, with a max
     buffer size; if a client stops reading and the buffer would exceed the
     cap, disconnect it (slow-consumer policy) — also a standalone class
   - clean disconnect handling, no fd leaks

2. tests/test_framing.cpp: FrameReader with frames split at every possible
   byte boundary (parameterized test), multiple frames in one read, garbage
   before a valid frame.

3. A 20-line python3 script tools/smoke_client.py that connects, sends one
   NewOrder, prints the Ack. This is my manual smoke test.

Comment the poll loop: what each event flag means and why we watch it.
Explain in a comment why the frame reassembly can't assume one recv() = one frame.
```

**Must pass:** framing tests at every split boundary; smoke client gets its ack; server survives `kill -9` of the client and a client sending garbage.

**Commits:** `feat: TCP listener with poll-based event loop` · `feat: frame reassembly across partial reads` · `feat: write buffering with slow-consumer disconnect` · `test: framing at all byte boundaries` · `chore: python smoke client`

**Answer cold**

- Walk through what happens when a 100-byte frame arrives in three TCP segments.
- What's your slow-consumer policy and why is unbounded buffering dangerous?
- Why non-blocking sockets with poll instead of a thread per connection? At what scale would you revisit that?

---

## Session 1.3 — Session management

**Goal:** connections become sessions with identity and lifecycle.

**Build**

- `include/ome/session.hpp` — session id, connection state, owned order ids
- Heartbeats: server sends every N seconds; client considered dead after 3 missed; dead sessions cleaned up
- **Cancel-on-disconnect:** when a session dies, all its resting orders are cancelled. This is a real exchange behavior and the best interview story in Phase 1 — it forces the question of how the network thread safely tells the matching thread to cancel orders it doesn't own.
- Client-order-id → exchange-order-id mapping per session; duplicate client ids within a session rejected

**Prompt**

```
[Standing Context]

SESSION SCOPE: session layer on top of the TCP server. Still no matching
engine — stub the "cancel all orders for session X" as a logged no-op that
I'll wire in session 1.4.

1. Session struct: id, socket fd, state (CONNECTED/AUTHENTICATED if trivial,
   or just CONNECTED/DEAD), last-heartbeat timestamp, set of live
   client_order_ids.

2. Heartbeat: server->client Heartbeat every 5s per session; mark dead after
   15s of silence; sweep dead sessions each loop iteration; on death, emit a
   CancelAllForSession command (stub).

3. Duplicate client_order_id within a session -> Reject{DUPLICATE_ORDER_ID}.
   The reason code already exists (Session 0.3) — this session adds the
   enforcement, not the enum value.

4. tests/test_session.cpp: heartbeat timeout marks dead; duplicate id
   rejected; disconnect triggers cancel-all stub exactly once.

Design question to answer in comments: the session map is touched by the
network thread. When the matching thread later needs to send a Fill to a
session, how does that message get back to the right socket without the
matching thread touching the session map? (Answer we're building toward:
per-session egress queue drained by the network thread — say this in the
comment so future-me knows the plan.)
```

**Must pass:** session tests; manual test of killing the smoke client and seeing the cancel-all stub fire once.

**Commits:** `feat: session lifecycle with heartbeat timeout` · `feat: cancel-on-disconnect stub and duplicate order id rejection` · `test: session lifecycle coverage`

**Answer cold**

- Why cancel-on-disconnect? What goes wrong at a real exchange without it?
- How will a Fill generated on the matching thread reach the right socket?

---

## Session 1.4 — Wiring the queue for real

**Goal:** the load-bearing moment. Network thread produces into the SPSC queue; matching thread consumes; results flow back through egress queues. After this session, the queue you built in the spring is structurally necessary.

**Build**

- Inbound: `OrderCommand{session_id, type, payload}` — the single type that crosses the queue. Carried by `SpscQueue<OrderCommand, N>`, which is generic as of Session 0.3.
- The matching thread: a loop that drains the queue, applies commands to the engine, produces `OrderEvent{session_id, Ack|Reject|Fill}` results.
- **Reading results out of the engine.** `processOrder` returns `ApplyResult` (Session 0.3), which gives you accept/reject and the reason directly. The *trades* are obtained by the documented pattern: record `trade_log_size()`, apply, slice `trade_log()` from that index, dispatch the fills, then `clear_trade_log()`. Do this every iteration so the log stays bounded.
- **The `order_id → session_id` ownership map.** This is the piece the original plan skipped and nothing works without: `Trade` carries `buyer_id` and `seller_id`, which are *order* ids, not session ids. To route a fill to the right socket, the matching thread keeps `unordered_map<uint64_t /*exchange_order_id*/, SessionId>`, inserted on ack, erased when an order fills to zero or is cancelled. This same map is what makes `CancelAllForSession` possible. **It is owned exclusively by the matching thread** — it is book-adjacent state, and putting it anywhere else reintroduces the locking this architecture exists to avoid.
- **Exchange order id assignment** comes from a plain `uint64_t` counter owned by the matching thread. Not `Order::make` — its static counter is test-only (Session 0.3) and a non-atomic increment reached from network threads would be a race.
- **`Modify` = cancel + new order**, executed as a unit on the matching thread, with the time-priority rule from `PROTOCOL.md`. No engine change; one path.
- Egress: per-session SPSC queues (matching thread produces, network thread consumes and writes to sockets). The network thread's poll loop now also drains egress queues.
- Wake-up strategy: the matching thread busy-spins with a bounded backoff, or blocks on a **self-pipe** the producer writes a byte to (`eventfd` is Linux-only and this is macOS; `kqueue` is the platform-native alternative but ties the loop to Darwin). **Make this an explicit decision** — "how does the consumer sleep" is a real design conversation.
- Wire cancel-on-disconnect for real: the stub becomes a `CancelAllForSession` command through the same queue. One path for everything.

**Ordering note carried from the protocol:** because the engine rests an aggressive limit before matching it, `Ack` is pushed to the egress queue before any resulting `Fill`. That ordering must be preserved end to end — it is what the client's state machine expects.

**Prompt**

```
[Standing Context]

SESSION SCOPE: connect network to engine through the SPSC queue. This is the
architectural core of the project — go slow, comment heavily.

1. Define OrderCommand (session_id + variant of NewOrder/Cancel/Modify/
   CancelAllForSession) and OrderEvent (session_id + variant of
   Ack/Reject/Fill). These are the ONLY types that cross threads.

2. Matching thread main loop: drain inbound SPSC queue, apply to engine,
   push events to the correct per-session egress queue. The engine and book
   are touched by this thread ONLY — assert thread identity in debug builds.

   Per command: record trade_log_size(), call processOrder, read the returned
   ApplyResult for accept/reject+reason, slice trade_log() from the recorded
   index for the fills this command caused, then clear_trade_log(). Do not
   let the trade log grow across iterations.

3. Matching-thread-owned state (NOT shared, NOT locked):
   - unordered_map<uint64_t exchange_order_id, SessionId> owner_of_
     Inserted on ack, erased on fill-to-zero and on cancel. Trade carries
     ORDER ids, not session ids, so this map is the only way a Fill reaches
     the right socket. Comment that clearly at the definition.
   - uint64_t next_exchange_order_id_ — a plain counter, no atomics needed
     because only this thread touches it. Do NOT use Order::make.
   - A per-session set of live client_order_ids for CancelAllForSession.

4. Modify: implement as cancel + new order applied as a unit on the matching
   thread, following the time-priority rule already written in PROTOCOL.md.
   No engine changes.

5. Per-session egress queues (SPSC: matching produces, network consumes).
   Network thread drains them in its poll loop and writes to sockets through
   the existing write-buffer path. Preserve Ack-before-Fill ordering.

6. Consumer wake-up: present me both options (bounded spin+backoff vs a
   self-pipe the producer writes to) with the latency/CPU tradeoff, wait for
   my pick, implement it. NOTE: eventfd does not exist on macOS. If you want
   to argue for kqueue instead of a self-pipe, make the portability cost
   explicit and let me decide.

7. Replace the cancel-on-disconnect stub with a real CancelAllForSession
   command through the queue.

8. tests/test_pipeline.cpp: an in-process integration test that spins up the
   real matching thread, injects commands, and asserts the right events come
   out of the right egress queues. Include: order -> ack -> aggressive order
   from another session -> both sides get correct fills, each routed via
   owner_of_ to the correct session. Also test: modify losing priority on a
   price change; cancel of an unknown id -> Reject{UNKNOWN_ORDER}.

9. Lifetime question to answer in comments: a session dies while its egress
   queue still has events. Who frees the queue, and how do we guarantee the
   matching thread isn't mid-push when it's freed? Implement the answer
   (likely: queues owned by a registry, tombstoned by the matching thread
   itself via the command stream, freed only by the matching thread).

Do not add the WAL yet. Do not add broadcast yet.
```

**Must pass:** pipeline integration test including cross-session fills; a manual two-client smoke test where client A's resting order is filled by client B and both see correct messages; ThreadSanitizer build (`-fsanitize=thread`) runs the pipeline test clean.

**The TSan run is not optional.** "I ran it under ThreadSanitizer" is one sentence that separates you from every student who says "lock-free" and hopes.

**Commits:** `feat: OrderCommand/OrderEvent thread-crossing types` · `feat: matching thread loop with single-writer book ownership` · `feat: order-to-session ownership map for fill routing` · `feat: modify as cancel-replace on the matching thread` · `feat: per-session egress queues` · `feat: cancel-on-disconnect through the command path` · `test: cross-session fill pipeline integration`

(The ThreadSanitizer build config already exists from Session 0.1 — this session uses it rather than adding it.)

**Answer cold**

- Why does the matching thread own egress queue destruction?
- A `Trade` gives you two order ids. How does a fill find its socket?
- What did you choose for consumer wake-up and what does it cost?
- What did TSan check, and what wouldn't it catch? (Answer includes: it detects data races on this execution's interleavings; it doesn't prove your memory ordering is minimal or your algorithm correct.)

---

## Session 1.5 — Risk checks and validation

**Goal:** the gateway rejects bad orders before they reach the book. Small session, big interview value — "where do checks live and why" is a system-design staple.

**Build**

- Pre-queue (network thread): malformed frames, unknown types, duplicate client ids, per-session rate limit (token bucket, N orders/sec)
- Post-queue (matching thread): max order size, price band (reject orders > X% away from last trade or mid), qty > 0, price positive and within range. **Note:** with integer ticks (Session 0.2), the price is a tick count by construction — there is no "tick alignment" check to write unless you introduce a tick *size* greater than 1. If you want that check, define `kTickSize` in config and reject `price_ticks % kTickSize != 0`; otherwise drop the check and say why in a comment. Do not write an epsilon comparison — there are no floats here.
- **The design point:** stateless-per-session checks live on the network thread (cheap, keeps garbage out of the queue); anything needing book state lives on the matching thread (only it can read the book). Be able to say this unprompted.

**Prompt**

```
[Standing Context]

SESSION SCOPE: validation and risk checks. Two layers.

1. Network-thread checks (before enqueue): malformed -> Reject{MALFORMED},
   duplicate client id (exists), token-bucket rate limit per session ->
   Reject{RATE_LIMITED}. Token bucket as a standalone tested class.

2. Matching-thread checks (before book): qty > 0 and <= MAX_ORDER_QTY ->
   Reject{INVALID_QTY} / Reject{RISK_MAX_ORDER_SIZE}; price_ticks > 0 and
   (if kTickSize > 1) divisible by kTickSize -> Reject{INVALID_PRICE};
   price within +/-10% of reference (last trade, else mid, else skip) ->
   Reject{RISK_PRICE_BAND}. All integer arithmetic — compute the band with
   integer math and say in a comment how you handle the rounding.

3. Config struct for the limits, loaded from a simple key=value file. No
   YAML/JSON dependency.

4. tests/test_risk.cpp: each check has an accept-boundary and reject-boundary
   case. Token bucket: burst allowed up to capacity, then limited, refill
   over time (use a fake clock — make the bucket take a clock interface).

In comments: state why each check lives on the thread it lives on.
```

**Must pass:** boundary tests both sides of every limit; fake-clock token bucket tests deterministic.

**Commits:** `feat: network-side validation and per-session rate limiting` · `feat: matching-side risk checks with config` · `test: risk boundary and token bucket coverage`

**Answer cold**

- Why does the price band check have to be on the matching thread?
- Token bucket vs fixed window — what burst behavior does each allow?

---

## Session 1.6 — Market data broadcast

**Goal:** subscribers receive book updates. Fanout is the last architectural piece.

**Build**

- `Subscribe{depth}` puts the session on the broadcast list
- After each book-mutating command, matching thread emits a book delta (or top-N snapshot — start with snapshot-per-change at depth ≤ 10; deltas are an optimization to *mention*, not build)
- Broadcast fanout through the same per-session egress queues — no new mechanism
- Conflation: if a slow subscriber's egress queue is full, drop the older book update and keep the newest (correct market-data behavior: latest state beats every state). **Order events (acks/fills) are never conflated or dropped — slow-consumer disconnect is the only policy for those.** Being able to explain why market data conflates but order flow can't is a genuinely senior distinction.
- `tools/live_feed.py` — subscribes to the gateway and re-emits `BookUpdate`s as the JSONL format `tools/book_replay.html` already consumes. **Both the visualizer and the format were built in Session 0.1**, and the format uses `(int64 ticks, uint64 qty)` pairs precisely so that a live `BookUpdate` maps onto it without conversion. The visualizer needs zero changes — that's by construction, not luck.

**Prompt**

```
[Standing Context]

SESSION SCOPE: market data broadcast.

1. Subscribe handling: session added to broadcast set (owned by matching
   thread, mutated via the command stream like everything else).

2. After each book-mutating event, matching thread builds a BookUpdate
   (top-10 snapshot) and pushes to each subscriber's egress queue.

3. Conflation policy: BookUpdate pushes to a full egress queue REPLACE the
   most recent pending BookUpdate rather than disconnecting. Ack/Reject/Fill
   are never conflated — those still use the slow-consumer disconnect.
   Implement this distinction explicitly and comment why.

4. tools/live_feed.py: a python client that connects, subscribes, and writes
   each BookUpdate as one JSONL line in the format tools/book_replay.html
   already consumes (defined in Session 0.1):
   {"t":<u64 ns>,"seq":<u64>,"bids":[[<i64 ticks>,<u64 qty>],...],"asks":[...]}
   Do NOT modify book_replay.html. If the live BookUpdate doesn't map cleanly
   onto that record, tell me — that's a protocol design problem to fix in the
   protocol, not by forking the visualizer.

5. tests/test_broadcast.cpp: two subscribers get the same update; a full
   subscriber queue conflates book updates but a full order-event queue
   disconnects; unsubscribed sessions get nothing.

Do not implement delta encoding. Note it in docs as a known optimization.
```

**Must pass:** broadcast tests; live demo of `book_replay.html` fed by `live_feed.py` while the smoke client trades.

**That live demo is a recruiter moment:** the same visualizer that played file replays now shows a live book moving as clients trade. Record it.

**Commits:** `feat: subscription and book snapshot broadcast` · `feat: conflation for market data, disconnect for order flow` · `chore: live feed bridge to the replay visualizer` · `test: broadcast and conflation coverage`

**Answer cold**

- Why is it safe to drop an old BookUpdate but never a Fill?
- Why is the broadcast set owned by the matching thread?

---

## Session 1.7 — Load generator and latency benchmark

**Goal:** the headline number. p50/p99 order-to-ack latency under N concurrent clients.

**Build**

- `bench/loadgen.cpp` — C++ load generator: N client connections, each sending orders at a target rate, timestamping send and ack per order, computing percentiles
- Latency measured client-side, monotonic clock, timestamp embedded in `client_order_id` or a parallel map
- Sweep: 1, 10, 50, 100 clients × orders/sec targets; find where p99 degrades
- Methodology follows the skeleton written in Session 0.1: warm-up excluded, ≥10 runs, median of runs reported, hardware and build flags recorded
- Results → `docs/BENCHMARK.md` (extend the existing file, don't fork a new one)

**Honesty requirements for the writeup:** loopback TCP is not a network — say so. Percentiles include client-side scheduling noise — say so. This is a single-machine measurement — say so. The claim survives scrutiny *because* the boundary is stated.

**Prompt**

```
[Standing Context]

SESSION SCOPE: load generation and latency measurement.

1. bench/loadgen.cpp: config = num_clients, orders_per_sec_per_client,
   duration_sec, warmup_sec. Each client: own connection, paced sends
   (steady_clock), record send_ts per client_order_id, on Ack record
   recv_ts. Output: count, throughput achieved, p50/p90/p99/p999/max
   latency in microseconds. Warmup window excluded from stats.

2. Percentiles computed from the full sample vector (sort), not a
   histogram approximation — sample counts here are small enough.

3. A run script bench/run_sweep.sh: sweeps {1,10,50,100} clients at
   {100,1000,5000} orders/sec/client, 3 runs each, writes CSV.

4. Extend docs/BENCHMARK.md (the file and its methodology skeleton exist from
   Session 0.1 — add to it, do not rewrite it): a section for the service
   benchmark,
   explicitly stating: loopback TCP, single machine, client-side measurement
   includes scheduling noise, what is and isn't in the timed path.

5. While the sweep runs, watch for: does p99 degrade with client count
   (poll loop scaling) or with rate (queue contention)? Whichever it is,
   profile one representative bad case and name the hotspot in BENCHMARK.md.
```

**Must pass:** clean sweep results committed; the p99 story understood (not just recorded — you know *what* degrades and *why*).

**Commits:** `bench: multi-client latency load generator` · `bench: sweep script and results` · `docs: service benchmark methodology and findings`

**Answer cold**

- What's your p99 at 50 clients and what's the first thing that degrades?
- Why measure client-side and what noise does that include?
- Why is loopback flattering, and what would a real network add?

**PHASE 1 COMPLETE.** If recruiting eats the schedule after this point, you already have a complete story: networked gateway, real concurrency, measured latency, live demo.

---

# Phase 2 — Durability

**Outcome:** kill -9 the gateway mid-trading; restart; the book is provably identical. The payments-ledger framing lives here.

**Time estimate:** 3-4 sessions.

---

## Session 2.1 — Write-ahead log

**Goal:** every accepted command is durable before it's applied.

**Build**

- `include/ome/wal.hpp` — append-only binary log: `{u32 len, u32 crc32, u64 seq, payload}` per record; payload is the already-built OrderCommand serialization (reuse the protocol layer)
- Write path: matching thread appends **before** applying to the book (that ordering is the entire meaning of "write-ahead" — be able to say why)
- Durability policy: `fsync` batching — sync every N ms or M records, whichever first. **Present the tradeoff explicitly:** fsync-per-record = maximal durability, brutal latency; batched = bounded loss window. Pick batched, state the window.
- CRC32 per record so torn/partial tail writes are detectable on recovery

**Prompt**

```
[Standing Context]

SESSION SCOPE: write-ahead log. Append path only — recovery is next session.

1. WAL record format: u32 length, u32 crc32 (of payload), u64 sequence,
   payload (serialized OrderCommand via the existing protocol functions).
   Document the format at the top of wal.hpp.

2. Append on the matching thread BEFORE the command is applied to the book.
   Comment at the call site: why before, and what double-apply-vs-lost-apply
   asymmetry this ordering chooses.

3. Fsync policy: group commit — fsync every 10ms or 100 records. Make both
   knobs config. In comments: what the loss window is under this policy and
   what fsync-per-record would cost (cite the latency benchmark once measured).

4. Rejected orders are NOT logged (they don't mutate state). Log only
   commands that will be applied.

5. tests/test_wal.cpp: append/read-back byte fidelity; CRC catches a
   corrupted byte; a torn final record (truncate mid-record) is detected
   and cleanly ignored; sequence numbers strictly increase.

6. Re-run the latency sweep at one representative point and record the
   WAL's latency cost in BENCHMARK.md. The delta IS the durability price —
   we state it, not hide it.
```

**Must pass:** WAL tests including torn-tail; the latency delta measured and written down.

**Commits:** `feat: append-only WAL with group commit` · `test: WAL corruption and torn-write detection` · `bench: latency cost of durability`

**Answer cold**

- Why append before apply? What failure mode does the opposite order create?
- What's your loss window and why is that acceptable here?
- How does recovery know the last record is torn?

---

## Session 2.2 — Recovery

**Goal:** restart rebuilds the book by replaying the WAL. Determinism is the claim; prove it.

**Build**

- Startup: scan WAL, validate CRCs, replay every command through **the exact same engine code path** as live traffic (no separate "recovery apply" — one code path or the determinism claim is a lie)
- Torn tail: truncate at the last valid record, log what was dropped
- The determinism test: run a random workload, snapshot the book, kill, recover, snapshot again, byte-compare. Then do it 100 times with different seeds.
- Sequence gap detection: if seq numbers skip, refuse to start (a gap means a bug or a lost file — silence would be worse)

**Prompt**

```
[Standing Context]

SESSION SCOPE: WAL recovery.

1. Recovery on startup: read WAL, CRC-validate, truncate torn tail (log how
   many bytes dropped), replay commands through the normal engine apply path
   — assert this is literally the same function live traffic uses.

2. Refuse to start on a sequence gap, with a clear error naming the gap.

3. The book needs a canonical serialization for comparison: add
   OrderBook::digest() — a deterministic hash over (price_ticks, qty,
   order_count) per level per side, plus a full debug dump function.
   Prices are int64 ticks (Session 0.2), so this is an exact integer hash and
   "digests are equal" is a real claim, not a float comparison. Say that in a
   comment — it is the reason 0.2 happened.

4. tests/test_recovery.cpp:
   a. fixed scenario: apply K commands live, digest, recover from WAL into a
      fresh engine, digests equal
   b. property test: 100 seeds x random command streams (valid commands only),
      live digest == recovered digest every time
   c. torn tail: recovery succeeds and equals the book as of the last intact
      record
   d. gap: startup refuses

5. Wire recovery into gateway startup behind a --recover flag for now.
```

**Must pass:** all recovery tests, especially the 100-seed property test.

**Commits:** `feat: WAL replay recovery through the live apply path` · `feat: book digest for state comparison` · `test: 100-seed recovery determinism property test` · `feat: sequence gap refusal on startup`

**Answer cold**

- Why must recovery use the live apply path?
- What makes replay deterministic here, and what would break it? (Anything time-based or random in the apply path — which is why the engine has neither. Also: floating-point price arithmetic, which is why Session 0.2 removed it.)
- Why refuse on a gap instead of continuing?

---

## Session 2.3 — Snapshots

**Goal:** bounded recovery time. Snapshot + WAL-tail replay instead of full-log replay.

**Build**

- Periodic snapshot: serialize full book + last-applied seq to `snapshot.NNNN`, atomically (write temp, fsync, rename)
- Snapshotting must not stall matching. Two honest options: (a) pause-the-world — matching thread writes it, simple, measurable stall; (b) copy-then-write — matching thread copies book state (fast), a background thread serializes the copy. **Start with (a), measure the stall, implement (b) only if the stall is embarrassing at your book sizes.** "I measured the simple thing first" is a better story than premature cleverness.
- Recovery becomes: newest valid snapshot → replay WAL from `snapshot.seq + 1`
- WAL truncation after a successful snapshot (keep one snapshot back for safety)

**Prompt**

```
[Standing Context]

SESSION SCOPE: snapshots and bounded recovery.

1. Snapshot: full book serialization + last_applied_seq, written
   temp+fsync+rename for atomicity. Triggered every N commands (config).
   Implement pause-the-world first; measure the pause at 10K and 100K
   resting orders and record both numbers.

2. Recovery: load newest snapshot whose CRC validates, replay WAL from
   seq+1. Fall back to full WAL replay if no valid snapshot.

3. WAL truncation: after a snapshot at seq S is durable, delete WAL records
   <= previous snapshot's seq (i.e., always retain the last full interval).

4. tests/test_snapshot.cpp: recover from snapshot+tail == recover from full
   WAL (digests equal); corrupted snapshot falls back cleanly; the 100-seed
   property test from 2.2 re-run through the snapshot path.

5. Record recovery time in BENCHMARK.md: full-log replay vs snapshot+tail
   at a fixed workload size. That ratio is the point of this session.
```

**Must pass:** snapshot-path property test; measured recovery-time improvement written down.

**Commits:** `feat: atomic book snapshots with pause measurement` · `feat: snapshot-anchored recovery with WAL truncation` · `test: snapshot recovery equivalence` · `docs: recovery time comparison`

**Answer cold**

- Why temp+fsync+rename? What does rename buy you?
- What was the measured pause, and at what size would you switch strategies?
- Why keep one snapshot interval of WAL after truncation?

---

## Session 2.4 — Kill testing

**Goal:** adversarial confidence. A harness that kills the gateway at random moments under load and verifies invariants on recovery.

**Build**

- `tools/kill_test.sh` + a verifier: start gateway → loadgen traffic → `kill -9` at a random moment → restart with recovery → run verifications → repeat 50 times
- Verifications per iteration: recovery completes; digest matches a shadow book built from the WAL by an independent tool; invariants hold (bid < ask, no negative qty, order count consistency)
- The independent verifier matters: a small separate binary that reads the WAL and computes the digest with its own minimal book — so the check isn't the engine agreeing with itself

**Prompt**

```
[Standing Context]

SESSION SCOPE: crash testing harness.

1. tools/wal_verify.cpp: standalone binary, reads a WAL (+optional snapshot),
   builds a minimal independent book (simplest correct implementation —
   clarity over speed), prints the digest. Shares only the protocol/serialization
   code with the engine, NOT the book implementation.

2. tools/kill_test.sh: loop N=50: start gateway; start loadgen (moderate
   load); sleep random 1-10s; kill -9 gateway; restart with recovery;
   compare engine digest vs wal_verify digest; run invariant checks; any
   mismatch -> preserve the WAL+snapshot as an artifact and stop.

3. Run the full 50-iteration suite. If anything fails, we debug it in this
   session — a kill-test failure is the most valuable bug this project can
   surface. Preserved artifacts make it reproducible.

4. Add a short "Crash safety" section to the README draft notes: what the
   harness does, how many iterations it's passed.
```

**Must pass:** 50/50 clean, or the bug found and fixed and then 50/50 clean. Do not rationalize a flaky run.

**Commits:** `feat: independent WAL verifier` · `test: randomized kill-recovery harness` · `docs: crash safety notes` (+ whatever fixes the harness forces)

**Answer cold**

- Why does the verifier need its own book implementation?
- What's the difference between what the kill test proves and what the property test proves? (Property test: determinism of replay. Kill test: durability boundary + torn-state handling under real SIGKILL timing.)

**PHASE 2 COMPLETE.** This is where the ledger sentence becomes true: an append-only fsync'd command log from which state is deterministically reconstructed, verified by an independent reader — that is the shape of a payments ledger, and you built and crash-tested one.

---

# Phase 3 — Observability

**Outcome:** the service tells you what it's doing. 2 short sessions.

---

## Session 3.1 — Metrics

**Build**

- `include/ome/metrics.hpp` — counters (orders accepted/rejected by reason, fills, disconnects, conflated updates) and latency histograms (enqueue→apply, apply→egress), lock-free: matching thread increments plain atomics, nothing blocks the hot path
- A read-only HTTP endpoint on a separate port/thread: `GET /metrics` in Prometheus text exposition format (hand-rolled — it's ~50 lines, and "I implemented the exposition format" beats "I linked a library")
- Histograms: fixed exponential buckets, atomic counts per bucket

**Prompt**

```
[Standing Context]

SESSION SCOPE: metrics.

1. Metrics registry: named atomic counters and fixed-bucket histograms
   (exponential bounds from 1us to 1s). Hot-path cost = one relaxed atomic
   increment; justify relaxed ordering in a comment (counters are
   monotonic, cross-thread read skew is acceptable for monitoring).

2. Instrument: order lifecycle counters by reject reason, fills, session
   connects/disconnects, conflation drops, WAL fsync count, snapshot count,
   and two histograms: command enqueue->apply, apply->egress-push.

3. Metrics HTTP server: separate thread, separate port, GET /metrics only,
   Prometheus text format, hand-rolled minimal HTTP (parse request line,
   ignore headers, write response). Reject anything that isn't GET /metrics.

4. tests/test_metrics.cpp: counter increments visible; histogram bucket
   assignment correct at boundaries; exposition output parses (regex-level
   check is fine).

5. Run the load sweep once more and screenshot/curl the metrics output for
   the docs.
```

**Commits:** `feat: lock-free metrics registry` · `feat: prometheus-format metrics endpoint` · `test: metrics and exposition coverage`

**Answer cold**

- Why is relaxed ordering fine for these counters and not for the queue indices?
- What does the enqueue→apply histogram tell you that the client-side p99 can't?

---

## Session 3.2 — Structured logging

**Build**

- Structured log lines (key=value or JSON-lines): every order lifecycle event with `seq`, `session`, `client_order_id`, timestamps
- Log writing off the hot path: matching thread pushes log records into (what else) an SPSC queue drained by a logger thread. The architecture reuses itself — say that in the README.
- Log levels, and a `--log-level` flag

**Prompt**

```
[Standing Context]

SESSION SCOPE: structured logging.

1. Log record: level, timestamp, event, and typed key-value fields. Emitted
   as JSON lines to a file or stdout.

2. Async: matching/network threads construct records and push to an SPSC
   log queue; a dedicated logger thread formats and writes. If the log
   queue fills, drop DEBUG/INFO records and increment a dropped-logs
   metric; never block the hot path, never drop WARN/ERROR.

3. Instrument the order lifecycle: received, rejected(reason), enqueued,
   applied, filled, cancelled, session events, recovery start/end with
   counts.

4. tests/test_logging.cpp: record formatting; overflow drops INFO but
   keeps ERROR; dropped counter increments.
```

**Commits:** `feat: async structured logging off the hot path` · `test: log overflow policy coverage`

**Answer cold**

- Why can logging drop records but the WAL can't?
- Trace one order through the system using only log lines — which events appear and in what order?

---

# Phase 4 — Ship it

## Session 4.1 — README, diagram, demo

**Build**

- README restructured for the service (the ninety-second read): one-liner → **live demo GIF** (loadgen + visualizer via live_feed.py, book moving, metrics curl in a corner terminal) → architecture diagram (the ASCII target above, redrawn as the SVG in the portfolio style) → measured results table (p50/p99 @ N clients, WAL latency cost, recovery time, kill-test 50/50, replay-validation line) → build/run verified from clean clone → design notes → **Limitations** → license
- Limitations, honest and specific: single symbol; single matching thread by design; loopback benchmarks; no auth/TLS; no delta encoding; snapshot pause of X µs at book size Y; group-commit loss window of Z ms; **an aggressive limit rests before it matches, so Ack precedes Fill and a session can self-match**; **Modify is cancel-replace, so time priority is lost on a reprice**
- Two GIFs tell the whole story. The **correctness** GIF records `lobster_replay --jsonl` driving `tools/book_replay.html` (both from Session 0.1). The **service** GIF records `tools/live_feed.py` driving the same visualizer live while loadgen trades (Session 1.6). Same page, same format, file replay vs live service — that pairing is the point.

**Prompt**

```
[Standing Context]

SESSION SCOPE: documentation and demo. No code changes except what a clean
clone verification forces.

1. Restructure README.md per my ordering notes. Every number in the results
   table must come from docs/BENCHMARK.md — no unmeasured claims.

2. Redraw the architecture as an SVG matching my portfolio style (paper
   fill, 2px black borders, mono uppercase labels, blue arrows) —
   docs/architecture.svg.

3. Write the Limitations section from my notes, plainly. No hedging
   language, no "future work will fix" framing.

4. Verify build+run instructions from a clean clone in a fresh directory.
   Fix anything that doesn't work exactly as written.

5. Give me the shot list for BOTH demo GIFs, ~30 seconds each: what's on
   screen, in what order.
   - correctness GIF: lobster_replay --jsonl -> tools/book_replay.html
   - service GIF: gateway + loadgen + tools/live_feed.py -> the same
     visualizer, live, with a curl of /metrics in a corner terminal
   Include the ffmpeg command to convert screen recordings to reasonably
   sized GIFs.
```

**Commits:** `docs: service README with measured results` · `docs: architecture diagram` · `docs: limitations` · plus any clean-clone fixes

## Session 4.2 — Resume bullets and interview story

**Build**

- Resume bullets, drafted only from measured numbers:

```
Order Gateway | C++17, POSIX sockets, GoogleTest | github.com/...
- Built a networked order gateway in C++17: concurrent TCP sessions, lock-free
  SPSC handoff to a single-writer matching thread, p99 order-to-ack latency of
  [X]µs at [N] concurrent clients
- Implemented crash recovery via a CRC-checked write-ahead log with group
  commit and snapshotting; book state deterministically rebuilt and verified
  identical across 50 randomized kill -9 tests
- Validated matching correctness by differential replay of 269K NASDAQ LOBSTER
  messages against exchange ground truth
```

(Blanks filled from BENCHMARK.md only. If a number moved during the build, the bullet moves with it.)

- The 90-second and 5-minute walkthroughs, updated for the service story. The 5-minute version's spine: *client order's life* — socket → frame → validate → queue → WAL → match → fill → egress → socket, with the three design decisions (single-writer book, conflation asymmetry, append-before-apply) hung on that spine.
- Adversarial drill, service edition.

**Prompt**

```
[Standing Context]

SESSION SCOPE: interview preparation. No code.

1. Interview me as a skeptical backend engineer at a payments company.
   One question at a time, push on vagueness. Probe:
   - Walk me through one order, socket to fill.
   - Why single matching thread? When does that stop scaling and what then?
   - Why can market data conflate but fills can't?
   - Your WAL syncs every 10ms — what exactly can be lost, and why is that ok?
   - Why does recovery share the live apply path?
   - kill -9 vs graceful shutdown — what's different for your system?
   - What did ThreadSanitizer prove and not prove?
   - p99 degraded at [my answer] — why, and what's the fix?
   - How would you add a second symbol? A second matching thread?
   - What would you build next and why?

2. After each answer, score it: would it satisfy the interviewer, what was
   missing, and the one-sentence version of the ideal answer.

3. End by having me deliver the full 5-minute walkthrough uninterrupted,
   then critique it.
```

---

## Full definition of done

**Phase 0**
- [ ] Builds clean under `-Wall -Wextra -Werror`; all three sanitizer configurations build
- [ ] CI green on push: Release tests + ASan/UBSan tests + TSan tests
- [ ] Prices are `int64` ticks; LOBSTER replay accuracy at 1k/5k/100k events unchanged from before the refactor
- [ ] `RejectReason`, `ApplyResult`, trade drain, and generic `SpscQueue<T,N>` exist; two-thread queue test green under TSan
- [ ] `tools/book_replay.html` animates `lobster_replay --jsonl` output
- [ ] Every claim in the Standing Context is verifiable against the code

**Phase 1**
- [ ] Protocol documented; round-trip + malformed tests green; no floating-point fields on the wire
- [ ] Framing survives arbitrary split boundaries; slow-consumer policy enforced
- [ ] Cancel-on-disconnect through the command path; heartbeats sweep dead sessions
- [ ] Book is single-writer, TSan-clean pipeline integration test
- [ ] Fills route to the correct session via the matching-thread-owned ownership map
- [ ] Risk checks on the correct threads, boundary-tested
- [ ] Broadcast with conflation asymmetry (market data conflates, order flow never)
- [ ] p50/p99 sweep in BENCHMARK.md with stated methodology and known bottleneck

**Phase 2**
- [ ] WAL: append-before-apply, group commit, CRC, torn-tail handling — latency cost measured
- [ ] Recovery through the live apply path; 100-seed determinism property test green
- [ ] Snapshots atomic; recovery-time improvement measured; pause measured
- [ ] Kill harness 50/50 with an independent WAL verifier

**Phase 3**
- [ ] Metrics endpoint, hot-path cost one relaxed atomic; histograms boundary-tested
- [ ] Async structured logging; one order traceable end-to-end by log lines alone

**Phase 4**
- [ ] README: live demo GIF + replay GIF, results table sourced from BENCHMARK.md only, honest Limitations
- [ ] Clean-clone build verified
- [ ] Resume bullets contain only measured numbers
- [ ] 5-minute walkthrough delivered cold and critiqued

**Throughout**
- [ ] Atomic conventional commits, real timestamps, zero AI attribution
- [ ] CI green on every push
- [ ] I can explain every line I committed

---

## Cut lines if time runs out

**Phase 0 is not cuttable.** Every session in it unblocks a specific later session, and 0.2 in particular gets strictly more expensive the longer it waits — after Session 2.1 it means rewriting the wire format *and* the WAL record layout, and after 2.2 it invalidates the recovery property test. Do it first or pay compound interest.

For everything else, in order of what to drop, latest first:

1. **Drop Session 3.2 (logging)** — metrics alone carries observability.
2. **Drop Session 2.3 (snapshots)** — WAL + recovery + kill tests is already the full durability story; full-log replay is just slower. Keep 2.4.
3. **Stop after Phase 1 + Session 2.1-2.2** — "gateway with WAL recovery" is a complete narrative.
4. **Absolute floor: Phase 0 + Phase 1.** Networked, concurrent, measured, on a codebase with CI and integer prices. Still a step-change from the file-replay version.

Never cut: all of Phase 0, the TSan run (1.4), the kill test if any of Phase 2 exists (2.4), clean-clone verification (4.1).

---

## Interview vocabulary this project buys

For the behavioral/technical crossover moments — each of these is now a first-person story, not a textbook answer:

| Question | Your story |
|---|---|
| "Tell me about a concurrency bug" | Whatever TSan or the kill harness caught — and one of them will catch something |
| "How do you make a system crash-safe?" | Append-before-apply, group commit window, torn-tail CRC, 50 kill tests |
| "Design a rate limiter" | Built one — token bucket with a fake-clock test |
| "How do you handle slow consumers?" | Two policies, and why they differ by stream type |
| "What's your approach to benchmarking?" | Stated boundaries, warm-up, percentiles, named bottleneck |
| "Event sourcing?" | The WAL *is* one — state as a deterministic fold over an immutable log |
| "What would you do differently?" | The Limitations section, verbatim |

The last row is the trap question this plan defuses: you wrote the limitations yourself, so the honest answer is already rehearsed.
