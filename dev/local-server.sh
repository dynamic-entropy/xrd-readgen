#!/usr/bin/env bash
# Throwaway localhost xrootd server for development and CI.
#
# Usage:
#   dev/local-server.sh [--background|-b] [--data-dir DIR] [--port PORT]
#   dev/local-server.sh --stop
#   dev/local-server.sh --status
#   dev/local-server.sh --help
#
# Positional (legacy): dev/local-server.sh [data_dir] [port]
set -euo pipefail

DATA_DIR="/tmp/xrd-readgen-data"
PORT="10945"  # 10940 is often a local federation meta-manager
BACKGROUND=0
ACTION="start"
PID_FILE="/tmp/xrd-readgen-xrootd.pid"
LOG_FILE="/tmp/xrd-readgen-xrootd.log"

usage() {
  cat <<'EOF'
Usage: dev/local-server.sh [options] [data_dir] [port]

  --background, -b   Run xrootd in the background (PID in /tmp/xrd-readgen-xrootd.pid)
  --stop             Stop a background server started by this script
  --status           Show whether the background server is running
  --data-dir DIR     Export directory (default: /tmp/xrd-readgen-data)
  --port PORT        Listen port (default: 10945)
  --help, -h         Show this help

Legacy: positional [data_dir] [port] still work.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -b|--background) BACKGROUND=1; shift ;;
    --stop) ACTION="stop"; shift ;;
    --status) ACTION="status"; shift ;;
    --data-dir) DATA_DIR="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      # Legacy positionals: first → data_dir, second → port
      if [[ "$DATA_DIR" == "/tmp/xrd-readgen-data" && "$1" != /* && "$1" =~ ^[0-9]+$ ]]; then
        PORT="$1"
      elif [[ "$DATA_DIR" == "/tmp/xrd-readgen-data" ]]; then
        DATA_DIR="$1"
      else
        PORT="$1"
      fi
      shift
      ;;
  esac
done

is_running() {
  [[ -f "$PID_FILE" ]] || return 1
  local pid
  pid="$(cat "$PID_FILE" 2>/dev/null || true)"
  [[ -n "${pid:-}" ]] || return 1
  kill -0 "$pid" 2>/dev/null
}

case "$ACTION" in
  stop)
    if ! is_running; then
      echo "local xrootd: not running"
      rm -f "$PID_FILE"
      exit 0
    fi
    pid="$(cat "$PID_FILE")"
    echo "stopping xrootd pid $pid"
    kill "$pid" 2>/dev/null || true
    for _ in 1 2 3 4 5; do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.2
    done
    if kill -0 "$pid" 2>/dev/null; then
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$PID_FILE"
    echo "stopped"
    exit 0
    ;;
  status)
    if is_running; then
      echo "local xrootd: running (pid $(cat "$PID_FILE"), port $PORT)"
      ss -ltn 2>/dev/null | grep -E ":${PORT}\\b" || true
      exit 0
    fi
    echo "local xrootd: not running"
    exit 1
    ;;
esac

TEST_FILE="$DATA_DIR/test-256M.bin"

mkdir -p "$DATA_DIR"
if [[ ! -f "$TEST_FILE" ]]; then
  echo "creating $TEST_FILE"
  head -c $((256 * 1024 * 1024)) /dev/urandom > "$TEST_FILE"
fi

if ss -ltn 2>/dev/null | grep -qE ":${PORT}\\b"; then
  echo "port $PORT already in use — is xrootd already running?"
  echo "  try: $0 --status   or   $0 --stop"
  exit 1
fi

echo "serving $DATA_DIR on port $PORT"
echo "test URL: root://localhost:$PORT/$TEST_FILE"

if [[ "$BACKGROUND" -eq 1 ]]; then
  if is_running; then
    echo "already running (pid $(cat "$PID_FILE"))" >&2
    exit 1
  fi
  nohup xrootd -p "$PORT" "$DATA_DIR" >"$LOG_FILE" 2>&1 &
  echo $! >"$PID_FILE"
  sleep 0.5
  if ! is_running; then
    echo "failed to start — see $LOG_FILE" >&2
    rm -f "$PID_FILE"
    exit 1
  fi
  echo "background pid $(cat "$PID_FILE")  log $LOG_FILE"
  echo "stop with: $0 --stop"
  exit 0
fi

exec xrootd -p "$PORT" "$DATA_DIR"
