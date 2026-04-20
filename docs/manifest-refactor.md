# Cargo Manifest Refactor + Client-Hashed Rarity

Status: draft v3 (min-flow grade).
Supersedes float-cargo model.
Applies to: ship holds, station stockpiles, every craftable item.

## Thesis

Every item in the game has a content hash. A cargo manifest is an ordered
list of those hashed units. Callsign and class prefix are *render rules*
over the hash. Grade is a deterministic property of the inputs, cached
on each unit.

Crafting is a merkle tree. A product's hash is a pure function of its
recipe and input hashes; the same recipe with the same inputs produces
the same output hash anywhere, anytime, at any station. Its grade is the
minimum grade of its inputs.

Three pieces land together:
1. **Manifest data model** — bounded list of `cargo_unit_t` on ships and
   stations, replacing `float cargo[COMMODITY_COUNT]`.
2. **Min-flow grade propagation** — grade is input-determined and cached
   on every unit. No per-craft rarity roll.
3. **Option D, client-hashed** — fragment grade is mined by clients at
   fracture time; the server verifies. From there, grade cascades through
   smelt and crafting by the min-flow rule.

## What is and is not a manifest unit

Only **finished goods** are manifest units:

- `CARGO_KIND_INGOT` — produced at a furnace
- `CARGO_KIND_FRAME` — produced at a press
- `CARGO_KIND_LASER` — produced at a laser fab
- `CARGO_KIND_TRACTOR` — produced at a tractor-coil fab

**Fragments are not manifest units.** They live on `asteroid_t` as world
objects and are towed by the tractor. Towing does not mutate the ship's
manifest. A fragment becomes N ingots only when delivered to a furnace,
and those ingots are pushed into the furnace station's stockpile — not
the tower's hold.

Raw ore never enters anyone's manifest. The tower is paid fungible
credits by the station at smelt time (Option X, unchanged from today).

## Data model

```c
typedef enum {
    CARGO_KIND_INGOT   = 0,
    CARGO_KIND_FRAME   = 1,
    CARGO_KIND_LASER   = 2,
    CARGO_KIND_TRACTOR = 3,
    CARGO_KIND_COUNT
} cargo_kind_t;

typedef struct {
    uint8_t  kind;              /* cargo_kind_t */
    uint8_t  commodity;         /* commodity_t */
    uint8_t  grade;             /* mining_grade_t — deterministic min-flow */
    uint8_t  _pad;              /* reserved, zero */
    uint16_t recipe_id;         /* stable recipe enum */
    uint16_t _pad2;             /* align the 32-byte hashes */
    uint8_t  pub[32];           /* content hash of this unit */
    uint8_t  parent_merkle[32]; /* merkle root of sorted input pubs */
} cargo_unit_t;                 /* 72 bytes */

typedef struct {
    uint16_t     count;
    uint16_t     cap;
    cargo_unit_t *units;        /* heap-allocated */
} manifest_t;
```

Dynamic allocation keeps ship holds small (cap 32) while allowing station
stockpiles to grow larger. The existing `named_ingot` pool on stations
gets absorbed here — it becomes the "ingots in stockpile" view of the
manifest.

**How units enter each manifest:**

| Manifest | Entry paths |
|----------|-------------|
| Station  | Smelt output; fab output; player sells/delivers into station |
| Ship     | Player buys from station; player collects their own fab output |

Ship manifests are strictly populated at station boundaries (buy, collect,
migration). The tractor tows fragments — it does not mutate the ship
manifest.

**Full-stockpile policy per kind:**

| Kind     | Full policy                                           |
|----------|-------------------------------------------------------|
| INGOT    | LRU-evict oldest on overflow (high turnover, fungible) |
| FRAME    | Block production at source (rare, high stakes)        |
| LASER    | Block production at source                            |
| TRACTOR  | Block production at source                            |

"Oldest" = earliest insertion timestamp on the station.

## Hashing

`pub` is the **only** identity. `parent_merkle` is advisory — it lets
consumers rebuild provenance without a separate table.

There's one universal formula, parameterised by `output_index` for
recipes that emit multiple outputs:

```
unit.pub = sha256(DOMAIN || recipe_id_le || merkle_root || output_index_le)

DOMAIN           = the 8 bytes "SIGNALv1" (bytewise, not null-terminated)
recipe_id_le     = recipe_id as 2 little-endian bytes
merkle_root      = pairwise SHA-256 tree over sorted input pubs
output_index_le  = 2 little-endian bytes (0 for single-output recipes)
```

All string tags in this doc are written bytewise in little-endian with
their literal ASCII length — no null terminators, no padding.

`merkle_root` is a standard pairwise SHA-256 tree over input `pub`s
sorted ascending as 32-byte big-endian integers. Single-input root is
the input's pub. Odd last node duplicates itself. The sort makes input
order irrelevant.

`recipe_id` is a stable `uint16` enum distinct from `commodity` — it
names the *formula*, not the output type. Keeps "M-frame from 2×M-ingot"
distinct from "H-frame from 2×M-ingot" even if inputs collide.

**Smelt is a recipe.** The fragment is the single input, so
`merkle_root = fragment.pub`, and the N ingots coming out differ only in
`output_index`. No separate formula needed.

**Fragments are not manifest units.** Their `pub` lives on `asteroid_t`
and is derived once, at claim resolution:

```
fragment.pub = sha256("SIGNALv1" || "FRAG" || fracture_seed || winner_player_pub || burst_nonce_le)
```

If no valid claim lands before `deadline_ms`, the server derives
`fragment.pub` with `winner_player_pub = 0` and a server-chosen nonce
(existing burst=20 ceremony).

### Grade propagation — deterministic min-flow

Grade is a cached, deterministic property of every unit. It is **not**
classified from the output hash; it is computed at unit creation from
the inputs and stored in `cargo_unit_t.grade`. The rule is one line:

```
grade(product) = min(grade(input_i))   for all i
```

Set at creation:

| Unit                | Grade source                                    |
|---------------------|-------------------------------------------------|
| Fragment (on claim) | Option D client-hashed search result            |
| Ingot (from smelt)  | `fragment.grade` (single input, min is trivial) |
| Frame / Laser / Tractor | `min(input.grade) over inputs`              |
| Legacy-migrated     | `MINING_GRADE_COMMON`                           |

Grade is immutable post-creation. Any verifier can re-derive it from
scratch by walking `parent_merkle`; the stored field is a cache for the
hot render path, not a source of truth.

Consequences:

- A RATi fragment makes a stack of RATi ingots, which make a RATi frame
  when crafted in a RATi-only batch. The chain is predictable end-to-end.
- Mixing in a single common ingot caps the output at common. Dilution is
  real, scarcity is preserved, arbitrage is closed.
- No simulation is needed — grade distribution in the economy is
  entirely determined by fracture rolls, and `tools/mining_sim.c`
  already characterises those.

### Class propagation — stays emergent (v1)

Class prefix (`M-`, `H-`, `T-`, …) is **not** stored. It's derived on
demand by `mining_pubkey_class(unit.pub)`. With 8 possible classes the
combination rules are fiddly, and getting them wrong is worse than
leaving class as a biased-but-emergent property of the hash. Two `M-`
inputs *tend* to produce an `M-` output (their bytes dominate the merkle
entropy), but it's not guaranteed.

We revisit class propagation after real play data. Stored `grade` is the
only rarity axis that needs to be deterministic in v1.

## Rarity: Option D, client-hashed

The fragment hash carries the grade through everything downstream, so
searching at fracture time determines every ingot's grade that comes from
it. Search is performed by the client; server verifies.

### FRACTURE_CHALLENGE (server → nearby clients)

Server computes `fracture_seed` canonically and broadcasts the seed —
not the raw inputs — so there's no chance of canonicalization drift
between client and server.

```
type:         uint8  = NET_MSG_FRACTURE_CHALLENGE
fracture_id:  uint32  (server-assigned, monotonic)
seed:         uint8[32]
deadline_ms:  uint32  (server clock; ~500 ms in the future)
burst_cap:    uint16  (= 50)
```

Broadcast radius = signal range of the fracturing asteroid. Clients
outside range don't receive the challenge and can't compete.

Clients search locally:

```c
uint32_t best_nonce = 0;
mining_grade_t best_grade = MINING_GRADE_COMMON;
for (uint32_t n = 0; n < burst_cap; n++) {
    mining_keypair_t kp;
    mining_keypair_derive(seed, local_player_pub, n, &kp);
    char cs[12]; mining_render_callsign(kp.pub, cs);
    mining_grade_t g = mining_classify_base58(cs);
    if (g > best_grade) { best_grade = g; best_nonce = n; }
}
```

### FRACTURE_CLAIM (client → server)

```
type:          uint8  = NET_MSG_FRACTURE_CLAIM
fracture_id:   uint32
burst_nonce:   uint32
claimed_grade: uint8
```

Server maintains per-fracture state: `{fracture_id → best_verified_grade,
best_nonce, best_claimant, seen_claimants[]}`. On each claim:

1. If `fracture_id` unknown or resolved → drop.
2. If `claimant_pub` already in `seen_claimants` → drop (one per player).
3. If `burst_nonce >= burst_cap` → drop.
4. Re-derive `(seed, claimant_pub, burst_nonce) → kp`, classify. Cost:
   3 SHA-256 derivations + base58 classify per verification.
5. If `actual_grade < claimed_grade` → drop silently (tampered client).
6. If `actual_grade > best_verified_grade`, or tie with earlier arrival,
   → update best.
7. Append `claimant_pub` to `seen_claimants`.

At `deadline_ms` the server commits the best result (or runs the
no-claimant fallback) and broadcasts resolution.

### FRACTURE_RESOLVED (server → nearby clients)

```
type:           uint8  = NET_MSG_FRACTURE_RESOLVED
fracture_id:    uint32
fragment_pub:   uint8[32]   (now known; smelt uses this)
winner_pub:     uint8[32]   (zero bytes if no-claimant fallback)
grade:          uint8
```

This also lets UIs unblock "MINING…" immediately instead of waiting for
the next asteroid snapshot to carry the new `fragment_pub`.

### No-claimant fallback

If `deadline_ms` passes with zero valid claims, the server runs the
existing burst=20 ceremony against a neutral anchor (`player_pub = 0`),
adopts that grade, and broadcasts RESOLVED with `winner_pub = 0`. Solo
play and deserted sectors don't regress.

### What claimants do and don't earn

A valid claim influences the **fragment's hash only**. It does not grant
ownership, towing priority, or any payout. Rewards still flow through
normal gameplay:

- Whoever tows the fragment gets to choose where it smelts.
- The smelting station pays the tower in credits.
- The ingots enter the station's stockpile at graded value.

This keeps the PvP moment tight: every nearby player has a reason to be
watching the screen at fracture, but the economic outcome still depends
on who plays best afterward.

### Anti-abuse

- **Grade forgery** — server re-hashes with the claimant's own pub;
  mismatch → silent drop.
- **Stealing a neighbor's nonce** — derivation includes `player_pub`, so
  each claimant's grade space is disjoint. A valid nonce for Alice
  produces junk when the server re-hashes with Bob's pub.
- **Sandbagging** — only harms the sandbagger.
- **Compute DoS** — `burst_cap` ≤ 50, 3 SHA-256s per claim to verify,
  dedup by claimant before verifying. Worst case = NET_MAX_PLAYERS
  verifications per fracture.

## Recipes

Recipes are a stable `uint16` enum. Each entry names its input pattern
and produces 1..N outputs. Input class/commodity gates are *validation
rules*; they don't feed the hash. Output class is emergent — a gated
recipe raises your odds of keeping a class by limiting the inputs you're
allowed to consume, not by forcing the output hash.

```
RECIPE_SMELT           : 1 fragment           → 1 INGOT[same commodity]
                         (input is asteroid.fragment_pub, not a manifest unit)
RECIPE_FRAME_BASIC     : 2 INGOT[ferrite]     → 1 FRAME[steel]
RECIPE_LASER_BASIC     : 1 INGOT[copper] +
                         1 INGOT[silicon]     → 1 LASER
RECIPE_TRACTOR_COIL    : 2 INGOT[copper]      → 1 TRACTOR
RECIPE_LEGACY_MIGRATE  : (sentinel)           — used only by save migration
```

**No class-gated recipes in v1.** Under min-flow grade propagation, a
class gate wouldn't change rarity math (grade is input-bound regardless)
and its only value would be statistical class preservation. That's too
thin a benefit to carry the UX cost of an extra recipe variant. If
players ask for it later, add `RECIPE_FRAME_M_GATED` as a post-v1 patch
once there's data on how class actually distributes.

**No output_index in v1.** Every recipe emits exactly one output. The
formula still carries `output_index_le = 0` for forward compat with
future multi-output recipes, but the hash stays unambiguous.

One smelt produces exactly one ingot. Size isn't a useful N axis (only
S-tier fragments reach a smelter — the cascade resolves in-world), and
under min-flow grade propagation, splitting a rare fragment into N
ingots would just produce a stack of the same grade rather than a
portfolio. The rarity economy rides on the grade price multiplier alone.

```
ingot.pub    = sha256("SIGNALv1" || RECIPE_SMELT_le || fragment.pub || 0u16)
ingot.grade  = fragment.grade
```

If a multi-output variant is ever wanted (e.g., bonus ingots from XXL
fractures), the hash formula already carries `output_index`, so it's a
tuning change rather than a wire change.

## Save + wire migration

Save schema bump: v22 → v23.

**Migration v22 → v23 (one-shot, on load):**

For each commodity count in the v22 float:
1. Allocate `floor(count)` ingots in the manifest.
2. For each ingot, set
   `pub = sha256("SIGNALv1" || RECIPE_LEGACY_MIGRATE_le || owner_id_le || commodity_le || i_le)`.
3. `parent_merkle` = 32 zero bytes (sentinel for "pre-manifest era").
4. `recipe_id = RECIPE_LEGACY_MIGRATE`.
5. Discard the fractional remainder (< 1 ingot = dust).

Existing RATi v1 `named_ingot` entries map 1:1: their existing `pub`
becomes `cargo_unit_t.pub`, `recipe_id = RECIPE_SMELT`, `parent_merkle =
0` (their fragment pub wasn't persisted in v1).

Wire additions:

- `NET_MSG_FRACTURE_CHALLENGE` (new)
- `NET_MSG_FRACTURE_CLAIM` (new)
- `NET_MSG_FRACTURE_RESOLVED` (new)
- `NET_MSG_STATION_MANIFEST` (new, coexists with the existing
  `NET_MSG_STATION_INGOTS` during transition)
- `NET_MSG_HOLD_MANIFEST` (new, coexists with `NET_MSG_HOLD_INGOTS`)

The old `*_INGOTS` messages stay on the wire through slices 4–7 so v1
clients keep working. Slice 8 (delete floats) retires the old messages
and bumps the protocol version. Record size for a manifest unit: 68
bytes (2 header + 2 recipe + 32 pub + 32 merkle).

## Slices

Each slice lands green and shippable. Dual-write pattern means readers
and writers migrate independently before the float counters disappear.

1. **Types + empty manifests** (`#37`)
   Add `cargo_unit_t`, `manifest_t`, allocator helpers (`manifest_init`,
   `manifest_push`, `manifest_remove`, `manifest_find`). Attach empty
   instances to `ship_t` and `station_t` alongside existing floats. No
   behavior change. New types compile on both sides.

2. **Recipe table + hash helpers** (new `#45`)
   Introduce `recipe_id` enum, `recipe_table[]`, `hash_merkle_root()`,
   `hash_ingot()`, `hash_product()`. Unit-tested against known vectors.

3. **Client-hashed fracture** (`#44`)
   `FRACTURE_CHALLENGE` broadcast, client search loop, `FRACTURE_CLAIM`,
   server verify + best-grade-wins. Store result as
   `asteroid.fragment_pub`. No-claimant fallback to burst=20. UI: show
   the search running briefly ("MINING…") during the 500ms window.

4. **Dual-write smelt** (`#38`)
   Furnace pushes N `CARGO_KIND_INGOT` units into station manifest *and*
   increments legacy float. Ingot pub derives from `asteroid.fragment_pub`
   + output index.

5. **Dual-write fabs** (`#39`)
   Frame press, laser fab, tractor fab consume ingot units from the
   station manifest, produce one output unit via `hash_product`, push
   into station manifest. Legacy float incremented in parallel.

6. **UI reads manifest** (`#41`)
   Market tab + hold tab group units by `(kind, commodity, grade)` and
   render industrial framing ("fine ferrite × 3"). Buy/sell buttons act
   on units. Legacy float display removed.

7. **Consumers read manifest** (`#42`)
   Buy, sell, deliver, station-to-ship and ship-to-station transfers all
   operate on units. Floats become write-only.

8. **Delete floats + save v23** (`#43`)
   Remove `cargo[COMMODITY_COUNT]`, remove the dual-write paths, bump
   save schema, add v22→v23 migration on load.

## Open questions

- **Manifest caps** — ship 32, station 256 proposed. Adjust once dual-write
  runs and we see real distributions.
- **Market premium** — proposal: rely on `mining_payout_multiplier` only;
  graded units literally worth more via existing math. No extra logic.
- **Legacy ingots without fragment.pub** — migration sets `parent_merkle = 0`.
  They render as "origin unknown" in the market. Acceptable.
- **Fragment persistence across server restarts** — the client-hashed
  grade needs to survive if the fracture happens pre-crash. `fragment_pub`
  is part of asteroid save state. Fine.
- **Claim broadcast radius** — only clients rendering the fracture get the
  challenge. Players far away can't compete (and would lag anyway).
  Tentatively: same radius as signal range.
