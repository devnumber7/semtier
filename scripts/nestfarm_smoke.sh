#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

make

echo "== NUMA hardware =="
if command -v numactl >/dev/null 2>&1; then
  numactl --hardware || true
else
  echo "numactl not found; continuing without topology print"
fi

: "${SEMTIER_FAST_NODE:=0}"
: "${SEMTIER_SLOW_NODE:=1}"
: "${SEMTIER_EVENT_LOG:=/tmp/semtier-events.jsonl}"

rm -f "$SEMTIER_EVENT_LOG"

echo "== semtier pointer chasing =="
SEMTIER_FAST_NODE="$SEMTIER_FAST_NODE" \
SEMTIER_SLOW_NODE="$SEMTIER_SLOW_NODE" \
SEMTIER_EVENT_LOG="$SEMTIER_EVENT_LOG" \
  ./bench/pointer-chase --mode semtier --bench chase --nodes "${NODES:-1000000}" --iters "${ITERS:-4}"

echo "== malloc pointer chasing baseline =="
./bench/pointer-chase --mode malloc --bench chase --nodes "${NODES:-1000000}" --iters "${ITERS:-4}"

echo "== semtier streaming =="
SEMTIER_FAST_NODE="$SEMTIER_FAST_NODE" \
SEMTIER_SLOW_NODE="$SEMTIER_SLOW_NODE" \
SEMTIER_EVENT_LOG="$SEMTIER_EVENT_LOG" \
  ./bench/pointer-chase --mode semtier --bench stream --nodes "${NODES:-1000000}" --iters "${ITERS:-4}"

echo "== emitted events =="
tail -n 20 "$SEMTIER_EVENT_LOG" || true
