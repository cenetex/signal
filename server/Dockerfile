# Multi-stage build for the Signal authoritative game server.
# Build from repo root: docker build -f server/Dockerfile .
FROM alpine:3.19 AS builder
RUN apk add --no-cache gcc musl-dev make
WORKDIR /app
COPY shared/ ./shared/
COPY server/*.c server/*.h ./
RUN gcc -O2 -static -Ishared -o signal-server main.c game_sim.c mongoose.c -lm -DMG_ENABLE_LOG=0 -DGAME_SIM_VERBOSE

FROM alpine:3.19
RUN apk --no-cache add ca-certificates
WORKDIR /app
COPY --from=builder /app/signal-server .

EXPOSE 8080
ENV PORT=8080

ENTRYPOINT ["./signal-server"]
