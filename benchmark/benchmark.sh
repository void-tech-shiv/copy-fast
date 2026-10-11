#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIZE_MB="${1:-512}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
SRC="$TMP/source.bin"

echo "Creating ${SIZE_MB} MiB dense benchmark file..."
dd if=/dev/urandom of="$SRC" bs=1M count="$SIZE_MB" status=progress

run_one() {
  local backend="$1" dst="$2"
  local start end ms
  start=$(date +%s%N)
  "$ROOT/copyfast" --quiet --no-verify --backend "$backend" "$SRC" "$dst"
  end=$(date +%s%N)
  ms=$(( (end-start)/1000000 ))
  awk -v name="$backend" -v mb="$SIZE_MB" -v ms="$ms" 'BEGIN { printf "%-10s %8.3f s  %10.1f MiB/s\n", name, ms/1000, mb/(ms/1000) }'
  rm -f "$dst"
}

printf "\nBackend      Time          Throughput\n"
printf "--------------------------------------\n"
run_one naive "$TMP/naive.bin"
run_one threaded "$TMP/threaded.bin"
if "$ROOT/copyfast" --version | grep -q 'io_uring: compiled'; then
  run_one io_uring "$TMP/uring.bin" || echo "io_uring runtime unavailable on this kernel/environment"
fi

echo
echo "Note: page cache, filesystem, source/destination device and thermal state affect results."
echo "For a report, run multiple rounds and report median throughput."
