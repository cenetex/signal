#!/bin/sh
# signal — container entrypoint. Runs signal_server + a static HTTP server
# side-by-side, forwards SIGTERM/SIGINT to both, exits when either dies.
set -eu

DATA_DIR=${DATA_DIR:-/app/data}
cd "$DATA_DIR"

# Symlink the runtime-fixed paths the server expects into the data dir so
# saves/ stations/ chain/ world.sav all land in the bind-mounted volume.
# The signal_server binary reads/writes these by relative name from its cwd.
mkdir -p "$DATA_DIR/saves" "$DATA_DIR/stations" "$DATA_DIR/chain"

# Forward signals to children and wait on both.
cleanup() {
    [ -n "${SERVER_PID:-}" ] && kill "$SERVER_PID" 2>/dev/null || true
    [ -n "${HTTP_PID:-}"   ] && kill "$HTTP_PID"   2>/dev/null || true
}
trap cleanup INT TERM

# Pick the server binary. Prefer the bind-mounted host-built one when
# present (`scripts/local-build-server.sh` produced it); otherwise fall
# back to the image-baked binary. This lets local dev iterate on the
# server in seconds without rebuilding the Docker image.
SERVER_BIN=/app/signal_server
if [ -x "/app/srv-local/signal_server" ]; then
    SERVER_BIN=/app/srv-local/signal_server
    echo "[entrypoint] using bind-mounted server: $SERVER_BIN" >&2
fi

# Websocket + sim on :9091.
PORT=${PORT:-9091} "$SERVER_BIN" &
SERVER_PID=$!

# Static HTTP for the browser bundle on :8080. python's http.server is
# single-threaded but plenty for local play; swap to nginx later if wanted.
python3 -m http.server 8080 --directory /app/build-web &
HTTP_PID=$!

# Exit as soon as either child dies so docker restarts correctly.
# python:slim ships dash as /bin/sh, which lacks `wait -n`; poll instead.
while kill -0 "$SERVER_PID" 2>/dev/null && kill -0 "$HTTP_PID" 2>/dev/null; do
    sleep 1
done
cleanup
wait
