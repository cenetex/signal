# Physical Construction

## One Sentence

Scaffolds are manufactured at shipyards, towed through space, placed on station rings, supplied with materials, and activated — all using game verbs, no menus.

## Lifecycle

```
SHIPYARD produces typed scaffold (world object near yard)
  → PLAYER/NPC tows scaffold through space (tractor beam, spring physics)
    → RELEASE near open ring slot → scaffold snaps into place
      → SUPPLY materials (haul frames/ingots to placed scaffold)
        → CONSTRUCTION TIMER (10s)
          → MODULE ACTIVATES (services, NPCs, corridors)
```

Every phase is physical. Every phase is interruptible. Every phase is visible.

## Five Phases

### 1. Assemble (at shipyard)

Dock at a station with a SHIPYARD module. Buy a typed scaffold — "Furnace (FE) scaffold", "Frame Press scaffold", etc. Credits paid at purchase. Module type is baked in.

The scaffold spawns as a **world entity** near the shipyard dock. It floats there until someone tows it away.

Starter stations (Kepler Yard) act as the initial shipyard. Player outposts need a SHIPYARD module to produce scaffolds — this is the bootstrapping constraint that makes shipyard placement strategic.

**Outpost founding** is the special case: the scaffold kit becomes the station itself. Buy at any shipyard, carry as cargo (it's small enough), fly to location, place with B. This flow is unchanged.

### 2. Tow (through space)

Hook the scaffold with tractor beam. Spring physics, speed cap, tether line — same mechanics as towing ore fragments, but heavier. Scaffold mass determines tow speed.

Scaffolds are **vulnerable in transit**. Other players can intercept. PvP emerges naturally.

NPC role: `NPC_ROLE_TOW` picks up scaffolds and tows them to stations posting construction contracts.

### 3. Place (at station ring)

Release the scaffold near an open slot on the target station's ring. Spatial detection determines which slot — no menu picker needed. The scaffold snaps to the ring position and begins rotating with the ring.

Visual: ghost/wireframe circle locks into ring geometry. Construction-amber color.

### 4. Supply (deliver materials)

A supply contract is auto-generated when the scaffold is placed. The station needs N units of a specific material:

| Module Type | Material | Quantity |
|-------------|----------|----------|
| Furnace (FE) | Frames | 60 |
| Furnace (CU) | Cuprite Ingots | 100 |
| Furnace (CR) | Crystal Ingots | 140 |
| Frame Press | Frames | 80 |
| Laser Fab | Cuprite Ingots | 80 |
| Tractor Fab | Crystal Ingots | 80 |
| Repair Bay | Frames | 30 |
| Ore Buyer | Frames | 40 |
| Others | Frames | 20-40 |

Materials arrive via:
- Player docking and delivering cargo (existing `step_module_delivery()`)
- NPCs fulfilling supply contracts → station inventory → routed to scaffold
- Other players delivering to the contract

`build_progress` advances as materials arrive. 0.0 → 1.0.

### 5. Activate (commissioning)

Materials complete → 10s construction timer (`build_progress` 1.0 → 2.0) → module goes live.

On activation:
- `scaffold = false`
- Services rebuild (ore buying, repairs, etc.)
- Corridors appear connecting to adjacent modules
- NPCs spawn if production module (miners for furnaces, haulers for fabs)
- Signal chain recalculated if relay
- Visual/audio commissioning moment

## What Gets Deleted

- **BUILD overlay** (`build_overlay`, `build_ring`, `build_slot`) — the docked menu for module selection
- **B key docked behavior** — no more BUILD menu when docked
- **`intent.build_module` / `intent.build_module_type`** — menu-driven construction intents
- **Dual-path split** in input.c (starter station scaffold shop vs. player outpost BUILD menu)
- **Blueprint Desk module** — replaced by SHIPYARD

## What Gets Added

### Scaffold world entity

```c
typedef struct {
    bool active;
    module_type_t module_type;  // what it becomes
    int owner;                  // player who bought it
    vec2 pos;
    vec2 vel;
    float radius;               // collision, ~30-40 units
    float mass;                 // affects tow speed
    enum { SCAFFOLD_LOOSE,      // floating near shipyard
           SCAFFOLD_TOWING,     // attached to player/NPC tractor
           SCAFFOLD_PLACED      // snapped to ring slot, awaiting supply
    } state;
    int placed_station;         // station index when PLACED
    int placed_ring;
    int placed_slot;
} scaffold_t;

#define MAX_SCAFFOLDS 16
```

When state transitions from `SCAFFOLD_PLACED`, it becomes a `station_module_t` with `scaffold=true` — the existing module construction system takes over for supply + activation.

### Tow generalization

Current `towed_fragments[]` on ship indexes asteroids only. Add:

```c
int16_t towed_scaffold;  // scaffold index, -1 = none
```

Tow physics (spring, speed cap, tether) shared between fragments and scaffolds. Scaffolds are heavier → lower speed cap.

### Snap-to-slot

When a towed scaffold is released within range of a station ring with open slots:
- Find nearest open slot on nearest ring
- Scaffold lerps to slot world position over ~0.5s
- Transitions to `SCAFFOLD_PLACED`
- Converts to `station_module_t` with `scaffold=true, build_progress=0.0`
- Supply contract generated
- Scaffold entity deactivated (module system owns it now)

### NPC_ROLE_TOW

New NPC role. Behavior:
- Check for scaffolds in `SCAFFOLD_LOOSE` state near home station
- Check for construction contracts at nearby stations
- Pick up scaffold, tow to destination station
- Release near open ring slot

Uses same tow physics as player, same speed cap.

## What Stays

- **`has_scaffold_kit` / `scaffold_kit_type`** on ship — outpost founding only (the one case where scaffold IS the station)
- **`step_module_delivery()`** — supply delivery from docked cargo
- **`step_module_construction()`** — build timer + inventory routing
- **Supply contract system** — scaffolds generate contracts, NPCs/players fulfill them
- **All tow spring physics** — generalized to cover scaffolds
- **Collision geometry emitter** — already supports `is_scaffold` flag
- **Outpost founding flow** — unchanged (scaffold kit → place → supply → activate)

## Scaffold Prices (credits, at shipyard)

| Module | Price |
|--------|-------|
| Dock | 100 |
| Signal Relay | 150 |
| Furnace (FE) | 200 |
| Ore Buyer | 150 |
| Ore Silo | 100 |
| Frame Press | 300 |
| Furnace (CU) | 400 |
| Furnace (CR) | 600 |
| Laser Fab | 300 |
| Tractor Fab | 300 |
| Repair Bay | 200 |

## Bootstrap Problem

Your first outpost has no shipyard. Solutions:
- Starter stations (Kepler Yard) have a built-in shipyard that sells scaffolds
- First quest (#269) guides the player through buying a scaffold at Kepler → towing to outpost → supplying → activating
- Player outpost needs a SHIPYARD module before it can produce scaffolds for self-expansion
- SHIPYARD scaffold is itself bought at Kepler and towed to the outpost — the chicken-and-egg moment

## Implementation Order

1. **Scaffold entity** — `scaffold_t` array, spawn at shipyard on purchase, basic physics
2. **Tow generalization** — `towed_scaffold` on ship, shared spring/speed-cap logic
3. **Snap-to-slot** — release near ring → spatial detection → convert to module scaffold
4. **Supply delivery** — already half-built; ensure inventory routing works for NPC deliveries
5. **Commissioning** — activation effects, corridor appearance, NPC spawn
6. **NPC_ROLE_TOW** — NPC picks up loose scaffolds, tows to contract destinations
7. **BUILD overlay deletion** — remove menu, B-key docked path, related intents
8. **Outpost founding alignment** — verify founding still works, update onboarding text

## Verification

1. Dock at Kepler → buy Furnace scaffold → it appears floating near station
2. Tractor the scaffold → tow it across space → spring physics, speed cap, tether visible
3. Release near your outpost ring → scaffold snaps to open slot
4. Supply contract appears → haul frames → `build_progress` advances
5. Timer completes → furnace activates with visual payoff
6. NPC picks up loose scaffold → tows to station with construction contract
7. No BUILD overlay exists. B key does nothing while docked (or repurposed).
8. Outpost founding unchanged: scaffold kit → place in open space → supply → activate
9. All tests pass, Emscripten build succeeds

## Issues Absorbed

- #214 (Unified scaffold construction)
- #216 (Commissioning payoff)
- #217 (Outpost founding pass)
- #224 (Module scaffolds as manufactured goods)
- #266 (Physical station output — partial; scaffold supply side)
- #267 (Typed scaffold kits — superseded)
- CONSTRUCTION_PLAN.md (this document, rewritten)
