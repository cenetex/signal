# Signal P2P Mesh Gap Analysis

This is the migration plan from the current authoritative WebSocket server to a
permissionless peer mesh. The goal is to remove central gameplay servers without
removing authority: authority becomes a role that a browser tab, native build, or
headless node can perform for a station or signal zone.

## Current State

- `client/net.c` speaks one reliable ordered byte stream to one authority.
- `server/main.c` owns connection management, the fixed-step sim loop,
  persistence, REST/admin endpoints, and all broadcast fanout.
- `client/local_server.c` already runs the same authoritative sim in-process for
  singleplayer.
- `CMakeLists.txt` already separates shared sim sources from client rendering,
  so a future `signal_node` can reuse the same world code without Sokol.
- Signed chain logs, station authority, player identity, and cargo receipts are
  already present. They are the hard-state substrate for mesh validation.

## First Drop-In Slice

The browser build now accepts `rtc://...` / `rtcs://...` URLs in the same
`server=` path used by WebSocket multiplayer. The WebRTC DataChannel transport
carries the same `NET_MSG_*` bytes; no gameplay packet format changes.

Example local rendezvous:

```sh
node scripts/webrtc-rendezvous.mjs --listen=127.0.0.1:19092
```

Then use:

```text
http://localhost:8080/play.html?server=rtc://127.0.0.1:19092/signal-main
```

The rendezvous process only exchanges SDP/ICE signaling. It does not inspect,
relay, validate, or author game state.

## Second Drop-In Slice

Receipt bearer parity now exists for named cargo carried by a remote player.
Station buy/deliver flows send `NET_MSG_CARGO_RECEIPT_BUNDLE` as the full
chronological chain for that cargo, and the client stores the verified bundle by
`cargo_pub` until the authoritative manifest snapshot contains the matching
unit. This does not make the current server decentralized, but it moves one
hard-state invariant out of private server memory and onto the peer that must
eventually present it during mesh handoff.

## What Is Still Missing

### Authority Role

Today the only multiplayer authority is `signal_server`. In the mesh, authority
must be explicit:

- `client` light peer: renders, predicts, submits signed input.
- `authority` peer: simulates a station/zone and signs state transitions.
- `archive` peer: stores and serves chain logs.
- `witness` peer: verifies tips/snapshots and signs attestations.
- `rendezvous` peer: helps peers connect; no gameplay authority.

### Node Identity

Players and stations have keys; transport nodes do not yet. Add a node keypair
and signed peer hello:

```text
PEER_HELLO {
  node_pubkey,
  roles,
  build_hash,
  world_id,
  station_pubkeys,
  chain_tips
}
```

### Connection Model

The current client stores one assigned player id and one connection. Mesh needs:

- a peer table keyed by node pubkey,
- role-aware routing,
- authority selection for the current signal zone,
- backpressure per peer,
- reconnect and peer replacement policy.

### Hard State Gossip

Do not consensus every sim tick. Gossip durable artifacts:

```text
CHAIN_EVENT   station-signed event header + payload
TIP           station pubkey + event count + chain tip hash
GET_EVENTS    backfill request from event id or tip hash
ATTEST        witness signature over a tip or epoch summary
```

### Soft State Snapshots

Ship/asteroid/NPC pose remains soft state. The current `NET_MSG_WORLD_*`
messages can stay as authority-to-client snapshots. Later, add:

```text
SNAPSHOT {
  authority_node,
  zone_id,
  tick,
  state_hash,
  compressed current view
}
```

Witnesses can replay signed input batches and attest to epoch outputs.

### Handoff

Cross-zone travel must be explicit and signed:

```text
HANDOFF_OUT {
  player_pubkey,
  source_zone,
  dest_zone,
  ship_state,
  cargo_manifest,
  receipt_chains,
  expires_at_tick
}
```

The destination verifies the source signature, cargo receipts, and expiry before
emitting `HANDOFF_IN`.

The current gap immediately before this is `NET_MSG_PRESENT_RECEIPT_CHAIN`: a
client can receive and retain a portable receipt chain, but a destination
authority still needs the accept/verify path for chains presented by a peer or
by another operator.

### Browser, Native, Mobile

- Browser/WASM: WebRTC DataChannels, light peer, short-lived authority for local
  sessions, cannot be a reliable public listener.
- Native desktop: can use WebRTC for browser compatibility or QUIC/libp2p-style
  native streams later.
- Headless node: long-lived authority/archive/witness backbone.
- Mobile: same as browser in practice; assume intermittent peer with WebRTC and
  battery/network constraints.

## Implementation Order

1. Keep WebSocket default and stabilize the new transport seam.
2. Keep receipt bearer state attached to clients and implement
   `NET_MSG_PRESENT_RECEIPT_CHAIN` on authority ingress.
3. Add a `signal_node` core that can run the sim without HTTP/WebSocket coupling.
4. Add node identity and `PEER_HELLO`.
5. Add chain-tip/event gossip between nodes.
6. Add signed handoff tickets.
7. Add witness attestations for epoch snapshots.
8. Add multi-authority committees only after single-authority mesh works.

The critical constraint remains: remove central servers, not authority. A mesh
without authority would make mining, cargo, ledgers, and construction forgeable.
