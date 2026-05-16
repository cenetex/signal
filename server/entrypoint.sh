#!/bin/sh
set -eu

DATA_DIR="${SIGNAL_DATA_DIR:-/app/data}"
MODE="${SIGNAL_PERSISTENCE_MODE:-local}"
STATE_URI="${SIGNAL_STATE_S3_URI:-}"
SYNC_INTERVAL="${SIGNAL_STATE_SYNC_INTERVAL_SEC:-60}"
SERVER_PID=""
SYNC_PID=""
BOOT_SYNC_OK=0

log() {
  printf '[entrypoint] %s\n' "$*"
}

sync_down() {
  if [ "$MODE" != "external_s3" ]; then
    return 0
  fi
  if [ -z "$STATE_URI" ]; then
    log "ERROR: external_s3 mode requires SIGNAL_STATE_S3_URI"
    return 1
  fi
  log "sync down: $STATE_URI -> $DATA_DIR"
  aws s3 sync "$STATE_URI" "$DATA_DIR/" --only-show-errors
}

sync_up() {
  reason="${1:-periodic}"
  if [ "$MODE" != "external_s3" ] || [ "$BOOT_SYNC_OK" -ne 1 ]; then
    return 0
  fi
  log "sync up ($reason): $DATA_DIR -> $STATE_URI"
  aws s3 sync "$DATA_DIR/" "$STATE_URI" --delete --only-show-errors || \
    log "WARN: sync up failed ($reason)"
}

stop_children() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    log "stopping server pid $SERVER_PID"
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  if [ -n "$SYNC_PID" ] && kill -0 "$SYNC_PID" 2>/dev/null; then
    kill -TERM "$SYNC_PID" 2>/dev/null || true
    wait "$SYNC_PID" 2>/dev/null || true
  fi
  sync_up "shutdown"
}

trap 'stop_children; exit 143' TERM INT

mkdir -p "$DATA_DIR"

case "$MODE" in
  local)
    log "local persistence in $DATA_DIR"
    ;;
  ephemeral)
    log "ephemeral mode: local task filesystem is disposable and will not be restored"
    ;;
  external_s3)
    sync_down
    BOOT_SYNC_OK=1
    ;;
  *)
    log "ERROR: invalid SIGNAL_PERSISTENCE_MODE=$MODE"
    exit 2
    ;;
esac

cd "$DATA_DIR"

/app/signal-server &
SERVER_PID="$!"

if [ "$MODE" = "external_s3" ]; then
  (
    while true; do
      sleep "$SYNC_INTERVAL"
      sync_up "periodic"
    done
  ) &
  SYNC_PID="$!"
fi

set +e
wait "$SERVER_PID"
STATUS="$?"
set -e
SERVER_PID=""

if [ -n "$SYNC_PID" ] && kill -0 "$SYNC_PID" 2>/dev/null; then
  kill -TERM "$SYNC_PID" 2>/dev/null || true
  wait "$SYNC_PID" 2>/dev/null || true
fi
sync_up "exit"
exit "$STATUS"
