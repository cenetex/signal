# Signal

A multiplayer space mining game about frontier economies, signal coverage,
and the slow work of building an outpost network at the edge of charted
space.

Oh yeah and you kill each other with rocks.

**Play now:** [signal.ratimics.com/play](https://signal.ratimics.com/play)

Signal is built in C11 with Sokol — no engine, procedural drawing and physics
for the world geometry, and lightweight runtime media for music, station
portraits, and episode playback. You launch from a station, fracture asteroids,
tow fragments into furnaces, carry goods between sovereign currency zones, and
expand the network by building outposts at the edge of signal range. Every AI
dreams of being a space station.

The mining laser is a mining tool. It does not damage hulls. The only weapon
in the game is a rock under tractor tension, released. There are no lasers,
no missiles, no turrets, no directed-energy weapons, and there never will be.
Whatever combat means in Signal, it has to be built out of the same materials
as the economy: rocks, tractors, dock approaches, hopper levels, signal lines.

## Gameplay Loop

1. Launch from Prospect Refinery and work the asteroid belt.
2. Fracture large rocks with the mining beam and tractor ferrite, cuprite, and
   crystal fragments into station smelter beams.
3. Smelt fragments or deliver named contract cargo at docked stations. Credits
   are per-station — what you earn at Prospect can't be spent at Helios.
4. Let stations smelt fragments into ingots and fabricate ship parts, then buy
   what you need with `F`.
5. Press `B` in flight to create a planned outpost or reserve module slots on
   an existing outpost, then dock at a station with a shipyard and order a
   matching scaffold.
6. Use the tractor to tow loose scaffolds into place. New outposts still need
   frames delivered before they activate; placed module scaffolds enter a
   station-fed supply phase, then finish with a short commissioning timer.
7. Hail (`H`) to send a local scan/contact ping: reveal nearby tags, hear the
   current or nearest reachable station, and see your local ledger balance
   there. Dock and open CONTRACTS to accept, track, load, or deliver station
   work.

Signal range matters. Weak signal cuts ship response and mining speed, and both
players and NPCs are pushed back toward the connected station chain. Expansion
is signal expansion: an outpost's first role is being a relay.

Oh yeah and you kill each other with rocks.

## Stations

Stations are ring structures that rotate, with module ports around the arc and
a dock gap for ships to fly through. Players expand outposts through plan mode
plus shipyard-manufactured scaffolds. Station behavior is currently
simulation-authored: prices derive from inventory pressure, contracts are
generated from station need, and station-authored hail copy is signed into the
station chain before clients see it.

- `Prospect Refinery`: starter ferrite refinery. Smelts ferrite ore and sells
  ferrite ingots.
- `Kepler Yard`: frame press and shipyard hub. Sells frames and offers ship
  upgrades.
- `Helios Works`: cuprite/crystal processing plus mining and tractor upgrades,
  with its own shipyard.
- `Outposts`: begin as scaffolded relay hubs, then gain planned slots and
  shipyard-printed module scaffolds.

## Mining And Economy Chain

The starter mining laser is an L1 tool: it can fracture M-size ferrite rocks.
Each mining-laser upgrade raises the largest supported rock size by one step:
L2 reaches L rocks and unlocks cuprite, L3 reaches XL rocks and unlocks
crystal, and L4 reaches XXL rocks.

Station production currently flows through the physical fragment economy:

1. Fracture asteroids into S fragments and tractor them into ore hoppers or
   furnace beams.
2. Furnaces smelt ferrite, cuprite, and crystal fragments into matching ingots.
3. Frame Presses turn 1 ferrite ingot into 2 frames.
4. Laser Fabs turn 1 cuprite ingot + 1 frame into 1 laser module.
5. Tractor Fabs turn 1 crystal ingot + 1 frame into 1 tractor module.
6. Shipyards consume frames, laser modules, and tractor modules to commission
   ships, print scaffolds, and fabricate repair kits.

## Current Station Ops

Press `H` to send a hail/scan ping. If you are docked, the docked station
answers. Otherwise the nearest active station within dock range, signal range,
or the ship's scan fallback answers. The response shows that station's MOTD,
your local ledger balance there, and station work previews; dock at the station
to accept and resolve contracts. The ping also reveals short-lived local tags
for nearby world objects.

Station operators can set the MOTD, worker chatter, and special RATi-grade
delivery hail. See [`docs/operator-onboarding.md`](docs/operator-onboarding.md)
for the `swarm.rati.chat` avatar sync workflow.

## Docs

- [`docs/operator-onboarding.md`](docs/operator-onboarding.md): practical
  server/operator setup, station copy sync, chain health, and troubleshooting.
- [`docs/decentralization.md`](docs/decentralization.md): station identity,
  signed chain logs, cargo receipts, and the off-chain trust model.
- [`docs/cargo-architecture.md`](docs/cargo-architecture.md): canonical cargo
  vocabulary — fragments, bulk float, crates, manifests, and lineage.
- [`docs/holographic-gossip-network.md`](docs/holographic-gossip-network.md):
  contract gossip, decaying market memory, and neural worker coordination.
- [`docs/protocol-telemetry.md`](docs/protocol-telemetry.md): protocol
  discovery, stream classes, record sizes, and telemetry split.
- [`docs/fly-multiplayer.md`](docs/fly-multiplayer.md): cheap headless
  multiplayer relay deployment on Fly.io.
- [`docs/replay-harness.md`](docs/replay-harness.md): deterministic
  seed+prefix counterfactual replay harness for agent experiments.
- [`docs/anime-integration-plan.md`](docs/anime-integration-plan.md): current
  in-engine milestone-video playback architecture and remaining work.
- [`tests/fixtures/README.md`](tests/fixtures/README.md): deterministic
  `signal_verify` chain-log fixtures.

## Controls

- Flight: `W` or up thrusts, `S` or down brakes along current travel; from a
  stop, pressing it again can reverse. `A/D` or arrows turn, `M` fires the
  mining beam, and `E` docks or launches.
- Tractor: hold `Space` to tractor fragments or scaffolds. Tap `Space` to
  release a tow.
- Docked station controls: `Tab` cycles panels. In SHIP, `R` repairs,
  `M` upgrades the mining laser, `C` expands cargo hold, and `T` upgrades the
  tractor. In TRADE, `1`-`5` buy/sell visible rows, `F` pages, and `S` sells
  accepted cargo. In CONTRACTS, number keys select or track visible contracts;
  when the selected contract is ready, `S` loads pickup cargo or unloads
  delivery cargo. In YARD, `1`-`9` order scaffold kits where a shipyard is
  installed.
- Plan mode: while undocked and not towing, `B` enters or exits plan mode, `R`
  cycles module type, and `E` reserves the current slot. Press `B` in open
  signal to create a planned outpost.
- Scaffold placement: tow a scaffold with the tractor and press `E` to place
  it on a ring slot or found/materialize an outpost.
- Utility: `H` hail/scan the local area, `O` toggle mining autopilot, `[` and
  `]` switch music tracks, `/` toggle music pause, `X` self-destruct/reset in
  singleplayer, `Esc` quits.

## Build

Native desktop:

```sh
make build
./build/signal
```

Equivalent CMake path:

```sh
cmake -S . -B build
cmake --build build
./build/signal
```

Browser / WebAssembly with Emscripten:

```sh
make build-web
python3 -m http.server 8080 --directory build-web
```

Equivalent CMake path:

```sh
emcmake cmake -S . -B build-web
cmake --build build-web
python3 -m http.server 8080 --directory build-web
```

That produces `build-web/play.html` plus the `signal.js` and `signal.wasm`
runtime files.

Open `http://127.0.0.1:8080/play.html?singleplayer=1` for singleplayer, or
`http://127.0.0.1:8080/play.html?server=ws://127.0.0.1:9091/ws` when paired
with a local relay.

Local multiplayer dev:

```sh
make build-server
PORT=9091 SIGNAL_DATA_DIR=data ./build/signal_server
```

In a second terminal:

```sh
make build-web
python3 -m http.server 8080 --directory build-web
```

Then open `http://127.0.0.1:8080/play.html?server=ws://127.0.0.1:9091/ws`.

High-latency multiplayer test:

```sh
make build-server
PORT=9091 SIGNAL_DATA_DIR=data ./build/signal_server
```

In separate terminals:

```sh
make build-web
python3 -m http.server 8080 --directory build-web
make latency-proxy-high
```

Then open `http://localhost:8080/play.html?server=ws://127.0.0.1:19091/ws`.
The proxy delays browser→server and server→browser websocket frames while
preserving frame order on each logical delay lane. Tune it with
`LATENCY_CLIENT_MS`, `LATENCY_SERVER_MS`, `LATENCY_WORLD_PLAYERS_MS`,
`LATENCY_JITTER_MS`, `LATENCY_LISTEN`, and `LATENCY_UPSTREAM`.

The multiplayer HUD keeps two latency numbers separate:

- `ping` is an app-level `LATENCY_PING`/`LATENCY_PONG` round trip on the
  current transport.
- `ack` is input-sent to authoritative `WORLD_PLAYERS` acknowledgement age.
- `gap` is `ack - ping`; this is the cadence/sim/snapshot/render budget to
  optimize after raw transport is accounted for.

Tuning note: the relay keeps its 50 ms baseline `WORLD_PLAYERS` cadence for
idle players, but it also flushes an authoritative player-state packet when a
queued movement input is applied, capped at one flush every 25 ms. That lowers
ack age during active flight/mining/towing without doubling idle traffic; busy
control input can raise player-state traffic toward ~40 Hz.

With `make dev` and `make latency-proxy-high` still running, validate the
client-side correction metrics with:

```sh
make smoke-latency
```

For a self-contained local run that builds the client/server, starts an
ephemeral relay, launches both latency proxy modes, and runs both Playwright
latency assertions:

```sh
make smoke-latency-suite
```

The latency smoke reads wasm telemetry for ping RTT, authoritative ack age,
ack-minus-ping gap, player-state cadence, correction mode counts, replay depth,
tick skew, unacked inputs, and render-offset bounds.

Production runs the web client and headless multiplayer relay as one small
Fly.io app with a persistent volume and auto-start/auto-suspend. See
[`docs/fly-multiplayer.md`](docs/fly-multiplayer.md).

## Test

The `make test` target rebuilds `signal_test` and the native client from
current source before running fast, non-soak tests across shards, so a stale
binary cannot mask regressions. Default output is quiet — failures and the
final summary print:

```sh
make test                   # quiet: failures + summary only
make test TEST_VERBOSE=1    # full per-test "ok" stream
make test-soak              # long-running sim/contract/autopilot cases
make test-all               # fast + soak suites
make test-serial            # single-process fast suite for debugging
make smoke                  # build wasm and run Playwright browser smoke
make smoke-latency-suite    # build, launch local relay/proxies, run latency smokes
```

Or invoke the binary directly:

```sh
cmake -S . -B build-test -DBUILD_TESTS_ONLY=ON
cmake --build build-test
./build-test/signal_test            # verbose
./build-test/signal_test --quiet    # quiet (matches `make test`)
./build-test/signal_test --shard=0/4   # one of 4 parallel shards
```

## Notes

- Singleplayer runs against an in-process authoritative server. Multiplayer
  uses the same simulation over WebSocket.
- The game stays asset-light: world geometry and HUD text are drawn directly
  with Sokol. Music, station portraits/MOTD JSON, and MPEG episode clips are
  runtime assets loaded from `assets/` in native development or from the asset
  CDN in browser builds. The large media pack is not tracked in git; run
  [`assets/manifest.txt`](assets/manifest.txt).
- Native builds use Metal on macOS, OpenGL on Linux, and OpenGL on Windows
  through Sokol.
- The browser target uses WebGL 2 via Emscripten.
- Browser audio may stay muted until the page receives a click or key press to
  unlock WebAudio.
