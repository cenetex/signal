# Signal — Operator Onboarding

You have decided to host a Signal station. This document is the practical
recipe-style guide. For the architectural overview — what a station actually is,
why each piece of the identity stack exists, what trust assumptions remain —
read [`decentralization.md`](./decentralization.md) first.

The off-chain federation stack is shipped through Layer F. The cross-operator
handshake (different operators verifying each other's chain logs at the zone
boundary) relies on the shipped Layer D cargo receipt chain for off-chain cargo
settlement. Multiplayer clients now receive full receipt chains for named cargo
they carry, which is the bearer-state shape the federation handoff will use.
Before queued sell/deliver actions, the client presents matching verified
chains; the server-side `NET_MSG_PRESENT_RECEIPT_CHAIN` ingress path verifies
those peer-presented chains and attaches them to carried cargo. The signed
handoff ticket primitive can bind those proofs to a ship snapshot; the remaining
cross-operator product gap is wiring ticket issue/present/accept into actual
zone traversal.
Byzantine fork resistance still depends on the future on-chain anchor in #480.
Where this guide says "post-#480", it means you can run the station today, but
Byzantine fork resistance only becomes real once the anchor lands.

## What you're signing up for

Operating a Signal station is, today, the same as operating any small game
server: keep the process up, keep the disk healthy, keep an eye on the logs.
Federation adds a few cryptographic responsibilities on top.

- **Custody of the station's private key.** For a seeded station the key is
  rederivable from the station authority secret plus the world seed. For an
  outpost you operate, custody is the same authority secret plus the founding
  event stored in the save. Treat both the authority secret and world save
  like production state.
- **Continuous chain log integrity.** Your station's chain log is the
  authoritative history of every state mutation it authored. Don't truncate
  it. Don't edit it. Don't replace it from a backup unless you also revert
  the save to match — `chain_last_hash` and the on-disk tail must agree or
  the next emit will be rejected (the verifier will reject it too).
- **Periodic verification.** Run `signal_verify` or the in-process verifier on
  every server start to catch bit rot and partial writes. The walker is cheap;
  running it on boot is the right default.
- **Anchoring the chain tip (post-#480).** When the on-chain anchor lands,
  you'll periodically post your chain-tip hash to a public ledger so other
  operators can detect a fork. Until then, the only fork defense is "the
  community is small and operators are known."
- **Responding to fork claims (post-#480).** If another operator or a
  player presents a chain log fragment they say is yours but doesn't match
  your local log, you have to respond. The on-chain anchor will be the
  resolving authority.

## Hardware / cloud requirements

These are loose estimates for the current sim.

- **CPU.** The sim is fixed-step at 120 Hz and is comfortably single-core
  bound. Anything modern (1 vCPU on a small cloud instance) is plenty for
  the seeded station count and a handful of players.
- **Memory.** A few hundred MB for the server process itself; the sim
  state, the world snapshot, and the Mongoose websocket layer dominate.
  512 MB is comfortable, 1 GB is generous.
- **Disk for chain logs.** Each event is 184 bytes of header plus a small
  payload (typically tens of bytes) plus the `uint16` payload length. Even
  at a busy 10 events/sec, a station accumulates roughly 2 MB/hour of
  chain log — under 50 MB/day per station. A 10 GB volume will last a
  long time. Don't co-locate the chain log directory with anything that
  might rotate or `truncate` it.
- **Disk for saves.** The world save is one file (`world.sav`); player
  saves live under `saves/pubkey/<base58(pubkey)>.sav` (and the legacy
  fallback `saves/legacy/<token_hex>.sav`). Both grow slowly and cap at
  small sizes. See `/CLAUDE.md` "Save layout" for the canonical
  description.
- **Network.** Mongoose's websocket layer; a few hundred kbps per
  connected client is plenty. There are no large asset downloads —
  geometry is procedural; only avatars, episodes, and music are assets,
  and they ship with the client.
- **OS.** Linux, macOS, and Windows are all supported. Production
  instances are Linux (the Dockerfile and `task-definition.json` in
  [`server/`](../server/) target Fargate).

## Stand up your first station

The full path is: pick a name, generate a keypair, decide which station-index
slot you want it in (or plant an outpost), boot the server, verify the chain
log is being written, and verify it parses cleanly.

### 1. Pick a station name

Free-form, max 16 characters (the outpost-seed derivation truncates/pads to
exactly 16 bytes for hashing — see
[`server/station_authority.h`](../server/station_authority.h)). The name
shows up in the HUD and is part of the outpost's identity hash, so changing it
later changes the keypair. Pick something you can live with.

### 2. Generate a station keypair

The keypair is derived deterministically from a 32-byte seed via
`signal_crypto_keypair_from_seed` ([`shared/signal_crypto.h`](../shared/signal_crypto.h)).
The seed itself comes from one of two recipes. Both include the configured
station authority secret (`SIGNAL_STATION_AUTH_SECRET`, or `SIGNAL_API_TOKEN`
as a fallback) so the public world seed alone is not enough to reproduce a
station's private key:

- **Seeded slot (you operate one of indices 0/1/2).**
  `seed = SHA256("signal-station-v1" || operator_secret || world_seed_u32 || station_index_u32)`.
  Every server with the same operator secret and world seed agrees on this
  seeded station's keypair. The helper is `station_authority_seeded_seed`
  and the bootstrap is
  `station_authority_init_seeded`
  ([`server/station_authority.h`](../server/station_authority.h)).
- **Outpost (indices 3+).**
  `seed = SHA256("signal-outpost-v1" || operator_secret || founder_pub[32] || station_name[16] || planted_tick_u64)`.
  The founder is the player who planted the outpost, the name is the
  station name, and the planted tick is `world.time * 120` at the moment
  of planting. The helper is `station_authority_outpost_seed` and the
  bootstrap is `station_authority_init_outpost`. Both founder and tick
  are stamped onto `s->outpost_founder_pubkey` and
  `s->outpost_planted_tick` so the secret can be rederived on save load.

For stations you operate yourself, keep the same station authority secret
available across restarts and replicas. Changing it intentionally rekeys
stations; on load, the server preserves the saved public key as a
trusted-rotated identity and starts a fresh live chain identity under the newly
derived current key. The public registry is bounded and persisted with the
world save. Explicitly untrusted or revoked keys are never reactivated: a
configured secret that would derive one causes the load to fail closed.
The registry retains at most eight public keys. When it is full, the oldest
still-trusted rotated row may be evicted; explicit untrusted/revoked rows are
never discarded to make room. If every historical slot is an explicit deny
decision, another rekey is rejected until an operator resolves capacity
deliberately.

The private key is never written to disk and never sent over the wire. Layer B
keeps `station_secret` as the last field of `station_t` and re-derives it on
load via `station_authority_rederive_secret`
([`server/station_authority.h`](../server/station_authority.h)).
Only public keys and their independent lifecycle/trust decisions are saved. A
v76 or earlier world synthesizes one current record from each station's saved
pubkey; it does not invent historical trust. Historical chain-log files remain
addressable by those preserved public identities after a rekey.
An operator may also record an unknown public key as deny-only untrusted or
revoked policy. Such a row deliberately keeps its event lifecycle unspecified:
without a verified historical log, the server will not fabricate an origin
proof merely because a deny decision exists.

### 3. Wire your station's pubkey into the world

The current world bootstrap derives all three seeded stations' pubkeys
deterministically from `w->belt_seed` (which is `w->rng`, defaulting to
`2037u`) — see [`server/game_sim.c`](../server/game_sim.c). There is no
external `world_seed.json`; the world seed is the integer baked into the world
on `world_reset`.

If you are running one of the three seeded stations on a fresh world, no
extra wiring is required: the pubkey is already what you derived in step 2
because both you and the server agreed on the seed.

If you are planting an outpost, the founding event itself is the wiring:
`station_authority_init_outpost` runs at plant time, stamps the founder + tick
onto the station record, and the chain log emitted on plant carries the new
pubkey forward. Subsequent server starts rederive the secret from the station
authority secret plus the saved provenance.

If you are joining an *existing* world as a second operator running an
*additional* seeded slot, the world seed and station authority secret must be
agreed upon before launch. Coordinate them with the existing operator out of
band; once both servers boot with the same values, the seeded-station pubkeys
match by construction.

### 4. Run the server

Build:

```sh
cmake -S . -B build
cmake --build build
```

Run:

```sh
./build/signal_server
```

Relevant environment variables (read in [`server/main.c`](../server/main.c)):

- `PORT` — TCP port to bind. Defaults to `8080` when unset. The root
  Docker-compose dev flow sets this to `9091` because the same container also
  serves the static web client on host port `8080`.
- `SIGNAL_API_TOKEN` — bearer token for `/api/station/<id>/command` and other
  admin REST surfaces. If `SIGNAL_STATION_AUTH_SECRET` is unset, this token is
  also used as the station authority secret fallback.
- `SIGNAL_STATION_AUTH_SECRET` — operator-held secret mixed into every station
  keypair derivation. Server startup requires this or `SIGNAL_API_TOKEN`.
- `SIGNAL_ALLOW_DEV_STATION_AUTH_SECRET=1` — explicitly permits the public,
  deterministic development secret for disposable local worlds. Never set
  this in a deployed environment.
- `SIGNAL_REQUIRE_STATION_AUTH_SECRET` — explicit production guard used by the
  Fly configuration. When truthy, the development-secret override is refused.
- `SIGNAL_REQUIRE_API_TOKEN` — when set, refuses admin requests that don't
  present `SIGNAL_API_TOKEN`; startup fails if this is set without a token.
- `SIGNAL_INTERNAL_SHARED_KEY` — bearer for `/internal/v1/operator-post`.
  Without it, that internal endpoint rejects all requests. The public
  station-command workflow below uses `SIGNAL_API_TOKEN` instead.
- `SIGNAL_ALLOWED_ORIGIN` — CORS allowlist for the websocket upgrade.
- `SIGNAL_FRONTIER_VIRTUAL_PILOTS` — aggregate strategic pilot count for
  frontier expansion. This does not consume `MAX_PLAYERS` or `MAX_NPC_SHIPS`;
  it lets large simulated populations plan outposts and order relay scaffolds.

The server creates `chain/` on its first emit and writes per-station log
files into it. It creates `saves/pubkey/` and `saves/legacy/` lazily
when the first save needs to be written.

### 5. Sync station avatar content

Station copy is operational state, not a client-side cosmetic override. The
supported flow is:

1. Read the station state from `/api/station/<id>/state?include=activity_history,chain_history`.
2. Ask the station avatar model for a MOTD, RATi-grade delivery hail, and
   worker chatter lines for mining and hauling assignments.
3. Post the results back through `/api/station/<id>/command`.
4. Let the server emit `CHAIN_EVT_OPERATOR_POST`, materialize the fields into
   `station_t`, persist them in the station catalog, and rebroadcast station
   identity to clients.

Relationship rows never expose reconnect/session credentials. Current,
proof-resolved players appear as `actor_kind:"derived"` with a
domain-separated `actor_id`; token-keyed or otherwise unresolved historical
rows appear as `actor_kind:"legacy-unattributed"` with `actor_id:null`.
`activity_history.top_haulers` uses the same object shape. Treat these values
as public presentation identifiers, not authorization material.

When `chain_history` is included, `chain_history.route_history_aggregate[]`
exposes compact cross-station route-memory groups built from recent signed
route-history rows. Each aggregate includes route/action labels, signed row
count, total receipt evidence, peak confidence/salience, and latest observed
tick. `chain_history.route_history[]` still exposes the requested station's
recent signed route-history tail. Treat both arrays as station-authored context
for copy and operations; they are not payout, inventory, or settlement
authority.

Add `history_filter=outbound`, `history_filter=inbound`, or
`history_filter=local` to narrow the aggregate view for the requested station.
`all` is the default; `local` leaves `route_history_aggregate[]` empty and uses
the station-local signed `route_history[]` tail as the drill-down surface.

Use the helper script:

```sh
SWARM_API_KEY=... \
SIGNAL_API_TOKEN=... \
SIGNAL_SERVER_API=https://your-signal-server.example \
scripts/sync-station-operator-content.py --stations prospect,kepler,helios
```

Optional knobs:

- `SWARM_API_BASE` — defaults to `https://swarm.rati.chat/api/v1`.
- `--dry-run` — fetches station state and generates content, but does not
  post it back to the server.
- `--stations prospect,kepler,helios` — limits which starter avatar models
  are called.

The station command API accepts:

- `{"action":"set_hail","hail":"..."}` for the station MOTD/hail text.
- `{"action":"set_miner_chatter","slot":0,"message":"..."}` for worker
  chatter while the economy has assigned that worker to mining slots `0..7`.
- `{"action":"set_hauler_chatter","slot":0,"message":"..."}` for worker
  chatter while the economy has assigned that worker to hauling slots `0..7`.
- `{"action":"set_rati_hail","message":"..."}` for player-facing hail after
  RATi-grade ore delivery.
- `{"action":"set_currency_name","currency_name":"..."}` for the local
  station-currency label.
- `{"action":"set_price","commodity":0,"price":125}` for a station-local
  commodity price override. Server-side bounds reject non-finite, negative, or
  extreme values.
- `{"action":"build_module","module_type":2}` for admin/test construction of a
  station module using the server's placement rules.

Text/content commands return `audited:true` plus the signed station-chain
`event_id`. Currency, price, and admin construction commands return
`audited:false`; they mutate live/catalog station state, but they are not
station-chain operator posts. The lower-level `/internal/v1/operator-post`
endpoint still exists for internal services that already choose an
operator-post kind directly; known kinds are materialized into the same live
station fields.

### 6. Verify your chain log

Once the server has been up long enough to have authored a few state
mutations (smelt one fragment, transfer/sell a finished good, plant an outpost
— anything that emits a `CHAIN_EVT_*`), confirm the log is present and verifies.

```sh
./build/signal_verify chain/<base58(your_pubkey)>.log
```

`signal_verify` derives the station pubkey from the `<base58>.log` filename.
If you are checking a renamed file, pass
`--station-pubkey=<base58(station_pubkey)>` explicitly.

The same walker is callable from any C tool that links
[`server/chain_log.c`](../server/chain_log.c); call `chain_log_verify` with the
station record and check that it returns `true` and that `out_event_count`
matches the in-memory `s->chain_event_count`.

The running server also exposes chain health through `/health` and
`/api/station/<id>/state`. A healthy station reports `chain_health` as `ok` or
`empty` and `chain_append_blocked:false`. A station reporting `failed` or
`mismatch` will refuse new chain events until the save/log pairing is repaired.
For a readable local diagnosis, run:

```sh
scripts/chain-doctor.py --url http://127.0.0.1:9091/health
```

The doctor is read-only. It prints each station's verifier message and the
server-authored repair hint, then reminds the operator to preserve `world.sav`
and `chain/` before making manual repairs.

## Operational hygiene

- **Backup the world save and station authority secret, not per-station
  keypairs.** For seeded stations the keypair is rederivable from
  `SIGNAL_STATION_AUTH_SECRET` (or the `SIGNAL_API_TOKEN` fallback) plus the
  world seed. For outposts the founder pubkey + name + planted tick live in
  the world.sav; back up the save and the same authority secret.
- **Monitor disk for chain log growth.** Expect ~2 MB/hour at busy times
  per station. If it grows much faster, something is emitting more than
  it should — investigate before it eats the volume.
- **Verify on every server start.** Run `chain_log_verify` or standalone
  `signal_verify` before serving any clients. The server also performs this
  walk during `world_load()` and publishes the result in `/health`. A failed
  verification or save/log mismatch is a real signal; the station will block
  future appends instead of silently forking the log.
- **Don't truncate, edit, or replace chain logs in place.** They are
  append-only by contract. `world_reset()` intentionally does not delete chain
  files because normal load paths reset memory before the saved belt seed is
  known. If you are intentionally starting a new world, archive the old
  `world.sav` and chain directory together, then boot a fresh world so the new
  seed produces new station pubkeys and new log filenames.
- **Keep the server's clock sane.** Events have monotonic `event_id`
  per station and timestamps from `world.time`. A backwards clock jump
  between restarts is mostly harmless because `event_id` increments
  regardless, but it makes log analysis annoying. Run NTP.
- **Don't co-locate the chain log directory with rotators.** A logrotate
  rule that gzips and renames `chain/<pubkey>.log` will silently break
  the chain. The directory is *data*, not *logs* in the syslog sense.

## Federation handshake (forward-looking)

When a second operator joins the federation:

1. They generate a station keypair via the recipe in step 2 above.
2. They publish their pubkey out of band (the world seed change, or a
   federation manifest at the on-chain anchor post-#480).
3. They run their own server, with their own chain log directory, on
   their own infrastructure.
4. Each operator periodically anchors their chain-tip hash to the public
   ledger (post-#480). The anchor is the resolving authority for fork
   claims.
5. Cargo crossing the zone boundary carries a chain of signed transfer
   receipts. The destination station verifies the chain before accepting the
   unit.

The current multiplayer server already sends full receipt chains back to the
client for named cargo transfers, clients present matching chains before
sell/deliver actions, and destination authorities can verify and attach a
presented chain from a peer or foreign operator. The shared handoff ticket
format can bind those proofs to cross-zone ship state; the remaining federation
gap is consuming that ticket during an actual authority handoff.

Until #480 lands, federation is "informal" against Byzantine operators. The
off-chain receipt chain can prove cargo history, but without a public
chain-tip anchor an operator can still fork their own history for different
audiences. Plan accordingly.

## What if you mess up

A non-exhaustive list of recoverable failure modes.

### Lost the keypair

For a **seeded station**: regenerate from the station authority secret plus the
world seed. The seeded path is deterministic. Run the same secret and world
seed, and the same pubkey + secret falls out of `station_authority_init_seeded`.

For an **outpost**: if you have the world.sav and station authority secret, the
station secret is rederivable from that secret plus `outpost_founder_pubkey`,
station name, and `outpost_planted_tick`, all of which are persisted. Boot the
server against the save and the secret is rederived
automatically. If you have lost both the save and the founding event,
the outpost's identity is gone — start fresh by planting a new outpost.

### Server crash mid-event

The chain log emitter writes header + payload-length + payload then `fflush`
and closes ([`server/chain_log.c`](../server/chain_log.c)). The disk may
contain a partial last entry. The verifier will walk up to the last good
entry and report the count, but the server treats the station as
`CHAIN_HEALTH_FAILED` and blocks future appends.

A fully valid on-disk tail can also be ahead of the selected world generation
when the process dies after the durable append but before its gameplay mutation
and the next save. The server does not adopt that tail: advancing only the
saved continuation pointer could preserve an event while losing its cargo,
credit, or construction effect. It reports `CHAIN_HEALTH_MISMATCH`, leaves the
log untouched, and blocks appends. Restore a matching save/log pair or preserve
both for an exactly-once replay/rollback repair. If verification itself fails,
archive the damaged chain for investigation before starting a new station
identity.

### `world.sav` corruption

The chain log is the source of truth for state mutations. `world.sav` is a
derived snapshot. Replay the chain log to rebuild a fresh `world.sav`:
construct a fresh world, walk the per-station chain logs, and apply each
event in `(epoch, event_id)` order. The verifier and the chain-log walker
are the building blocks; full replay is not yet a one-shot CLI but the
primitives are present in [`server/chain_log.c`](../server/chain_log.c).
For deterministic seed+prefix counterfactual rollouts, use
[`signal_replay`](./replay-harness.md); it is a simulator harness, not a
chain-log recovery tool.

### Time desync

`event_id` is monotonic per `(station, epoch)`, so a backwards wall-clock
jump between restarts does not directly violate any chain invariant. What
*does* break is `epoch`: `epoch_ticks = world.time * 120` at emit. If the
saved `world.time` jumps backward between restarts, audit tools that group
events by epoch will see weird ordering. Run NTP and don't restore old
saves into a running federation; if you must restore, do it everywhere
simultaneously.

### Save corruption on `saves/pubkey/<...>.sav`

A corrupted player save affects only that player. The old arbitrary-basename
legacy claim flow is disabled and must not be used as a restore mechanism: it
did not prove ownership of the named token-keyed save. For a pubkey-keyed
save, restore from backup and accept that the player loses any progress
between the backup and the corruption.

For a canonical token-keyed save, the server may issue one opaque,
short-lived recovery offer after session and pubkey proof. There is no
operator-supplied filename and no namespace listing. Do not rename or copy a
legacy file into the pubkey namespace by hand: recovery publishes a complete
world/player generation and uses atomic no-replace semantics. The recovery
client prompt requires a fresh `ENTER` to confirm; `ESC` leaves the remote
source untouched and closes the provisional connection. The later docked UI
work in #658 expands that bootstrap prompt but does not change its opaque
signed-action boundary. The recovery
generation carries a manifest-authenticated consumption marker and
deliberately has no automatic fallback edge to the source-bearing generation;
if that new generation is damaged, restore it from backup rather than
re-enabling the consumed token save. A
`migration-failure` accompanied by unresolved ownership-quarantine diagnostics
is intentionally fail-closed; v81 discarded the bearer token needed to
attribute those historical rows safely. See
[`legacy-save-recovery.md`](legacy-save-recovery.md).

## Troubleshooting

A handful of concrete debug recipes for the failure modes you'll actually
hit.

### "My chain log is empty"

The station may not have authored anything yet. Smelt one fragment or transfer
a finished good — anything that flips a `CHAIN_EVT_*`. If the file is still
absent after that, check that the `chain/` directory exists and is
writable; `chain_log_emit` calls `ensure_chain_dir()` on each emit but
fails the emit if the `mkdir` fails. Look for `[chain] mkdir` warnings in
the server log.

### "The verifier reports event_count = 0 with no error"

Either the log file is missing (verifier returns `true` with zero events
for a missing log — the empty chain is trivially valid), or the file is
present but empty. Check the file's size; if it is a multiple of zero,
no event has been emitted. If the file is partially written but no full
header has been written, the verifier returns zero events.

### "Verifier reports `bad signature`"

The asserted `authority` pubkey on the failing event does not match the
station's `station_pubkey`. This usually means the chain log was authored
by a *different* keypair — for example, the station authority secret or world
seed changed between the run that authored the log and the current run, or you
replaced an outpost's founding event. Identify the run that authored the log
and restore the station authority secret plus world seed or founding event it
expects.

### "Verifier reports `bad prev-hash linkage`"

The on-disk log is internally broken: an event's `prev_hash` does not match the
hash of the previous event header. Do not continue writing to that file. The
server will mark the station `failed` and block appends. Restore a matching
backup or preserve the damaged log for audit and start a new chain identity.

### "Server refuses to emit; chain appends blocked"

Check `/health` for the station's `chain.health`, `append_blocked`, and
`message`, or run `scripts/chain-doctor.py`. `failed` means signature, payload,
or linkage verification failed. `mismatch` means the saved continuation pointer
and verified disk tail disagree. In both cases the policy is intentional:
continuing would create a fork that looks valid from the new head but loses
continuity with the permanent history.

### "Server refuses to emit; SIM_LOG says self-verify failed"

`chain_log_emit` runs a self-verify on the freshly-signed header before
writing it to disk ([`server/chain_log.c`](../server/chain_log.c)).
A failure here means the secret slot was zero or authority rederivation did
not complete. Check that the world load called
`station_authority_rederive_secret` for every station and that the authority
secret was configured before `world_reset` or `world_load`. A
`STATION_AUTHORITY_REDERIVE_REJECTED` result is intentional fail-closed
behavior: the derived key was explicitly untrusted/revoked, the saved public
registry was malformed, or its bounded capacity could not preserve a deny
decision. For outposts, the founder + tick must have been loaded from the save.

### "Disk is filling up faster than expected"

Chain logs grow append-only by design. Profile what's emitting: sort
events by `type` from a recent log slice and see whether one event type
dominates unexpectedly (a runaway `CHAIN_EVT_LEDGER` is a common
suspect). Investigate the producer before reaching for any kind of log
pruning — the chain is the audit trail and pruning it without an
on-chain anchor in place destroys the audit.

## Reading list

- [`docs/decentralization.md`](./decentralization.md) — architectural
  overview. Read first if you haven't.
- Issue #479 — the umbrella for off-chain decentralization. Layers A
  through F are tracked there.
- Issue #480 — on-chain anchoring, wrap, and bounty. The forward-looking
  half of federation.
- Issue #496 — substrate-attached player birth (Layer A.5 of #479).
- [`/CLAUDE.md`](../CLAUDE.md) — repo-level architecture notes,
  authoritative for build commands, save layout, and economy invariants.
- [`server/chain_log.h`](../server/chain_log.h) — schema and verifier.
- [`server/station_authority.h`](../server/station_authority.h) — keypair
  derivation.
- [`shared/signal_crypto.h`](../shared/signal_crypto.h) — Ed25519 surface.
- `signal_verify --help` — standalone verifier CLI.
