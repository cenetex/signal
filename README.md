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
4. Laser Fabs turn 1 crystal ingot + 1 frame into 1 laser module.
5. Tractor Fabs turn 1 cuprite ingot + 1 frame into 1 tractor module.
6. Shipyards consume frames, laser modules, and tractor modules to commission
   ships, print scaffolds, and fabricate repair kits.

Backlog target: conserved matter should use the visible quartering algebra
(`1 fragment -> 4 ingots`, `1 ingot -> 4 frames`, `4 frames -> 1 block`) while
prices and production time remain analog. Repair kits are legacy matter math;
the target repair model is block-count hull/station damage and frame/block
welding.

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
- [`docs/cargo-receipt-trust.md`](docs/cargo-receipt-trust.md): what portable
  receipts prove, station-policy limits, and semantic CLI verification.
- [`docs/cargo-architecture.md`](docs/cargo-architecture.md): canonical cargo
  vocabulary — fragments, bulk float, crates, manifests, and lineage.
- [`docs/holographic-gossip-network.md`](docs/holographic-gossip-network.md):
  contract gossip, decaying market memory, and neural worker coordination.
- [`docs/intelligence-vision.md`](docs/intelligence-vision.md): central
  NSRL/CRLPLRIMES intelligence spine, visibility plan, and next gaps.
- [`docs/protocol-telemetry.md`](docs/protocol-telemetry.md): protocol
  discovery, stream classes, record sizes, and telemetry split.
- [`docs/fly-multiplayer.md`](docs/fly-multiplayer.md): cheap headless
  multiplayer relay deployment on Fly.io.
- [`docs/replay-harness.md`](docs/replay-harness.md): deterministic
  seed+prefix counterfactual replay harness for agent experiments.
- [`docs/anime-integration-plan.md`](docs/anime-integration-plan.md): current
  in-engine milestone-video playback architecture and remaining work.
- [`docs/space-mining-3d-v1.md`](docs/space-mining-3d-v1.md): draft v1
  design for procedural 3D presentation over the existing Signal loop.
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
- Utility: `H` hail/scan the local area. After one manual paid ore delivery,
  `O` toggles mining autopilot. `[` and
  `]` switch music tracks, `/` toggle music pause, `X` self-destruct/reset in
  singleplayer, `Esc` quits.

## Build

Native desktop:

```sh
make build
./build/signal
```

Native stdout/stderr telemetry persists by default. On macOS it is appended to
`~/Library/Logs/signal/client.log`; Linux uses
`$XDG_STATE_HOME/signal/client.log` (default `~/.local/state`), and Windows uses
`%LOCALAPPDATA%\\signal\\client.log`. The log rotates to `client.log.1` at 8 MiB.
Set `SIGNAL_LOG_PATH` to override the file or `SIGNAL_LOG_PERSIST=0` to retain
line-buffered terminal output without persistence.

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
PORT=9091 SIGNAL_DATA_DIR=data SIGNAL_ALLOW_DEV_STATION_AUTH_SECRET=1 ./build/signal_server
```

In a second terminal:

```sh
make build-web
python3 -m http.server 8080 --directory build-web
```

Then open `http://127.0.0.1:8080/play.html?server=ws://127.0.0.1:9091/ws`.

Local WebRTC gateway dev:

```sh
make build-server
PORT=9091 SIGNAL_DATA_DIR=data SIGNAL_ALLOW_DEV_STATION_AUTH_SECRET=1 ./build/signal_server
```

In separate terminals:

```sh
make rtc-gateway
make build-web
python3 -m http.server 8080 --directory build-web
```

Then open
`http://127.0.0.1:8080/play.html?server=rtc://127.0.0.1:19093/signal-main`.
The gateway terminates a WebRTC DataChannel from the browser and proxies the
existing binary protocol to the native server at `ws://127.0.0.1:9091/ws`.

High-latency multiplayer test:

```sh
make build-server
PORT=9091 SIGNAL_DATA_DIR=data SIGNAL_ALLOW_DEV_STATION_AUTH_SECRET=1 ./build/signal_server
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
`LATENCY_INPUT_APPLIED_MS`, `LATENCY_JITTER_MS`, `LATENCY_LISTEN`, and
`LATENCY_UPSTREAM`.

The multiplayer HUD keeps two latency numbers separate:

- `ping` is app-level `LATENCY_PING`/`LATENCY_PONG` transport RTT. The client
  subtracts the pong's server-side turnaround from the control sample so a relay
  stall does not masquerade as network one-way delay. `LATENCY_PONG` also
  carries the current `server_tick`, so clients get a cheap prediction anchor
  without depending on a full world snapshot heartbeat. Clients ping at 1 Hz
  while bootstrapping, use a bounded 2 Hz recovery cadence when ping goes stale
  or the smoothed ack gap jumps, relax to 0.5 Hz once the sample is fresh, and
  stretch to 0.4 Hz when ping and ack are both fresh, low-gap, and stable.
  Active `INPUT_APPLIED` receipts and promoted private `STATE` acks echo input
  send/receive/send timestamps, so healthy held controls and correction acks
  can refresh the same transport RTT estimate; while those ack-derived samples
  are fresh, standalone pings stretch to 0.2 Hz.
- `ack` is input-sent to authoritative `INPUT_APPLIED` or private `STATE`
  receipt age. `WORLD_PLAYERS` can still mirror the latest ack on public
  semantic heartbeat packets for compatibility, but the local player's baseline
  and corrections now come from the private `STATE` lane.
- `gap` is `ack - ping`; this is the cadence/sim/snapshot/render budget to
  optimize after raw transport is accounted for.

Tuning note: the relay still evaluates player state on its 50 ms state tick,
but each recipient's full `WORLD_PLAYERS` batch omits that recipient's own
record during steady play. The local player gets its initial baseline and later
corrections through private authoritative `STATE`; the public self record is
kept only while the local mining beam is active so server-owned beam endpoints
remain authoritative. Unchanged full `WORLD_PLAYERS` batches wait for a 16000 ms
semantic heartbeat; remote flags, tow state, callsign, and beam endpoints still
send immediately, while dock/launch and thrust status ride the 2-byte
`WORLD_PLAYER_DOCK_Q` status record. Ack tail churn is folded into the
heartbeat because modern acks ride the private control lane. Remote undocked
pose drift uses the 10-byte quantized `WORLD_PLAYER_MOTION_Q` stream as an
absolute baseline/reset and then the 6-byte `WORLD_PLAYER_MOTIOND_Q` delta
stream. When velocity is unchanged at wire precision, the 4-byte
`WORLD_PLAYER_POSED_Q` position+angle delta carries the clean refresh while the
client keeps dead-reckoning with its retained velocity. Live sends combine
those compact delta shapes into `WORLD_PLAYER_MOTIONM_Q` so a recipient gets
one mixed motion packet rather than separate motion-delta and pose-delta
WebSocket frames. The relay still evaluates candidates on the 200 ms motion
cadence, but clean predicted drift is omitted until a coalesced per-recipient
0.5 Hz safety heartbeat; normal prediction error or visible angle drift can
refresh at 5 Hz, and large prediction or angle error can still punch through
early, instead of forcing a full 77-byte player record or 20 Hz float pose
stream.
Modern movement acks ride a private control lane: clean queued inputs receive
the fixed-size `INPUT_APPLIED` receipt, while the relay promotes the receipt to
a private authoritative `STATE` packet when the player needs an initial
baseline, forced action resync, 8 s safety correction, or a meaningful
pose/velocity/flag/tow drift update. Both ack forms carry the input
send/receive/send timestamp tail, so promoted correction acks also refresh the
transport RTT estimator. This keeps ordinary ack traffic tiny without sending a
full `WORLD_PLAYERS` broadcast. If the relay catches up multiple sim ticks in
one poll, it coalesces pending receipts to the latest applied sequence until an
event/action-result barrier or the end of the poll.
The client still sends control changes immediately, but held controls heartbeat
at 250 ms (~4 Hz) while advancing the ack sequence every 1000 ms (~1 Hz) during
healthy steady play, or every 1500 ms when ping and ack are both fresh, low-gap,
and stable. If ack freshness, ack gap, or unacked input count gets unhealthy,
held controls temporarily advance the ack sequence every 500 ms (~2 Hz), then
250 ms (~4 Hz) in hot recovery, until the lane recovers. Steady thrust/mining
therefore does not produce 8+ input acks per second per player.
No-control idle repeats are kept to 1000 ms; releases and other control changes
still send immediately, so the idle heartbeat is only a low-rate stale-state
safety net.
`make latency-proxy-ack-lag` delays both authoritative streams so ping stays low
while ack age rises.
Client input lead is driven by fresh smoothed transport ping when available,
falling back to fresh ack only before ping samples arrive or after they go
stale. The acknowledged apply-tick error then nudges a small lead margin up or
down so the client avoids late inputs without permanently padding ack latency.
If a `WORLD_PLAYERS` batch changes only in per-record `server_tick`, pose,
docked/thrust status, or ack-tail fields, the relay suppresses the duplicate
for that recipient until the 16000 ms semantic heartbeat; pose updates ride
`WORLD_PLAYER_MOTION_Q` absolute resets plus mixed `WORLD_PLAYER_MOTIONM_Q`
deltas, dock/launch/thrust status rides
`WORLD_PLAYER_DOCK_Q`, while other flags, tow state, callsign, and beam
endpoints still send immediately.
For docked ships, berth pose drift from rotating station rings is also folded
into that heartbeat instead of forcing the full 20 Hz player stream.

Asteroid updates are also delta-oriented: the first visible record, dirty state
changes, moving rocks, and explicit out-of-view removals are sent, while clean
static rocks already known by that player are omitted from the 10 Hz world tick.
Docked players do not receive asteroid identity or motion snapshots; the first
flight snapshot after launch sends the current relevance-filtered view. This
keeps station/session joins from competing with the ping/ack lane while the
player is still in station UI.
First-visible active asteroid upserts prefer compact `WORLD_ASTEROIDS8_Q` and
`WORLD_ASTEROIDS_Q` records: visual position/velocity use the same quantized
scales as the live motion streams, hp/ore/radius use 0.125-unit fixed point, and
grade/phase/crystal state are packed into a detail byte. This trims starter-belt
identity records from 35 bytes to 18-19 bytes before later motion deltas take
over, and keeps dense belt views from becoming a permanent full-snapshot stream.
Background first-visible asteroid identities outside the near field are
trickled in fourth-tick bursts rather than sprayed one record at a time. The
per-recipient burst budget scales down as more undocked players are receiving
asteroid snapshots: eight records for small scenes, four around 5-8 live
recipients, two around 9-16, and one above that. Near identities, dirty state,
removals, and motion corrections still send immediately. This spreads the
launch-time belt reveal across several ticks instead of letting it compete with
ping/pong and input receipts in one burst.
Already-known moving rocks can correct at about 3.3 Hz per recipient when
prediction error demands it, with one final compact settling motion sample when
velocity drops below the moving threshold so dead-reckoned clients stop cleanly.
Moving rocks outside the 600u interaction halo relax to a 1 Hz repeat cadence
only for fast far-field throws; ordinary outer-near/far drift runs at 0.5 Hz. Slow
very-far edge-of-view rocks relax to 0.1 Hz, while fast edge-of-view throws keep
0.33 Hz eligibility. Fast throws keep the 3.3 Hz correction cadence only once
they are inside the priority band near the player or are fast enough to be a
nearby hazard.
Perfectly predicted near/high-detail rocks use a 0.5 Hz safety heartbeat, while
slow near rocks use a 0.17 Hz safety heartbeat and a wider prediction-error
budget. Sub-1 px/s crawl drift has its own 0.025 Hz safety heartbeat and still
corrects early if prediction diverges beyond a wider visual error budget. Far
fast throws outside that priority band use the prediction-error gate and the
0.17 Hz far-field safety heartbeat with a wider error budget. Ordinary far-field
drift uses a looser prediction-error gate plus a 0.1 Hz safety heartbeat, and
very-far rocks use the widest edge-of-view error budget with a 0.05 Hz safety
heartbeat so background drift does not monopolize the ack lane. Clean
repeat corrections use compact motion streams: the normal
path is quantized `WORLD_ASTEROID_MOTION_Q`, cutting steady-motion records from
18 bytes to 10 bytes while staying within the near-field prediction error
budget. If the retained client velocity is unchanged or close enough that it
would drift only a few pixels over the interpolation window, the relay can elide
it with `WORLD_ASTEROID_POS_Q`, a 6-byte position-only correction that lets the
client keep dead-reckoning with its existing velocity. Low asteroid slots use
the byte-index `WORLD_ASTEROID_POS8_Q` variant, trimming common starter-belt
position corrections to 5 bytes per record. When the previous absolute
quantized asteroid position is a valid baseline and the new position is within
signed-byte reach, `WORLD_ASTEROID_POSD_Q` and low-slot
`WORLD_ASTEROID_POSD8_Q` carry only index plus x/y deltas, reducing those
position-only corrections to 4 or 3 bytes per record.
`WORLD_ASTEROID_MOTION` remains the float fallback for split serializers that
do not provide a quantized buffer. First-seen and structural identity records
stay on full `WORLD_ASTEROIDS`; out-of-view or destroyed slots use compact
`WORLD_ASTEROID_REMOVE` index records instead of 35-byte inactive asteroid
records. Already-known active dirty hp/ore/smelt refreshes ride compact
`WORLD_ASTEROID_STATE_Q` records. Numeric hp/ore/radius and smelt drift is
coalesced per recipient to 0.5 Hz, with an 8s exact-state heartbeat, while
grade/stage/phase changes still send immediately.
The compact motion stream is also prediction-gated per client: after the
minimum cadence, clean moving rocks send again only when server motion diverges
from the last sent position/velocity beyond a near/mid-field/very-far error
budget, with a slow heartbeat as a safety net for perfectly predictable drift.
Fracture challenge rebroadcasts are also per-player delivery-gated. The sim
still re-arms active challenges so late joiners or newly in-range pilots can
receive the claim window, but a player who already saw a challenge or resolution
does not get the same `fracture_id` again over the reliable WebSocket stream.
The relay also hash-suppresses unchanged per-player `WORLD_NPCS`,
`WORLD_SCAFFOLDS`, cargo-pod identity, and `WORLD_INTERACTIONS` payloads; EMF
logs include `TxSuppressed*` counters to show how many repeat bytes were avoided.
`WORLD_NPCS` additionally ignores pure position/velocity/angle/thrust,
rarity-tint drift, and visual status churn for its semantic hash: visible-set,
role, session token, or home-station changes send immediately, while
state/target/towed-fragment changes ride compact `WORLD_NPC_STATUS` records at
around 0.5 Hz. When target/towed references fit in one byte, the relay uses
`WORLD_NPC_STATUS8_Q` to trim status records from 6 bytes to 4 bytes.
`WORLD_NPC_STATUS` ignores thrust-only flips; the motion stream owns
engine-flame visuals. Pose/thrust-only motion rides quantized
`WORLD_NPC_MOTION8_Q` records with around 0.5 Hz minimum eligibility and
dead-reckons client-side between packets. This preferred stream keeps the
existing 4 px position scale but uses byte velocity and byte facing, trimming
full NPC visual motion from 12 bytes to 9 bytes per record;
`WORLD_NPC_MOTION_Q` remains the higher-precision compatibility fallback.
Clean NPC motion is prediction-gated per client: after the minimum cadence,
records send only when pose, velocity, or angle diverges beyond the visual error
budget, or the thrust flag changes, with a 0.17 Hz clean-motion safety heartbeat
inside the client extrapolation window. If velocity, angle, and thrust still
match the previous baseline, position-only corrections use compact
`WORLD_NPC_POS_Q` records; if velocity and thrust still match but facing
changed, compact `WORLD_NPC_POSE_Q` records carry position plus angle. If
angle and thrust still match but velocity changed, `WORLD_NPC_LINEAR_Q` carries
position plus velocity at 9 bytes per record instead of the 12-byte full
quantized motion record. Tint
display drift reconciles on the full
metadata heartbeat. The legacy
float `WORLD_NPC_MOTION` decoder remains available as a compatibility fallback.
Unchanged full NPC metadata is kept to a 0.05 Hz heartbeat.
`WORLD_CARGO_PODS_Q` is the preferred cargo identity upsert lane, trimming
records from the legacy 38-byte `WORLD_CARGO_PODS` layout to 28 bytes while
keeping cargo kind, commodity, quantity, shipment, tow owner, tractor
assignment, radius, and summary flags exact. Position, velocity, and rotation
are visual drift in that identity packet and use the same quantized scales as
motion updates, so semantic changes send immediately while pose churn does not.
Pure motion rides
compact quantized `WORLD_CARGO_POD_MOTION_Q` records around 0.5 Hz and
dead-reckons client-side between packets. Clean pod motion is prediction-gated
per client: after the 0.5 Hz minimum cadence, records send only when pose,
velocity, or rotation diverges beyond the visual error budget, with a 0.17 Hz
clean-motion safety heartbeat for perfectly predicted drift. If rotation still
matches the previous baseline, `WORLD_CARGO_POD_LINEAR_Q` carries only position
plus velocity at 9 bytes per record instead of the 11-byte full quantized
motion record.
`WORLD_CARGO_POD_REMOVE` carries one-byte pod indices for removals/view exits,
so `WORLD_CARGO_PODS_Q` can act as an identity upsert lane instead of resending
the full visible pod list. Unchanged full pod metadata is kept to a 0.05 Hz
reconciliation heartbeat. The legacy `WORLD_CARGO_PODS` and float
`WORLD_CARGO_POD_MOTION` decoders remain available as compatibility fallbacks.
`WORLD_SCAFFOLD_REMOVE` does the same for scaffold removals/view exits, letting
`WORLD_SCAFFOLDS` carry only scaffold identity/build upserts instead of a full
visible replacement list on every change. Pure scaffold position/velocity drift
rides compact quantized `WORLD_SCAFFOLD_MOTION_Q` records for already-known
slots.
Interaction identity prefers compact `WORLD_INTERACTIONS_Q`: source/target refs
stay exact, while initial endpoints, range, and intensity use the same quantized
visual fields as drift, trimming identity records from 38 bytes to 25 bytes.
Cargo-pod tractor visuals derive source anchors, targets, and intensity from the
live station/module and cargo-pod refs on the client; pure endpoint/intensity
drift is now only a 0.1 Hz quantized `WORLD_INTERACTION_DRIFT` safety refresh.
Both identity and drift streams are filtered per player, keeping beams whose
source, target, or midpoint is inside the same relevance radius used for world
entities. Unchanged full interaction identity metadata is kept to a 0.05 Hz
reconciliation heartbeat. The legacy `WORLD_INTERACTIONS` decoder remains
available as the float fallback.
`CONTRACTS_Q` is the preferred contract board wire format. It keeps the exact
contract order and base fields from legacy `CONTRACTS`, but sends provenance
tails only when nonzero, which keeps sparse join snapshots small. Contract
boards are still age-aware: new/removed/changed contracts send immediately, but
age-only price drift refreshes every 30 seconds instead of turning the global
work board into a once-per-second broadcast.
Full `WORLD_STATIONS` economy summaries are hash-suppressed per player, so the
1 Hz reconciliation fallback costs bytes only when inventory or station credit
pool values actually changed. `WORLD_STATIONS_Q` is the preferred station
economy wire format: it sends the same station order and exact float values,
but includes only nonzero inventory slots and the credit-pool field only when
present.
`STATION_IDENTITY_Q` is the preferred station identity wire format. It carries
the same station structure as legacy `STATION_IDENTITY`, but length-prefixes
operator text and sends only active module, arm, plan, pending-build, and policy
rows instead of fixed-capacity zero padding. Cache comparisons still ignore live
ring rotation/omega drift: station snapshots seed client-side ring prediction,
but fallback/dirty identity refreshes resend only when structural identity
fields actually change. The fixed `STATION_IDENTITY` decoder remains available
as a compatibility fallback.
`WORLD_TIME` is also a low-rate reconciliation stream: clients advance world
time locally and ease toward the server sample, so the relay sends time once on
join and then at 0.5 Hz instead of on every 10 Hz world snapshot.
The relay preserves an 8 KiB send-buffer reserve for ping/ack-critical packets
by deferring periodic
`WORLD_PLAYERS`, asteroid identity/motion/state deltas, `WORLD_NPCS`,
`WORLD_TIME`, `WORLD_SCAFFOLDS`, scaffold motion, cargo pod identity/motion,
and `WORLD_INTERACTIONS` snapshots before they can fill the connection queue.
App-level `LATENCY_PONG`, `INPUT_APPLIED`, private authoritative `STATE`,
action results, joins, handoffs, compact asteroid removals, and station/econ
changes keep the normal send path. EMF logs surface this protection separately
as `TxBackpressurePackets` and `TxBackpressureBytes`.
Docked players also defer outside-world live drift: NPC motion/status, scaffold
motion, cargo pod motion, and interaction drift stay quiet until launch while
semantic identity/removal lanes continue to reconcile. This keeps station-idle
clients from adding queue pressure to the ping/input-ack lane.
Station identity is no longer treated as a short heartbeat: joins and structural
dirty events still send immediately, while the fallback reconciliation interval
is 10 seconds so large identity payloads do not compete with movement ack traffic.
High-frequency local feedback events (`DAMAGE`, mining ticks, buy/sell/repair,
launch/dock, and rejected local actions) are routed only to the owning player
instead of being broadcast in every `EVENTS` batch. Global events such as
deaths, NPC kills, module activation, station connection, and scaffold readiness
still fan out so scoreboards and shared world notices stay consistent.

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

To inspect steady-state relay payload after the join burst, run a local server
and then sample raw WebSocket traffic by message type:

```sh
# in one shell
make build-server
PORT=9091 SIGNAL_DATA_DIR=data SIGNAL_ALLOW_DEV_STATION_AUTH_SECRET=1 ./build/signal_server

# in another shell
make relay-traffic-probe RELAY_PROBE_CLIENTS=2
```

The probe sends the conservative 0.5 Hz steady app-level latency pings and 1 Hz
held input ack sequence cadence by default, ignores the warmup window, then
reports packets/sec and payload bytes/sec for streams such as
`LATENCY_PONG`, `WORLD_PLAYERS`, `WORLD_ASTEROIDS`, and `INPUT_APPLIED`.
For local scale tests that need more than four clients from `127.0.0.1`, start
the relay with `SIGNAL_TRUST_PROXY_HEADERS=1` and pass
`--spoof-forwarded-for` to `scripts/relay-traffic-probe.mjs`; this keeps the
production per-client-IP cap intact while letting the harness simulate distinct
proxied clients. `--no-session` verifies that unauthenticated upgraded sockets
receive only the lightweight open/protocol packets; the large station,
asteroid, highscore, and signal-channel snapshot bundle is held until `SESSION`
is accepted.

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
make relay-traffic-probe    # sample raw relay payload by message type
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
