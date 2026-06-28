# Singleplayer / Multiplayer Duplication And Divergence

Audit date: 2026-06-23

## Summary

Signal mostly succeeds at the intended architecture: singleplayer runs an
in-process authoritative server and the client consumes serialized protocol
packets through `net.c` loopback. Core simulation mutations are shared in
`server/game_sim.c` and helpers, not reimplemented separately for SP and MP.

The remaining duplication is in the transport shell:

- client packet ingress is duplicated between `client/local_server.c` and
  `server/main.c`
- sim-event side effects are duplicated between `local_server_emit_events()`
  and `srv_dispatch_sim_event()`
- snapshot timing differs sharply between loopback and remote
- several client UI checks still treat `g.net_authority_enabled` as "remote MP",
  even though current SP loopback also sets it

## Shared Paths

These are healthy parity points.

- `client/main.c` starts local loopback via `start_local_loopback_authority()`
  when no remote server URL is present. Loopback calls `net_init_loopback()`,
  sends initial snapshots, and then the same `NetCallbacks` process remote and
  local packets.
- `client/local_server.c` routes client packets through
  `local_server_loopback_send()` and delegates actual mutations to shared
  server reducers such as `server_dispatch_input_message()`,
  `server_dispatch_signed_action_payload()`,
  `server_dispatch_handoff_request()`, and
  `server_dispatch_fracture_claim_message()`.
- Both SP and MP use `world_sim_step()` as the authoritative sim step.
- Both SP and MP use `server_emit_world_snapshot_for_player()` and
  `server_emit_private_snapshot_for_player()` for serialized world/private
  state.
- Client-side death cinematic now primarily arrives through `NET_MSG_DEATH` in
  both remote WebSocket and local loopback modes.

## Intentional Divergences

These differences appear designed rather than accidental.

- MP has WebSocket rate limiting, origin/session constraints, analytics, save
  persistence, reconnect transfer, highscore replay, and REST/operator API
  paths in `server/main.c`. SP loopback intentionally omits most of that
  production shell.
- MP broadcasts public state at coarse cadences:
  `STATE_TICK_MS=50`, `WORLD_TICK_MS=100`, and `SHIP_TICK_MS=250`.
  SP loopback emits station, world, private, contract, and signal-channel
  snapshots every local sim frame.
- MP uses per-player private-packet caches (`ws_private_packet_sink()` and
  `ws_send_if_changed()`), while SP loopback sends every private snapshot
  without cache suppression.
- MP filters dirty station identity rebroadcasts by signal range; SP loopback
  sends full station snapshots with `include_world_stations=true`.
- MP owns highscore persistence and replay from station chain logs. SP loopback
  has no highscore table in `local_server_t`.

## Risky Divergences

### 1. SP loopback did not emit `NET_MSG_INPUT_APPLIED`

Status: fixed in the current worktree. `server_emit_input_applied_if_changed()`
is now shared by MP `run_sim_ticks()` and SP `local_server_step_loopback()`, so
loopback singleplayer emits the same applied-input ACK when the authoritative
world advances a new input sequence.

MP sends `NET_MSG_INPUT_APPLIED` from `run_sim_ticks()` after the authoritative
world applies a new input sequence. The client callback
`on_remote_input_applied()` calls `net_record_input_ack()`, which updates
`g.net_last_server_ack`, ack RTT, input tick error, and net-motion counters.

Previously, SP loopback handled `NET_MSG_INPUT`, queued movement, and sent
action ACKs for one-shot actions, but never serialized
`NET_MSG_INPUT_APPLIED`.

Impact:

- loopback SP runs with `g.net_authority_enabled=true` but does not exercise the
  same input-ack path as MP
- SP net HUD/telemetry can show missing ack RTT or growing unacked input state
- action and replay diagnostics are less representative in SP than in MP

Recommended fix:

- In `local_server_step_loopback()`, mirror the MP `run_sim_ticks()` pattern:
  capture `last_input_seq` before `world_sim_step()`, then emit
  `serialize_input_applied()` when it changes for the local player.
- Add a focused loopback test or protocol-level test proving an input packet
  produces `NET_MSG_INPUT_APPLIED`.

### 2. `g.net_authority_enabled` hid SP-only ledger UI under loopback

Status: fixed in the current worktree for balance rows. Station UI now uses a
`ui_remote_authority_enabled()` predicate where it needs to distinguish remote
authority, solo loopback reads local station ledger data, and MP receives
`NET_MSG_PLAYER_KNOWN_LEDGER` as a compact per-player station/balance snapshot.

Docs say singleplayer can show a cross-station station-local ledger strip and
local-credit bridge notice, while multiplayer still needs known-ledger
snapshots.

Current code disagrees in an important way:

- `client/main.c` sets `g.net_authority_enabled` to the result of
  `start_local_loopback_authority()` for SP.
- `ui_build_ledger_strip()` returns after the current station row whenever
  `g.net_authority_enabled` is true.
- `ui_local_player_pubkey()` returns false whenever `g.net_authority_enabled`
  is true.
- `station_credit_bridge_line()` returns false whenever
  `g.net_authority_enabled` is true.

Impact:

- the intended SP ledger strip and credit-bridge notice are disabled in current
  loopback SP unless there is some older non-loopback path still in use
- the docs' "SP closed, MP open" claim is stale against current code
- MP previously lacked the wire payload for honest cross-station balance rows

Recommended fix:

- Split the predicate into "remote authority" versus "loopback authority";
  likely use `g.net_authority_enabled && !net_is_loopback()` for UI that should
  only be blocked in remote MP.
- For MP parity, keep `NET_MSG_PLAYER_KNOWN_LEDGER` in the private player
  snapshot and decode it into station UI known-ledger rows.
- Add an acceptance test around the documented flow: earn at Prospect, dock at
  Helios, and see Prospect non-zero plus Helios zero in SP; repeat in MP after
  the wire snapshot exists.

### 3. Client packet ingress policy is duplicated and can drift

`local_server_loopback_send()` and `handle_ws_message()` switch over nearly the
same message set and call many of the same shared reducers. The reducers are a
good extraction, but the policy around them is duplicated.

MP-only policy currently includes:

- pre-session allowlist via `ws_message_allowed_before_session()`
- rate limiting via `ws_rate`
- unsigned-action counters and logs
- station identity dirty flags after shipyard/order actions
- signed-action rejection logging
- legacy-save claiming
- session reconnect/live-state transfer
- player-save load by token/pubkey
- join/leave broadcasts and analytics

SP-only behavior currently includes:

- a single fixed pid (`0`)
- no pre-session message gate at the loopback shell
- immediate local credit seeding on same-pubkey/proof/session
- no legacy-save claim support
- no reconnect/live-state transfer path

Impact:

- new message types must be remembered in two switch statements
- security/session rules are not identical between loopback and remote
- SP may fail to exercise MP-only state dirtying, analytics, persistence, and
  reconnect behavior

Recommended fix:

- Extract a shared `server_dispatch_client_packet()` shell that accepts a policy
  struct/callbacks for transport-specific side effects:
  `send_packet`, `broadcast_join_leave`, `mark_station_identity_dirty`,
  `record_analytics`, `load/save identity`, and `reject/log`.
- Keep production-only behavior in callbacks, but keep the message allowlist,
  reducer dispatch, and action ACK/result emission shape shared.

### 4. Sim-event side effects were not dispatched through one shared event bus

Status: partially fixed in the current worktree. MP and loopback now call
`server_process_sim_event_transport()` with transport hooks, and the shared
`server_sim_event_effects()` bucket table is covered by a protocol regression
test. Generic event serialization, pending action results, and fracture updates
are still emitted by each transport's frame loop, and MP-only persistence/
highscore side effects remain MP hooks.

Both SP and MP serialize the generic event stream with `serialize_events()`.
Only selected events get extra transport-specific side effects:

- SP `local_server_emit_events()` special-cases `SIM_EVENT_HAIL_RESPONSE` and
  local `SIM_EVENT_DEATH`.
- MP `srv_dispatch_sim_event()` handles outpost placement, buy/sell/repair/
  upgrade/dock/launch, death, contract complete, hail response, module/outpost
  activation, scaffold readiness, highscore/chain-log death side effects, and
  dirty station flags.

Some MP side effects are unnecessary in SP because SP sends full snapshots every
frame. Others are gameplay-visible or persistence-visible and are simply absent
from SP, most notably highscore projection/broadcast and death chain-log
append.

Impact:

- adding a new sim event requires updating at least client handling, MP event
  side effects, and possibly SP loopback side effects
- SP can miss persistence/broadcast consequences that are not needed for local
  rendering but are still part of "server truth"

Recommended fix:

- Introduce a shared `server_process_sim_events(world, transport_hooks)` helper.
- The helper should emit generic events, pending action results, fracture
  updates, and typed event side effects via hooks.
- SP hooks can no-op analytics/persistence/highscore if intentionally local, but
  the missing behavior becomes explicit.

### 5. Snapshot cadence and dirty-state behavior can mask MP bugs in SP

SP loopback emits all relevant snapshots every frame. MP emits public/private
snapshots at 20 Hz / 10 Hz / 4 Hz and relies on dirty flags plus cache
invalidation for immediate correctness.

Impact:

- SP can look correct even when an MP dirty flag is missing
- MP can stay stale until a fallback cadence or reconnect, while SP updates
  immediately
- tests that only use `world_sim_step()` or loopback-style snapshots do not
  prove MP broadcast freshness

Recommended fix:

- Add parity tests at the serializer/broadcast-helper layer for events that must
  refresh MP clients immediately: shipyard orders, station manifest changes,
  module activation, outpost placement, hail credit changes, death/respawn, and
  delivery ledger changes.
- Prefer dirty-flag assertions over visual/client-only assertions where
  possible.

### 6. Documentation still describes an older SP architecture

`docs/engineering-report-2026-06-09.md` says singleplayer "memcpy-mirrors the
world into the client each frame." Current `client/local_server.c` explicitly
says direct copies are legacy and sends serialized protocol packets via
loopback.

Impact:

- future audits may reason from stale architecture
- SP-only UI decisions can accidentally key off `g.net_authority_enabled`
  because the docs imply "network authority" means MP

Recommended fix:

- Update architecture docs to say: singleplayer is loopback network authority,
  not direct world mirroring.
- Name the predicates explicitly:
  - `network authority`: remote or loopback authoritative server
  - `remote multiplayer`: non-loopback WebSocket/WebRTC authority
  - `solo loopback`: local authoritative server using protocol packets

## Suggested Priority

1. Emit `NET_MSG_INPUT_APPLIED` from loopback SP.
2. Fix the ledger predicate split so loopback SP can show documented
   cross-station balances again.
3. Add MP known-ledger snapshot parity.
4. Extract shared client-packet dispatch policy.
5. Extract shared sim-event transport side-effect dispatch.
6. Add MP dirty-state freshness tests.
7. Refresh stale architecture docs after the code-level predicates are settled.

Current follow-up status:

- Done: loopback SP `NET_MSG_INPUT_APPLIED` emission.
- Done: loopback-vs-remote ledger predicate split for SP ledger UI.
- Done: MP known-ledger snapshot parity for station-local balance rows.
- Done: shared sim-event transport hook routing plus freshness-bucket test.
- Done: stale singleplayer architecture docs refreshed.
- Still open: shared client-packet dispatch extraction, deeper event side-effect
  parity for persistence/highscores, and broader MP dirty-state freshness tests.
