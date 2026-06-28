#!/bin/sh
set -eu

PUBLIC_PORT=${PORT:-8080}
SERVER_PORT=${SIGNAL_SERVER_PORT:-9091}
DATA_DIR=${SIGNAL_DATA_DIR:-/app/data}
STATIC_DIR=${SIGNAL_STATIC_DIR:-/app/public}
RTC_PREFIX=${RTC_GATEWAY_PREFIX:-/rtc}
RTC_UPSTREAM=${RTC_GATEWAY_UPSTREAM:-ws://127.0.0.1:${SERVER_PORT}/ws}
RTC_PROXY=${RTC_GATEWAY_PROXY:-http://127.0.0.1:${SERVER_PORT}}

mkdir -p "$DATA_DIR" "$DATA_DIR/saves" "$DATA_DIR/stations" "$DATA_DIR/chain"

cleanup() {
    [ -n "${GATEWAY_PID:-}" ] && kill "$GATEWAY_PID" 2>/dev/null || true
    [ -n "${SERVER_PID:-}" ] && kill "$SERVER_PID" 2>/dev/null || true
}
trap cleanup INT TERM

PORT="$SERVER_PORT" \
SIGNAL_DATA_DIR="$DATA_DIR" \
SIGNAL_STATIC_DIR="$STATIC_DIR" \
    /app/signal-server &
SERVER_PID=$!

set -- \
    --listen="0.0.0.0:${PUBLIC_PORT}" \
    --upstream="$RTC_UPSTREAM" \
    --proxy="$RTC_PROXY" \
    --rtc-prefix="$RTC_PREFIX"

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

node /app/scripts/webrtc-gateway.mjs "$@" &
GATEWAY_PID=$!

while kill -0 "$SERVER_PID" 2>/dev/null && kill -0 "$GATEWAY_PID" 2>/dev/null; do
    sleep 1
done

cleanup
wait
