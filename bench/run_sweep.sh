#!/usr/bin/env bash
# Latency sweep: clients x offered rate, repeated, median of runs.
#
# Methodology is docs/BENCHMARK.md. The parts this script enforces:
#   - Release build only
#   - a warm-up window excluded from statistics, by SEND time
#   - RUNS repetitions per cell, reporting the MEDIAN of the per-run p50/p99
#     rather than pooling every sample together. Pooling hides run-to-run
#     variance, and a single run is not trustworthy: an early version of this
#     sweep reported a p99 of 54ms for one cell that five repeats put at
#     440-570us. That outlier is why the repetition is not optional.
#   - risk limits from config/bench.conf, so the rate limiter is not what is
#     being measured
#
# Usage:  bench/run_sweep.sh [output.csv]

set -euo pipefail
cd "$(dirname "$0")/.."

OUT="${1:-bench/sweep.csv}"
RUNS="${RUNS:-5}"
DURATION="${DURATION:-5}"
WARMUP="${WARMUP:-2}"
CLIENTS="${CLIENTS:-1 10 50 100}"
RATES="${RATES:-100 1000 5000}"

if [[ ! -x ./build/gateway || ! -x ./build/loadgen ]]; then
  echo "build first: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
  exit 1
fi

BUILD_TYPE=$(grep -m1 CMAKE_BUILD_TYPE:STRING build/CMakeCache.txt | cut -d= -f2 || echo unknown)
if [[ "$BUILD_TYPE" != "Release" ]]; then
  echo "refusing to benchmark a $BUILD_TYPE build — see docs/BENCHMARK.md" >&2
  exit 1
fi

# A FRESH GATEWAY PER CELL, and this is methodology rather than tidiness.
#
# The workload quotes a bid below and an ask above a fixed price, so nothing
# ever crosses and every order rests forever. A shared gateway therefore carries
# the book from every previous cell into the next one, and cell N is measured
# against state built by cells 1..N-1.
#
# It is not a small effect. A single shared gateway made every cell from 100
# clients onward report zero acks and hundreds of thousands of rejects, because
# each cell ends with its sessions disconnecting and each disconnect triggers a
# cancel-all over the thousands of orders that session left resting. That work
# lands on the matching thread, the inbound queue backs up, and the next cell
# is rejected wholesale. Run standalone, the same cell is perfectly healthy.
start_gateway() {
  ./build/gateway --risk config/bench.conf --port 0 > /tmp/sweep_gw.log 2>&1 &
  GW=$!
  until grep -q listening /tmp/sweep_gw.log 2>/dev/null; do sleep 0.2; done
  PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\)/\1/p' /tmp/sweep_gw.log)
}
stop_gateway() { kill -9 "$GW" 2>/dev/null || true; wait "$GW" 2>/dev/null || true; }
trap 'stop_gateway' EXIT

echo "$RUNS runs per cell, ${DURATION}s each (+${WARMUP}s warmup), fresh gateway per run" >&2

echo "clients,rate_per_client,offered,achieved,samples,rejected,send_failed,p50_us,p90_us,p99_us,p999_us,max_us" > "$OUT"

median() { sort -n | awk '{a[NR]=$1} END{print (NR%2) ? a[(NR+1)/2] : (a[NR/2]+a[NR/2+1])/2}'; }

for c in $CLIENTS; do
  for r in $RATES; do
    rows=()
    for ((i=0; i<RUNS; i++)); do
      start_gateway
      rows+=("$(./build/loadgen --port "$PORT" --clients "$c" --rate "$r" \
                  --duration "$DURATION" --warmup "$WARMUP" --csv)")
      stop_gateway
    done
    # Median each column independently. Mixing columns from different runs is
    # acceptable here because each is reported as "the median run's value for
    # this statistic", not as a single coherent run.
    line="$c,$r"
    for col in 3 4 5 6 7 8 9 10 11 12; do
      line="$line,$(printf '%s\n' "${rows[@]}" | cut -d, -f$col | median)"
    done
    echo "$line" | tee -a "$OUT"
  done
done

echo "wrote $OUT" >&2
