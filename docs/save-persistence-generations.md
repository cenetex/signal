# Crash-consistent save generations

Issue #666 Phase 1 replaces the dedicated server's independently published
world, catalog, and player files with one committed snapshot envelope.

## Commit protocol

The server writes `.signal-generations/generation-N/` as a new immutable
directory. A generation contains:

- `world.sav`
- `stations/*.cat`
- `players/legacy/*.sav` and `players/pubkey/*.sav`
- `MANIFEST`

Before writing current live players, the writer copies the complete player
namespace from the last validated generation. On the first commit only, it
imports the legacy `saves/` namespace. This preserves inactive accounts across
generations.

Every manifest entry contains the canonical relative path, byte size, and
SHA-256 of one artifact. The manifest is written and synced after all
artifacts. `CURRENT` is a fixed, checksummed marker containing the new
generation and its manifest hash plus the previously published generation and
its manifest hash. It is written to `CURRENT.tmp`, synced, atomically renamed,
and its parent directory is synced last.

Generation IDs are monotonically allocated above every directory already
present, including unpublished crash debris. An incomplete directory is never
reused and is never selected for recovery.

## Recovery and compatibility

Startup validates the marker, required directory structure, exact artifact
set, manifest hash, and every artifact hash before loading anything. World,
catalog, and later player reconnects all use paths from that one resolution.

If the current generation is damaged, recovery may use only the previous
generation authenticated by the published `CURRENT` marker. It never scans for
the highest valid-looking directory, because that directory may have been
written completely but crashed before publication. If neither member of the
published lineage validates, startup fails closed.

When no `CURRENT` marker exists, the server loads the historical `world.sav`,
`stations/`, and `saves/` layout. The first successful save imports that player
namespace and publishes generation 1. Once a marker exists, legacy files are
not consulted.

Disconnect and published-authentication-failure saves request a complete
generation commit. They no longer mutate a player file inside either the
legacy layout or an immutable generation. A failed attempt leaves `CURRENT`
and the runtime reconnect path on the prior generation and retries at a
bounded cadence.

## Deliberately remaining #666 work

This phase is synchronous. Catalog, world, player, manifest, and directory
fsync work still runs on the simulation thread; it does **not** claim the
bounded asynchronous writer, dirty-only serialization, tick-latency proof, or
shutdown writer-drain acceptance criteria.

Chain logs remain outside the generation envelope. The world snapshot contains
its persisted chain heads, but Phase 1 does not introduce a WAL or atomically
roll chain files back with a recovered generation. Exactly-once replay and
proof that chain mutations and snapshot mutations cannot diverge remain open.
If a verified chain is ahead of the selected snapshot, startup now preserves
the snapshot head, reports `CHAIN_HEALTH_MISMATCH`, and blocks further appends
instead of adopting event history whose gameplay mutation may be absent. This
is a fail-closed safety boundary, not a substitute for WAL replay or rollback.

Generation garbage collection is also deferred. Old complete generations and
unpublished crash debris are retained; at minimum the published current and
previous generations must remain available when cleanup is added.
