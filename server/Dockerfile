# Multi-stage build for the Signal authoritative game server.
FROM alpine:3.19 AS builder
RUN apk add --no-cache gcc musl-dev make
WORKDIR /app
COPY *.c *.h ./
RUN gcc -O2 -static -o signal-server main.c game_sim.c mongoose.c -lm -DMG_ENABLE_LOG=0

FROM alpine:3.19
RUN apk --no-cache add ca-certificates
WORKDIR /app
COPY --from=builder /app/signal-server .

EXPOSE 8080
ENV PORT=8080

ENTRYPOINT ["./signal-server"]
