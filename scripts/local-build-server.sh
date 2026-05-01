#!/bin/sh
# Fast, incremental build of signal_server inside a long-lived
# `signal-builder:dev` Docker image (alpine + gcc + cmake), with the
# build tree persisted on a named volume so cmake reruns are
# millisecond-incremental.
#
# Output: build-server-bin/signal_server (bind-mounted into the runtime
# container as /app/signal_server.local).
#
# Usage:
#   scripts/local-build-server.sh           # uses HEAD short-hash
#   scripts/local-build-server.sh <hash>    # explicit GIT_HASH
#
# First run takes ~30s to build the builder image. Subsequent runs are
# 5-15s for an incremental rebuild — fast enough for post-commit
# auto-sync.
set -eu

ROOT=$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel)
cd "$ROOT"

GIT_HASH=${1:-$(git rev-parse --short HEAD)}
OUT_DIR="$ROOT/build-server-bin"
mkdir -p "$OUT_DIR"

# Bootstrap the builder image on first run. Cached on subsequent runs.
if ! docker image inspect signal-builder:dev >/dev/null 2>&1; then
    echo "[local-build] bootstrapping signal-builder:dev (one-time, ~30s)"
    # `--load` forces the image into the local docker image store
    # (default buildx driver outputs only to build cache otherwise).
    docker build --load -t signal-builder:dev - <<'DOCKERFILE'
FROM alpine:3.19
RUN apk add --no-cache gcc musl-dev make cmake
WORKDIR /src
DOCKERFILE
fi

# Container arch must match the runtime container's arch. docker-compose
# runs the runtime image at host-native arch, so we match.
PLATFORM=linux/$(docker info -f '{{.OSType}}/{{.Architecture}}' 2>/dev/null | cut -d/ -f2)
case "$PLATFORM" in
    linux/aarch64) PLATFORM=linux/arm64 ;;
    linux/x86_64)  PLATFORM=linux/amd64 ;;
esac

# Mount the source read-only, the build tree on a named volume (so cmake
# is incremental across runs), and the output dir for the binary.
docker run --rm \
    --platform "$PLATFORM" \
    -v "$ROOT:/src:ro" \
    -v signal-builder-build:/build \
    -v "$OUT_DIR:/out" \
    -e GIT_HASH="$GIT_HASH" \
    signal-builder:dev \
    sh -c 'cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release \
                 -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
                 -DCMAKE_EXE_LINKER_FLAGS="-static" \
                 -DBUILD_SERVER_ONLY=ON \
                 -DBUILD_TOOLS=OFF \
                 -DGIT_HASH="$GIT_HASH" \
        && cmake --build /build --target signal_server --parallel \
        && cp /build/signal_server /out/signal_server'

echo "[local-build] signal_server (re)built at $GIT_HASH → $OUT_DIR/signal_server"
