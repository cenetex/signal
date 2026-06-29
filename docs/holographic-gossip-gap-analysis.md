# Holographic Gossip Network - Docs Review And Gap Analysis

**Status:** current review of docs/code against the contract-gossip,
neural-worker, and holographic-memory vision.

**Audience:** engineers working on contracts, neural workers, station economy,
operator copy, or holographic pilot memory.

**Review date:** 2026-06-14.

## Vision Contract

Signal's economy should not depend on global omniscience or fixed NPC roles.
Stations sign local truth. Ships transport goods and information. Workers learn
what work matters by moving through the world, docking, exchanging knowledge,
and forgetting stale rumors.

The operating rule is:

> Contracts produce market pressure. Gossip transports that pressure. Neural
> workers choose jobs from the pressure they have heard, then resolve against
> exact station authority when they dock.

That gives the system a hard boundary:

- authoritative state answers **what is true**
- gossip state answers **what is worth trying now**
- holographic memory answers **what pattern feels familiar**

Cargo is matter with provenance. Gossip is attention with provenance. HNN
bundles are compressed attention, never authority.

## Executive Verdict

The written vision is directionally right and the implementation is no longer
fighting it. Signal now has the intended three-layer economy:

- exact station authority for contracts, ledgers, manifests, receipts, and
  chain logs
- structured gossip for local, decaying, inspectable economic attention
- holographic memory for compact, fuzzy, portable familiarity

The main gap is not architecture. It is legibility. Workers can already choose
from richer pressure than the UI can comfortably explain, and durable
route-history events can now reach a first in-game contracts-board history
surface before the game has a fuller institutional-history browser. That is a
healthy but dangerous stage: the system is becoming interesting enough to look
arbitrary if its reasons remain hidden.

The docs should hold one hard line: **neural worker** is the product concept.
"Miner," "hauler," "tow," "repair," "scout," and "delivery-proof" are selected
job outcomes, not permanent NPC identities. Existing `NPC_ROLE_*` names are
compatibility labels until the protocol, tests, and save migration can retire
them.

The review call is: do not add another hidden intelligence layer yet. The next
evolution should make the intelligence visible, social, and historical:

1. selected worker jobs explain their source chain
2. repeated receipt-backed routes become station-readable history
3. no-omniscience soaks prove that memory really moves by traffic
4. HNN resonance stays advisory until every visible claim has a structured
   source-memory fallback

That keeps the game readable while the economy becomes stranger and more alive.

## Review Method

This review treats the vision as a product contract, not just a technical
architecture. A feature only counts as aligned when it satisfies all four
questions:

1. **Authority:** what exact station-owned state decides payment or mutation?
2. **Transport:** how does the relevant memory physically reach another actor?
3. **Cognition:** how can a worker use that memory without becoming
   omniscient?
4. **Presentation:** how can a player tell whether they are seeing a command,
   a rumor, a proof, or a remembered pattern?

The strongest docs now answer the first three. The fourth is still the
highest-risk gap.

## Review Findings

| Finding | Why It Matters | Doc/Build Implication |
| --- | --- | --- |
| The authority/cognition boundary is solid. | HNN and gossip can now bias work without becoming hidden truth. | Keep repeating that only contracts, manifests, receipts, ledgers, and chain logs mutate the world. |
| The docs have one coherent spine. | README, PRD, architecture, cargo, decentralization, and gossip docs now point at the same product: physical labor creates verifiable history. | Keep `docs/holographic-gossip-network.md` canonical; supporting docs should link rather than duplicate roadmap detail. |
| The biggest product risk is invisible intelligence. | Good fuzzy choices look like bugs when the player cannot inspect the source memory, proof, or route reason. | Build selected-detail views for worker decisions and route history before making HNN scoring more powerful. |
| Holographic storage is the right metaphor. | Its natural decay/interference matches rumor, route familiarity, and carried economic attention. | Treat holograms as portable familiarity, never authority or proof. |
| Cold-start traffic is still the proving ground. | Local-only pressure exists, but a no-omniscience world must keep moving without a hidden global contract feed. `signal_replay --active-workers` now keeps seeded NPC workers alive, validates active NPCs/exchanged knowledge/HNN market memory over 10k ticks, and has a fast HNN-backed worker-tow pickup fixture. | Run the WASM gate on PRs and add broader autonomy metrics before treating this as fully covered. |
| Anonymous session-token ledgers remain a security risk. | Legacy and anonymous paths can still key real station saves/ledgers from a client-chosen bearer token. | Schedule the account/credit-theft fix separately from AI/gossip work; do not let NPC-token namespacing obscure the underlying credential issue. |

## Gap Summary

| Gap | Current Evidence | Missing Evidence | Review Decision |
| --- | --- | --- | --- |
| Worker reasons are partially legible. | Selected-job details, source-memory rows, proof anchors, explicit anchor-only/local-chain gap labels, local receipt-link paging, a focused receipt-relay view, and station-local receipt-chain retrieval exist. | Cross-station/remote relay retrieval is still thin when the proof anchor is not carried locally and not present in station storage. | Add remote relay follow-through before increasing HNN influence. |
| Station memory is emerging. | Route-history chain events, compact Contracts-board history rows, and a signed HISTORY tab exist; the HISTORY tab now groups signed rows into aggregate cross-station route memory and can filter all/outbound/inbound/local views. | There is no deep institution browser yet. | Keep history presentation read-only context, not payout logic. |
| Physical gossip is plausible. | Dock/contact gossip, carried structured memories, station-local bootstrap, and HNN market pools exist. | No long no-omniscience soak proves the economy stays alive without hidden global knowledge. | Define the soak and success metrics before declaring the network autonomous. |
| Determinism gates under-cover AI economy drift. | Native/WASM replay gates, PR-triggered replay CI, long probes, and active-worker/gossip/HNN replay scenarios exist. Active-worker rows hash station/NPC knowledge, job diagnostics, HNN trace bodies, worker assignment outcomes, and scaffold tow state. | Active-worker replay is still a bounded probe rather than a full no-omniscience economy soak. | Add broader active-worker success/failure metrics beyond the tow fixture. |
| Anonymous bearer-token saves/ledgers are still open. | `ledger_balance`, `ledger_earn`, and spend paths still support pseudo-pubkeys derived from 8-byte session tokens for legacy/anonymous play. | A client-chosen token can address real station ledger state until the legacy path is constrained or bound to signed identity. | Treat as a separate security task, not part of the neural-worker merge. |
| Holographic cognition is bounded. | Market HNN traces cover current job families and decay/replacement caps are documented. | Resonance explanations are still thin when the matching structured source is absent or remote. | Keep HNN advisory until source-memory fallback is visible for the claim. |
| Legacy role vocabulary lingers. | Canonical docs say worker roles are assignments. | Some older docs still use "hauler" as player/economic shorthand or dated audit language. | Keep gameplay persona language, but mark NPC `hauler/miner` language as compatibility when it describes implementation. |

## Vision Fit Scorecard

| Vision Promise | Current Fit | Gap Severity | Review Call |
| --- | --- | --- | --- |
| Stations sign truth. | Strong | Low | Keep authority exact and boring. |
| Ships transport memory. | Medium-strong | Medium | Prove longer physical-spread soaks before declaring the architecture done. |
| Holograms are cognitive cargo. | Medium | Medium | Keep resonance advisory and require structured-memory fallback for visible claims. |
| Workers specialize from economy pressure. | Strong | Medium | Remove legacy role language and deepen cross-role diagnostics rather than adding fixed classes. |
| The economy explains itself. | Medium | High | This is the main product gap: focused inspect/detail views must catch up to worker scoring. |
| Gossip can become institution memory. | Medium-strong | Medium | Route-history events exist and have a first contracts-board presentation; fuller institution/history browsing is still missing. |

The weakest promise is not "the AI chooses jobs." It does. The weakest promise
is "the player understands why the AI chose that job and what evidence the
station will recognize." That is the next legibility bar.

## Documentation Review

The docs now have a strong spine:

- [`docs/holographic-gossip-network.md`](./holographic-gossip-network.md) is
  the canonical design target for market gossip, neural worker coordination,
  and holographic economic memory.
- [`ARCHITECTURE.md`](../ARCHITECTURE.md) names gossip/holographic memory as a
  core subsystem instead of a side channel.
- [`docs/cargo-architecture.md`](./cargo-architecture.md) gives the correct
  matter/provenance vocabulary: fragments, bulk float, crates, manifests, and
  receipts.
- [`docs/decentralization-synthesis.md`](./decentralization-synthesis.md)
  correctly links physical information transport to federation/P2P design.
- [`docs/metaproduct.md`](./metaproduct.md) supplies the product reason: routes,
  trust, and institutions should emerge from physical labor.
- [`PRD.md`](../PRD.md) now frames worker behavior as pressure-derived
  specialization instead of a fixed hauler/miner taxonomy.

The weak spots are not conceptual anymore. They are mostly vocabulary,
maturity markers, and observability:

- supporting docs still need to call historical miner/hauler labels
  compatibility or assignment labels when they refer to `NPC_ROLE_*`
- exact `known_contracts[]`, structured `market_memory_t`, and HNN bundles all
  exist in some form, so docs must say which layer owns each behavior
- HNN memory is still easiest to describe as pilot experience, but the vision
  now also depends on it compressing economic attention
- worker scoring has become more capable than the player-facing explanations
- player-facing control docs must keep the same split as the UI: hail scans,
  docking accepts and resolves work, inspect explains why workers chose jobs

If a worker makes a good fuzzy decision for invisible reasons, players will read
it as broken AI. That is the main product risk.

## Layer Audit

| Layer | What Belongs Here | Current Code/Docs State | Review Call |
| --- | --- | --- | --- |
| Authority | `contract_t`, station ledgers, manifests, cargo receipts, chain logs | Strong and well documented in architecture/cargo/decentralization docs. | Keep this exact and boring; do not let HNN or gossip mutate it. |
| Structured memory | `knowledge_item_t`, `market_memory_t`, confidence, salience, hops, anchors | Strong substrate; docs now name demand, supply, route, receipt, trust, and risk memories. | Treat this as the inspectable source for worker decisions. |
| Compatibility cache | `known_contracts[]` | Still required by UI, protocol visibility, tests, and exact contract candidates. | Keep calling this compatibility, not the destination architecture. |
| Holographic memory | bounded HNN traces and resonance | Medium; worker-local market encoding, station-pool market transport, weighted replacement/capacity, idle decay, and advisory haul, mine, tow, scout/fracture, delivery-proof, and repair resonance exist; compact source metadata exists where a structured memory backs the resonance. | Use it as bias and compression only, never as claim/proof. |
| Presentation | docked contract commands, scan/hail, worker inspect rows | Improving but fragile; stale copy can make the right model feel wrong. | Contract menus stay actionable; provenance lives in inspect/detail surfaces. |

## Vision Readiness Review

The current docs describe the right system, but they still mix three different
time horizons:

- **today's shipped substrate:** contracts, station authority, cargo receipts,
  structured market memories, dock gossip, worker job offers
- **near-term cognition:** HNN market bundles biasing neural workers from
  decaying carried memory
- **civilization memory:** repeated receipt-backed routes becoming durable
  station history and institutions

The vision becomes easier to evaluate if every feature answers four questions:

1. **What is exact?** Contracts, ledgers, manifests, receipts, and chain logs.
2. **What is remembered?** Structured market memory with provenance, age,
   confidence, salience, hops, and anchors.
3. **What is felt?** Holographic resonance over repeated memories, allowed to
   decay and interfere.
4. **What is shown?** Player-facing UI must distinguish command hints,
   contract state, worker reasoning, and historical reputation.

That split is the product shape. Exact state lets the economy pay correctly.
Structured memory lets workers explain what they heard. Holographic storage
lets the economy become associative without becoming omniscient. UI prevents
that fuzziness from reading as arbitrary behavior.

## Holographic Storage Fit

Holographic storage is a strong fit for the gossip network because its failure
mode matches the fantasy:

- old memories naturally lose force instead of needing perfect garbage
  collection
- repeated route evidence reinforces a pattern without requiring a global route
  table
- contradictory or stale rumors interfere instead of becoming permanent truth
- compact vectors can ride with ships as cargo-like memory
- a neural worker can recognize "this kind of route usually pays" without
  pretending it has exact station truth

The docs should keep using this as the core mental model:

```text
structured memory = receipt-like attention
holographic memory = decaying familiarity
contract ledger = authority
```

The important boundary is still hard: HNN resonance can make a worker more
interested in a job, but it cannot make a contract real, move cargo, mint
credits, complete construction, or prove provenance.

### Holograms As Transported Attention

The strongest version of the vision is not "AI workers have better memories."
It is:

```text
contracts create pressure
ships carry structured memories and holograms
stations blend the holograms they receive
workers download local familiarity
old routes fade unless traffic keeps refreshing them
```

That makes holographic storage a logistics primitive. A hologram should behave
like cognitive cargo: portable, lossy, copyable, mergeable, and naturally
decaying. This is the right shape for a no-omniscience economy because it gives
workers an associative sense of "this kind of route has mattered here" without
giving any station or pilot perfect remote knowledge.

Design tests:

- if a hologram cannot be transported by docking or local contact, it is not
  part of the gossip network
- if a hologram cannot decay or be overwritten by newer attention, it is too
  authoritative
- if a hologram directly settles work, it has crossed into station authority
  and is wrong
- if a player-facing hologram claim cannot point to a structured memory,
  receipt, contract, or chain event when inspected, it should stay hidden or
  be labeled as weak resonance

This framing turns the current market HNN work into more than a scorer. It is
the beginning of a carried, decaying institutional nervous system: structured
memory is the audit trail, holographic memory is the intuition, and station
authority is the law.

## Maturity Map

| Layer | Current Fit | Notes |
| --- | --- | --- |
| Station authority | Strong | Contracts, ledgers, manifests, receipts, and chain logs remain exact. |
| Physical information flow | Medium-strong | Dock/contact gossip is real; nearby workers exchange structured knowledge and carried market HNN traces; reset/load bootstrap is station-local instead of global; regression coverage now proves a worker can physically transport remote contract memory into another station. Idle workers with known contract pressure can now run no-cargo gossip courier trips; longer no-omniscience soaks remain the next proving ground. |
| Structured market memory | Strong | Demand, supply, route success/danger, receipts, route reputation/risk, station trust/risk, and stale-work risk are live or in active migration. |
| Worker specialization | Strong | Haul, mine, scaffold tow, distress/fracture scout, repair, and delivery-proof pressure share job-offer vocabulary and have first execution paths. |
| Remote logistics | Medium-strong | Supply memories can create remote pickup legs, compare competing source memories, score pickup legs from the worker's current ship position, and attach route success/danger/risk evidence to selected hauler job diagnostics; richer recent-failure planning remains open. |
| Holographic market memory | Medium | Pilot HNN exchange exists; a bounded prototype encodes carried market memories into dedicated neural worker market HNN traces, transports them through station market pools with weighted replacement, idle decay, and an effective-experience cap, exposes advisory resonance for haul, mine, tow, scout/fracture, delivery-proof, and repair, and copies compact source metadata from matching structured memories when available. HNN now has a separate inspect factor from proof/evidence; richer resonance explanations remain open. |
| Explainability | Medium-strong | Compact inspect factors, source/destination paths, selected-job reason codes, explicit gossip-courier reasons, readable source-chain rows, first-hop source/hops/age provenance, full proof hashes on job rows, prioritized matching source-memory rows, prioritized matching carried receipt rows, station-local receipt-chain retrieval, per-link receipt diagnostics, full subject/anchor/source/witness hashes on market diagnostic rows, proof/hash prefixes, compact heard/relay/witness labels, separate hologram score factors, HNN-backed source metadata, and a compact selected-job detail block with `[TAB]` paging over locally visible signed receipt links are live; receipt links now name known stations, registered player recipients, and worker custodians visible through `WORLD_NPCS`, and fall back to hashes for unknown relays. Remote missing-link retrieval remains thin. |
| Institutional memory | Medium-strong | Receipt-backed route reputation exists; newly heard delivery receipts promote into route reputation and station trust during physical gossip exchange; stations emit signed `CHAIN_EVT_ROUTE_HISTORY` summaries when distinct receipt evidence crosses the promotion threshold; station chain-history queries can now expose route-history tails and filterable aggregate route-memory rows; docked Contracts panels show a compact read-only `HISTORY` strip; and the docked HISTORY tab now shows aggregate cross-station route memory with all/outbound/inbound/local filters plus station-local event/epoch/tick/evidence context. Deep institution browsing remains open. |

## Vision Gap Matrix

| Vision Claim | Current Evidence | Gap | Next Doc/Build Decision |
| --- | --- | --- | --- |
| Ships transport economic information like cargo. | Dock/contact gossip, nearby worker contact exchange, known-contract compatibility, structured market memories, station stock supply memories, carried market HNN traces, station-local reset/load bootstrap, and idle-worker gossip courier trips. | Contact exchange is bounded and lower-bandwidth than dock exchange; starter traffic has a first behavior but still needs longer soak metrics. | Define the minimum starter traffic and soak metrics for a no-omniscience world. |
| Contracts create pressure, not omniscient task lists. | Contract summaries dual-write demand memories; stale active work dampens demand and adds station risk. | Contract UI and worker diagnostics can still look like command lists without explaining what is rumor versus exact work. | Keep contract boards actionable; move provenance into inspect and selected-job explanation copy. |
| Neural workers specialize from the economy. | Haul, mine, scaffold tow, distress/fracture scout, repair, and delivery-proof offers share `npc_job_offer_t`; remote supply can create current-position pickup legs; selected hauler diagnostics can now cite route memory or route risk evidence; delivery-proof workers can now bind, deliver, and clear NPC-owned shipments; HNN resonance can bias all current offer families. | Assignment still lacks deeper recent-failure planning, and HNN resonance is still a small advisory factor rather than a full planning layer. | Expand failure-history planning and resonance explanations without weakening station authority. |
| Holographic memory compresses repeated economic attention. | Worker-local bounded market HNN traces, runtime station market HNN pools, weighted replacement/capacity/idle-decay policy, station/pilot flight-HNN experience exchange, advisory haul/mine/tow/scout/proof/repair resonance, compact HNN source metadata, matching structured-memory expansion rows, matching carried receipt rows, station-local receipt-chain retrieval, per-link carried receipt diagnostics, compact relay/witness labels, selected job detail lines, and a focused receipt-relay view exist. | Full resonance explanations still lack drill-down into remote receipt links and remote relay identity. | Connect anchors to remote relay follow-through once station-local retrieval is exhausted. |
| The economy explains itself. | Inspect rows show source/destination, compact factors, explicit reason codes, readable source-chain lines, source station, hop count, observed-age buckets, full proof hashes on job rows, matching source-memory rows, matching carried receipt rows, station-retrieved receipt rows, full subject/anchor/source/witness hashes on market rows, four-byte proof/hash prefixes for market-memory-backed and HNN-backed jobs, explicit selected-job gap labels for proof-anchor-only, anchor-known/chain-not-local, carried-chain/no-local-link, and station-chain states, compact `[TAB]` paging, and a focused `[TAB]` receipt-relay view through locally visible receipt-link rows with known station/player/worker names. | Deep provenance still lacks remote retrieval for receipt links that are anchored but not available in carried or station-local stores. | Add remote anchor retrieval without bloating the main row. |
| Gossip can become history. | Delivery receipts, route success, route risk, station trust/risk memories exist; distinct receipt memories heard through gossip now reinforce route reputation and station trust, while duplicate receipt echoes do not inflate evidence; threshold crossings emit `CHAIN_EVT_ROUTE_HISTORY`; `/api/station/<id>/state?include=chain_history` exposes route-history tails and filterable aggregate route-memory rows; docked Contracts panels show compact read-only history rows; and the HISTORY tab shows aggregate cross-station route memory plus all/outbound/inbound/local filters and recent station-signed summaries with evidence and event context. | The durable record is now visible as compact filtered aggregate memory, but there is no deep route/station institution browser yet. | Build richer drill-down over route-history chain events without letting it affect payouts. |

## Current Implementation Snapshot

### Done

The current implementation has moved from bounded contract copies to a real
market-memory layer:

- `contract_summary_t` copies exact contracts into bounded gossip pools
- `known_contracts[]` remains as compatibility state for UI and behavior
- `knowledge_view_t` and `knowledge_item_t` carry situated knowledge
- `KNOW_PAYLOAD_MARKET_MEMORY` stores structured `market_memory_t`
- contract gossip dual-writes fuzzy demand memories
- station stock emits supply memories during dock gossip
- completed deliveries emit route-success and delivery-receipt memories
- route danger, route reputation, route risk, station trust, and station
  risk/default memories shape later scoring
- selected hauler job diagnostics can cite the strongest matching route
  success, delivery receipt, route reputation, route danger, or route-risk
  memory as provenance
- failed remote supply pickups and defaulted delivery shipments produce
  negative station-risk evidence
- chronically stale active contracts dampen demand memory and reinforce
  destination station-risk evidence
- delivery receipts collapse matching demand memories
- market memories decay by confidence and salience
- station and worker market HNN traces decay by effective experience when idle
  exchange provides no reinforcing structured memory
- dock handshakes exchange station/ship knowledge
- nearby worker contacts exchange bounded contract summaries, structured
  knowledge, and carried market HNN traces
- NPC scans, station Contracts panels, and inspect snapshots expose compact
  market-memory and job-choice diagnostics
- worker inspect rows now show source/destination paths, compact score factors,
  explicit reason/provenance codes, and first-hop source/hops/age metadata for
  market-memory-backed jobs; proof-backed rows also expose compact proof/hash
  prefixes
- NPC inspect snapshots prioritize the structured market-memory row that
  matches a selected job's carried proof hash, and market diagnostic rows carry
  full subject, chain-anchor, source, and witness hashes for source expansion
- NPC inspect snapshots also prioritize a carried cargo receipt row when its
  latest receipt hash matches a selected job's carried proof hash, then emit
  per-link receipt diagnostics while row budget remains
- compact scan labels distinguish heard job memory, relay/source hash, witness
  hash, subject hash, and proof anchor instead of collapsing them into a vague
  source line
- hauler assignment scores exact contracts and fuzzy demand memories
- hauler scoring uses supply, route pressure, receipt evidence, station
  trust/risk signals, and destination risk
- supply memories can create remote pickup legs
- hauling, mining, and scaffold tow assignment share `npc_job_offer_t`
- workers still resolve against exact contracts before loading or unloading
- holographic pilot experience already exchanges through stations
- neural workers can encode a bounded set of carried market memories into HNN
  traces at dock time and use haul, mine, tow, scout/fracture, delivery-proof,
  and repair resonance as small advisory scoring factors
- stations keep separate runtime market HNN pools so docked workers can
  physically transport holographic economic attention between stations without
  mixing it into flight-control memory
- market HNN pool writes downweight older traces under new attention and clamp
  effective experience so scoring stays bounded
- HNN-backed worker diagnostics attach a separate hologram score factor plus
  compact structured-memory source metadata where a matching memory is present,
  or a no-proof generated pressure label where the HNN trace has no local
  structured source

### Compatibility Still Present

`known_contracts[]` is intentionally still alive:

- client UI uses it to hide unknown contracts
- network sync uses it for player contract visibility
- worker behavior still includes exact known-contract candidates
- tests seed it directly in several places

Treat it as a compatibility cache, not the target model.

## Gap Analysis

### Partially Closed Gap 1: HNN Market Bundle Expansion

Structured market memory is live. Holographic market memory now has a bounded
station/worker runtime prototype, but it is not yet a full planning layer.
The current HNN value vocabulary can represent haul, mine, tow, scout,
delivery-proof, and repair jobs. The live market-memory encoder now stores
tractor demand as haul pressure, delivery demand and delivery receipts as
delivery-proof pressure, fracture demand as scout pressure, repair-kit supply
as repair pressure, ore pressure as mine pressure, and scaffold pressure as tow
pressure. Docked workers can upload/download those market HNN traces through
station-local pools. That is useful, but it should still be
documented as a runtime prototype, not the destination.

Needed encoding:

```text
market_key =
  bind(KIND, demand|supply|route|receipt|trust|risk)
  bundle bind(STATION_A, source_or_subject)
  bundle bind(STATION_B, destination)
  bundle bind(COMMODITY, commodity)
  bundle bind(CONFIDENCE, bucket)
  bundle bind(SALIENCE, bucket)
  bundle bind(AGE, bucket)
```

Action pressure:

```text
JOB_HAUL
JOB_MINE
JOB_TOW
JOB_SCOUT
JOB_DELIVER_PROOF
JOB_REPAIR
```

Mapping policy:

- tractor demand and ordinary finished-goods supply bias haul work
- delivery demand and delivery receipts bias delivery-proof work
- fracture demand biases scout/fracture work
- repair-kit supply biases repair work
- ore pressure and refinery starvation bias mining or fracture scouting
- build-plan and scaffold pressure bias tow work
- repair-kit scarcity, damaged workers, and repair-bay trust should further
  bias repair
- route danger and stuck-worker evidence should still bias scout/fracture work
- every non-haul HNN resonance path must have a structured-memory explanation
  path before it affects real assignments

Recommendation: keep both representations.

- structured memory is deterministic, inspectable, and testable
- holographic memory is fuzzy, compact, and useful for neural resonance

Remaining acceptance line:

- station and ship gossip can encode all current worker-relevant market
  pressures into HNN bundles and expose read-only resonance scores across haul,
  mine, tow, scout, proof, and repair job selection without changing payment or
  ledger authority. The UI can also show the structured memories that made each
  resonance believable.

### Closed Gap 2: NPC Delivery-Proof Execution

The shared offer vocabulary now covers hauling, mining, scaffold tow,
distress/fracture scout, repair, and delivery-proof pressure. Scout/fracture
work can execute through the miner path, and repair work can execute at a dock.
Delivery-proof work now executes through NPC-owned delivery shipments.

Target:

```c
typedef enum {
    NPC_JOB_NONE,
    NPC_JOB_MINE,
    NPC_JOB_HAUL,
    NPC_JOB_TOW,
    NPC_JOB_DELIVER_PROOF,
    NPC_JOB_SCOUT,
    NPC_JOB_REPAIR,
} npc_job_kind_t;
```

Each offer should carry:

- required hull/capability
- source object or station
- destination station
- commodity/material
- expected value
- confidence
- freshness
- danger cost
- route cost
- provenance/memory source

Acceptance line:

- a worker can not only compare haul, mine, tow, scout, proof, and repair
  candidates in one offer list, but can also execute delivery-proof jobs through
  NPC-safe shipment pickup, bound cargo, destination proof, debt clearing, and
  receipt emission.

Implementation note:

- the chooser evaluates all six offer families and records compact diagnostics
  for each visible job type. Delivery-proof now encodes worker debtors above the
  player id range, binds real cargo pubs into `delivery_shipment_t`, pays at the
  destination, emits receipt/trust/reputation memory, and clears origin debt
  when the worker returns proof to the source station.

### Gap 3: Starter Traffic Is Still Thin

`gossip_bootstrap_world_stations()` now seeds only station-local pressure at
world load/reset: local stock supply, ore pressure, scaffold pressure, and
contracts visible at that station. It no longer floods every station with every
active contract.

Remaining direction:

- worker pools begin with home-station memories through actual dock contact
- idle workers with contract memory can run no-cargo gossip courier trips
- starter traffic spreads demand/supply physically over longer soaks
- standing local jobs keep early workers from idling
- no hidden peer-station broadcast returns as a convenience path

Acceptance line:

- a fresh world can run for a deterministic soak period with no global contract
  flood and still produce mining, hauling, delivery, and scaffold activity.

### Partially Closed Gap 4: Worker Explainability Needs Source Chains

Market memories now have enough shape for scoring, and inspect rows expose
compact factors plus explicit reason codes, first-hop source/hops/age metadata,
compact proof/hash prefixes, matching source-memory detail rows, matching
carried cargo receipt rows, and full subject/anchor/source/witness hashes for
market diagnostic rows. Inspect HUD can now compose a compact selected-job
detail block from the selected job, matching source memory, matching carried
receipt, station-local retrieved receipts, visible receipt-link count, and
`[TAB]` paging over locally visible signed receipt links. Known station
authors/recipients, registered player recipients, and worker custodians visible
through `WORLD_NPCS` are named in those link rows; unknown relays still fall
back to short hashes. The remaining layer is remote retrieval/presentation for
receipt links that are anchored but neither carried by the scanned ship nor
present in station-local receipt storage.

Needed explanation fragments:

- heard at station
- relayed by worker
- receipt confirmed
- stale rumor
- route danger reported
- station trust
- station risk/default
- source hops

The main contract board should stay quiet and actionable. Provenance belongs in
inspect/debug surfaces and selected-job explanation copy.

Acceptance line:

- inspecting a worker can answer "why this job?" with demand, supply, route,
  freshness, confidence, proof/receipt factors, compact reason/provenance, and
  the memory source that supplied each claim, including receipt/chain anchors
  when present.

The next step is not more text in the main worker row. The selected-job detail
now clearly marks proof-anchor-only, anchor-known/chain-not-local,
carried-chain/no-local-link, and station-chain states, and `[TAB]` can open a
focused receipt-relay view over locally visible links. What remains is remote
retrieval or follow-through for anchored receipt links that are not locally
known.

### Partially Closed Gap 5: Remote Logistics

Station supply can influence hauler scoring and can produce a haul job whose
pickup station is not the worker's home. Route scoring now prices the
worker's current ship position to pickup to destination, so a mobile neural
worker can choose a nearby heard-about source instead of pretending every route
starts at home.

Remaining gaps:

- route scoring should include richer recent failure history
- pickup failures should produce sharper stale-supply or failed-pickup memory
- selected job diagnostics should explain why a source won, including stronger
  supply-vs-route-vs-demand tradeoff detail when those memories conflict

Acceptance line:

- a worker can hear "Kepler has frames" at Prospect, fly to Kepler to load, and
  deliver to the station whose demand made the job worthwhile, while inspect UI
  can explain that path.

### Gap 6: Gossip-To-History Promotion

`docs/metaproduct.md` wants trusted haulers, repaired routes, dangerous
privateers, construction corps, and maintenance trusts. The gossip layer now has
many of the short-lived inputs that could mature into those histories. A first
runtime promotion rule exists: distinct delivery receipt memories heard through
gossip reinforce route reputation and destination station trust without letting
duplicate echoes inflate evidence. When route evidence crosses the promotion
threshold, the receiving station signs a `CHAIN_EVT_ROUTE_HISTORY` summary.
Station state chain-history queries can expose recent route-history summaries.
Those API rows now include readable route, action, memory-kind, and summary
labels for operator/player-facing tooling. Docked Contracts panels can also
show a compact read-only `HISTORY` strip from the station's signed
route-history tail, and stations with signed route-history rows expose a
read-only HISTORY tab with aggregate cross-station route memory and recent
station-local signed summaries, evidence count, confidence, salience, event id,
epoch, and observed tick. What remains is filtered drill-down over the durable
route/station history.

Needed:

- expand the aggregate HISTORY tab with filtered route/station drill-down once
  the runtime reputation layer has seen enough distinct receipt evidence
- distinguish ephemeral "this route is hot right now" from durable "this route
  has verified reputation"
- let station history summarize completed work without making stale gossip
  permanent by accident
- expose route/station/pilot history as queries over receipts and chain logs,
  not as leaderboard counters

Acceptance line:

- a repeated route pattern can start as gossip, become receipt-backed runtime
  reputation, and appear as station-authored chain history without losing the
  authority/cognition boundary.

### Gap 7: Supporting Docs Need Vocabulary Cleanup

The canonical doc is current. Supporting docs need targeted edits.

| File | Review Status |
| --- | --- |
| `README.md` | Links the canonical gossip doc. Updated to make `H` scan/contact only and docked CONTRACTS the place to accept, load, and deliver work. |
| `ARCHITECTURE.md` | Good high-level fit. It should stay concise and link outward instead of absorbing roadmap detail. |
| `PRD.md` | Updated to describe neural worker assignment families instead of fixed Miner/Hauler NPC identities. |
| `ENG.md` | Updated to make `NPC_ROLE_*` compatibility labels explicit and to document `NPC_JOB_*` offer families as the target architecture. |
| `docs/cargo-architecture.md` | Good conceptual fit. Keep reinforcing that receipts prove matter movement while gossip carries attention. |
| `docs/decentralization.md` | Good sovereignty framing. Keep "stations sign truth; ships transport memory" language aligned with the gossip doc. |
| `docs/decentralization-synthesis.md` | Good bridge between federation/P2P and physical memory transport. |
| `docs/operator-onboarding.md` | Historical `set_miner_chatter`/`set_hauler_chatter` API names remain, but copy now frames them as economy-assigned worker chatter rather than permanent NPC identities. |
| `CONSTRUCTION_PLAN.md` | Updated to describe construction-spawned worker pools as job-offer specialization rather than fixed tow roles. |

## Decision Log For The Vision

These are the docs-review decisions that should steer the next implementation
passes:

| Decision | Rationale | Build Implication |
| --- | --- | --- |
| Keep authority, structured memory, and HNN resonance separate. | This prevents fuzzy associative memory from becoming hidden station truth. | HNN scores can bias offers, but every accepted job still resolves against contracts, manifests, receipts, and station ledgers. |
| Make holographic storage a compression layer for repeated attention. | Natural decay/interference is the right failure mode for rumors and route familiarity. | Encode broader market bundles only after the structured-memory fallback exists for the same claim. |
| Treat worker roles as assignments, not identities. | This matches the economy-sorts-specializations vision. | Keep new work inside `npc_job_offer_t`; next, broaden route planning and HNN resonance instead of adding fixed worker identities. |
| Keep the contract board quiet. | Players need command clarity first; provenance detail belongs elsewhere. | Selected contract rows should show only valid commands; inspect/detail surfaces should explain memory provenance. |
| Remove bootstrap last, not first. | Physical information flow is the goal, but early-world deadlock would make the system look inert. | First add enough local pressure, starter traffic, and HNN transport for deterministic no-bootstrap soaks. |

## Product Contract For Holographic Storage

Holographic gossip is useful precisely because it is fuzzy, decaying, compact,
and portable. It should not become a hidden authority channel.

Rules:

- HNN bundles may suggest or bias work
- HNN bundles may compress repeated memories
- HNN bundles may decay naturally through interference, replacement, and idle
  lack of reinforcement
- HNN bundles must never pay, verify, or mutate station ledgers
- every player-facing claim still needs a structured memory, contract, receipt,
  or chain-log anchor
- every HNN market scorer needs a structured-memory fallback or explanation
  path

## Target Architecture Checklist

Use this as the gap-analysis checklist before each implementation slice:

| Capability | Done When |
| --- | --- |
| Local truth boundary | Stations never accept HNN or gossip as proof; every payout and mutation resolves through exact state. |
| Physical memory transport | A memory or hologram can only spread by station/ship dock exchange or local contact, aside from explicit debug/bootstrap modes. |
| Worker specialization | New worker behavior enters through `npc_job_offer_t` and shared diagnostics, not a new permanent NPC class. |
| Hologram cargo | Workers can upload/download bounded HNN market traces at stations, with decay/replacement that prevents infinite memory. |
| Explainability | Inspect can show the exact contract/source memory/receipt anchor/reason code behind a selected worker job. |
| History promotion | Distinct receipt-backed success heard through gossip can reinforce runtime route/station history and threshold into a station-authored route-history chain event. |
| UI separation | Docked contract menus show commands; inspect/detail views show provenance and cognitive reasons. |

## Documentation Gap Actions

These are the documentation changes that would most improve alignment with the
vision:

| Gap | Action |
| --- | --- |
| Legacy role language | Any doc that mentions miner/hauler/tow NPCs should say whether it means an old `NPC_ROLE_*` compatibility label or a current worker job outcome. |
| HNN as only pilot memory | Keep expanding HNN language from "flight experience" to "portable familiarity" so market-memory transport feels intentional rather than bolted on. |
| Route history visibility | Operator API rows and the docked Contracts history strip are now readable context; next, design a fuller institution/history surface that still explains route history is not payout authority. |
| No-omniscience proof | Define a named soak scenario and success metrics: no global contract flood, local pressure only, physical memory spread, and measurable completed work. |
| Inspect vocabulary | Standardize labels for heard memory, source relay, witness, proof anchor, hologram match, and station risk so UI copy and docs use the same words. |

## Priority Gap Order

This is the implementation order implied by the docs review:

| Rank | Gap | Why It Comes Before The Others |
| --- | --- | --- |
| 1 | Remote receipt anchor retrieval | Local receipt-link paging, station-local receipt-chain retrieval, and the focused receipt-relay view exist, name known stations, registered players, and visible workers, and distinguish proof-anchor-only, anchor-known/chain-not-local, carried-chain/no-local-link, and station-chain states. The remaining legibility gap is retrieval/browsing for anchored links that are neither locally carried nor present in station storage. |
| 2 | Active-worker success/failure metrics | Active-worker replay now exercises selected worker diagnostics, gossip, and HNN market exchange on PRs, but the gate still measures presence and determinism more than useful economic outcomes. |
| 3 | Deep route/station history drill-down | Route-history chain events, a compact board strip, filterable aggregate API rows, and a HISTORY tab with filtered aggregate cross-station route memory exist; players still need deeper route/station browsing to inspect how gossip matures into institution memory. |
| 4 | Remote route planning detail | Multi-leg pickup is live; now the AI needs clearer evidence when it picks one source over another. |
| 5 | Session-token hardening | Legacy anonymous session-token ledger access is a real account/credit-theft risk and should be scheduled as security work, separate from the neural-worker merge. |
| 6 | No-omniscience soak | Once explanations, active-worker determinism, and starter traffic are stable, prove memory transport without hidden global knowledge. |
| 7 | Pilot reputation | Reputation is powerful enough to wait until source chains can distinguish witnessed labor from repeated rumor. |

## Recommended Next Slices

### Slice 1: Non-Local Receipt Anchor Retrieval

Expand the focused receipt-relay view from locally carried receipt links into a
way to retrieve or browse missing anchored links. The structured memory, source
station, hops, observed age, witness/source hash, chain anchor, matching carried
receipt row, per-link receipt diagnostics, and paged signed-link list now ride
in inspect snapshots when local data exists. Keep resonance as an additional
scorer input, not an authority source.

Why next: transported market holograms now cover the current worker
specializations, have bounded replacement, and carry compact source metadata.
The scan pane now translates compact source metadata into readable heard,
relay, witness, subject, and anchor labels; carries the full provenance hash in
the diagnostic row; prioritizes the matching structured memory row; surfaces
the matching carried receipt row; emits per-link receipt diagnostics; composes a
compact selected-job detail block; can retrieve station-local receipt chains;
and can open a focused `[TAB]` receipt-relay view over locally visible signed
links, using station, registered-player, and visible-worker names when a link
identity is locally known. The next risk is that the player cannot retrieve
remote links that are known only as anchors.

Acceptance line:

- selecting a worker/job proof can expand from `job -> structured memory ->
  carried or station-local receipt chain -> receipt links -> relay/source
  identity`, with clear named relay actors where known and a way to follow
  anchor-known links that are not available in local storage.

### Slice 2: Active-Worker Outcome Metrics

Extend active-worker replay from presence/determinism checks into useful
outcome checks.

Why next: this is the determinism counterpart to the product vision. The
economy layer most likely to accumulate subtle drift now has deterministic
fixtures for selected worker decisions, gossip, and HNN market exchange; the
remaining gap is proving those workers complete, fail, or recover from real
economic work in measurable ways.

Acceptance line:

- active-worker replay rows include bounded success/failure counters for worker
  assignments, HNN-backed scaffold moves, deliveries, repair/proof work, or
  route support, and the PR native/WASM gate validates those counters without
  cross-build hash drift.

### Slice 3: Aggregate Station History Browser

Build a fuller player/operator drill-down browser over durable route-history
events.

Why next: the runtime promotion, query path, operator-readable API summary, and
compact Contracts-board history strip exist now, and the docked HISTORY tab
makes aggregate signed route memory readable in-game. A filtered drill-down
surface would connect the worker economy to the metaproduct promise that
routes, trust, and institutions are made out of physical labor.

Acceptance line:

- route success/risk/trust memories can be inspected from
  `CHAIN_EVT_ROUTE_HISTORY` events in a readable UI without making those
  summaries authoritative for payouts or inventory.

### Slice 4: Remote Route Planning Detail

Improve the remote pickup path with route-danger/failure history, clearer
source-selection diagnostics, and failed-pickup memory.

Why next: current-position multi-leg source selection is live, so the next risk
is explaining why the selected source beat the other rumors.

Acceptance line:

- inspecting a worker can explain why it chose one source station over another
  using demand, supply, route cost, danger/risk, stale-supply evidence, and
  current position.

### Slice 5: Session-Token Hardening

Move legacy anonymous ledger/save access away from client-chosen bearer tokens,
or constrain that path so real station balances and saves require signed
identity.

Why next: the neural-worker push did not worsen this, but the substrate is now
valuable enough that old anonymous convenience paths are no longer harmless.

Acceptance line:

- station ledger mutations for real saves are keyed by verified pubkey or a
  server-issued migration credential, not by a client-selected 8-byte session
  token.

### Slice 6: No-Omniscience Soak

After local pressure, HNN bundles, and early standing work are reliable, prove
the local-only bootstrap under longer deterministic traffic soaks.

Why next: this validates the physical information-flow promise.

Acceptance line:

- a deterministic fresh-world soak with no global contract flood still produces
  mining, hauling, delivery-proof, repair/scout/tow pressure, and repeated
  physically transported cross-station memories. Single-hop physical memory
  transport is covered by `test_neural_worker_physically_transports_contract_memory_between_stations`.

### Slice 7: Pilot Reputation

Add pilot/worker reputation memories only after source-chain explanations can
distinguish witnessed work from repeated rumor.

Why next: reputation is powerful, but it becomes noise unless the source chain
can explain who earned it and where it was witnessed.

Acceptance line:

- reputation rows cite witnessed receipt/chain-log evidence and never use HNN
  resonance as identity proof.

## Risks

- **Invisible AI decisions.** Fuzzy scoring without inspectability will read as
  broken behavior.
- **Parallel memory drift.** `known_contracts[]`, `knowledge_view_t`, and HNN
  bundles need a clear migration order.
- **Over-authoritative gossip.** Memories should influence discovery and
  scoring, never payment or ledger mutation.
- **Cold-start deadlock.** Removing bootstrap too early can stall the economy.
- **Bounded active economy drift coverage.** PR replay now exercises a selected
  worker/HNN diagnostic fixture across native and WASM, and the long set keeps
  active workers exchanging gossip/HNN market memory. It is still a bounded
  gate, not a full no-omniscience autonomy soak.
- **Anonymous bearer credentials.** Client-chosen session tokens still touch
  legacy ledger/save paths and need a dedicated security pass.
- **UI noise.** Contract menus should stay actionable; provenance belongs in
  inspect surfaces and focused selected-row hints.

## Take

The docs and code now agree on the spine:

> Stations sign truth. Ships transport memory. Holograms let the economy think
> by moving through space.

The next work should make that spine durable: non-local receipt-anchor
retrieval, aggregate station-history browsing, stronger route-planning
diagnostics, and a deterministic no-omniscience soak that proves memory really
moves by traffic.
