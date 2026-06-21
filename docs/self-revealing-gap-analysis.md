# Signal Self-Revealing Gap Analysis

**Status:** detailed product/UX gap analysis, refreshed after the first
self-revealing implementation pass
**Audience:** contributors choosing implementation slices after the legibility
pass  
**Scope:** current worktree as of this review; especially
`docs/self-revealing-roadmap.md`, `client/hud.c`, `client/world_draw.c`,
`client/station_ui.c`, `server/game_sim.c`, `server/gossip.c`, and
`shared/types.h`.

## Executive Read

Signal is now past the "nothing is legible" phase. Several high-value surfaces
are live:

- throw previews render release vectors and target brackets from live slingshot
  physics
- flight HUD exposes signal band, mining efficiency, control percentage, and a
  central low-signal warning before total loss
- docked station UI shows station-local ledger context in single-player, and
  first zero-balance hails explain that credits stay local
- docked trade rows hide forensic receipt clutter by default and can attach
  representative lineage when the local manifest proves the whole row
- contract rows show immediate step, route/cargo, and payout currency
- NPC scan output uses the clarity grammar and station/route memory signals
- station HISTORY can summarize route memory and signed local events
- module activation notices name the capability that came online

The remaining product risk is subtler: the game often reveals facts, but not
the rule the player needs to infer from those facts. The biggest remaining gaps
are NPC contact packaging, player-facing station memory summaries, rock
economic usefulness beyond tracked contracts, and multiplayer snapshots for
station-local ledgers.

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
| Why is this rock valuable? | Partial | Throw danger, tracked-contract fit, station demand, and rare grade are visible; smelt outcome and market-memory usefulness are not unified yet. | P1 | Extend target/tow usefulness into smelt destination, carried memory, and route value. |
| Why did my ship stop responding? | Closed | Low-signal control loss now has both compact telemetry and a central CTRL warning before near-zero loss. | Done | Tune thresholds only if playtests show confusion. |
| Why can I spend credits here but not there? | Partial | Single-player dock/hail explains station-local money; multiplayer still needs a compact cross-station ledger snapshot. | P2 | MP known-ledger snapshot or response extension. |
| Why did that NPC choose that route? | Partial | Inspect data has motive and clarity; the contact card does not consistently lead with cargo, destination, and strongest "why." | P1 | Contact-card rewrite over existing inspect rows. |
| Who remembers what I did? | Partial | HISTORY exists, but station identity and personal traces are not summarized in normal station framing. | P1 | Station "known for" / "you here" memory rows. |
| What changed because I built this? | Closed | Module activation notices now name the new capability; persistent station identity rows remain a separate memory problem. | Done | Add station memory rows, not more activation copy. |

## Evidence Map

These are the concrete code/doc hooks behind the matrix above.

| Area | Evidence |
|---|---|
| Self-revealing criteria | `docs/self-revealing-roadmap.md:23` defines the six answer types; `docs/self-revealing-roadmap.md:38` starts the six player questions. |
| Throw danger | `client/world_draw.c:42` mirrors server throw base speed; `client/world_draw.c:2072` computes preview speed/hotness; `client/world_draw.c:2104` draws hot/cold arrows; `client/world_draw.c:2161` gates lock brackets. |
| Low-signal HUD | `client/hud.c:2239` draws `[ SIGNAL LOST ]` near zero and `[ LOW SIGNAL -- CTRL n% ]` below operational signal; compact/wide HUD also prints `CTRL%` telemetry below operational signal. |
| Current-station balance | `shared/types.h:643` stores station currency names; `shared/types.h:656` stores station-local ledgers; `client/station_ui.c:599` builds a single-player ledger strip; `client/main.c:856` explains first zero-balance hails after earning elsewhere. |
| Contract payout currency | `client/station_ui.c:2749` selects destination-station currency first for contract payout rows. |
| Trade lineage | `client/station_ui.c:1153` only attaches station-stock lineage when the row is fully represented by local manifest data; `client/station_ui.c:1251` applies the same caution for player-held sell rows. |
| NPC contact/motive | `client/hud.c:1395` renders the NPC contact ticker; `client/hud.c:1451` renders contact identity; `client/hud.c:1461` renders role/state/home/destination; job motive helpers sit around `client/hud.c:928`. |
| Station gossip/memory | `client/station_ui.c:1633` renders OVERHEARD rows; `client/station_ui.c:1690` renders compact route HISTORY rows; `client/station_ui.c:1836` renders aggregate history; `server/gossip.c:786` promotes repeated route memory into chain-log history. |
| Construction feedback | `client/station_ui.c` previews scaffold kit ordering and renders construction queue state; `client/main.c:869` maps module activation to capability text such as relay reach, smelting, shipyard ordering, and docking. |

## Detailed Findings

### 1. Rock Value And Throw Legibility

**Status:** Partial; closed for combat danger and first-pass fragment
usefulness.

**Evidence of current strength:**

- `client/world_draw.c` mirrors the server release floor with
  `ROCK_THROW_BASE_SPEED` so the preview is grounded in real throw physics.
- `throw_preview_for_fragment()` computes predicted release velocity,
  compares it to a damage threshold, and derives hotness from that result.
- `draw_throw_arrow()` maps hotness into length, color, opacity, and arrowhead
  size.
- `draw_throw_locks()` draws target brackets only when the hottest preview can
  plausibly hit a player or NPC target.
- `hud_asteroid_usefulness()` names tracked contract fit, high station demand
  for towed S-tier fragments, and rare grade fallback.

**Remaining gap:**

The player can now learn "this release is dangerous" and can get a first useful
economic hint when a towed fragment fits tracked work or a station shortage.
The remaining gap is deeper synthesis. The target/tow surface still does not
combine all of these into one stable rock story:

- grade
- commodity
- contract relevance beyond the explicitly tracked contract
- station demand and carried market memory
- smelt/lineage future

The result is improved but still split: rock-as-weapon is visible in flight;
rock as future institutional value is partly visible before docking and becomes
much clearer only after smelting, trading, or opening station rows.

**Impact:**

This weakens the core "matter becomes history" loop. A player may understand
how to throw rocks before understanding why a specific fragment matters
economically or socially.

**Recommended implementation:**

Extend the compact target/tow usefulness line so it can also read downstream
world state:

- `contract fit` when a towed/targeted fragment satisfies tracked or nearby
  station work
- `wanted at <station>` or `needed at <station>` when local market memory or
  station demand makes it useful
- `smelts to <cargo>` when a nearby station can process the fragment
- `route remembers <cargo>` when signed route history makes the fragment useful
  beyond the nearest dock
- `high grade` / grade color when no stronger economic reason exists
- `throw hot` / bracket behavior remains the combat affordance

**Acceptance test:**

With a tracked tractor contract or station shortage and a towed matching
fragment, the player sees that the same object is both physically dangerous
when hot and economically useful for a named station. A later pass should prove
the same read when the value comes from smelt route or station memory rather
than direct current demand.

### 2. Low-Signal Control Legibility

**Status:** Closed.

**Evidence of current strength:**

- Compact and wide HUD print signal band and percent.
- When signal is below operational, the HUD prints mining and control
  percentages (`M%d%% CTRL%d%%`).
- The central warning now renders `[ LOW SIGNAL -- CTRL n% ]` before total
  signal loss.
- Shared signal model names the relevant bands and control curve.

**Remaining follow-up:**

This should move from implementation backlog to playtest tuning. The key
questions are threshold feel, banner intensity, and whether repeated low-signal
entries become noisy.

**Impact:**

The major confusion risk is closed. Remaining risk is annoyance or threshold
tuning, not hidden mechanics.

**Recommended implementation:**

Keep the current split:

- `LOW SIGNAL -- CTRL n%` below operational signal
- `[ SIGNAL LOST ]` near zero
- compact/wide `CTRL%` as supporting telemetry

**Acceptance test:**

Entering fringe/frontier signal produces a readable low-signal warning before
the ship reaches near-zero control.

### 3. Station-Local Economy

**Status:** Partial; closed for single-player dock/hail, open for multiplayer
cross-station snapshots.

**Evidence of current strength:**

- `station_t` contains station-local currency names and per-player ledgers.
- Hail responses report station-local balance for the hailed station.
- Dock header shows current station balance and currency when there is room.
- Single-player station headers can show current and other known balances in a
  compact ledger strip.
- First zero-balance hails after earning elsewhere explain that credits stay
  local and goods move value.
- Contract rows name the payout currency using the destination station first.

**Remaining gap:**

The UI now exposes "balances are local" in the single-player station flow. The
remaining gap is multiplayer parity: the wire/event shape for hail response
carries only one station and one credit value, so multiplayer clients still need
a compact known-ledger snapshot before they can safely render cross-station
balances.

This was the largest self-revealing gap because the underlying design violates
common player expectations on purpose. It is no longer the top single-player
gap, but it remains important before pushing the economy harder in multiplayer.

**Impact:**

Station sovereignty is now a playable premise in local play. In multiplayer,
missing snapshots can still make sovereignty look like missing money rather
than local authority.

**Recommended implementation:**

Keep the docked ledger strip:

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
previous station. Repeat in multiplayer once known-ledger snapshots exist.

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

**Status:** Closed for activation notices; persistent post-build memory remains
part of station identity.

**Evidence of current strength:**

- Station UI shows scaffold progress, needed construction materials, pending
  scaffold queue, blockers, and stock.
- Main event handling plays commission feedback and shows notices for scaffold
  readiness and module activation.
- Module activation text now names the consequence: relay reach, furnace
  smelting, shipyard ordering, dock traffic, or module storage/service.
- Rejection messages explain common scaffold placement failures.

**Remaining gap:**

The activation moment now names the concrete capability that came online. The
remaining product work is not more completion copy; it is making finished
infrastructure continue to shape station identity, route memory, and local
work pressure after the notice fades.

Examples of shipped consequence text:

- `Relay online -- signal boundary extended.`
- `Furnace online -- ferrite smelting available here.`
- `Shipyard online -- scaffold kits can now be ordered.`
- `Dock online -- station accepts traffic here.`

**Impact:**

The largest immediate risk is closed. Long-term construction can still feel
too momentary unless stations later remember who built or supplied them.

**Recommended implementation:**

Keep activation notices tied to real module capabilities. Fold build history
into the station memory summary rather than adding more one-off banners.

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

This pattern is now live for the station-local credit rule in single-player.
Reuse the same structure for the next gaps: contact motive, station memory, and
fragment usefulness.

### B. Certainty Grammar Exists, But Not Everywhere

NPC memory and OVERHEARD rows use clarity. Station balances, construction
consequences, direct station demand, and trade demand mostly render as hard UI
facts. That is correct for local ledger truths and local station shortages, but
route/gossip-derived rows should keep using clarity styling so truth and rumor
do not flatten together.

### C. Inspect Surfaces Are Better Than First-Read Surfaces

HISTORY, inspect panes, receipt detail, and several flight/dock warnings are
becoming strong. The dock header memory layer and NPC contact card still need
the strongest "first answer." Target/tow HUD has a first-pass usefulness line;
the next version should join direct demand to smelt and route memory. The
product should default to story and drill into proof.

### D. Multiplayer Wire Shape Lags Behind The Product Direction

Some single-player state is visible because the client owns the world. The
local-economy gap likely requires wire additions:

- known ledger rows
- station-local player relationship summaries
- maybe route-memory summaries scoped to what the player should know

Avoid building SP-only UI that implies MP knowledge the client does not have.

## Recommended Priority Order

### P0: NPC Contact Card Stabilization

This is now the highest-value single-player gap. Workers are the cast in a solo
session, and the sim already has cargo, destination, memory, and motive pieces
that can be promoted into a stable first read.

Deliverables:

- stable strongest motive line
- cargo/destination lead
- degraded certainty presentation
- slower or no rotation for essential motive text

### P1: Station Memory Summary

Make stations feel like institutions, not shops with a HISTORY appendix.

Deliverables:

- "known for" aggregate line
- "you here" local relationship line
- clear source split between signed memory and overheard gossip

### P1: Rock Economic Usefulness

First-pass demand labels now exist for towed high-value fragments. The next
rock work should explain value when it comes from smelting, route memory, or
nearby untracked work.

Deliverables:

- smelt destination hint
- route/memory usefulness label
- priority rules when combat danger and economic usefulness both apply

### P2: Multiplayer Ledger Snapshot

The single-player ledger strip and local-credit notice should get multiplayer
parity before station sovereignty becomes a public-session support problem.

Deliverables:

- compact known-ledger rows over sync or hail response
- clear stale/unknown handling
- tests for no global-wallet regression

### P2: Route/Lineage Drill-Down

Build only after deciding the exact proof/story vocabulary for the compact
rows.

Deliverables:

- selected cargo story view
- route memory overflow view
- proof detail hidden behind player meaning

## Regression Risks

- Do not turn the HUD into a tutorial overlay. Each new line must be tied to a
  live state fact.
- Do not imply global money. Ledger UI must reinforce station-local credits.
- Do not let proof bytes displace player meaning in default rows.
- Do not rotate essential NPC motive text too quickly to read.
- Do not let construction messages claim capabilities that the module does not
  actually unlock.
- Do not regress compact layout constraints; new station rows need row budgets.
- Do not let direct station-demand labels imply rumored market knowledge; rumor
  and route memory need clarity treatment.

## Completion Tests For The Next Self-Revealing Slice

A future implementation slice should be accepted only if at least one of these
statements becomes true in normal play:

- I know what an NPC is doing because the contact card names cargo,
  destination, and motive.
- I know this station remembers me because docking shows a local trace of my
  verified work.
- I know why this fragment matters before docking because target/tow UI names
  smelt path, route memory, station demand, or contract fit.
- I know my credits are local in multiplayer because this station shows its
  balance beside other known balances.

## Bottom Line

Signal has started surfacing its intelligence. The next gap is making those
surfaces synthesize meaning. The highest priority is not more data on screen;
it is fewer moments where the player sees a correct fact but cannot infer the
world rule behind it.
