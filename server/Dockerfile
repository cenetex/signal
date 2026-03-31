# Multi-stage build for the Signal authoritative game server.
# Build from repo root: docker build -f server/Dockerfile --build-arg GIT_HASH=abc1234 .
FROM alpine:3.19 AS builder
RUN apk add --no-cache gcc musl-dev make
ARG GIT_HASH=dev
WORKDIR /app
COPY shared/ ./shared/
COPY src/*.h src/*.c ./src/
COPY server/*.c server/*.h ./
RUN gcc -O2 -static -Ishared -Isrc -o signal-server main.c game_sim.c mongoose.c src/commodity.c src/ship.c src/economy.c src/asteroid.c -lm -DMG_ENABLE_LOG=0 -DGAME_SIM_VERBOSE -DGIT_HASH=\"$GIT_HASH\"

FROM alpine:3.19
RUN apk --no-cache add ca-certificates
WORKDIR /app
COPY --from=builder /app/signal-server .

EXPOSE 8080
ENV PORT=8080

ENTRYPOINT ["./signal-server"]
