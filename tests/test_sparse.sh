#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
SRC="$TMP/sparse.bin"; DST="$TMP/sparse-copy.bin"
truncate -s 134217728 "$SRC"
printf 'BEGIN' | dd of="$SRC" bs=1 seek=0 conv=notrunc status=none
printf 'MIDDLE' | dd of="$SRC" bs=1 seek=67108864 conv=notrunc status=none
printf 'END' | dd of="$SRC" bs=1 seek=134217720 conv=notrunc status=none
"$ROOT/copyfast" --quiet "$SRC" "$DST"
cmp "$SRC" "$DST"
SRC_BLOCKS=$(stat -c %b "$SRC")
DST_BLOCKS=$(stat -c %b "$DST")
[[ "$DST_BLOCKS" -le $((SRC_BLOCKS + 32)) ]]
[[ "$(stat -c %s "$SRC")" == "$(stat -c %s "$DST")" ]]
echo "PASS sparse preservation (src blocks=$SRC_BLOCKS dst blocks=$DST_BLOCKS)"
