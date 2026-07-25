# Protocol And Telemetry Streams

Signal's wire protocol is split into static identity, live diagnostics, economy
snapshots, per-player state, and authority/provenance streams. New clients and
external tools should discover stream sizes from the server instead of
hardcoding constants.

## Discovery

On every websocket connection the server sends `NET_MSG_PROTOCOL_INFO` before
the large world snapshots.

Wire layout:

```text
[type:1=0x41][version:u16][capabilities:u32][stream_count:1]
  stream_count x
    [msg:1][class:1][flags:u16]
    [header_size:u16][record_size:u16]
    [max_records:u16][cadence_ms:u16]
```

The same data is available over HTTP:

```sh
curl http://127.0.0.1:8080/api/protocol
```

Use this as a compatibility check for live-brain, replay, and telemetry tools.
If a stream's advertised `record_size`, `header_size`, or `version` differs
from what the tool expects, fail fast and report the mismatch.

For the repo's baseline compatibility check against a local relay:

```sh
make protocol-check
```

Override `PROTOCOL_CHECK_URL` when checking a non-default local relay or a
staging endpoint.

## Stream Classes

- `static`: identity/config snapshots such as `NET_MSG_STATION_IDENTITY`.
- `live`: high-cadence telemetry such as `NET_MSG_WORLD_PLAYERS` and
  `NET_MSG_STATION_DIAG`.
- `econ`: station inventory, manifests, and contracts.
- `player`: private per-player state such as `NET_MSG_PLAYER_SHIP` and
  `NET_MSG_PLAYER_MANIFEST`.
- `event`: event or append-log streams.
- `auth`: authority/provenance streams such as cargo receipt presentation.

## Current Split

- `NET_MSG_STATION_IDENTITY` is static station identity: layout, prices, text,
  services, and station pubkey. It is dirty-driven with a fallback refresh.
- `NET_MSG_STATION_DIAG` is live module flow state. It carries one byte per
  station module and is rate-limited independently from identity.
- `NET_MSG_WORLD_STATIONS` and the atomic summary/detail
  `NET_MSG_STATION_MANIFEST` are economy/stock streams.
- `NET_MSG_WORLD_PLAYERS`, `NET_MSG_PLAYER_SHIP`, `NET_MSG_PLAYER_MANIFEST`,
  and `NET_MSG_INSPECT_SNAPSHOT` are player/scan streams with their own
  cadence.

That split is intentional. Future telemetry should add or extend small streams
instead of appending live state to static identity packets.
