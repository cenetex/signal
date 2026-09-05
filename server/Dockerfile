# Multi-stage build for the Signal web app and authoritative game server.
#
# Build from the repository root:
#   docker build -f server/Dockerfile --build-arg GIT_HASH=$(git rev-parse --short HEAD) .
FROM emscripten/emsdk:4.0.15 AS web-builder

ARG GIT_HASH=dev
WORKDIR /src
COPY CMakeLists.txt ./
COPY client/ ./client/
COPY server/ ./server/
COPY shared/ ./shared/
COPY vendor/ ./vendor/
COPY web/ ./web/
COPY assets/ ./assets/

RUN emcmake cmake -S . -B build-web \
      -DCMAKE_BUILD_TYPE=Release \
      -DGIT_HASH=${GIT_HASH} \
    && emmake cmake --build build-web --parallel \
    && mkdir -p public \
    && cp build-web/signal.js build-web/signal.wasm \
          build-web/play.html build-web/signal-touch-controls.js public/ \
    && if [ -d build-web/anime ]; then cp -R build-web/anime public/; fi \
    && if [ -d build-web/music ]; then cp -R build-web/music public/; fi \
    && if [ -d assets/stations ]; then cp -R assets/stations public/; fi \
    && if [ -f assets/media-provenance.json ]; then cp assets/media-provenance.json public/; fi \
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

RUN apk add --no-cache ca-certificates libstdc++ nodejs npm

WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci --omit=dev \
    && npm cache clean --force \
    && rm -rf /root/.npm

COPY --from=builder /src/build/signal_server ./signal-server
COPY --from=web-builder /src/public ./public
COPY scripts/webrtc-gateway.mjs ./scripts/webrtc-gateway.mjs
COPY scripts/fly-webrtc-entrypoint.sh ./scripts/fly-webrtc-entrypoint.sh
COPY scripts/lightsail-webrtc-entrypoint.sh ./scripts/lightsail-webrtc-entrypoint.sh

ENV PORT=8080
ENV SIGNAL_SERVER_PORT=9091
ENV SIGNAL_DATA_DIR=/app/data
ENV SIGNAL_STATIC_DIR=/app/public

EXPOSE 8080
EXPOSE 50000/udp

CMD ["sh", "./scripts/fly-webrtc-entrypoint.sh"]
