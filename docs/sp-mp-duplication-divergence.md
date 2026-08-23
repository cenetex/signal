# Singleplayer / Multiplayer Parity

Audit updated: 2026-08-22

## Result

Singleplayer and multiplayer now use the same gameplay path where the player
can see or affect the result:

- the same authoritative simulation in `server/game_sim.c`
- the same gameplay packet dispatcher in `server/net_protocol.h`
- the same snapshot serializers and client packet decoders
- the same packet-driven interpolation for ships, asteroids, cargo pods,
  scaffolds, NPCs, and interactions
- matching 20 Hz player, 10 Hz world, and 4 Hz private snapshot cadences

Singleplayer runs a separate in-process server world. It does not share the
client's presentation objects. Packets still pass through the loopback network
adapter, including serialization and decoding.

## Parity Fixes

### Disconnects preserve the selected mode

A lost multiplayer connection no longer starts a fresh singleplayer universe.
The client stays in remote mode, keeps the last visible state, and offers a
reconnect. This removes the apparent lag-and-reset failure that could happen
after a transport drop.

### Asteroids use one presentation path

Singleplayer no longer replaces decoded asteroid poses with direct reads from
the in-process authority. Both modes render the packet stream and use the same
prediction and correction code. Towing bugs can no longer be hidden by a
singleplayer-only 120 Hz pose shortcut.

### Private player knowledge uses packets in both modes

Market memories are rebuilt from `NET_MSG_PLAYER_MARKET_MEMORIES` in both
modes. The old loopback exception assumed that the client and local authority
shared one ship object, which is no longer true.

### Gameplay packet mutation is shared

The common dispatcher owns movement input, one-shot action ACK/result handling,
plans, old cargo messages, receipt presentation, handoff, and fracture claims.
The WebSocket and loopback shells now provide only their transport-specific
callbacks, such as sending a packet, logging a rejection, or marking a station
dirty.

Identity setup, latency probes, telemetry, signed legacy-save recovery, and
production session policy remain in their transport shells. Their state
reducers are still shared. These are transport and persistence concerns, not
separate gameplay rules.

### Event-driven snapshots refresh in both modes

Both servers route simulation events through the shared event-effect table.
The loopback server now marks private, station economy, station identity, and
global snapshots dirty for the same visible event classes used remotely,
including outpost placement and contract completion. Normal cadence remains a
safety fallback.

### Earlier parity work retained

- loopback sends `NET_MSG_INPUT_APPLIED`, so prediction and ACK diagnostics use
  the same path as multiplayer
- both modes receive known station-ledger rows through the private snapshot
- UI code distinguishes remote authority from local loopback where that
  distinction is actually needed
- reset/self-destruct is documented as available in both modes

## Intentional Differences

The following differences are part of the product boundary and should not be
silently copied between modes:

- Multiplayer has real network delay, rate limits, origin checks, reconnect
  transfer, multiple players, and server operations APIs. Loopback does not.
- Multiplayer persists its world, player identities, chain logs, and
  highscores. A normal singleplayer launch starts a fresh local session.
- Multiplayer filters and caches outgoing data per connected player.
  Singleplayer has one recipient, but still uses bounded packet cadences and
  the same semantic snapshot caches.
- Durable legacy-save takeover exists only on the production server. Loopback
  returns a bounded no-match result rather than touching remote save storage.

Adding persistent singleplayer saves would be a separate product feature. It
needs an explicit save location, migration policy, reset flow, and ownership
rules; it is not safe to introduce as an automatic multiplayer parity fix.

## Regression Coverage

- Native protocol coverage checks the shared gameplay dispatcher, including
  action ACKs, dirty station routing, unsigned-action rejection, forced
  resync, malformed known packets, and transport-owned packets.
- Shared event-effect tests cover player state, death, contract completion,
  hail response, outpost placement, and structure changes.
- Browser coverage requires singleplayer asteroid rendering to keep the local
  authority pose bypass disabled.
- The full native suite exercises the same reducers and serializers used by
  both server shells.
