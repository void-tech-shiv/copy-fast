#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
for t in tests/test_basic.sh tests/test_directory.sh tests/test_sparse.sh tests/test_metadata.sh tests/test_resume.sh tests/test_naive.sh; do
  echo "==> $t"
  bash "$t"
done
echo "All CopyFast tests passed."
