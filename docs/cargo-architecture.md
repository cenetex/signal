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

But the chain log has real gaps that the original slice plan missed:
the fragment-tow smelt path mints ingots with full lineage but emits
no chain event at all, while the (probably-dead) hopper-path emits
events with zero lineage. So the audit log today is dominated by
events that don't have provenance and missing the events that do.

The work is in chain-log coverage, instrumentation to confirm dead
code, and player-facing display — not in the cargo data model.

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
            └────────┬────────┘   ── _inventory_cache[ORE], smelter integration
                     │
                     │  furnace produces output
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

### State 2 — Bulk Float (transient, at-station-only, possibly dead)

This is the form we were about to migrate, until we realized it's
either a working buffer or a vestigial code path. Either way, not a
crate.

`station._inventory_cache[c]` for `c < COMMODITY_RAW_ORE_COUNT` (i.e.
the three raw ore slots) holds floats representing ore that has *just
been deposited at a hopper* and *is about to be smelted*. The typical
lifetime of a value in this float is a few sim ticks — deposit, brief
buffer, consume.

But here's the thing: **the deposit-side population paths look mostly
or entirely vestigial in today's code.** The agent's mapping shows the
write sites for raw-ore floats are:

- Player ore-sell at dock (`game_sim.c:1089`) — *vestigial per #259*
- Player ore-deliver against contract (`game_sim.c:1171`) — *same*
- Player ore-buy from station refill (`game_sim.c:1231`)
- Economy sim production (`economy.c:90`)

Players no longer carry raw ore in `ship.cargo[]` after the #259 tow
migration (comment at `game_sim.c:1148-1154` confirms). NPCs never
deposit raw ore — they tow fragments via `npc_ship_t.towed_fragment`
(single int16, not an array) and deliver through the fragment-tow path.

So in practice, the hopper float for raw ore may never be populated in
normal multiplayer play. The "smelt from hopper float" code path
(`sim_production.c:240-280`) might be dead. Worth instrumenting before
deciding whether to delete or heal.

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
manifests (`EVT_TRANSFER`), be consumed as an input to another craft
(`EVT_CRAFT`), or be destroyed silently (e.g. consumed in repair). The
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
| Storage | `world.asteroids[]`. Each `asteroid_t` carries `fragment_pub[32]`, `fracture_seed[32]`, `last_towed_token[8]`, `last_fractured_token[8]`, `grade`, `rock_pub[32]` (`shared/types.h:538-555`). |
| Player tow list | `ship.towed_fragments[10]` of int16 indices, plus `towed_count` (`shared/types.h:208-209`). |
| NPC tow | NPCs use a *different* shape: `npc_ship_t.towed_fragment` (single int16, not an array) — set at `server/sim_ai.c:1560`. NPC ships only tow one fragment at a time. |
| Tow-add site | `server/game_sim.c:1888` — tractor pulse in `step_fragment_collection()`. Fragment ownership is stamped via `last_towed_by` and `last_towed_token[8]` at the same instant. |
| Tow-remove sites (player) | `server/sim_production.c:732-736` (smelt), `server/game_sim.c:1827-1828` (asteroid destroyed), `:1916-1917` (fracture child escapes gravity), `:1927-1928` (lands in station beam), `:1950-1951` (manual R-release). |
| Fragment generation | Initial spawn from `shared/belt.c` and `src/asteroid_field.c`. Fracture children created in `server/sim_asteroid.c`. |

### Bulk float

| Concern | Where |
|---|---|
| Storage | `station._inventory_cache[c]` for `c < COMMODITY_RAW_ORE_COUNT` (`shared/types.h:327`). Underscore prefix and "private; use accessors" comment indicate the field is no longer treated as authoritative for finished goods, but for raw ore it's still where the value lives. |
| Public accessor | `station_inventory_amount(station, commodity)` (`src/commodity.c:194`). Used by client UI for *finished-goods* display only; raw ore display is intentionally skipped. |
| Write — fragment-smelt completion | `server/sim_production.c:748` writes `_inventory_cache[output] += a->ore` — note `output` is the *ingot* commodity, not the ore. The ore commodity slot itself is barely written on this path; the float-as-ingot accumulator is. |
| Write — player ore-sell | `server/game_sim.c:1089` — `_inventory_cache[commodity] += 1.0f` when a docked player sells ore from `ship.cargo[]`. *This is a vestigial path*; comment at `:1148-1154` says players no longer carry raw ore post-#259. Reachable but probably never exercised in normal play. Worth confirming. |
| Write — player ore-deliver | `server/game_sim.c:1171` — same path for contract delivery. Same vestigial concern. |
| Write — player ore-buy | `server/game_sim.c:1231` — when a player buys ore from a station (refill scenario). |
| Write — economy sim | `src/economy.c:90` — production recipe execution. |
| Read — furnace intake | `server/sim_production.c:251, 275` — gates the smelt loop on float > threshold. |
| Read — smelt rate/consume | `server/sim_production.c:279-280` — bulk-float drain per tick. |
| Read — UI display | `src/station_ui.c:676` — trade UI ore-side display. |
| Read — price scaling | `src/commodity.c:172` — `station_buy_price` reads hopper fill. |
| Persistence | The float is **not** persistent at meaningful timescales. Furnace smelt rate (`REFINERY_BASE_SMELT_RATE = 2.0`/sec, hopper cap 500) drains it within seconds at typical throughput. |

### Crate

| Concern | Where |
|---|---|
| Type | `cargo_unit_t` (`shared/types.h:138-160`). 80 bytes. As of slice 0 (PR #526), byte 7 is `quantity` (u8, default 1). |
| Storage | `manifest_t` (`shared/types.h:151-155`) — held by `ship_t.manifest` and `station_t.manifest`. |
| Creation | Three hash helpers in `src/manifest.c`: `hash_ingot` (`:481`), `hash_product` (`:503`), `hash_legacy_migrate_unit` (`:536`). All set `quantity = 1`. |
| Mutation | `manifest_push`, `manifest_remove`, `manifest_consume_by_commodity` in `src/manifest.c`. |
| Chain witnessing | `chain_log_emit(EVT_SMELT)` at `server/sim_production.c:314` for ingot mint. `chain_log_emit(EVT_CRAFT)` at `:160` for fab/craft. `chain_log_emit(EVT_TRANSFER)` at `server/cargo_receipt_issue.c:58` for inter-holder moves. |
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

2. **Bulk repair kits / consumables.** A repair kit that's "100 kits
   from this fab batch" could compress similarly. Anonymous,
   per-batch identity, quantity > 1.

So slice 0's work isn't wasted. It just turns out the field's natural
use is for compressing *finished* goods of low individual value, not
for representing *raw* ore that doesn't want crate identity at all.

## What the substrate doesn't yet give us

The actual gaps are not in the data model. They're in:

### Gap 1 — Chain log misses the fragment-tow smelt path entirely

Surprise: there's only **one** `EVT_SMELT` emission site in the
production code — `server/sim_production.c:314`, which is the
hopper-path path. The richer **fragment-tow path mints ingots without
emitting any chain event at all.**

So the picture inverts what I initially assumed:

- **Hopper-path:** emits `EVT_SMELT` events, but with `parent_merkle =
  0` (no fragment to attribute to). Witnessed but broken-lineage.
- **Fragment-tow path:** mints ingots with full lineage
  (`parent_merkle = fragment_pub` via `hash_ingot()` at
  `sim_production.c:768`), but emits no chain event. Honest lineage
  but invisible to the audit log.

The fragment-tow path is the one that runs in normal play (every
player smelt goes through it). The hopper path is suspected dead post-#259
but still wired up.

**Net effect:** the chain log today is dominated by `EVT_SMELT` events
that *don't* have lineage, and missing the events that *do*. Heritage
queries that filter on fragment lineage can't be answered against the
chain log.

The fix has two pieces:
1. **Add `EVT_SMELT` emission to the fragment-tow path** at
   `server/sim_production.c:768` (or wherever the fragment-completion
   transition fires). Populate `fragment_pub` from the asteroid record.
2. **Add three additional chain event types** —
   `EVT_FRAGMENT_TOW`, `EVT_FRAGMENT_DEPOSIT`, `EVT_FRAGMENT_LOST` —
   and emit them at the corresponding sim transitions in
   `server/game_sim.c` (lines 1888 / 1827-1828 / 1916-1917 /
   1927-1928 / 1950-1951). This gives the in-flight phase witness
   coverage.

Both pieces are pure chain-log work. The substrate already has the
data; the emit calls just aren't there.

### Gap 2 — Hopper-path smelt mints ingots with broken lineage

When the smelter consumes from `_inventory_cache[ORE]` (the bulk-float
path), the resulting ingot is minted with `parent_merkle = 0` — no
source fragment to attribute to. The hardcoded origin string is
`"REFN0000"` (`station_finished_accumulate` callsite). The comment at
`server/sim_production.c:304-306` is explicit about this:

> "We don't know the source fragment_pub for hopper-path smelts (#280
> fragment-tow path is separate); use zero so verifiers can
> distinguish."

The substrate is honest about the gap, which is good. But it also
means any ingot that came through the hopper-path has zero-merkle
lineage and can't participate in heritage queries.

**There's a related discovery worth flagging.** The agent's mapping
shows that the hopper-float-population paths in current code are:

- Player sell-ore-at-dock (`game_sim.c:1089`)
- Player deliver-ore-on-contract (`game_sim.c:1171`)
- Player buy-ore-from-station (`game_sim.c:1231`)
- Economy sim production (`economy.c:90`)

The first two are *vestigial* per the comment at
`server/game_sim.c:1148-1154` — players stopped carrying raw ore in
`ship.cargo[]` after #259. The third is a refill path. The fourth is
NPC-economy production. **No NPC ever deposits raw ore at a hopper**;
NPC miners use `npc_ship_t.towed_fragment` and deliver to furnaces via
the fragment-tow path, same as players (`server/sim_ai.c:989-1006`,
`:1560`).

So the practical question is: **is the hopper-float path actually
exercised in normal multiplayer play, or is it dead code from #259's
half-finished migration?** If it's dead, this whole "gap 2" disappears
— there's no hopper-path smelt happening, so there are no zero-merkle
ingots being minted.

This is worth instrumenting before fixing. Add a counter on
`server/sim_production.c:280` (the hopper-float drain) and watch a
multi-hour live game session. If the counter stays at zero,
"gap 2" downgrades to "dead code path; consider removal" instead of
"lineage-fix project."

Two possible outcomes:

- **If hopper-path is dead in practice:** delete the
  `_inventory_cache[ORE]` write paths at `game_sim.c:1089/1171/1231`
  and the matching read at `sim_production.c:240-280`. Bulk float for
  raw ore goes away entirely. Pure fragment-tow → ingot pipeline.
- **If hopper-path is live:** add a synthetic batch-lineage anchor at
  hopper deposit, used as `parent_merkle` for ingots smelted from the
  resulting float. Each "deposit batch" gets a content-hashed pub
  derived from `(station, epoch, depositor_session)`.

Don't pick yet; instrument first.

### Gap 3 — Players can't see provenance at all

The HUD shows hold contents as a commodity-and-grade summary. There's no
display surface that says "this ferrite ingot came from Belt-7's
fracture by 0F3H-CH at tick 4422." The substrate has the data; the UI
ignores it.

The fix is the "lineage-as-name" mechanic from the prior design
discussion. Cargo rows render with a one-line provenance haiku
generated from `cargo_unit_t.parent_merkle` walked against the chain
log. No new screens, no new keys, just a string format change.

## Recommended next moves

Ranked by value-per-effort after the current implementation pass.

**1. Watch the hopper-path telemetry in live sessions.** The legacy
refinery-hopper path now increments `world.hopper_smelt_events` and
`world.hopper_smelt_units`, exposed from `/health`. If the counters stay
at zero during real play, remove the hopper path and its zero-fragment
`EVT_SMELT` compatibility behavior. If they move, keep it and add
synthetic batch lineage for hopper-fed ore.

**2. Expand lineage beyond named ingots.** Trade rows now surface
representative `origin_station` / `mined_block` text from local
manifests, and multiplayer preserves detailed named-ingot snapshots.
Anonymous ingots and fabricated goods still need either richer wire
records or a compact provenance-summary record if they should display
the same player-facing history.

**3. Heritage contract templates.** Fragment-tow smelts emit
`EVT_SMELT`, and fragment tow/release transitions are logged. Contracts
can now start filtering on `parent_merkle` chains and real chain-log
history. The player-facing payoff: the universe's history becomes the
quest content.

What's *not* on this list:

- ❌ Migrate `_inventory_cache[ORE]` to manifest crates. The float is
  intentionally a working buffer (or dead code). Not a problem; don't
  "fix" by complicating it.
- ❌ Add ore-merge / ore-split logic. Ore doesn't have crate identity
  to merge or split.
- ❌ Add `EVT_TRANSFER` for ore deposits as raw float movement. The
  fragment-lifecycle events are the right unit of work; bulk ore
  transfers between locations don't happen meaningfully today.

## Out of scope (and why)

| Idea | Why we ruled it out |
|---|---|
| Fragments-as-cargo_unit_t | Fragments live in space with physics. Crates live in manifests as data. Conflating them duplicates identity (fragment_pub already exists) and violates the spatial/abstract divide. |
| Quantity > 1 on ore crates | There are no ore crates. Fragments are individual; bulk float is anonymous. |
| EVT_SPLIT / EVT_MERGE for ore | No crate identity to split or merge. |
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
| `src/manifest.c` | Crate implementation |
| `server/sim_production.c` | The smelt boundary lives here. Both fragment-tow and hopper-float smelt paths. |
| `server/sim_ai.c` | NPC autopilot. NPCs tow fragments via `npc_ship_t.towed_fragment` (single-slot) and deliver via the fragment-tow path; they never deposit raw ore at hoppers. |
| `server/sim_save.c` | Save format, including the manifest persistence and migration paths |
| `server/chain_log.h` / `chain_log.c` | Append-only signed event log per station |
| `server/cargo_receipt_issue.c` | EVT_TRANSFER emission |
| `shared/belt.c` / `src/asteroid_field.c` / `server/sim_asteroid.c` | Fragment generation and fracture |

---

## Decision

**Adopt the three-state model as canonical.** Update `CLAUDE.md` to
reference it as the cargo architecture's foundational vocabulary.

**Stop the original slice plan from PR #526.** The quantity field
landed and is genuinely useful for a different purpose (anonymous
batch compression of finished goods). The rest of the slices (1-5)
targeted "ore as crate," a problem that doesn't exist. The new
roadmap is the six moves above:

1. Instrument the hopper-path to confirm or reject the dead-code
   suspicion.
2. Add `EVT_SMELT` emission to the fragment-tow path with proper
   `fragment_pub` attribution.
3. Player-visible lineage display.
4. Chain events for fragment in-flight lifecycle (tow / deposit /
   lost).
5. Resolve gap 2 based on (1)'s instrumentation result — either
   delete the hopper-path or add synthetic batch lineage to it.
6. Heritage contract templates that filter on `parent_merkle` chains.

**No further data-model changes to ore.** Ore stays as fragments and
bulk float. The work is in chain-log coverage and player-facing
display, not in the cargo data model.
