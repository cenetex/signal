# Signal Self-Revealing Roadmap

**Status:** product direction / implementation triage  
**Audience:** contributors choosing the next work after the high-level meta
analysis and UI legibility pass.

## Thesis

The priority is making Signal more self-revealing rather than merely bigger.

Signal already has unusually rich systems: physical cargo, station-local
ledgers, signed chain history, gossip, route memory, NPC job pressure, signal
bands, construction, and thrown-rock combat. The next product step is not to
add another hidden layer. It is to make the existing layers explain themselves
through play.

The first implementation passes have already moved several items into baseline:
throw release vectors and target brackets, low-signal control warnings,
single-player station ledger strips and local-credit notices, construction
activation consequence copy, and first-pass fragment usefulness labels for
tracked work, direct station demand, and rare grade. The economy clarity pass
also made mining gates, laser upgrade unlocks, construction supply needs, and
module input/output consequences visible in normal HUD, refit, station,
station-level production summaries, and scan copy. The next pass should build
on those cues rather than re-spec them.

The immediate exception is the first gated progression step. The UI now
correctly says that Cuprite requires L2 mining and Crystal requires L3 mining,
and blocked starter laser refits expose that the first mining upgrade currently
requires Laser Modules made from Crystal Ingots plus Frames. Before adding more
economy, prove the intended bootstrap: seeded Laser Module stock, an
L3-capable NPC/import path, or a starter-reachable first upgrade recipe.

Self-revealing does not mean tutorial-heavy. It means the player can infer what
the world knows, wants, remembers, and withholds by looking at the world and
acting inside it.

## Definition

A feature is self-revealing when it answers at least one of these questions in
the normal course of play:

- What is happening?
- Why is it happening?
- Who knows it?
- How certain is it?
- What changed because I acted?
- What can I do next?

The answer should come from live game state, not invented flavor. If the sim
does not know something, the UI should not pretend it does.

## The Player Questions To Optimize For

### 1. Why is this rock valuable?

Relevant systems:

- asteroid tier, commodity, grade
- fragment provenance
- tow state
- smelt output
- contract fit
- future lineage

Self-revealing surface:

- target/tow HUD should show whether a fragment is useful, dangerous, or
  contract-relevant
- smelt/lineage surfaces should show the transformation from physical fragment
  to named cargo
- trade/contract rows should use origin and grade as player-facing meaning, not
  receipt internals

Done check:

- A new player can tell the difference between a random fragment, a valuable
  fragment, a dangerous thrown fragment, and a contract-relevant fragment before
  reading docs.

### 2. Why did my ship stop responding?

Relevant systems:

- signal quality
- signal band
- control responsiveness
- mining efficiency
- frontier push
- NPC support availability

Self-revealing surface:

- flight HUD should expose control percentage before the ship feels broken
- low-signal visual treatment should communicate loss of institution, not only
  grayscale mood
- warnings should trigger at the frontier/fringe transition, not only at total
  signal collapse

Done check:

- A player entering low signal understands "civilization is thinning here"
  before they conclude input is buggy.

### 3. Why can I spend credits here but not there?

Relevant systems:

- per-station ledgers
- station currency names
- player pubkey / pseudo pubkey compatibility
- local station balance
- pending credits
- cross-station hauling

Self-revealing surface:

- station UI should show current-station balance and other known non-zero
  balances as separate rows
- first zero-balance dock after earning elsewhere should explain that credits
  stay local
- contracts and trade should make goods, not money, feel like the bridge
  between stations

Done check:

- A player who earns at Prospect and docks at Helios can explain why the
  balance changed and what to do about it.

### 4. Why did that NPC choose that route?

Relevant systems:

- worker job offers
- structured market memory
- HNN resonance
- route risk/success memory
- contract authority
- cargo and scaffold availability

Self-revealing surface:

- NPC contact cards should show cargo, destination, and the strongest available
  "why"
- the clarity grammar should reveal whether the motive is witnessed, relayed,
  stale, or fuzzy
- route history should show repeated behavior as institutional memory, not
  debugging detail

Done check:

- Scanning an NPC gives a plausible state-grounded answer: "this worker is
  hauling ferrite to Kepler because Prospect work pressure is fresh."

### 5. Who remembers what I did?

Relevant systems:

- station chain logs
- delivery receipts
- route-history chain events
- player callsigns
- station trust/risk memory
- cargo lineage

Self-revealing surface:

- station HISTORY should distinguish local signed events, aggregate route
  memory, and carried gossip
- cargo lineage should tell a readable story from origin to current use
- station copy and contract rows should acknowledge repeated verified work

Done check:

- After a meaningful delivery or construction contribution, the player can find
  a visible trace of it without using external tools.

### 6. What changed because I built this?

Relevant systems:

- planned outposts
- scaffold lifecycle
- station module graph
- signal grid
- station production
- route viability
- NPC frontier planning

Self-revealing surface:

- construction UI should show the before/after effect of a module or relay
- signal expansion should be visible as a changed playable boundary
- stations should show what new work, routes, or production became possible

Done check:

- After placing a relay or module, the player can point to the concrete change:
  more signal, new production, safer route, new contract pressure, or new
  station capability.

## Operating Rules

### 1. Surface Before Expanding

Before adding a new economic or AI subsystem, ask whether an existing hidden
subsystem can be made visible. Prefer surfacing:

- station need
- local money
- route memory
- cargo origin
- worker motive
- signal consequence

### 2. State Before Flavor

Every new line of copy should be traceable to a state fact. Good sources:

- contract fields
- manifest rows
- station ledgers
- market memories
- route-history events
- chain-log payloads
- signal band values
- NPC job assignments

Avoid copy that implies personality, motive, or social knowledge the sim does
not actually have.

### 3. Certainty As Presentation

Do not print raw confidence unless debugging. Certainty should change:

- color intensity
- text degradation
- bracket solidity
- row priority
- whether the UI says "known", "heard", "faint", or "rumor"

This keeps uncertainty diegetic and scannable.

### 4. Default To Story, Drill Into Proof

Default player surfaces should say:

- origin
- destination
- route
- material
- station
- epoch/tick when useful
- trust/risk state

Deep inspect surfaces can show:

- pubkeys
- receipt chain length
- parent merkle
- event hashes
- signature validation

The player wants meaning first. The auditor wants bytes later.

### 5. Every Menu Needs A World Answer

Station UI should not be a generic management dashboard. Each panel should
answer a local-world question:

- SHIP: what can this dock do for my ship?
- TRADE: what does this station have and need?
- CONTRACTS: what work does this station recognize?
- HISTORY: what does this place remember?
- YARD: what infrastructure can this place make possible?

If a row does not answer one of those questions, it probably belongs in an
inspect/debug surface.

## Priority Lanes

### Lane A: Signature Verb Legibility

Goal: make thrown-rock combat and tractor tension teach themselves.

Build toward:

- release vector stubs for towed fragments
- hot/cold coloring based on release lethality
- target bracket only when a release can actually damage a ship
- clear visual distinction between towing, charging, throwing, and harmless
  dragging

Why first:

- the rock is the game's hook
- combat must remain physical
- players need to learn by motion, not a manual

### Lane B: Local Economy Legibility

Goal: make station sovereignty impossible to miss.

Build toward:

- visible local balance strip
- non-zero other-station balances when known
- first-time "credits stay local" dock note
- trade rows that frame goods as inter-station value transfer
- contract rows that name payout station/currency clearly

Why first:

- local money is the seed of hauling and federation
- hidden sovereignty feels like a bug
- the required data mostly already exists in singleplayer and partially in
  multiplayer

### Lane C: NPCs As Society

Goal: make NPC workers readable as participants in the same economy as the
player.

Build toward:

- contact cards with cargo, destination, and motive
- clarity grammar for uncertain or relayed motives
- route-history snippets for repeated runs
- no fake personality beyond current evidence

Why first:

- solo play is the bar
- the NPC economy is the cast when player population is low
- worker behavior already has more cause than the player can see

### Lane D: Station Memory

Goal: make stations feel like institutions rather than shops.

Build toward:

- HISTORY rows that separate signed events, route memory, and gossip
- station-local "known for" summaries derived from chain/history data
- visible station trust/risk consequences
- route drill-down once aggregate memory is too dense for compact strips

Why first:

- memory is the metagame
- station identity depends on remembered local truth
- signed history should be meaningful before it is externally audited

### Lane E: Construction Consequence

Goal: make building reveal its effect immediately.

Build toward:

- planned module previews showing expected station capability
- relay placement previews showing signal reach changes
- post-commission feedback that names what changed
- station work updates caused by new modules or repaired routes

Why first:

- the macro game is infrastructure
- players need to feel construction as world change, not checklist progress
- signal expansion is the central civilization verb

## Kill Criteria

Avoid work that makes the game larger but not more self-revealing:

- new commodities without clearer cargo meaning
- new station panels that do not answer a local-world question
- new AI scoring factors with no inspectable motive
- new provenance fields shown only as hashes
- new contracts that do not explain origin, destination, payout, and fit
- new signal mechanics that only appear as hidden multipliers
- new verification surfaces that lead with proof before player meaning

## Acceptance Tests For A Self-Revealing Slice

A slice counts if a player can answer one new real question without reading
external docs.

Good examples:

- "I know this fragment can hurt that ship because the release vector is hot
  and bracketed."
- "I know I cannot spend Prospect credits here because the ledger strip shows
  Helios has its own zero balance."
- "I know this hauler is going to Kepler because it is carrying frames and the
  contact card says the route memory is fresh."
- "I know this cargo matters because its lineage says it came from a fragment I
  smelted at Prospect."
- "I know this relay mattered because the signal boundary moved and the route
  warning changed."

Weak examples:

- "The UI has more numbers."
- "The station has another tab."
- "The NPC line sounds more flavorful."
- "The receipt chain is technically visible."
- "The docs explain it."

## Suggested Next Backlog

1. Convert scan/hail NPC output into stable clarity-based contact cards that
   lead with cargo, destination, and motive.
2. Add a dock-level station memory summary: "known for" and "you here" from
   signed local state, with gossip styled as lower certainty.
3. Deepen fragment usefulness beyond direct demand: smelt destination, route
   memory, and nearby untracked work.
4. Add multiplayer known-ledger snapshots so the single-player ledger strip can
   render honestly in public sessions.
5. Add a compact lineage story view for selected cargo.
6. Add route memory drill-down for HISTORY once compact rows overflow.
7. Tune throw and low-signal cues only from playtest evidence.

## Final Rule

If a system is important enough to simulate, it is important enough to make
perceptible.

If making it perceptible would expose that the system is not yet meaningful,
that is useful product information.
