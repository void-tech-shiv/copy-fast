#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/src/a/b"
printf 'hello\n' > "$TMP/src/a/one.txt"
head -c 1048576 /dev/urandom > "$TMP/src/a/b/random.bin"
ln -s ../one.txt "$TMP/src/a/b/link-to-one"
"$ROOT/copyfast" --quiet -j 2 "$TMP/src" "$TMP/dst"
cmp "$TMP/src/a/one.txt" "$TMP/dst/a/one.txt"
cmp "$TMP/src/a/b/random.bin" "$TMP/dst/a/b/random.bin"
[[ "$(readlink "$TMP/dst/a/b/link-to-one")" == "../one.txt" ]]
echo "PASS recursive directory + symlink"
