# Construction Flow

## Status

This file describes the construction loop that is currently implemented in the
repo. The longer-term target is still the fuller physical post-placement supply
loop discussed in `#214` and `#224`, but that is not the shipped behavior
today.

## One Sentence

Shipyards manufacture typed scaffolds, players plan destinations with `B`, tow
the loose scaffold through space with the tractor, and commit placement with
`E`.

## Current Lifecycle

```
PLAYER enters plan mode with B
  → reserve slot or create planned outpost with E
    → dock at SHIPYARD and order matching scaffold
      → station inventory feeds a nascent scaffold at station center
        → completed scaffold ejects as a loose world entity
          → PLAYER tractors scaffold through space
            → press E while towing to snap onto a ring slot
              → MODULE TIMER (10s) or OUTPOST FRAME DELIVERY
                → station/module activates
```

## Current Flow

### 1. Plan

While undocked and not towing, `B` enters plan mode.

- Near an existing outpost or planned station: plan mode targets the nearest
  open slot.
- In open signal-covered space: plan mode creates a planned outpost ghost.
- In plan mode, `R` cycles the module type and `E` reserves the current slot.

While docked, `B` is a shortcut to the SHIPYARD tab if the station has one.

### 2. Order

Dock at a station with a `MODULE_SHIPYARD`, open the SHIPYARD tab, and press
`1-9` to order a scaffold that matches one of the currently planned module
types.

- Credits are charged up front.
- The order goes into that station's pending scaffold queue.
- Material cost is pulled from station inventory during manufacture, not after
  placement.

### 3. Manufacture

Pending shipyard orders create a nascent scaffold at the station center.
Station inventory feeds that nascent scaffold until its build amount reaches
the module's material cost.

When complete, the scaffold ejects as a loose world entity near the station and
can be tractored away.

### 4. Tow

The tractor beam is toggled with `R`.

- Turning the tractor off drops towed fragments and scaffolds.
- Scaffolds use spring-style tow physics and a speed cap.
- Loose scaffolds can be picked up from shipyards and moved across the map.

### 5. Place

While towing a scaffold, press `E` to place it.

- Near a planned outpost ghost: the ghost materializes into a scaffolded
  outpost.
- Near an active outpost ring: the scaffold snaps to the nearest valid open
  slot.
- Away from existing outposts: placement can found a new outpost if the
  position is inside signal coverage and respects minimum station spacing.

The old build overlay is gone. Placement is now commit-based rather than
menu-based.

### 6. Activate

There are two activation paths in the current implementation:

- **Outpost founding**: the newly founded outpost is itself scaffolded and
  still requires frame delivery before it activates.
- **Placed module scaffolds**: after snap-to-slot, placed modules currently
  enter the 10-second construction timer immediately. They do not yet require a
  second post-placement material delivery pass.

On module activation:

- `scaffold` flips off
- station services are rebuilt
- signal is recalculated if needed
- production modules can spawn NPC miners or haulers

## Implemented Data Model

### Scaffold states

The current scaffold entity supports:

- `SCAFFOLD_NASCENT`
- `SCAFFOLD_LOOSE`
- `SCAFFOLD_TOWING`
- `SCAFFOLD_SNAPPING`
- `SCAFFOLD_PLACED`

The important difference from the earlier design doc is that module scaffolds
do not currently linger in a post-placement supply state. Placement finalizes
them with `build_progress = 1.0f`, then the timer finishes the job.

### Planning model

Planning is server-side and faction-shared:

- planned outpost ghosts live in the station array
- placement plans reserve ring/slot/module-type combinations
- shipyard menus only surface scaffold types that are already present in the
  shared planned set

## Known Gaps Relative to the Target Design

- Post-placement material delivery for module scaffolds is not landed yet.
- `NPC_ROLE_TOW` autonomous scaffold delivery is still reserved, not active.
- Construction contracts are still station-level and can outlive the scaffold
  need they were originally posted for.
- Full physical intermediate-goods flow (`#266`) is still future work.

## Verification

1. Press `B` in flight to create a planned outpost or reserve a module slot.
2. Dock at Kepler or Helios, open SHIPYARD, and order a scaffold that matches
   the active plan.
3. Wait for the scaffold to eject as a loose object near the station.
4. Toggle the tractor with `R`, tow the scaffold, and press `E` to place it.
5. For a new outpost, deliver frames until the station activates.
6. For a placed module, wait out the 10-second commissioning timer.

## Follow-On Issues

- `#214` unified post-placement supply for module scaffolds
- `#216` stronger commissioning payoff
- `#224` fuller physical scaffold construction loop
- `#266` physical station output and intermediate goods
