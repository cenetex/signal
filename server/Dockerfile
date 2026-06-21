# Multi-stage build for the Signal web app and authoritative game server.
#
# Build from the repository root:
#   docker build -f server/Dockerfile --build-arg GIT_HASH=$(git rev-parse --short HEAD) .
FROM emscripten/emsdk:4.0.15 AS web-builder

ARG GIT_HASH=dev
WORKDIR /deps

RUN npm init -y >/dev/null \
    && npm install --no-save --ignore-scripts --no-audit --no-fund @wllama/wllama@3.4.1

WORKDIR /src
COPY CMakeLists.txt ./
COPY client/ ./client/
COPY server/ ./server/
COPY shared/ ./shared/
COPY vendor/ ./vendor/
COPY web/ ./web/

RUN mkdir -p node_modules/@wllama \
    && cp -R /deps/node_modules/@wllama/wllama node_modules/@wllama/wllama \
    && emcmake cmake -S . -B build-web \
      -DCMAKE_BUILD_TYPE=Release \
      -DGIT_HASH=${GIT_HASH} \
    && emmake cmake --build build-web --parallel \
    && mkdir -p public \
    && cp build-web/signal.js build-web/signal.wasm \
          build-web/play.html build-web/signal-touch-controls.js \
          build-web/signal-hail-llm.js build-web/signal-hail-gguf-worker.js \
          build-web/signal-hail-wllama-runtime.js public/ \
    && cp -R build-web/wllama public/wllama \
    && cp web/index.html web/mine.html web/ost.html web/ost-manifest.json \
          web/ost-cover.jpg public/

FROM alpine:3.19 AS builder

RUN apk add --no-cache cmake gcc make musl-dev

ARG GIT_HASH=dev
WORKDIR /src

COPY CMakeLists.txt ./
COPY client/ ./client/
COPY server/ ./server/
COPY shared/ ./shared/
COPY vendor/ ./vendor/

RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
      -DCMAKE_EXE_LINKER_FLAGS="-static" \
      -DBUILD_SERVER_ONLY=ON \
      -DBUILD_TOOLS=OFF \
      -DGIT_HASH=${GIT_HASH} \
    && cmake --build build --target signal_server --parallel

FROM alpine:3.19

RUN apk add --no-cache ca-certificates

WORKDIR /app
COPY --from=builder /src/build/signal_server ./signal-server
COPY --from=web-builder /src/public ./public

ENV PORT=8080
ENV SIGNAL_DATA_DIR=/app/data
ENV SIGNAL_STATIC_DIR=/app/public

EXPOSE 8080

CMD ["./signal-server"]
