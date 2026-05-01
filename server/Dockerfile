# Multi-stage build for the Signal authoritative game server.
#
# Uses the canonical CMake build so flags, sources, and defines match
# every other build (CI tests, local dev, `cmake --build`). Adding a
# new server source file means editing SIGNAL_SIM_SOURCES in the
# top-level CMakeLists.txt only — no change here.
#
# Build from repo root:
#   docker build -f server/Dockerfile --build-arg GIT_HASH=abc1234 .
FROM alpine:3.19 AS builder
RUN apk add --no-cache gcc musl-dev make cmake
ARG GIT_HASH=dev
WORKDIR /app

# Only the files the server target actually needs. We COPY all vendor/ here
# and rely on .dockerignore to exclude client-only deps (vendor/sokol,
# vendor/minimp3, etc). This prevents silent drift when a server source adds
# a new #include "vendor/..." without updating the COPY list (see #518, #520).
# A CI check validates that all server/shared #includes match the ignore list.
COPY CMakeLists.txt ./
COPY shared/ ./shared/
COPY src/ ./src/
COPY server/ ./server/
COPY vendor/ ./vendor/

# Flags preserved from the old hand-rolled gcc invocation:
#   -static                          (fully static musl binary)
#   -O2                              (match prior prod; CMake default
#                                     would be -O3 for Release)
#   -fstack-protector-strong         (via CMakeLists.txt server target)
#   -D_FORTIFY_SOURCE=2              (via CMakeLists.txt, Release-only)
#   -DMG_ENABLE_LOG=0                (via CMakeLists.txt server target)
#   -DGAME_SIM_VERBOSE               (via CMakeLists.txt server target)
RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
      -DCMAKE_EXE_LINKER_FLAGS="-static" \
      -DBUILD_SERVER_ONLY=ON \
      -DBUILD_TOOLS=OFF \
      -DGIT_HASH=${GIT_HASH} \
    && cmake --build build --target signal_server --parallel

FROM alpine:3.19
RUN apk --no-cache add ca-certificates
WORKDIR /app
COPY --from=builder /app/build/signal_server ./signal-server

EXPOSE 8080
ENV PORT=8080

ENTRYPOINT ["./signal-server"]
