# Lightsail + Lambda Local-First Multiplayer

Signal's low-population multiplayer should be local-first:

1. Browser starts in local singleplayer unless `?server=...` or `?online=1` is present.
2. Optional `?lobby=wss://...` connects to the AWS WebSocket lobby in the background.
3. When the lobby sees enough compatible players in a room, it wakes the relay.
4. Clients reload into the authoritative RTC session with `?server=rtcs://...`.
5. If the remote session drops, the client starts a fresh local loopback authority.

The hot gameplay path stays:

```text
browser WebRTC DataChannel -> Lightsail webrtc-gateway -> native signal-server
```

Lambda is only the rendezvous/control plane.

## Lightsail Runtime

Use the normal Docker image, but run the Lightsail entrypoint:

```sh
docker run -d --name signal-relay \
  -p 80:8080 \
  -p 50000:50000/udp \
  -v /opt/signal/data:/app/data \
  -e PORT=8080 \
  -e SIGNAL_SERVER_PORT=9091 \
  -e SIGNAL_DATA_DIR=/app/data \
  -e SIGNAL_STATIC_DIR=/app/public \
  -e SIGNAL_ALLOWED_ORIGIN=https://signal.ratimics.com \
  -e SIGNAL_STATION_AUTH_SECRET=replace-me \
  -e RTC_GATEWAY_PREFIX=/rtc \
  -e RTC_GATEWAY_ICE_PORT=50000 \
  -e RTC_GATEWAY_ICE_UDP_MUX=1 \
  -e RTC_GATEWAY_WAKE_TOKEN=replace-me \
  signal:latest \
  sh ./scripts/lightsail-webrtc-entrypoint.sh
```

Put TLS in front with Caddy, nginx, or a Lightsail-origin certificate setup.
Do not set Fly's `RTC_GATEWAY_ICE_BIND=fly-global-services` on Lightsail.

Open:

- TCP 80/443
- UDP 50000
- SSH restricted to the operator

## Wake API

The gateway exposes:

```text
POST /wake
Authorization: Bearer <RTC_GATEWAY_WAKE_TOKEN>
```

It starts `/app/signal-server` only if needed, polls the native `/health`,
and returns the managed authority status.

Direct RTC joins also wake the server. Lambda is a smoother prewarm path, not
a hard dependency for explicit `?server=` links.

## Lambda Lobby

The lobby lives in `aws/lambda/signal-lobby`.

Deploy with SAM:

```sh
cd aws/lambda/signal-lobby
npm install
sam deploy --guided \
  --parameter-overrides \
    RelayRtcUrl=rtcs://relay.signal.ratimics.com/rtc/signal-main \
    RelayWakeUrl=https://relay.signal.ratimics.com/wake \
    RelayWakeToken=replace-me
```

Then launch the game with:

```text
https://signal.ratimics.com/play?lobby=wss://<api-id>.execute-api.<region>.amazonaws.com/prod
```

The page starts local. When the lobby emits `serverReady`, the page reloads
with `?server=<rtcs-url>&network=1`.

## Persistence

The authoritative server remains the only writer for shared world state.
On Lightsail it writes under `SIGNAL_DATA_DIR`:

```text
/app/data/world.sav
/app/data/saves/
/app/data/stations/
/app/data/chain/
```

Back up `/opt/signal/data` regularly. A simple first production setup is:

```sh
tar -C /opt/signal -czf /tmp/signal-data.tgz data
aws s3 cp /tmp/signal-data.tgz s3://<bucket>/signal/$(date +%Y%m%d-%H%M%S).tgz
```

For multi-region ephemeral servers, add a world lease:

```text
DynamoDB world lease: world_id -> holder, expires_at, relay_id
S3: worlds/<world_id>/snapshot.tgz
```

Boot flow:

1. Acquire the world lease with a conditional write.
2. Download the latest snapshot.
3. Start the native server with `SIGNAL_DATA_DIR` pointed at that restored dir.
4. Refresh the lease while players are connected.
5. On shutdown, stop accepting players, save, upload the snapshot, release lease.

Do not merge two local singleplayer worlds in v1. Multiplayer joins a canonical
shared world, while local singleplayer remains local.
