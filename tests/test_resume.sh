#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
head -c 33554432 /dev/urandom > "$TMP/src.bin"
set +e
COPYFAST_TEST_DELAY_US=20000 "$ROOT/copyfast" --quiet --resume -B 1M "$TMP/src.bin" "$TMP/dst.bin" &
PID=$!
sleep 0.15
kill -INT "$PID" 2>/dev/null
wait "$PID"
RC=$?
set -e
[[ "$RC" -eq 130 ]]
[[ -f "$TMP/dst.bin.copyfast.resume" ]]
OFFSET=$(awk -F= '/^offset=/{print $2}' "$TMP/dst.bin.copyfast.resume")
[[ "$OFFSET" -gt 0 && "$OFFSET" -lt 33554432 ]]
"$ROOT/copyfast" --quiet --resume "$TMP/src.bin" "$TMP/dst.bin"
cmp "$TMP/src.bin" "$TMP/dst.bin"
[[ ! -e "$TMP/dst.bin.copyfast.resume" ]]
echo "PASS resume after SIGINT (saved offset=$OFFSET)"
