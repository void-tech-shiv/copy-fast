#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
head -c 4194304 /dev/urandom > "$TMP/source.bin"
"$ROOT/copyfast" --quiet "$TMP/source.bin" "$TMP/dest.bin"
cmp "$TMP/source.bin" "$TMP/dest.bin"
echo "PASS basic copy + verification"
