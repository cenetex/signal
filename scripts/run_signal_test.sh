#!/usr/bin/env bash
# Launch signal_test with the stack contract required by WORLD_DECL fixtures.
#
# Linux normally starts processes with an 8 MiB soft stack limit. Several
# legacy tests intentionally place multi-megabyte world_t values on the stack,
# so every supported Makefile/direct invocation must pass through this wrapper.

set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 test-command [arguments ...]" >&2
    exit 2
fi

if [ "$(uname -s)" = "Linux" ]; then
    if ! ulimit -s 65536; then
        echo "run_signal_test: Linux tests require a 64 MiB stack; raise the hard stack limit or run in a compatible shell" >&2
        exit 2
    fi
fi

exec "$@"
