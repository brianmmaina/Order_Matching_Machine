#!/usr/bin/env bash
# Randomized crash-recovery harness.
#
# Each iteration: run the gateway under load, kill -9 it at an unpredictable
# moment, restart with recovery, and check that the rebuilt book is right.
#
# "Right" means two things, and the second is the one that matters:
#
#   1. the engine recovers without refusing to start
#   2. the engine's digest equals the digest computed by tools/wal_verify,
#      which rebuilds the book from the same log using a COMPLETELY SEPARATE
#      implementation
#
# Without (2) this would be the engine checking its own work: a matching bug
# would produce the same wrong book on both sides and the test would pass. The
# verifier also asserts invariants a digest cannot — no crossed book, no empty
# levels, no zero-quantity orders resting.
#
# The kill lands at a random point mid-load, so over enough iterations it hits
# the interesting moments: mid-append, mid-fsync, between append and apply,
# mid-egress-drain.
#
# WHAT THIS PROVES, AND WHAT IT DOES NOT
#
# kill -9 destroys the process without letting it flush anything. It does NOT
# destroy the page cache, so everything already handed to write() survives. That
# is the honest scope: this proves the recovery path, the torn-tail handling and
# the append-before-apply ordering are correct. It does not prove behavior under
# power loss, which would additionally lose the group-commit window. Testing
# that needs hardware this harness does not have.
#
# On any mismatch the WAL is preserved and the run stops, so the failure is
# reproducible rather than a line of scrollback.
#
# Usage:  tools/kill_test.sh [iterations]

set -uo pipefail
cd "$(dirname "$0")/.."

ITERATIONS="${1:-50}"
CLIENTS="${CLIENTS:-4}"
RATE="${RATE:-500}"
RISK="${RISK:-config/bench.conf}"
ARTIFACTS="${ARTIFACTS:-/tmp/kill_test_artifacts}"
# Snapshots off by default so the harness exercises pure WAL replay. Set
# SNAPSHOT_EVERY to a command count to test the snapshot+tail path instead —
# that path has its own failure modes (a torn snapshot, a truncated WAL, the
# sequence accounting between them) that full replay never touches.
SNAPSHOT_EVERY="${SNAPSHOT_EVERY:-0}"

for bin in gateway loadgen wal_verify; do
  if [[ ! -x "./build/$bin" ]]; then
    echo "missing ./build/$bin — build first" >&2
    exit 1
  fi
done

mkdir -p "$ARTIFACTS"
pass=0

for ((i = 1; i <= ITERATIONS; i++)); do
  WAL="/tmp/kill_test_$i.wal"
  SNAP="/tmp/kill_test_$i.snap"
  rm -f "$WAL" "$SNAP" "$SNAP.prev"

  # NOTE the ${arr[@]+"${arr[@]}"} form below: bash 3.2, which macOS ships,
  # treats "${arr[@]}" on an empty array as an unbound variable under set -u.
  SNAP_ARGS=()
  if [[ "$SNAPSHOT_EVERY" -gt 0 ]]; then
    SNAP_ARGS=(--snapshot "$SNAP" --snapshot-every "$SNAPSHOT_EVERY")
  fi

  ./build/gateway --wal "$WAL" --risk "$RISK" ${SNAP_ARGS[@]+"${SNAP_ARGS[@]}"} --port 0 > /tmp/kt_gw.log 2>&1 &
  GW=$!
  for _ in $(seq 1 100); do
    grep -q listening /tmp/kt_gw.log 2>/dev/null && break
    sleep 0.1
  done
  PORT=$(sed -n 's/.*127.0.0.1:\([0-9]*\)/\1/p' /tmp/kt_gw.log)
  if [[ -z "$PORT" ]]; then
    echo "iteration $i: gateway never started" >&2
    kill -9 $GW 2>/dev/null
    exit 1
  fi

  # Long duration; the kill is what ends it. Clients stay connected so the
  # kill catches live state — if they disconnected first, cancel-on-disconnect
  # would empty the book and every iteration would verify nothing.
  # --cross matters: without it the workload never matches, and the harness
  # would only be checking that two implementations can both accumulate a book.
  # A deliberately broken verifier passed 1/1 before this was added.
  ./build/loadgen --port "$PORT" --clients "$CLIENTS" --rate "$RATE" --cross \
      --duration 30 --warmup 0 > /dev/null 2>&1 &
  LG=$!

  # Random point in the run. Coarse on purpose: the aim is to land somewhere
  # unpredictable relative to the group commit and the apply loop.
  SLEEP=$(awk -v s="$RANDOM" 'BEGIN{srand(s); printf "%.2f", 0.8 + rand()*2.5}')
  sleep "$SLEEP"

  kill -9 $GW 2>/dev/null
  kill -9 $LG 2>/dev/null
  wait $GW 2>/dev/null
  wait $LG 2>/dev/null

  RECORDS=$(( $(wc -c < "$WAL") / 47 ))

  # Recover.
  ./build/gateway --wal "$WAL" --risk "$RISK" ${SNAP_ARGS[@]+"${SNAP_ARGS[@]}"} --recover --port 0 \
      > /tmp/kt_rec.log 2>&1 &
  GW2=$!
  for _ in $(seq 1 100); do
    grep -qE "listening|refusing" /tmp/kt_rec.log 2>/dev/null && break
    sleep 0.1
  done
  ENGINE=$(grep -oE "digest after recovery: [0-9]+" /tmp/kt_rec.log | grep -oE "[0-9]+$")
  kill -TERM $GW2 2>/dev/null
  wait $GW2 2>/dev/null

  if [[ -z "$ENGINE" ]]; then
    echo "iteration $i: FAILED — gateway did not recover" >&2
    cp "$WAL" "$ARTIFACTS/fail_${i}.wal"
    cp /tmp/kt_rec.log "$ARTIFACTS/fail_${i}_recover.log"
    echo "artifacts in $ARTIFACTS" >&2
    exit 1
  fi

  # Independent rebuild. Exit 4 means an invariant was violated.
  VER_ARGS=()
  if [[ "$SNAPSHOT_EVERY" -gt 0 && -f "$SNAP" ]]; then
    VER_ARGS=(--snapshot "$SNAP")
  fi
  VERIFY=$(./build/wal_verify --wal "$WAL" ${VER_ARGS[@]+"${VER_ARGS[@]}"} --risk "$RISK" 2> /tmp/kt_ver.log)
  VRC=$?
  if [[ $VRC -ne 0 ]]; then
    echo "iteration $i: FAILED — verifier exited $VRC" >&2
    cat /tmp/kt_ver.log >&2
    cp "$WAL" "$ARTIFACTS/fail_${i}.wal"
    exit 1
  fi

  if [[ "$ENGINE" != "$VERIFY" ]]; then
    echo "iteration $i: FAILED — digest mismatch after $RECORDS records" >&2
    echo "  engine:   $ENGINE" >&2
    echo "  verifier: $VERIFY" >&2
    cp "$WAL" "$ARTIFACTS/fail_${i}.wal"
    [[ -f "$SNAP" ]] && cp "$SNAP" "$ARTIFACTS/fail_${i}.snap"
    ./build/wal_verify --wal "$WAL" ${VER_ARGS[@]+"${VER_ARGS[@]}"} --risk "$RISK" --dump > /dev/null \
        2> "$ARTIFACTS/fail_${i}_verifier_book.txt"
    echo "artifacts in $ARTIFACTS — reproduce with:" >&2
    echo "  ./build/wal_verify --wal $ARTIFACTS/fail_${i}.wal --risk $RISK --dump" >&2
    exit 1
  fi

  pass=$((pass + 1))
  printf "iteration %2d/%d  killed after %ss  %5d records  digest %s  OK\n" \
    "$i" "$ITERATIONS" "$SLEEP" "$RECORDS" "${ENGINE:0:12}"
  rm -f "$WAL" "$SNAP" "$SNAP.prev"
done

echo
echo "$pass/$ITERATIONS passed"
