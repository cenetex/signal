# Fly Multiplayer App

Signal now deploys the browser client and authoritative multiplayer server as
one Fly.io app. Fly is a good fit for bringing multiplayer back cheaply because
one small Machine can serve the static WebAssembly bundle, host the WebSocket
relay, auto-start on demand, and keep persistent state on a small volume.

## Shape

- One Fly Machine in one region.
- One Fly Volume mounted at `/app/data`.
- The server listens on `PORT=8080`, serves the web client from `/app/public`,
  and exposes `/ws`, `/health`, and `/api/protocol`.
- `/play` redirects to `/play.html`; by default `play.html` connects to the
  same-origin `wss://.../ws` relay. `?singleplayer=1` still forces the
  in-process local sim.
- `auto_stop_machines = "suspend"` and `min_machines_running = 0` keep the
  server off when there are no connections, while resume is faster than a cold
  stop when Fly can preserve a snapshot.

Keep it single-region for now. The current simulation is one authoritative
world with local disk state, so multiple active regions would become multiple
worlds unless state is externalized or sharded.

## First Deploy

The GitHub Actions path is the intended bootstrap:

1. Create a repository secret named `FLY_API_TOKEN`. The first `launch=true`
   run needs a token that can create apps and volumes. After the app exists,
   you can replace it with an app deploy token from
   `fly tokens create deploy -x 999999h`.
2. Create a repository secret named `SIGNAL_STATION_AUTH_SECRET` with a stable
   random value, for example `openssl rand -hex 32`.
3. Run the `Deploy Fly App` workflow manually with `launch=true`.

The workflow runs `fly launch --copy-config --no-deploy`, creates the
`signal_data` volume if needed, stages `SIGNAL_STATION_AUTH_SECRET`, deploys
the app, and checks `/health`. Pushes to `main` deploy Fly after that. The old
Arweave/Irys deployment workflow and upload helpers have been removed.

You can override the defaults with repository variables:

- `FLY_APP` (default `signal-relay-kind-pond-4338`)
- `FLY_REGION` (default `lax`)
- `FLY_VOLUME` (default `signal_data`)
- `FLY_VOLUME_SIZE` (default `1`)
- `FLY_ORG` (unset by default)
- `FLYCTL_VERSION` (default `0.4.59`)

For local bootstrap instead, install and log in to `flyctl`, then create the app
and volume:

```sh
fly launch --copy-config --no-deploy --no-github-workflow
fly volumes create signal_data --size 1 --region lax
fly secrets set SIGNAL_STATION_AUTH_SECRET="$(openssl rand -hex 32)"
fly deploy --build-arg GIT_HASH="$(git rev-parse --short HEAD)"
```

If Fly asks for a different app name, update `app = "..."` in `fly.toml`
before creating the volume. If Vancouver users are the first audience, `lax`
is a reasonable starting region; try `sjc` as the alternate if latency testing
says it wins.

Check the relay:

```sh
fly status
fly logs
curl https://signal-relay-kind-pond-4338.fly.dev/health
```

Open the browser client with:

```text
https://signal-relay-kind-pond-4338.fly.dev/play
```

Once DNS is pointed at Fly, the canonical URL is:

```text
https://signal.ratimics.com/play
```

## Local Relay Check

Build and run the same headless server locally:

```sh
make build-server
PORT=9091 SIGNAL_DATA_DIR=data ./build/signal_server
```

In a second terminal, serve the browser build:

```sh
make build-web
python3 -m http.server 8080 --directory build-web
```

Then open:

```text
http://127.0.0.1:8080/play.html?server=ws://127.0.0.1:9091/ws
```

## Smoke Tests

The latency suite builds the WebAssembly client and native server, starts a
temporary relay with a temporary data directory, runs both latency proxy modes,
and drives Playwright against the browser client:

```sh
make smoke-latency-suite
```

For a live Fly app, run the browser smoke directly:

```sh
SMOKE_URL="https://signal.ratimics.com/play" \
  npx playwright test tests/browser-smoke.spec.ts --project=chromium --grep "connects"
```

## Cost Controls

- `shared-cpu-1x` with `512mb` RAM is the starting point. Move to `1gb` only
  if `/health`, logs, or Fly metrics show memory pressure.
- `auto_stop_machines = "suspend"` means CPU and RAM billing stop while idle;
  the persistent cost is the root filesystem, the `signal_data` volume, and
  any public egress.
- A connected WebSocket counts as traffic, so Fly should not suspend the
  Machine while players are online.
- Volumes are one-Machine, one-region storage. Create backups before treating
  the relay as durable production state.
