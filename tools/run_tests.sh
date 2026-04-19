#!/bin/sh
# Fan out signal_test across N parallel shards. Usage:
#   tools/run_tests.sh            # default N = logical CPU count
#   tools/run_tests.sh 4          # force 4 shards
#
# Requires build/signal_test to exist (cmake --build build --target signal_test).
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$SCRIPT_DIR/.." && pwd)
BIN="$REPO/build/signal_test"
[ -x "$BIN" ] || { echo "missing $BIN — build first" >&2; exit 1; }

if [ $# -ge 1 ]; then
    N=$1
else
    N=$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)
fi

LOGDIR=$(mktemp -d -t signal_tests.XXXXXX)
trap 'rm -rf "$LOGDIR"' EXIT

seq 0 $((N - 1)) | xargs -P "$N" -I {} sh -c \
    "'$BIN' --shard={}/$N > '$LOGDIR/shard.{}.log' 2>&1"

# Aggregate totals (exit 1 if any shard reports failures)
awk '/tests run/ { t+=$1; p+=$4; f+=$6 }
     END { printf("%d tests run, %d passed, %d failed\n", t, p, f);
           exit (f > 0) }' "$LOGDIR"/shard.*.log && STATUS=0 || STATUS=$?

if [ "$STATUS" -ne 0 ]; then
    echo "--- failures ---"
    grep -B1 -A1 "FAIL$" "$LOGDIR"/shard.*.log || true
fi
exit "$STATUS"
