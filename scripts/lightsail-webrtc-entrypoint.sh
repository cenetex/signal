#!/bin/sh
set -eu

PUBLIC_PORT=${PORT:-8080}
SERVER_PORT=${SIGNAL_SERVER_PORT:-9091}
DATA_DIR=${SIGNAL_DATA_DIR:-/app/data}
STATIC_DIR=${SIGNAL_STATIC_DIR:-/app/public}
RTC_PREFIX=${RTC_GATEWAY_PREFIX:-/rtc}
RTC_UPSTREAM=${RTC_GATEWAY_UPSTREAM:-ws://127.0.0.1:${SERVER_PORT}/ws}
SERVER_READY_URL=${RTC_GATEWAY_SERVER_READY_URL:-http://127.0.0.1:${SERVER_PORT}/health}
SERVER_IDLE_MS=${RTC_GATEWAY_SERVER_IDLE_MS:-300000}
SERVER_STOP_TIMEOUT_MS=${RTC_GATEWAY_SERVER_STOP_TIMEOUT_MS:-5000}
WAKE_PATH=${RTC_GATEWAY_WAKE_PATH:-/wake}

mkdir -p "$DATA_DIR" "$DATA_DIR/saves" "$DATA_DIR/stations" "$DATA_DIR/chain"

SERVER_COMMAND="PORT=${SERVER_PORT} SIGNAL_DATA_DIR=${DATA_DIR} SIGNAL_STATIC_DIR=${STATIC_DIR} /app/signal-server"

set -- \
    --listen="0.0.0.0:${PUBLIC_PORT}" \
    --upstream="$RTC_UPSTREAM" \
    --static="$STATIC_DIR" \
    --rtc-prefix="$RTC_PREFIX" \
    --wake-path="$WAKE_PATH" \
    --server-command="$SERVER_COMMAND" \
    --server-ready-url="$SERVER_READY_URL" \
    --server-idle-ms="$SERVER_IDLE_MS" \
    --server-stop-timeout-ms="$SERVER_STOP_TIMEOUT_MS"

if [ -n "${RTC_GATEWAY_WAKE_TOKEN:-}" ]; then
    set -- "$@" --wake-token="$RTC_GATEWAY_WAKE_TOKEN"
fi
if [ "${RTC_GATEWAY_STUN+x}" ]; then
    set -- "$@" --stun="$RTC_GATEWAY_STUN"
fi
if [ -n "${RTC_GATEWAY_TURN:-}" ]; then
    set -- "$@" --turn="$RTC_GATEWAY_TURN"
fi
if [ -n "${RTC_GATEWAY_TURN_USER:-}" ]; then
    set -- "$@" --turn-user="$RTC_GATEWAY_TURN_USER"
fi
if [ -n "${RTC_GATEWAY_TURN_PASS:-}" ]; then
    set -- "$@" --turn-pass="$RTC_GATEWAY_TURN_PASS"
fi
if [ -n "${RTC_GATEWAY_ICE_BIND:-}" ]; then
    set -- "$@" --ice-bind="$RTC_GATEWAY_ICE_BIND"
fi
if [ -n "${RTC_GATEWAY_ICE_PORT:-}" ]; then
    set -- "$@" --ice-port="$RTC_GATEWAY_ICE_PORT"
fi
if [ -n "${RTC_GATEWAY_ICE_PORT_RANGE:-}" ]; then
    set -- "$@" --ice-port-range="$RTC_GATEWAY_ICE_PORT_RANGE"
fi
if [ "${RTC_GATEWAY_ICE_UDP_MUX:-0}" = "1" ]; then
    set -- "$@" --ice-udp-mux
fi
if [ "${RTC_GATEWAY_ICE_TCP:-0}" = "1" ]; then
    set -- "$@" --ice-tcp
fi

exec node /app/scripts/webrtc-gateway.mjs "$@"
