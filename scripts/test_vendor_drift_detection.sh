#!/bin/sh
# Mutation tests for the Docker vendor-context checker.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
CHECKER="$ROOT/scripts/check_vendor_drift.sh"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/signal-vendor-drift.XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM

IGNORE_FILE="$TMP_DIR/.dockerignore"
DOCKERFILE_FILE="$TMP_DIR/Dockerfile"

write_complete_dockerfile() {
    printf '%s\n' \
        'FROM example AS web-builder' \
        'COPY vendor/ ./vendor/' \
        'FROM example AS native-builder' \
        'COPY vendor/ ./vendor/' > "$DOCKERFILE_FILE"
}

run_fixture() {
    SIGNAL_DOCKERIGNORE_FILE="$IGNORE_FILE" \
    SIGNAL_DOCKERFILE_FILE="$DOCKERFILE_FILE" \
        "$CHECKER"
}

printf '%s\n' '# build output only' 'build-*/' > "$IGNORE_FILE"
write_complete_dockerfile
run_fixture

printf '%s\n' 'vendor/sokol/' > "$IGNORE_FILE"
if run_fixture 2>&1 | grep -q "excludes vendor content"; then
    echo "vendor exclusion mutation detected"
else
    echo "FAIL: vendor exclusion mutation was accepted" >&2
    exit 1
fi

printf '%s\n' '# vendor/sokol/ is only a comment' 'assets/voice/' > "$IGNORE_FILE"
write_complete_dockerfile
run_fixture

printf '%s\n' '# no vendor exclusions' > "$IGNORE_FILE"
printf '%s\n' \
    'FROM example AS web-builder' \
    'COPY vendor/ ./vendor/' > "$DOCKERFILE_FILE"
if run_fixture 2>&1 | grep -q "both web and native build stages"; then
    echo "missing vendor COPY mutation detected"
else
    echo "FAIL: missing vendor COPY mutation was accepted" >&2
    exit 1
fi

"$CHECKER"
echo "Vendor context mutation tests passed"
