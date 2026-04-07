# LOBSTER data *(local only)*

LOBSTER CSVs are **gitignored** (see repo `.gitignore`). The official samples unpack into one folder per ticker, for example:

```text
data/lobster/LOBSTER_SampleFile_AMZN_2012-06-21_10/AMZN_2012-06-21_34200000_57600000_message_10.csv
data/lobster/LOBSTER_SampleFile_AMZN_2012-06-21_10/AMZN_2012-06-21_34200000_57600000_orderbook_10.csv
```

*(AAPL, GOOG, INTC, MSFT sample folders follow the same naming pattern.)*

## Replay + validation

From the **project root** (after `cmake --build build`):

```bash
./build/lobster_replay \
  --messages "data/lobster/LOBSTER_SampleFile_AMZN_2012-06-21_10/AMZN_2012-06-21_34200000_57600000_message_10.csv" \
  --orderbook "data/lobster/LOBSTER_SampleFile_AMZN_2012-06-21_10/AMZN_2012-06-21_34200000_57600000_orderbook_10.csv" \
  --events 10000
```

- **`--events N`**: replay the first **N** parsed message rows (types 1–5 and **7** trading-halt markers; executions **4/5** may use **order id 0** as in LOBSTER output).
- **`--orderbook-line L`** *(optional)*: **0-based** index of the orderbook row used as the reference top-of-book. **Default:** `L = N - 1`. If accuracy looks “almost right” but shifted, try `L = N - 2` or `L = N` (same row count as the message file; indices must stay in range).

The tool converts that orderbook row (`AskPx1, AskSz1, BidPx1, BidSz1, …`) into the 20-line snapshot `LobsterValidator` expects.

**Exit codes:** `0` = all 20 levels match; `1` = mismatch or I/O/conversion error.

## Accuracy expectations

This engine is a **simplified** matcher (resting book, Market/Limit/Cancel, crossed-book and market consumption). **NASDAQ / LOBSTER** reconstruction has richer rules (execution direction semantics, hidden liquidity, halts, etc.), so **bit-perfect agreement on long replays is not guaranteed**. Use the validator to see **where** the book diverges; tightening parity is follow-on work.

Reference: `LOBSTER_SampleFiles_ReadMe.txt` inside each sample folder.
