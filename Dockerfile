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

# ----- stage 2: native server build ----------------------------------------
FROM emscripten/emsdk:3.1.64 AS native-build
WORKDIR /src
RUN apt-get update && apt-get install -y --no-install-recommends cmake git build-essential \
    && rm -rf /var/lib/apt/lists/*
COPY . /src
RUN GIT_HASH=$(git -C /src rev-parse --short HEAD 2>/dev/null || echo local) \
    && cmake -S /src -B /src/build -DGIT_HASH=$GIT_HASH \
    && cmake --build /src/build --target signal_server --parallel

# ----- stage 3: runtime -----------------------------------------------------
FROM python:3.12-slim AS runtime
WORKDIR /app

# Only what the server binary needs at runtime (libc, libm are in slim).
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates tini \
    && rm -rf /var/lib/apt/lists/*

COPY --from=native-build /src/build/signal_server /app/signal_server
COPY --from=wasm-build   /src/build-web            /app/build-web

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
