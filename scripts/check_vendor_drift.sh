#!/bin/sh
# Enforce the current Docker build-context contract for vendored dependencies.
#
# server/Dockerfile builds both the web client and native server from the same
# context. Both stages COPY vendor/, so .dockerignore must not exclude any
# vendor subtree. This is intentionally different from the retired contract
# that tried to classify "client-only" vendor directories.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
DOCKERIGNORE_FILE=${SIGNAL_DOCKERIGNORE_FILE:-"$ROOT/.dockerignore"}
DOCKERFILE_FILE=${SIGNAL_DOCKERFILE_FILE:-"$ROOT/server/Dockerfile"}

if [ ! -f "$DOCKERIGNORE_FILE" ]; then
    echo "ERROR: missing Docker ignore file: $DOCKERIGNORE_FILE" >&2
    exit 1
fi
if [ ! -f "$DOCKERFILE_FILE" ]; then
    echo "ERROR: missing Dockerfile: $DOCKERFILE_FILE" >&2
    exit 1
fi

# Reject any active rule that names vendor as a path segment. Negated rules do
# not make this safe: Docker ignore ordering is easy to drift, while both build
# stages require the complete tree. Keep the invariant simple and explicit.
vendor_rules=$(
    awk '
        {
            original = $0
            sub(/^[[:space:]]+/, "", $0)
            sub(/[[:space:]]+$/, "", $0)
            if ($0 == "" || substr($0, 1, 1) == "#") next
            if (substr($0, 1, 1) == "!") $0 = substr($0, 2)
            sub(/^\/+/, "", $0)
            if ($0 == "vendor" || $0 ~ /^vendor\// ||
                $0 ~ /(^|\/)vendor(\/|$)/) {
                print original
            }
        }
    ' "$DOCKERIGNORE_FILE"
)

if [ -n "$vendor_rules" ]; then
    echo "ERROR: .dockerignore excludes vendor content required by server/Dockerfile:" >&2
    printf '%s\n' "$vendor_rules" >&2
    echo "Remove vendor ignore rules; the web-builder and server builder both COPY vendor/." >&2
    exit 1
fi

copy_count=$(
    awk '
        /^[[:space:]]*COPY[[:space:]]+vendor\/[[:space:]]+\.\/vendor\/[[:space:]]*$/ {
            count++
        }
        END { print count + 0 }
    ' "$DOCKERFILE_FILE"
)

if [ "$copy_count" -lt 2 ]; then
    echo "ERROR: server/Dockerfile copies vendor/ only $copy_count time(s); both web and native build stages require it" >&2
    exit 1
fi

echo "Docker vendor context check passed ($copy_count build stages copy the complete vendor tree)"
