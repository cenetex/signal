# Signal Self-Revealing Gap Analysis

**Status:** detailed product/UX gap analysis  
**Audience:** contributors choosing implementation slices after the legibility
pass  
**Scope:** current worktree as of this review; especially
`docs/self-revealing-roadmap.md`, `client/hud.c`, `client/world_draw.c`,
`client/station_ui.c`, `server/game_sim.c`, `server/gossip.c`, and
`shared/types.h`.

## Executive Read

Signal is now past the "nothing is legible" phase. Several high-value surfaces
already exist:

- throw previews render release vectors and target brackets from live slingshot
  physics
- compact flight HUD exposes signal band, mining efficiency, and control
  percentage below operational signal
- docked trade rows hide forensic receipt clutter by default and can attach
  representative lineage when the local manifest proves the whole row
- contract rows show immediate step, route/cargo, and payout currency
- NPC scan output uses the clarity grammar and station/route memory signals
- station HISTORY can summarize route memory and signed local events

The remaining product risk is subtler: the game often reveals facts, but not
the rule the player needs to infer from those facts. The biggest gaps are
cross-station economy, low-signal thresholds, NPC contact packaging,
construction consequence, and player-facing station memory summaries.

## Gap Scale

- **Closed:** current code appears to satisfy the roadmap requirement.
- **Partial:** live state exists, but the player question is not reliably
  answered.
- **Open:** the relevant state exists or is implied, but the normal UI does not
  reveal it.
- **Unknown:** insufficient evidence in this pass; needs runtime or design
  validation.

## Requirements Basis

The roadmap defines a self-revealing feature as one that answers what is
happening, why it is happening, who knows it, how certain it is, what changed,
or what to do next. It then names six player questions:

1. Why is this rock valuable?
2. Why did my ship stop responding?
3. Why can I spend credits here but not there?
4. Why did that NPC choose that route?
5. Who remembers what I did?
6. What changed because I built this?

This gap analysis evaluates the current code against those questions rather
than against whether a screen has more text.

## Gap Matrix

| Player question | Current state | Gap | Severity | Next slice |
|---|---:|---|---:|---|
| Why is this rock valuable? | Partial | Throw danger is mostly visible; contract fit and fragment value are still not unified at target/tow time. | P1 | Target/tow usefulness labels tied to contract fit and grade. |
| Why did my ship stop responding? | Partial | CTRL% is visible below operational signal, but the prominent warning still waits until near-zero signal. | P1 | Frontier/fringe low-signal banner with control percentage. |
| Why can I spend credits here but not there? | Open | Current dock balance is visible, but cross-station ledger state is not. | P0 | Station ledger strip and first zero-balance explanation. |
| Why did that NPC choose that route? | Partial | Inspect data has motive and clarity; the contact card does not consistently lead with cargo, destination, and strongest "why." | P1 | Contact-card rewrite over existing inspect rows. |
| Who remembers what I did? | Partial | HISTORY exists, but station identity and personal traces are not summarized in normal station framing. | P1 | Station "known for" / "you here" memory rows. |
| What changed because I built this? | Partial | Construction progress, queue, notices, and rejection messages exist; post-build capability changes are not named. | P1 | Module/outpost activation feedback with unlocked capability. |

## Evidence Map

These are the concrete code/doc hooks behind the matrix above.

| Area | Evidence |
|---|---|
| Self-revealing criteria | `docs/self-revealing-roadmap.md:23` defines the six answer types; `docs/self-revealing-roadmap.md:38` starts the six player questions. |
| Throw danger | `client/world_draw.c:42` mirrors server throw base speed; `client/world_draw.c:2072` computes preview speed/hotness; `client/world_draw.c:2104` draws hot/cold arrows; `client/world_draw.c:2161` gates lock brackets. |
| Low-signal HUD | `client/hud.c:2034` only draws `[ SIGNAL LOST ]` below near-zero signal; `client/hud.c:3964` shows band/percent; `client/hud.c:3965` adds mining/control telemetry below operational signal. |
| Current-station balance | `shared/types.h:641` stores station currency names; `shared/types.h:651` stores station-local ledgers; `server/game_sim.c:4877` emits hail balance for one station; `client/station_ui.c:600` renders current dock balance. |
| Contract payout currency | `client/station_ui.c:2749` selects destination-station currency first for contract payout rows. |
| Trade lineage | `client/station_ui.c:1153` only attaches station-stock lineage when the row is fully represented by local manifest data; `client/station_ui.c:1251` applies the same caution for player-held sell rows. |
| NPC contact/motive | `client/hud.c:1395` renders the NPC contact ticker; `client/hud.c:1451` renders contact identity; `client/hud.c:1461` renders role/state/home/destination; job motive helpers sit around `client/hud.c:928`. |
| Station gossip/memory | `client/station_ui.c:1633` renders OVERHEARD rows; `client/station_ui.c:1690` renders compact route HISTORY rows; `client/station_ui.c:1836` renders aggregate history; `server/gossip.c:786` promotes repeated route memory into chain-log history. |
| Construction feedback | `client/station_ui.c:2920` previews scaffold kit ordering; `client/station_ui.c:2992` renders construction queue state; `client/main.c:838` handles module activation; `client/main.c:849` currently emits only `"<module> online."`. |

## Detailed Findings

### 1. Rock Value And Throw Legibility

**Status:** Partial, leaning closed for combat danger.

**Evidence of current strength:**

- `client/world_draw.c` mirrors the server release floor with
  `ROCK_THROW_BASE_SPEED` so the preview is grounded in real throw physics.
- `throw_preview_for_fragment()` computes predicted release velocity,
  compares it to a damage threshold, and derives hotness from that result.
- `draw_throw_arrow()` maps hotness into length, color, opacity, and arrowhead
  size.
- `draw_throw_locks()` draws target brackets only when the hottest preview can
  plausibly hit a player or NPC target.

**Remaining gap:**

The player can learn "this release is dangerous," but not yet "this rock is
valuable to the economy" in the same world-space moment. Tow beams show rarity
and stretch, trade/contract rows can show cargo lineage later, and contracts
can test fit on hail/board logic, but the target/tow surface does not combine:

- grade
- commodity
- contract relevance
- station demand
- smelt/lineage future

The result is a split mental model: rock-as-weapon is visible in flight; rock as
future institutional value is mostly visible only after docking.

**Impact:**

This weakens the core "matter becomes history" loop. A player may understand
how to throw rocks before understanding why a specific fragment matters
economically or socially.

**Recommended implementation:**

Add a compact target/tow usefulness line driven by live state:

- `contract fit` when a towed/targeted fragment satisfies tracked or nearby
  station work
- `wanted at <station>` when local market memory or station demand makes it
  useful
- `high grade` / grade color when no stronger economic reason exists
- `throw hot` / bracket behavior remains the combat affordance

**Acceptance test:**

With a tracked tractor contract and a towed matching fragment, the player sees
that the same object is both physically dangerous when hot and economically
useful for a named station.

### 2. Low-Signal Control Legibility

**Status:** Partial.

**Evidence of current strength:**

- Compact HUD prints signal band and percent.
- When signal is below operational, compact HUD also prints mining and control
  percentages (`M%d%% CTRL%d%%`).
- Shared signal model names the relevant bands and control curve.

**Remaining gap:**

The prominent warning path still only renders `[ SIGNAL LOST ]` when signal is
below `0.01`. That is too late for the self-revealing goal. The player can lose
meaningful control in the frontier/fringe transition before the central warning
frames the experience as a consequence of signal.

The current state answers "what is my control percentage?" for a scanning
player, but not reliably "why did my ship start feeling wrong?" for a player
focused on flight.

**Impact:**

Low signal can still read as input lag or broken controls, especially in the
first frontier encounter.

**Recommended implementation:**

Replace the binary lost-only warning with a thresholded low-signal callout:

- `LOW SIGNAL -- CTRL 42%` at frontier/fringe thresholds
- reserve `[ SIGNAL LOST ]` for near-zero signal
- trigger the warning before total collapse, with lower intensity than the
  lost state
- keep the compact HUD `CTRL%` line as supporting telemetry

**Acceptance test:**

Entering fringe/frontier signal produces a readable low-signal warning before
the ship reaches near-zero control.

### 3. Station-Local Economy

**Status:** Open.

**Evidence of current strength:**

- `station_t` contains station-local currency names and per-player ledgers.
- Hail responses report station-local balance for the hailed station.
- Dock header shows current station balance and currency when there is room.
- Contract rows name the payout currency using the destination station first.

**Remaining gap:**

The UI exposes "balance here" but not the rule "balances are local." There is
no normal station strip showing current-station balance alongside known
non-zero balances elsewhere. The wire/event shape for hail response carries
only one station and one credit value, so multiplayer clients do not appear to
receive a compact cross-station ledger snapshot.

This is the largest self-revealing gap because the underlying design violates
common player expectations on purpose. Without a cross-station display, the
player may infer that money disappeared or that the economy is broken.

**Impact:**

Station sovereignty becomes a hidden constraint rather than a playable premise.
This directly undermines hauling, arbitrage, receipt value, and station
identity.

**Recommended implementation:**

Add a docked ledger strip:

```text
PROSPECT 240   HELIOS 0   KEPLER 88
```

Rules:

- highlight the current station
- show known non-zero balances
- show known zero for the current station
- on first zero-balance dock after earning elsewhere, show one subtitle:
  `Credits stay where you earn them. Carry goods, not money.`
- in multiplayer, add a compact ledger snapshot to sync or a new response that
  carries the player's known ledger rows

**Acceptance test:**

Earn at Prospect, dock at Helios, and verify the UI shows both the Helios zero
and the Prospect non-zero balance without requiring the player to remember the
previous station.

### 4. NPC Route Motive

**Status:** Partial.

**Evidence of current strength:**

- NPC inspect cards already render a contact surface with role, state, home,
  destination, memory stream, clarity meter, and degraded text.
- Job rows compute "why" labels from job reason, proof prefix, and factor
  scores.
- Clarity is derived from confidence, salience, and hops rather than printed as
  a raw debug number.

**Remaining gap:**

The surface is still memory-stream first. It does not consistently answer the
contact-card sentence:

```text
<who> is hauling <what> to <where> because <strongest state-grounded reason>
```

The data exists across snapshot role/state, cargo rows, job cause rows, route
history, and memory rows, but the first read is still distributed. A player can
inspect long enough to infer motive; the card should make the motive the lead.

**Impact:**

NPCs may still feel arbitrary even when the simulation has enough evidence to
explain their work. This is especially harmful for solo play, where NPCs are
the cast.

**Recommended implementation:**

Reframe `hud_draw_npc_memory_ticker()` as a contact card:

- line 1: callsign plus clarity word/meter
- line 2: role/state plus cargo if known
- line 3: route or destination
- line 4: strongest "why" from job reason/source chain
- line 5: route memory or proof source when available

Keep degraded text and dim colors, but make the strongest known motive stable
for the card lifetime rather than rotating the most interesting diagnostic row
every 0.85 seconds.

**Acceptance test:**

Scanning a worker with a delivery or hauling job yields a stable, readable
answer such as: `hauling ferrite -> Kepler; why Prospect demand, fresh`.

### 5. Station Memory And Player Trace

**Status:** Partial.

**Evidence of current strength:**

- `draw_station_gossip_rows()` surfaces station-local market memory as
  OVERHEARD rows.
- `draw_station_route_history_rows()` surfaces compact signed route history.
- HISTORY tab visibility depends on route-history availability.
- HISTORY can show aggregate route memory and recent signed events.
- Gossip promotes repeated route reputation/risk into chain-log route-history
  events once evidence crosses a threshold.

**Remaining gap:**

The player can find signed memory, but station identity is not yet summarized
as "what this place is known for" or "what this place remembers about you." The
HISTORY tab is good as a proof-adjacent browser; it is weaker as a dock-level
identity surface.

Specific missing reads:

- repeated verified player work at this station
- current station trust/risk consequence of the player's recent behavior
- a short "known for" line derived from aggregate route memory
- distinction between local signed truth, carried gossip, and station
  reputation in the first station frame

**Impact:**

The memory metagame remains more inspectable than felt. Stations can still read
as shops with a history appendix instead of institutions whose behavior and
identity are shaped by memory.

**Recommended implementation:**

Add one dock-header or SHIP-panel memory strip:

```text
Known for: Helios ferrite route x6, fresh
You here: 3 docks, 180 earned, last ran frames
```

Rules:

- derive from ledger stats, route-history aggregates, and local chain rows
- keep it to one or two lines
- avoid raw event hashes unless in HISTORY detail
- prefer "known", "heard", "fresh", "old" over numerical confidence

**Acceptance test:**

After repeated deliveries or construction contributions, docking at the station
shows a short local-memory trace without opening the HISTORY tab.

### 6. Construction Consequence

**Status:** Partial.

**Evidence of current strength:**

- Station UI shows scaffold progress, needed construction materials, pending
  scaffold queue, blockers, and stock.
- Main event handling plays commission feedback and shows notices for scaffold
  readiness and module activation.
- Rejection messages explain common scaffold placement failures.

**Remaining gap:**

The player sees that a module came online, but not what changed because it came
online. `"<module> online."` is correct but underspecified. It does not name
the new station capability, route consequence, production consequence, or
signal change.

Examples of missing consequence text:

- `Relay online -- signal boundary extended.`
- `Furnace online -- ferrite smelting available here.`
- `Shipyard online -- scaffold kits can now be ordered.`
- `Dock online -- station accepts traffic here.`

**Impact:**

Construction can feel like checklist progress rather than infrastructure
becoming real. This is especially damaging because building is supposed to be
the macro expression of "civilization is physically earned."

**Recommended implementation:**

Add a `module_consequence_label(module_type_t, station_t*)` helper for notices
and station rows. Use it when modules activate and when YARD previews scaffold
kits.

**Acceptance test:**

Completing a relay, furnace, dock, or shipyard produces a message naming a
newly available capability or signal consequence.

## Cross-Cutting Gaps

### A. Facts Are Often Present Without A Player-Level Sentence

The game now has many honest facts: balance, signal, cargo, route, lineage,
proof, memory, confidence. The missing layer is often a single stable sentence
that joins them into a player answer.

Preferred pattern:

```text
state fact + world meaning + next verb
```

Example:

```text
Helios 0 credits. Prospect 240. Carry goods to move value.
```

### B. Certainty Grammar Exists, But Not Everywhere

NPC memory and OVERHEARD rows use clarity. Station balances, construction
consequences, and trade demand mostly render as hard UI facts. That is correct
for local ledger truths, but route/gossip-derived rows should keep using
clarity styling so truth and rumor do not flatten together.

### C. Inspect Surfaces Are Better Than First-Read Surfaces

HISTORY, inspect panes, and receipt detail are becoming strong. The dock header,
target/tow HUD, and NPC contact card still need the strongest "first answer."
The product should default to story and drill into proof.

### D. Multiplayer Wire Shape Lags Behind The Product Direction

Some single-player state is visible because the client owns the world. The
local-economy gap likely requires wire additions:

- known ledger rows
- station-local player relationship summaries
- maybe route-memory summaries scoped to what the player should know

Avoid building SP-only UI that implies MP knowledge the client does not have.

## Recommended Priority Order

### P0: Ledger Strip

This is the highest-value gap because it explains a deliberately unusual rule.
It should come before adding new economy features.

Deliverables:

- local dock strip for current and known balances
- MP snapshot or response extension
- first zero-balance subtitle
- tests for no global-wallet regression

### P1: Low-Signal Banner

Small implementation, high perception value. Builds on existing signal
telemetry.

Deliverables:

- low-signal threshold warning before total signal loss
- central warning text with `CTRL%`
- retained `[ SIGNAL LOST ]` state near zero

### P1: NPC Contact Card Stabilization

Use existing inspect/job/clarity data, but change hierarchy and rotation.

Deliverables:

- stable strongest motive line
- cargo/destination lead
- degraded certainty presentation

### P1: Construction Consequence Labels

A small helper can make construction feel dramatically more legible.

Deliverables:

- module consequence helper
- activation notice text
- YARD preview consequence where space allows

### P2: Station Memory Summary

Build only after deciding the exact player-memory vocabulary.

Deliverables:

- "known for" aggregate line
- "you here" local relationship line
- clear source split between signed memory and overheard gossip

### P2: Rock Economic Usefulness

Throw danger is already visible, so the next rock work should focus on
economic/contract meaning rather than more combat chrome.

Deliverables:

- target/tow contract-fit hint
- demand/usefulness label
- priority rules when combat danger and economic usefulness both apply

## Regression Risks

- Do not turn the HUD into a tutorial overlay. Each new line must be tied to a
  live state fact.
- Do not imply global money. Ledger UI must reinforce station-local credits.
- Do not let proof bytes displace player meaning in default rows.
- Do not rotate essential NPC motive text too quickly to read.
- Do not let construction messages claim capabilities that the module does not
  actually unlock.
- Do not regress compact layout constraints; new station rows need row budgets.

## Completion Tests For The Next Self-Revealing Slice

A future implementation slice should be accepted only if at least one of these
statements becomes true in normal play:

- I know my credits are local because this station shows its balance beside
  other known balances.
- I know my ship is sluggish because the warning names low signal and control
  percentage before total loss.
- I know what an NPC is doing because the contact card names cargo,
  destination, and motive.
- I know this station remembers me because docking shows a local trace of my
  verified work.
- I know what construction changed because the completion message names a new
  capability or signal boundary change.
- I know why this fragment matters before docking because target/tow UI names
  contract fit or station demand.

## Bottom Line

Signal has started surfacing its intelligence. The next gap is making those
surfaces synthesize meaning. The highest priority is not more data on screen;
it is fewer moments where the player sees a correct fact but cannot infer the
world rule behind it.
