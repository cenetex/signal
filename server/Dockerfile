# Multi-stage build for the Signal relay server.
FROM golang:1.21-alpine AS builder

WORKDIR /app
COPY go.mod go.sum ./
RUN go mod download
COPY main.go .
RUN CGO_ENABLED=0 GOOS=linux go build -o relay-server .

FROM alpine:3.19
RUN apk --no-cache add ca-certificates
WORKDIR /app
COPY --from=builder /app/relay-server .

EXPOSE 8080
ENV PORT=8080

ENTRYPOINT ["./relay-server"]
