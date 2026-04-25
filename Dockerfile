# syntax=docker/dockerfile:1.6
#
# Self-contained local runtime for Signal.
#
#   docker build -t signal .
#   docker run --rm -p 8080:8080 -p 9091:9091 -v $(pwd)/data:/app/data signal
#
# Or with docker-compose:  docker-compose up --build
#
# Two build stages + one tiny runtime:
#   1) `wasm-build`    — emscripten SDK base, produces build-web/signal.* for the
#                        browser.
#   2) `native-build`  — reuses the same base (gcc + cmake already present) to
#                        produce build/signal_server for the websocket sim.
#   3) `runtime`       — python:slim just to serve the static wasm bundle and
#                        run the server; both bound to the host via the
#                        entrypoint script. Ports 8080 (HTTP) and 9091 (WS).

# ----- stage 1: wasm build --------------------------------------------------
FROM emscripten/emsdk:3.1.64 AS wasm-build
WORKDIR /src
RUN apt-get update && apt-get install -y --no-install-recommends cmake git \
    && rm -rf /var/lib/apt/lists/*
COPY . /src
# GIT_HASH stamp — baked into both binaries. Falls back to "local" when the
# context isn't a git repo (e.g. a tarball build).
RUN GIT_HASH=$(git -C /src rev-parse --short HEAD 2>/dev/null || echo local) \
    && emcmake cmake -S /src -B /src/build-web -DGIT_HASH=$GIT_HASH \
    && cmake --build /src/build-web --parallel

# ----- stage 2: runtime (native server build happens here) -----------------
# Build the native server in the runtime image's own arch so it runs
# natively on both amd64 and arm64 hosts. Pinning the runtime to amd64
# made the binary segfault under qemu on M-series Macs (signal 11 in
# the contract/production loop) — much worse than the original "wrong
# loader" symptom that the pin was working around. Solution: rebuild
# signal_server inside the runtime stage so it matches the host arch.
FROM python:3.12-slim AS runtime
WORKDIR /app

# Build toolchain + runtime deps. Building signal_server here (instead of
# in a cross-arch stage) means the binary always matches the host arch.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates tini cmake build-essential git \
    && rm -rf /var/lib/apt/lists/*

# Compile signal_server in this stage so it's native arch.
COPY . /src
RUN GIT_HASH=$(git -C /src rev-parse --short HEAD 2>/dev/null || echo local) \
    && cmake -S /src -B /src/build -DBUILD_SERVER_ONLY=ON -DGIT_HASH=$GIT_HASH \
    && cmake --build /src/build --target signal_server --parallel \
    && cp /src/build/signal_server /app/signal_server \
    && rm -rf /src

COPY --from=wasm-build /src/build-web /app/build-web
# Ship the play/shell wrappers alongside the emscripten bundle. These set
# window.SIGNAL_SERVER = ws://<host>:9091/ws when served from a local
# host, so the wasm client connects to the in-container server instead
# of falling into singleplayer ("offline"). Without these, opening
# /signal.html directly bypasses the WS autoconfig.
COPY --from=wasm-build /src/web/play.html  /app/build-web/play.html
COPY --from=wasm-build /src/web/shell.html /app/build-web/shell.html

# Persistence dirs (world.sav, chain/, saves/, stations/). Bind a host dir
# to /app/data via `docker run -v $(pwd)/data:/app/data` to make them
# survive container restarts.
RUN mkdir -p /app/data/saves /app/data/stations /app/data/chain

COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

ENV PORT=9091
EXPOSE 8080 9091

# tini as PID 1 so SIGTERM → graceful shutdown of both the HTTP server and
# signal_server. The entrypoint script fans out into a tiny supervisor.
ENTRYPOINT ["/usr/bin/tini", "--", "/app/entrypoint.sh"]
