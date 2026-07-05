# Cargo Architecture: matter, identity, and what counts as a crate

**Status:** draft for review
**Audience:** anyone touching the cargo / manifest / chain-log code
**Supersedes:** the implicit "we'll crateify everything" assumption that
shaped the original slice plan in PR #526. That plan had the right
substrate work but the wrong target.

## TL;DR

Matter in Signal exists in **three** states — not one. **Crate identity
is born at the smelt/craft boundary**, not at the moment of mining. The
data model is correct; we don't need to crateify ore.

The chain log coverage gap that motivated this doc has mostly been closed:
normal fragment-tow smelts now emit `CHAIN_EVT_SMELT`, fragment tow/release
transitions emit their own lifecycle events, and the legacy hopper-path smelt
compatibility behavior is retired. Remote multiplayer now receives, caches, and
actively presents full receipt chains before queued sell/deliver actions on
matching named cargo transfers, and authorities can verify presented chains for
carried cargo. Signed handoff tickets now bind ship snapshots to
manifest/receipt roots, and the first issue/present/accept handoff protocol can
hydrate a destination ship after verifying those roots. The remaining work is
automatic boundary routing, persistent replay logs, chain compaction/backfill,
and player-facing lineage display — not a cargo data-model rewrite.

## The conceptual model

Matter moves through Signal in three states. Each has a different
identity story, lives in a different part of the codebase, and answers
to a different invariant.

```
            ┌─────────────────┐
            │   FRAGMENT      │   in-space, physical, fragment_pub
            │   (in space)    │   ── identity by content hash, position
            └────────┬────────┘   ── tracked as asteroid_t in world.asteroids[]
                     │
                     │  tow → arrive at hopper → smelt
                     │
                     ▼
            ┌─────────────────┐
            │   BULK FLOAT    │   ephemeral working buffer
            │   (at station)  │   ── no identity, no chain entry
            └────────┬────────┘   ── _inventory_cache[ORE], legacy/demand only
                     │
                     │  retired as a smelt source
                     │
                     ▼
            ┌─────────────────┐
            │     CRATE       │   named, content-addressed, provenance-bearing
            │   (anywhere)    │   ── cargo_unit_t with parent_merkle
            └─────────────────┘   ── lives in ship.manifest / station.manifest
```

### State 1 — Fragment (pre-crate, in space)

A fractured piece of an asteroid. Lives as an `asteroid_t` in the
world. Has a `fragment_pub[32]` content hash that uniquely identifies
*this specific chunk of mineral* across the universe. Has physics —
position, velocity, mass.

**Critically, a fragment is not in any manifest.** It's not a
`cargo_unit_t`. It exists as a physical object in space that ships can
tow via `ship.towed_fragments[10]` (an array of int16 indices into the
asteroid array, max 10 with upgrades). When you tow a fragment you're
not "putting it in your hold" — you're dragging a physical object
through space behind your ship.

A fragment carries its own provenance via `fragment_pub` and
`fracture_seed`. That provenance survives forever in the chain log if
the fragment is ever smelted (the resulting ingot's `parent_merkle ==
fragment_pub`). If the fragment is destroyed in the void without being
smelted, its identity is lost — there's nothing to inherit it.

### State 2 — Bulk Float (legacy raw-ore station float)

This is the form we were about to migrate, until we realized the raw-ore
side is vestigial in normal play. Either way, not a crate.

`station._inventory_cache[c]` for `c < COMMODITY_RAW_ORE_COUNT` (i.e.
the three raw ore slots) can still hold floats from old saves, tests,
pricing fixtures, and demand scoring. It is no longer consumed by
production. Physical fragments now cross the smelt boundary directly
into ingot crates.

The deposit-side population paths are vestigial in today's code. The
raw-ore float write sites are the legacy player service paths in
`server/game_sim.c`, the station refill path, and the economy sim
production helper in `shared/economy.c`.

Players no longer carry raw ore in `ship.cargo[]` after the #259 tow
migration (the service-sell path in `server/game_sim.c` says this directly).
NPCs never deposit raw ore: they tow a single fragment through the embedded
`npc_ship_t.ship.towed_fragments[0]` slot, with `npc_ship_t.towed_fragment`
kept as a legacy save/wire/UI mirror, and deliver through the fragment-tow path.

So in practice, the hopper float for raw ore is not populated in normal
multiplayer play. The old "smelt from hopper float" code path
(`server/sim_production.c`) has been retired: `_inventory_cache[ORE]`
can still exist for old saves, pricing fixtures, and demand scoring, but
it is no longer a production input. `world.hopper_smelt_events` and
`world.hopper_smelt_units` remain exposed as regression telemetry and
should stay at zero.

**Bulk float has no crate identity by design.** It represents
undifferentiated material in transit through the smelter's working
volume. Putting it in the manifest would be like assigning a serial
number to the iron filings sitting in a foundry's input chute — the
abstraction doesn't fit the real-world thing. *And* it might be
unused.

### State 3 — Crate (named, identity-bearing, persistent)

`cargo_unit_t` — 80 bytes, content-addressed via `pub`,
provenance-attached via `parent_merkle`. The substrate of all named
matter in Signal: ingots, frames, lasers, tractors, repair kits.

Crates live in manifests:
- `ship.manifest.units[]` (cap 32 by default)
- `station.manifest.units[]` (cap 256 by default)

Crates are **created** at one of three boundaries:
- `hash_ingot(commodity, grade, fragment_pub, idx)` — smelt
  produces an ingot whose `parent_merkle = fragment_pub`. Identity
  is born here.
- `hash_product(recipe_id, inputs[], idx)` — fab/craft consumes
  multiple input crates and produces a new output crate whose
  `parent_merkle = merkle_root(sorted_input_pubs)`. Identity
  inherits from inputs.
- `hash_legacy_migrate_unit(origin, commodity, idx)` — synthesizes
  a placeholder crate for finished goods loaded from pre-manifest
  saves. `parent_merkle` is zero (no provable parents).

Once a crate exists, its identity never changes. It can move between
manifests (`CHAIN_EVT_TRANSFER`), be consumed as an input to another craft
(`CHAIN_EVT_CRAFT`), or be destroyed silently (e.g. consumed in repair). The
chain log preserves its existence forever via the events that
referenced its `pub`.

## The smelt boundary: where identity is born

This is the central insight that the original slice plan missed.

**Below the smelt boundary** (fragment, bulk float): matter is
characterized but not crateified. A fragment has identity in the form of
`fragment_pub` but is not a `cargo_unit_t`; bulk float doesn't even have
that. They're matter waiting to become a thing.

**At the smelt boundary**: the furnace performs an irreversible
transformation. Input matter (a fragment, or units of bulk float)
becomes output matter (an ingot crate). The crate's `parent_merkle`
captures *what was consumed*. From this moment forward the matter is
crate-form: it has a name, a provenance graph, and a chain-log
trail.

**Above the smelt boundary**: every transformation produces another
crate. Frames are crates whose parents are ingot crates. Lasers are
crates whose parents are ingot crates. Repair kits are crates whose
parents are frames + lasers (which are themselves crates). The
provenance DAG can be walked backward from any leaf to its
fragment-shaped roots.

This is the right factoring because it matches the real-world
intuition: a ferrite ingot has a specific shape, a specific weight, a
specific bar code; "12 units of raw ore" is a quantity in a ledger,
not a thing you can put your hand on.

## Mapping the model to code

### Fragment

| Concern | Where |
|---|---|
| Storage | `world.asteroids[]`. Each `asteroid_t` carries `fragment_pub[32]`, `fracture_seed[32]`, `last_towed_token[8]`, `last_fractured_token[8]`, `grade`, and `rock_pub[32]` (`shared/types.h`). |
| Player tow list | `ship.towed_fragments[10]` of int16 indices, plus `towed_count` (`shared/types.h`). |
| NPC tow | NPCs use `npc_ship_t.ship.towed_fragments[0]` as the ship-shaped tow slot, plus `npc_ship_t.towed_fragment` as a legacy mirror (`shared/types.h`). NPC ships only tow one fragment at a time. |
| Tow-add site | `server/game_sim.c` tractor collection. Fragment ownership is stamped via `last_towed_by` and `last_towed_token[8]` at the same instant. |
| Tow-remove sites (player) | `server/sim_production.c` for smelt completion; `server/game_sim.c` for asteroid destruction, band snap, station-beam landing, and manual `R` release. |
| Fragment generation | Initial spawn/materialization from `shared/belt.c` and `server/sim_asteroid.c`. Fracture children are also created in `server/sim_asteroid.c`. |

### Bulk float

| Concern | Where |
|---|---|
| Storage | `station._inventory_cache[c]` for `c < COMMODITY_RAW_ORE_COUNT` (`shared/types.h`). Underscore prefix and "private; use accessors" comment indicate the field is no longer treated as authoritative for finished goods, but for raw ore it is still where the value lives. |
| Public accessor | `station_inventory_amount(station, commodity)` (`shared/commodity.c`). Used by client UI for *finished-goods* display only; raw ore display is intentionally skipped. |
| Write — fragment-smelt completion | `server/sim_production.c` accumulates the produced *ingot* commodity, not the raw ore. The ore commodity slot itself is barely written on this path. |
| Write — legacy player ore paths | `server/game_sim.c` still has vestigial service sell/deliver/refill paths around `ship.cargo[]`; normal play no longer puts raw ore there. |
| Write — economy sim | `shared/economy.c` production recipe execution. |
| Read — furnace intake | Retired. `sim_step_refinery_production` is a no-op, and raw-ore hoppers no longer feed furnace buffers. |
| Read — smelt rate/consume | Retired. Hopper telemetry counters remain as zero-valued regression guards. |
| Read — UI display | `client/station_ui.c` trade UI ore-side display. |
| Read — price scaling | `shared/commodity.c` `station_buy_price` reads hopper fill. |
| Persistence | The float is **not** persistent at meaningful timescales. Furnace smelt rate (`REFINERY_BASE_SMELT_RATE = 2.0`/sec, hopper cap 500) drains it within seconds at typical throughput. |

### Crate

| Concern | Where |
|---|---|
| Type | `cargo_unit_t` (`shared/types.h`). 80 bytes. As of slice 0 (PR #526), byte 7 is `quantity` (u8, default 1). |
| Storage | `manifest_t` (`shared/types.h`) — held by `ship_t.manifest` and `station_t.manifest`. |
| Creation | Three hash helpers in `shared/manifest.c`: `hash_ingot`, `hash_product`, `hash_legacy_migrate_unit`. Current live producers set `quantity = 1`. |
| Mutation | `manifest_push`, `manifest_remove`, `manifest_consume_by_commodity` in `shared/manifest.c`. |
| Chain witnessing | `CHAIN_EVT_SMELT` from `server/sim_production.c`, `CHAIN_EVT_CRAFT` from fab/craft production including shipyard repair-kit batches, and `CHAIN_EVT_TRANSFER` from `server/cargo_receipt_issue.c` for inter-holder moves. |
| Wire | `NET_MSG_PLAYER_MANIFEST` and `NET_MSG_STATION_MANIFEST` send `(commodity, grade) → count` summaries derived from manifests. They do *not* send the bulk float. The hopper float is server-side only. |

## What slice 0 actually bought us

PR #526 added a `quantity` field (u8) to `cargo_unit_t` in the byte
formerly used as `_pad`. The original justification was "ore crates can
pool many fragments under one provenance signature." Per the model
above, that use case **does not exist** — ore doesn't become a crate.

The field is still useful, just for a different reason. **It's a hook
for future bulk-mode operations on crates that genuinely warrant
identity but where individual addressability would balloon the
manifest.** Two plausible future use cases:

1. **Anonymous ingot stockpiles.** A station that has smelted 80
   anonymous ferrite ingots from this morning's mining run might
   reasonably represent that as one crate with `quantity = 80`,
   `prefix_class = ANONYMOUS`, `parent_merkle = ANONYMOUS_BATCH`,
   instead of 80 separate crates. The provenance is "anonymous batch
   from epoch X" — fine to compress. The 80-crate explosion was a
   legitimate concern; quantity solves it for the shape of matter
   where it actually applies.

2. **Legacy bulk consumables.** The current repair-kit path can use
   quantity as a compatibility compression hook, but it should not be
   treated as the final matter model. A "100 kits from this fab batch"
   object is exactly the kind of hidden multiplier the visible matter
   algebra should retire. The destination is block-count hull/station
   repair: damage removes visible blocks, and repair welds frames or
   blocks back on.

So slice 0's work isn't wasted. It just turns out the field's natural
use is for compressing *finished* goods of low individual value, not
for representing *raw* ore that doesn't want crate identity at all.

## What the substrate gives us now and still doesn't

The important points are:

### Resolved gap 1 — Fragment-tow smelts are witnessed

The normal fragment-tow path now emits `CHAIN_EVT_SMELT` with the
fragment's `fragment_pub`, so a named ingot's `parent_merkle` can be
matched back to the physical fragment that entered the furnace.

The in-flight phase is also witnessed:

- `CHAIN_EVT_FRAGMENT_TOW` fires when a player tractor takes possession
  of a fragment.
- `CHAIN_EVT_FRAGMENT_RELEASE` fires when the tow ends without a smelt
  (manual release / PvP throw, band snap, with destroyed reserved for a
  future direct producer).
- `CHAIN_EVT_SMELT` is the productive terminus when a towed fragment
  actually becomes an ingot.

There is intentionally no separate `FRAGMENT_DEPOSIT` event today. A
successful deposit is a smelt; non-smelt endings are releases. That keeps
the chain vocabulary aligned with semantic transitions rather than every
physics contact.

### Closed gap — Hopper-path smelt no longer mints ingots

The old concern was: if the smelter consumed from `_inventory_cache[ORE]`
(the bulk-float path), the resulting ingot was minted with
`parent_merkle = 0` because there was no source fragment to attribute.
That compatibility path is now disabled. `sim_step_refinery_production`
is a no-op, raw-ore storage hoppers do not feed furnace buffers, and
`CHAIN_EVT_SMELT` is emitted only from `step_furnace_smelting` after a physical
fragment reaches the furnace/hopper beam.

The removed hopper-float-population paths were:

- Legacy player ore service paths in `server/game_sim.c`
- Player ore refill from station stock in `server/game_sim.c`
- Economy sim production in `shared/economy.c`

The legacy player paths are vestigial per the service-path comments:
players stopped carrying raw ore in `ship.cargo[]` after #259. Refill is
explicitly a station-stock path. The fourth is NPC-economy production.
No NPC deposits raw ore at a hopper; NPC miners tow through the embedded
`ship.towed_fragments[0]` slot, keep `npc_ship_t.towed_fragment` synchronized
as a legacy mirror, and deliver to furnaces via the fragment-tow path, same as
players.

The decision is picked: pure fragment-tow to ingot pipeline. Tests now
assert that directly seeded raw ore does not smelt, does not drain through
raw hopper flow, and does not emit zero-fragment `CHAIN_EVT_SMELT`.

### Closed gap — Docked cargo rows show player-readable lineage

The docked TRADE view now surfaces the representative cargo unit behind
held and station-stock rows. Rows backed by a concrete `cargo_unit_t`
show a compact inspection line with the unit serial/callsign, recipe,
parent summary, origin station, mint epoch when available, and attached
receipt seal count when the local manifest has the chain.

This is intentionally a player-facing first slice, not a full proof
endpoint. It makes the existing manifest/receipt substrate visible in
normal trade without adding a debug screen or new keybinding. Full tree
walks, station-root freshness, credit-note panels, and on-demand proof
fetching remain future proof-surface work.

The same receipt substrate now feeds the gossip economy. Completed delivery
shipments can emit receipt-backed market memories, reinforcing successful
routes and collapsing matching demand memories without letting gossip mutate
the authoritative contract ledger directly.

## Recommended next moves

Ranked by value-per-effort after the fragment-chain coverage pass.

**1. Extend inspection beyond the shipped inspect slices.** Trade rows
now surface representative serial, recipe, parent, origin, epoch, and
receipt count from local manifests, and worker hauling-assignment inspect
snapshots carry the same cargo identity rows. The next useful slice is
scan-to-inspect for player-held cargo, station stock, and tracked contract
matching using the same player-facing vocabulary.

**2. Group manifest presentation before showing every hash.** Common
anonymous stock should compress into commodity/grade buckets, while
named or receipt-bearing units stay individually addressable. This keeps
inspection readable as station and hauler manifests grow.

**3. Deeper heritage contract templates.** Heritage contracts now cover
recipe provenance and contract-level station-origin bans. The seeded baseline
does not define founding-station enemies because the starter supply loop depends
on Prospect → Kepler → Helios → Prospect. Station policy is now shaped as a
small card library with trade, construction, and finance budgets. Each station
caches its ranked cards once per sim tick, contract pricing reads those cached
cards, and `/training/v1/station-policy-trace` exports the selected cards,
scores, top demand, and per-commodity policy price modifiers. Future neural
scorers can rank the same cards while the deterministic baseline remains the
auditable teacher/fallback. The next useful layer is filtering on
`parent_merkle` chains and real chain-log history. The player-facing payoff:
the universe's history becomes the quest content.

What's *not* on this list:

- ❌ Migrate `_inventory_cache[ORE]` to manifest crates. The float is
  intentionally a working buffer (or dead code). Not a problem; don't
  "fix" by complicating it.
- ❌ Add ore-merge / ore-split logic. Ore doesn't have crate identity
  to merge or split.
- ❌ Add `CHAIN_EVT_TRANSFER` for ore deposits as raw float movement. The
  fragment-lifecycle events are the right unit of work; bulk ore
  transfers between locations don't happen meaningfully today.

## Out of scope (and why)

| Idea | Why we ruled it out |
|---|---|
| Fragments-as-cargo_unit_t | Fragments live in space with physics. Crates live in manifests as data. Conflating them duplicates identity (fragment_pub already exists) and violates the spatial/abstract divide. |
| Quantity > 1 on ore crates | There are no ore crates. Fragments are individual; bulk float is anonymous. |
| CHAIN_EVT_SPLIT / CHAIN_EVT_MERGE for ore | No crate identity to split or merge. |
| One unified "container" type that wraps all of {fragment, float, crate} | The three states have genuinely different identity stories. Forcing a single type makes the union as expensive as the maximum of all three, with conditionals everywhere. The current factoring is right. |
| Chain events for every bulk float mutation | Bulk float is meant to be ephemeral. Witnessing every tick-level integration value would explode chain log volume without adding semantic information. Witness fragment-lifecycle transitions instead. |

## What slice 0's quantity field IS for

Restated since this is the most likely thing to be misremembered:

The `quantity` field added in PR #526 is for **batch-level
identity-bearing crates** that compress multiple anonymous units of
the same provenance signature into one manifest entry. It is **not**
for raw ore.

Concrete near-term use case: anonymous ingot stockpiles at stations
where individual ingot identity carries no value beyond "this batch
came from this furnace at this epoch." A station with 80 anonymous
ferrite ingots from this morning's smelting can be one crate with
`quantity = 80`, `prefix_class = ANONYMOUS`, `parent_merkle =
batch_hash`. Saves 79 manifest entries; loses nothing meaningful.

When this becomes worth implementing depends on how often the
finished-goods manifest hits its 256-entry cap in practice. Worth
instrumenting before optimizing.

## Appendix: file map

| File | Role |
|---|---|
| `shared/types.h` | All struct definitions: `asteroid_t`, `ship_t`, `station_t`, `cargo_unit_t`, `manifest_t`, `commodity_t` enum |
| `shared/manifest.h` | Crate API: push/remove/find, hash_*, migration helpers |
| `shared/manifest.c` | Crate implementation |
| `server/sim_production.c` | The smelt boundary lives here. Fragment-tow smelting is the only production smelt path; the hopper-float compatibility function is a no-op. |
| `server/sim_ai.c` | NPC autopilot. NPCs tow fragments through the embedded `ship.towed_fragments[0]` slot and mirror that to `npc_ship_t.towed_fragment` for compatibility; they deliver via the fragment-tow path and never deposit raw ore at hoppers. |
| `server/sim_save.c` | Save format, including the manifest persistence and migration paths |
| `server/chain_log.h` / `chain_log.c` | Append-only signed event log per station |
| `server/cargo_receipt_issue.c` | `CHAIN_EVT_TRANSFER` emission |
| `shared/belt.c` / `server/sim_asteroid.c` | Fragment generation and fracture |

---

## Decision

**Adopt the three-state model as canonical.** Update `CLAUDE.md` to
reference it as the cargo architecture's foundational vocabulary.

**Stop the original slice plan from PR #526.** The quantity field
landed and is genuinely useful for a different purpose (anonymous
batch compression of finished goods). The rest of the slices (1-5)
targeted "ore as crate," a problem that doesn't exist. The new
roadmap is now:

1. Player-visible lineage display for cargo and inspected worker hauls.
2. Group common manifest rows while keeping named/receipt-bearing units
   individually visible.
3. Deeper heritage contract templates that filter on `parent_merkle` chains.
4. Use `quantity > 1` only for future grouped anonymous crates where
   individual identity adds no gameplay value.

**No further data-model changes to ore.** Ore stays as fragments and
bulk float. The work is in chain-log coverage and player-facing
display, not in the cargo data model.
