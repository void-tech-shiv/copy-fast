#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
printf 'metadata\n' > "$TMP/src"
chmod 640 "$TMP/src"
touch -m -d '2024-02-03 04:05:06 UTC' "$TMP/src"
"$ROOT/copyfast" --quiet "$TMP/src" "$TMP/dst"
[[ "$(stat -c %a "$TMP/src")" == "$(stat -c %a "$TMP/dst")" ]]
[[ "$(stat -c %Y "$TMP/src")" == "$(stat -c %Y "$TMP/dst")" ]]
echo "PASS mode + mtime preservation"
