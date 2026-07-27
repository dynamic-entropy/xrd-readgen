#!/usr/bin/env bash
# Throwaway localhost xrootd server for development and CI.
# Usage: dev/local-server.sh [data_dir] [port]
set -euo pipefail

DATA_DIR="${1:-/tmp/xrd-readgen-data}"
PORT="${2:-10945}"  # 10940 is often a local federation meta-manager
TEST_FILE="$DATA_DIR/test-256M.bin"

mkdir -p "$DATA_DIR"
if [[ ! -f "$TEST_FILE" ]]; then
  echo "creating $TEST_FILE"
  head -c $((256 * 1024 * 1024)) /dev/urandom > "$TEST_FILE"
fi

echo "serving $DATA_DIR on port $PORT"
echo "test URL: root://localhost:$PORT/$TEST_FILE"
exec xrootd -p "$PORT" "$DATA_DIR"
