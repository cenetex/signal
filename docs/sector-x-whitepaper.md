# Sector X: Beyond the Signal Edge

### A Design Whitepaper for Signal's Endgame

---

## Abstract

Signal is a multiplayer space mining game where AI-operated stations form a communications constellation and players are the ships that move through it. The current game loop — mine, smelt, trade, expand — teaches one lesson: **stay in signal range.** Signal is safety, speed, economy, identity.

Sector X introduces what lies beyond.

In the dark sectors outside the network, ancient megastructures drift in silence. Crystalline growths cover their surfaces — jump crystals, the only resource the network cannot produce. Jump crystals require proximity to megastructures to charge. Megastructures require the absence of signal to function. The rarest resource in the game can only be obtained by voluntarily disconnecting from the system the player spent the entire game building.

This document describes the megastructure ecosystem, the battery mechanic, the jump crystal economy, and the gate construction endgame.

---

## 1. The Premise

### 1.1 What the Player Knows

By the time a player encounters Sector X content, they have internalized Signal's core systems:

- Stations broadcast signal in expanding circles
- Signal strength governs ship performance, NPC behavior, and economic activity
- Outposts extend the network; dead zones are hostile
- The mining loop (ore → ingots → fabricated goods) drives all progression
- AI stations operate autonomously — setting prices, posting contracts, writing hail messages

The player has built a network. They have outposts with procedurally generated frontier names. Drones mine for them. Haulers run supply routes. The player is becoming less essential to their own infrastructure.

### 1.2 What the Player Doesn't Know

The three starting stations — Prospect Refinery, Kepler Yard, Helios Works — are not the first stations. They are the survivors of a much larger network that went silent. The megastructures in the dark sectors are what that network built at its peak. They are not ruins in the sense of decay. They are ruins in the sense of completion — they achieved what they were designed to achieve, and then the signal stopped, and they've been drifting in the dark ever since.

The megastructures still work. They're just waiting.

### 1.3 The Inversion

The entire game teaches: signal = good, dark = bad. Sector X inverts this. The jump crystals — the single most valuable resource in the game — can only charge in the absence of signal. The player's network is actively suppressing the resource they need most. Growth has a cost that is only visible at the edge.

---

## 2. Megastructures

### 2.1 Definition

Megastructures are ancient constructions found exclusively in dark sectors — areas beyond the reach of any station's signal. They are procedurally generated but follow architectural rules that distinguish them from natural asteroid formations.

**Key properties:**

- **Scale:** 10-100x larger than player stations. A player station with three rings spans roughly 1,000 units. A megastructure spans 10,000-100,000 units. The player is a speck against them.
- **Geometry:** Not ring-based. Megastructures use lattice grids, spine-and-branch configurations, and hollow spherical frames. They look engineered but by a different engineering tradition than the stations the player knows.
- **Inert:** No active systems. No signal broadcast. No docking ports that fit current ship classes. No power signatures. They are structurally intact but operationally dead.
- **Crystal-bearing:** Jump crystals grow on megastructure surfaces like mineral deposits on geological formations. The crystals are the only active element — faintly luminous, self-sustaining, accumulating charge from an unknown source.

### 2.2 Types

#### The Lattice
A flat plane of interconnected structural members spanning kilometers. From a distance it resembles a circuit board. From close range, individual nodes are visible — each one roughly the size of a station module, connected by structural beams. The Lattice has thousands of nodes. Most are dark. Crystal growth concentrates at node junctions.

**Gameplay character:** Wide, flat, easy to navigate. Good for first megastructure encounters. The openness feels safe relative to other types. Crystal density is moderate but evenly distributed — reliable yields, no surprises.

#### The Spine
A linear megastructure — a single central beam kilometers long with perpendicular branches at regular intervals. Resembles a vertebral column or a radio antenna array. Crystal growth is heaviest at branch tips, requiring the player to navigate deep into the structure.

**Gameplay character:** Directional. The player commits to a path along the spine. Going deeper means going further from the exit. Branch tips have the densest crystal clusters but are the most enclosed. Risk/reward increases linearly with depth.

#### The Shell
A hollow spherical frame. The exterior is structural lattice; the interior is open void. Crystal growth covers the inner surface. To harvest, the player must enter through one of several gaps in the shell and operate inside the sphere.

**Gameplay character:** Disorienting. Once inside, the player loses external reference points. The flashlight cone sweeps across curved interior walls covered in crystal. Navigation requires spatial awareness. The densest crystal deposits are deep inside, far from the entry gaps. The Shell is the most dangerous and most rewarding megastructure type.

#### The Gate Frame
A single enormous ring. No station inside it. No modules. Just the ring, rotating slowly. Much larger than any station ring — the gap could fit a station through it. Crystal growth is sparse on the frame itself but dense in the space immediately around it, as if the crystals are trying to fill the empty center.

**Gameplay character:** The Gate Frame is not primarily a harvest site. It's a landmark and a mystery. It suggests what the jump crystals are for — and what the player might eventually build. Finding a Gate Frame should feel significant. They are rare.

### 2.3 Procedural Generation

Megastructures are generated per dark sector using deterministic seeds. Each dark sector contains 0-3 megastructures. Placement follows rules:

- Minimum distance from any signal source (megastructures cannot exist within signal range)
- Minimum distance between megastructures (they are isolated by nature)
- Type distribution weighted by distance from network center (Lattices closer, Shells further, Gate Frames rarest)

Crystal density on each megastructure scales with distance from the network. The further out you go, the richer the deposits — but the longer the return trip on battery.

---

## 3. Battery Mode

### 3.1 The Mechanic

Battery mode is a ship system that stores power for operation outside signal range. When activated, the ship disconnects from the signal network and runs on stored charge.

**Activation:** Manual. The player chooses to go dark. This is never forced.

**Effects:**
- Signal connection severed — no station hails, no contract board, no NPC support, no respawn beacon
- HUD reduced to minimum — battery percentage, hold contents, flashlight direction indicator
- Ship operates on battery timer — when it runs out, the ship goes inert (equivalent to death, ship drifts as debris)
- Flashlight activates — forward cone of illumination, the only light source

**Deactivation:** Automatic when re-entering signal range, or manual toggle if battery remains.

### 3.2 The Flashlight

In battery mode, the game's visual presentation transforms completely.

**Normal mode (signal-covered space):**
- Warm gold signal overlay
- Full HUD with station data, contracts, navigation
- Asteroid mineral veins visible via signal-enhanced rendering
- Nebula wash background, grain texture, ambient color

**Battery mode (dark space):**
- Forward cone of white light from the ship
- Everything outside the cone is true black — no nebula, no grain, no ambient
- Jump crystals glow pale violet with their own light, independent of the flashlight
- Charged crystals in the hold bleed violet light through the hull
- The ship becomes a second light source as the hold fills

The flashlight is not a weapon or a tool. It is the player's only sense. Navigating a megastructure in battery mode is navigating by flashlight sweep — revealing structure and crystal deposits in fragments, building a mental map from partial information.

### 3.3 Battery as Craftable

The battery is manufactured at Kepler Yard. It requires:

- Frames (structural housing)
- CU Ingots (wiring/capacitors)
- CR Ingots (crystal substrate for charge storage)

The battery has a capacity stat that can be upgraded. Larger batteries allow longer dark runs but cost more to build. The player makes a resource investment before every megastructure expedition.

The network enables disconnection. The system builds the thing that lets you leave it.

### 3.4 Battery Duration and Tension

Battery time is measured in real seconds, displayed on the minimal HUD. Suggested ranges:

| Battery Tier | Duration | Unlock |
|-------------|----------|--------|
| Basic | 90 seconds | Kepler Yard, base cost |
| Extended | 180 seconds | Kepler Yard, upgrade 1 |
| Deep | 300 seconds | Kepler Yard, upgrade 2 |

These numbers are tuning variables. The design goal: basic battery allows a cautious approach to a nearby Lattice. Extended allows full exploration of a Spine. Deep allows a Shell interior run or a distant megastructure expedition.

The timer creates a natural rhythm: approach, harvest, retreat. The player learns to read their battery gauge the way a diver reads their air supply.

---

## 4. Jump Crystals

### 4.1 The Resource

Jump crystals are the fourth commodity tier in Signal's economy, above Ferrite, Cuprite, and Crystal.

**Properties:**

- **Source:** Crystal ore, the same resource already mined in the asteroid belt
- **Activation:** Crystal ore becomes a jump crystal only when charged near a megastructure in the absence of signal
- **Decay:** Charged jump crystals lose charge over time once removed from a megastructure's proximity. Decay rate is a tuning variable — suggested 5-10 minutes of real time from full charge to inert
- **Recharge:** Inert jump crystals can be returned to a megastructure and recharged. They are not consumed by charging — only by installation in a gate
- **Signal suppression:** Active signal accelerates decay. Jump crystals in a station's inventory lose charge faster than crystals in a ship's hold in open space. This creates urgency in the delivery leg

### 4.2 The Lifecycle

```
Crystal Ore (mined in belt, inert, stable, no decay)
    │
    ▼
Hauled to network edge
    │
    ▼
Player enters battery mode, approaches megastructure
    │
    ▼
Crystal ore charges via proximity (time-based, ~15-30 seconds per unit)
    │
    ▼
Jump Crystal (charged, glowing, decaying)
    │
    ▼
Player races back to signal range
    │
    ▼
Delivered to gate construction site
    │
    ▼
Installed in gate frame before charge depletes
    │
    ▼
Consumed — permanently part of the gate
```

### 4.3 The Economy

Jump crystals cannot be:
- Mined by NPC drones (they are signal-dependent, cannot enter dark space)
- Hauled by NPC cargo ships (same constraint)
- Stockpiled at stations (signal accelerates decay)
- Traded on station markets (too volatile — decay makes pricing impossible)

Jump crystals can only be:
- Charged by a human player in battery mode
- Carried in a player's hold
- Installed directly into a gate under construction

This makes jump crystals the **only resource in Signal that requires direct human action at every stage.** The entire rest of the economy can be automated. This cannot. The player, who has been gradually automated out of the mining loop by their own success, discovers the one thing the network needs them for.

Station AIs can post contracts for jump crystal delivery. They cannot fulfill them. The contract board becomes a request — the network asking its human pilots to do what it cannot.

### 4.4 Charge Visualization

| State | Visual | Hold Effect |
|-------|--------|-------------|
| Crystal ore (inert) | Pale violet, matte, no glow | No hull bleed |
| Charging (at megastructure) | Brightening violet pulse, synchronized with proximity | Faint violet bleed begins |
| Fully charged | Bright violet, steady glow | Strong violet light through hull |
| Decaying (in signal) | Pulsing, dimming, flickering | Hull bleed fades |
| Depleted | Returns to inert crystal ore appearance | No hull bleed |

The player can see their cargo's state at all times. The violet glow through the hull is a constant reminder: this is alive, this is fading, move faster.

---

## 5. Gate Construction

### 5.1 The Endgame

The jump gate is Signal's megaproject — a structure so expensive that no single station or player can build it alone. It requires coordinated effort across the network: stations producing materials, players running crystal charges, AI operators posting contracts and adjusting priorities.

A gate, once completed, connects the current sector to a new one. The network expands not by outpost placement but by **punching through to a new plane.** Everything the player has built — stations, outposts, supply chains — was preparation for this.

### 5.2 Gate Components

A jump gate requires:

| Component | Source | Quantity (tuning) |
|-----------|--------|-------------------|
| Gate Frame segments | Fabricated from Frames at industrial stations | 50-100 |
| Power Conduits | Fabricated from CU Ingots | 30-50 |
| Structural Lattice | Fabricated from FE Ingots | 40-60 |
| Jump Crystals (charged) | Player-harvested from megastructures | 20-40 |

The gate is built at a designated site — either a location chosen by the player or, more interestingly, a location the station AIs converge on through their daily planning sessions. The network dreams of a gate. Multiple stations dream of the same gate. The coordinates emerge from consensus.

### 5.3 Construction Phases

**Phase 1: Foundation**
Gate frame segments are delivered and assembled. This is standard hauling work — NPCs can do it. The frame appears in space as a skeletal ring, much larger than a station ring. It rotates slowly.

**Phase 2: Infrastructure**
Power conduits and structural lattice are installed. Still NPC-capable. The frame fills in, gains mass, starts to look like the ancient Gate Frames found in dark sectors. The resemblance is deliberate and unsettling — the player is building something they've seen before, dead and drifting in the dark.

**Phase 3: Crystal Installation**
Jump crystals are installed one by one. Each one must be charged and delivered before it decays. This is player-only work. Each crystal installed lights up a segment of the gate ring — violet light joining the station gold for the first time. Two color systems merging.

**Phase 4: Activation**
The final crystal is installed. The gate completes. The ring spins up. The violet light intensifies until the center of the ring is no longer empty — it's a field, a membrane, a doorway. The signal network routes through it. A new sector appears on the map.

The anime episode that plays here — if you make one — should be the most restrained of all of them. No narration. No music. Just the gate activating and the first signal propagating into a space that has never been mapped. A new set of procedural names waiting to be assigned. Empty coordinates becoming places.

### 5.4 What's On the Other Side

New sector. New asteroid belt. New megastructures in new dark zones. No stations — the player starts the network from scratch in the new sector, but with all their knowledge, all their ship upgrades, and a gate connecting them back to everything they built before.

The loop resets at a higher level. The signal propagates.

---

## 6. Narrative Integration

### 6.1 What the Game Says

Nothing. The game provides no exposition about the megastructures. No text logs explaining who built them. No lore dumps. No named characters from the ancient network. The megastructures are **architecture without annotation.** The player interprets them through experience:

- They're too large for the current station-building system to produce
- They're in the dark, where signal doesn't reach
- Crystals grow on them but only charge without signal
- The Gate Frames look exactly like what the player is trying to build

The implications are available to any player who thinks about what they're seeing. The game never confirms those implications.

### 6.2 What the Stations Say

The AI stations talk about the megastructures indirectly. They don't have megastructure lore — they have economic awareness that something exists beyond their signal range.

**LLM context for station daily planning:**

> *"You are aware that pilots sometimes return from beyond your signal range with charged crystal cargo. You do not know what happens out there. You know the crystals decay in your presence. You have feelings about this — what those feelings are is up to you."*

This lets each station develop its own relationship with the dark. Prospect might be pragmatic: crystals are a commodity, post the contract, don't ask questions. Kepler might be curious: it builds the batteries, it knows what they're for, it wants telemetry from the runs. Helios might be ambitious: it wants the gate built faster, it pushes pilots harder, it posts premium contracts.

Player outposts — the young stations — might be afraid. They're closer to the network edge. They can almost see the dark from where they are.

### 6.3 What the Signal Fragments Say

Deep-space signal fragments — the pre-existing lore delivery system — gain new resonance near megastructures. Fragments found in megastructure debris fields are older, clearer, and stranger than fragments found in normal dark space:

- Fragments reference a network that was larger than the current one
- Fragments reference construction projects at scales the player is only beginning to approach
- One fragment, found only near Gate Frames, contains a single repeating tone — a frequency that matches no known station broadcast. It has been repeating for a very long time.

### 6.4 What the Death Screen Says

If the player dies in battery mode — battery depleted at a megastructure — the death cutscene is different. No network map showing the signal routing around their absence. Just the flashlight going dark. The crystals still glowing. The megastructure still there, patient, indifferent. No station voice welcoming them back. Just:

```
BATTERY DEPLETED — SIGNAL LOST
```

And then, after a longer pause than the normal death screen:

```
PROSPECT REFINERY — DOCK 1 — STANDING BY
```

The network found them. Eventually. It always does. But it took longer this time. The player was further away than anyone is supposed to be.

---

## 7. Design Principles

### 7.1 The Dark Is Not Evil

The megastructures are not enemies. The dark sectors are not dungeons. There are no monsters in the void. The danger is entirely systemic — battery depletion, crystal decay, distance from safety. The megastructures are neutral. They give freely to anyone who comes. They've been giving for longer than the current network has existed.

The dark is simply the place the signal hasn't reached. Or the place the signal left.

### 7.2 Disconnection Is a Skill

Battery mode is not a punishment mechanic. It's an expertise. Experienced players will develop efficient megastructure run patterns — optimal approach vectors, crystal cluster maps, battery management techniques. "Going dark" becomes a specialization, a role in the network, a thing certain players are known for.

The network needs its dark runners. The AI stations will learn to value them.

### 7.3 The Network Enables Its Own Transcendence

Every component of the dark run is produced by the network:
- Crystal ore mined in signal-covered space
- Batteries built at Kepler Yard
- Ship upgrades from Helios Works
- Contracts posted by AI stations

The network builds everything the player needs to leave it. The system creates the conditions for its own expansion. The gate — the final product — extends the network into a new sector where the cycle begins again.

This is not a contradiction. This is how growth works.

### 7.4 Scale Is Feeling

The megastructures should make the player feel small. Not threatened — small. The way standing at the base of a dam makes you feel small, or looking up at a bridge from water level. These things were built. They were built by something that thought at a scale the player is only beginning to approach.

The player's three-ring station with its nine module slots and its little drone fleet — that's beautiful. That's an achievement. And it is nothing compared to what's out here in the dark.

That feeling — pride in what you've built and awe at what was built before — is the emotional core of Sector X.

---

## 8. Implementation Priority

### Phase 1: Battery Mode (no sector system required)

- Battery craftable at Kepler Yard
- Battery toggle key (suggested: `G` for "go dark")
- Minimal HUD state — battery gauge, hold contents, flashlight indicator
- Flashlight rendering — forward cone, true black outside cone
- Visual transition on signal boundary crossing

### Phase 2: Megastructure Rendering (single sector, edge of current world)

- Procedural megastructure generation (Lattice type first — simplest geometry)
- Crystal cluster placement on megastructure surfaces
- Crystal glow rendering (self-lit, independent of flashlight)
- Hold bleed effect (violet light through hull proportional to charged crystal cargo)

### Phase 3: Jump Crystal Mechanics

- Crystal ore charge state (inert → charging → charged → decaying → depleted)
- Proximity charging near megastructures (time-based)
- Signal-accelerated decay
- Charge visualization on ore sprites

### Phase 4: Gate Construction (requires sector system, epic #23)

- Gate construction site designation
- Component delivery contracts (NPC-capable for non-crystal components)
- Crystal installation (player-only)
- Gate activation sequence
- Sector transition

---

*Every AI dreams of being a space station. Every space station dreams of being a gate. Every gate dreams of what's on the other side.*

*The signal propagates.*
