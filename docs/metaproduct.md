# Signal Metaproduct

**Status:** canonical framing for product, engineering, and backlog grooming.
**Audience:** contributors deciding what to build next and how to describe why it matters.

Signal's visible product is a multiplayer frontier economy game: mine rocks,
tow fragments, smelt, haul, build outposts, expand signal, and kill other
players with rocks.

The metaproduct is the deterministic machine underneath that turns play into
verifiable history, provenance, settlement artifacts, and eventually communal
infrastructure.

The bigger vision is that Signal becomes a civilization game disguised as an
arcade physics game.

Signal is not "space mining," "blockchain Asteroids," or "procedural MMO."
Those are surfaces. Signal is a physics-native civilization network: players
mine matter, stations sign history, communities build signal, and the universe
expands only when the network earns it.

The core rule:

**Every large-scale social structure in the game must be made out of physical
actions the players actually performed.**

No abstract guild bases that appear from menus. No global wallet teleporting
value. No land-claim button. No settlement layer that exists outside the play
loop. A station exists because someone hauled frames. A route exists because
haulers kept it alive. A currency has credibility because the issuing station
has inventory, labor, history, and a signed record.

The ladder starts with the most honest primitive in the game:

```text
rock
  -> fragment
  -> ingot
  -> frame
  -> outpost
  -> station
  -> route
  -> sector
  -> civilization
```

## Product Stack

Signal is four products wearing one jacket, plus a settlement substrate below
them:

1. **Player product:** a fun physics economy game. The player promise is:
   your labor leaves durable traces in the world.
2. **Operator product:** a recipe for running a sovereign economic zone. The
   operator promise is: run a station without inventing a trust stack.
3. **Auditor/developer product:** tools that prove what happened. The verifier
   promise is: chain logs, cargo inventories, and replay hashes can be inspected
   after the fact.
4. **Research product:** a real-time deterministic economy/physics workload for
   decentralized simulation.
5. **Settlement substrate:** the event-distillation layer that turns important
   gameplay transitions into station history, receipts, checkpoints, and
   portable artifacts.

The public hierarchy stays player-first:

1. Fun physics economy game.
2. Every meaningful object has history.
3. Stations are sovereign communities.
4. That history can be verified.
5. Communities can anchor and extend it outside one server.

Do not lead with layer 5. Lead with rocks.

## Secret Genre

Signal's secret genre is cooperative infrastructure drama.

The drama comes from building and maintaining shared systems under constraint:
distance, entropy, bad logistics, thin trust, dead routes, forked history,
signal loss, resource decay, and coordination failure. Privateers fit because
violence against haulers is violence against infrastructure, not a detached
combat minigame.

The player fantasy is not "you are a hero in a vast universe." It is:

**You are a worker in a fragile universe that only becomes vast if everyone
builds it.**

The emotional arc should be:

```text
I am surviving.
I am earning.
I am hauling.
I am building.
I am trusted.
I am part of a route.
I am part of a station.
I am part of a sector.
I am part of the network.
We can cross the dark.
```

## World Primitives

The long-term world is built from five primitives:

- **Matter:** rocks, fragments, ingots, frames, modules, crystals, scaffolds.
- **Signal:** the civilizational boundary where control, communication, NPC
  support, visibility, and safety exist.
- **Stations:** sovereign local institutions that issue credits, maintain
  ledgers, sign history, price goods, and express personality.
- **Receipts:** portable trust attached to custody, origin, and transformation.
- **Memory:** the permanent consequence layer: destroyed rocks stay destroyed,
  chain logs remember events, and player labor becomes historical texture.

These are not just technical systems. They are the meaning of the world.

## Event Distillation

The sim runs at 120 Hz, but settlement should not care about every frame of
motion. It should care when physical play crosses a boundary that changes the
economy.

Canonical durable facts include:

- a rock was fractured and a `fragment_pub` was born
- a fragment was smelted into a named `cargo_unit_t`
- ingots were crafted into a product or scaffold
- a cargo unit transferred between holders or stations
- a delivery fulfilled a contract
- a module or outpost crossed a construction milestone
- a signal channel advanced its content root
- a player died to a witnessed physical cause
- a RATi-bearing vessel was born, repaired, transferred, destroyed, or bridged

The intended flow is:

```text
play
  -> physical transformation
  -> content-addressed object or milestone
  -> station-signed event
  -> verifiable station history
  -> portable receipt/checkpoint/permaweb artifact
```

The killer demo should be a lineage view that starts with one rock and ends in
shared infrastructure:

```text
Gate Alpha, Segment 3
  built from Frame 8F4...
    crafted at Kepler Yard
      from Ferrite Ingot A91...
        smelted at Prospect Refinery
          from Fragment 77C...
            fractured from Rock 1B0...
              mined by player KRX-472
              hauled through Relay Nine
              signed by Prospect at tick 8,220,144
```

That artifact is the whole product in miniature.

## Visible Matter Algebra

The canonical construction authority is
[`rock-cell-grammar.md`](rock-cell-grammar.md). Matter keeps the visible 2x2
quartering at manufactured conversion boundaries, then enters one 60°
structural lattice:

```text
1 rock fragment -> smelt -> 4 ingots
1 ingot         -> press -> 4 standard struts
3 struts        -> assemble -> 1 directional triangle
6 struts        -> assemble -> 1 standard hex rim
12 struts       -> assemble -> 1 reinforced hex hub
```

The balance target is not "make every number smaller." It is to keep current
effective rock costs while making every structural unit visible. A dock that
currently costs 20 frames is one old rock because one rock produces 20 frames;
in the accepted algebra it costs 16 struts because one rock produces 16
struts. A furnace that currently costs 60 frames remains three rocks by
becoming 48 struts. The full throughput and cell balance worksheet lives in the
canonical grammar document. Prices remain tuned separately because price is
information, not matter.

The visual rule is the product rule. Fracture remains irregular: a rock breaks
into two to four messy fragments because nature is uncounted. The furnace is
where nature becomes geometry. Smelt visibly cuts a fragment into four exact
square ingots, and the press draws each ingot into four standard struts. Those
struts form triangle, hex, and reinforced-hex cells. Squares remain visible as
packed payload inside hex carriers; they are not structural docking cells.
Modules show their recipe directly as a structural rim around an interior
commodity treatment. Nothing in the matter ladder should require the player to
memorize a hidden multiplier.

This is also the line between analog flow and conserved matter. Smelt rates,
craft duration, demand pressure, and prices may remain continuous because they
are time or information. Conserved matter should be countable on screen.

Repair kits are the known violation. A recipe that turns one fabrication set
into 100 kits creates a thing no sprite can honestly explain. The destination
is hull and stations as cells: damage shears legal cells free, repair welds
struts or cells back on, and hull is a cell graph rather than an HP bar plus kit
stack. That convergence belongs in a gated epic, but every interim repair slice
should point toward it rather than further entrenching kits.

## Grand Arc

### Act I: Sector One — Learn Civilization

Sector One teaches the practical frontier grammar:

- signal = safety
- stations = community
- matter = value
- routes = economy
- outposts = expansion

Prospect, Kepler, and Helios are fragile institutions keeping local
civilization alive. The goal is not to "beat" Sector One. The goal is to make
the player understand why the network matters.

### Act II: Sector X — Choose Disconnection

Sector X should invert the lesson. The player chooses to leave signal on
purpose. The dark is not "evil space"; it is unwitnessed space.

Inside signal, actions can be seen, signed, routed, insured, credited, and
remembered. Outside signal there are no easy comms, no instant market quote, no
NPC safety net, no station guarantee, no complete telemetry, and no automatic
trust.

Dark-sector resources should be things civilization cannot produce from inside
comfort. Jump crystals work because they require direct human risk in places
automation cannot stabilize.

### Act III: The Gate — Build A Shared Future

The long-arc payoff is a megaproject, not a boss. A gate is physical,
economic, social, symbolic, and infrastructural at once.

To build one, the network needs massive frame production, rare dark-sector
crystal runs, multi-station contracts, verified cargo provenance, station
political alignment, defended routes, public progress, and ceremonial
activation.

The question is not "did you win?" It is: **did your sector become capable of
crossing?**

## Institutional Memory

The metagame should be institutional memory rather than generic leaderboards.
Status comes from recognized history:

- first station to become self-sufficient
- longest continuously active trade route
- most trusted hauler by verified delivery count
- most dangerous privateer by verified disruption
- oldest surviving outpost
- first gate-frame contributor
- most repaired route after collapse
- most valuable cargo lineage
- first crystal returned from beyond signal

These are queries over history, not achievements pasted on top. The
chain-log/provenance layer matters because the world can answer what happened.

Player institutions should be functions before labels: hauling cooperatives,
station defense unions, credit underwriters, dark-run expeditions, construction
corps, salvage crews, cartography bureaus, receipt auditors, maintenance
trusts, privateer clans, and insurance pools. They matter only when the
mechanics make their work legible.

## RATi Identity

RATi should be framed as a cross-world identity and provenance namespace, not
as a normal in-world wallet.

Worlds are local contexts. Stations, ledgers, cargo, and routes can reset, fork,
or die. RATi is the thing that persists across those worlds. A RATi-bearing
vessel is the local embodiment of that persistent identity inside one world:
the vessel can be built from physical materials, witnessed by a station, and
bound to portable provenance without turning the whole game into a global
currency layer.

That makes RATi a natural fit for "the first latent space cryptocurrency and
decentralized identity system": the token is not merely a spendable balance. It
is a bearer identity that can be expressed through vessels, provenance, station
recognition, bounty eligibility, and cross-world continuity.

Backlog implication: substrate-attached player birth and RATi vessel identity
belong after manifest-backed transfers and canonical settlement events. Identity
should be born as a witnessed economic fact, not patched onto saves as account
metadata.

## Arweave Core, Solana Adapter

Signal's native authority is per-station history: station keys, chain logs,
cargo receipts, checkpoint roots, and replayable settlement state.

Arweave/permaweb anchoring is the core long-term persistence path because it
matches the product metaphor: durable frontier history, station-local logs,
snapshots, discovery manifests, and client bootstrap from public artifacts.

Solana-style state-root commitments, burn-to-mint programs, wrapped assets, and
RATi Foundation bounty payouts are valid adapters. They should consume Signal's
native station history rather than define it.

## Groomed Backlog

### Now

1. **#588 determinism acceptance:** decide whether strict native/WASM replay
   gates are the staged acceptance path or whether full `q32.32` remains the
   next mandatory rewrite.
2. **#340 / #339 manifest-backed transfers:** buy, sell, deliver, and production
   should move concrete `cargo_unit_t` rows; retire finished-goods float
   authority once compatibility is no longer needed.
3. **Visible matter algebra migration:** restate conserved production around
   the quartering rule: `REFINERY_INGOTS_PER_FRAGMENT = 4`, frame press
   `1 ingot -> 4 frames`, module fab `1 ingot + 1 frame -> 1 module`,
   station blocks `4 frames -> 1 block`, and build costs restated to preserve
   effective rock costs (`dock 16 frames = 1 rock`, `furnace 48 frames = 3
   rocks`). Do not treat the repair-kit x100 path as a permanent system; route
   repair work toward block-count hull/station repair.
4. **Lineage view:** the CLI can print cargo lineage from chain logs; next make
   rock -> fragment -> ingot -> frame -> outpost/gate contribution inspectable
   in the player-facing UI.
5. **#587 typed provenance contracts:** support explicit target pubkeys and
   fracture/death fulfillment so contracts can price witnessed events.
6. **Player-facing lineage:** expose cargo history, local station ledger facts,
   and provenance requirements in the docked UI.

### Next

1. **#354 game-sim validation to settlement events:** make meaningful validated
   actions emit canonical settlement facts.
2. **#355 construction milestone events:** outpost/module progress becomes
   station history.
3. **#356 signal-channel continuity:** clients can verify station-authored
   hail/work/history roots.
4. **#294 unified ship/controller model:** remove cargo/provenance divergence
   between NPCs and players.
5. **Institution tools:** shared contracts, escrowed cargo, station-endorsed
   bounties, route health dashboards, and public construction manifests.

### Later, Gated

1. **#590 / #591 / #589 permaweb and P2P:** Arweave reads, peer anchoring, and
   WebRTC quorum behavior.
2. **#496 RATi vessel identity:** substrate-born cross-world identity.
3. **Hull/station block convergence:** converge #343 hull-as-merkle, #603
   sheared blocks, and ship/station repair into one visible block grammar:
   damage removes blocks, repair welds frames/blocks back, and repair kits
   disappear as a player-facing matter type.
4. **#285 streaming entity pool:** cap lifting and broader `game_sim.c`
   decomposition.
5. **External-chain adapters:** Solana bridge, burn-to-mint, and bounty payout
   flows over native Signal settlement history.

## Positioning Sentence

Signal is a physics-native civilization network: players mine matter, stations
sign history, communities build signal, and the universe expands only when the
network earns it.
