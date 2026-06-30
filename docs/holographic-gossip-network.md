# Holographic Gossip Network

**Status:** canonical design target for market gossip, neural worker
coordination, and holographic economic memory.

## Purpose

Signal's economy should not depend on global omniscience or fixed NPC roles.
Stations know their own truth. Ships learn by docking, traveling, and meeting
other actors. Neural workers choose jobs from situated, decaying market memory.

The contract ledger remains exact. Gossip is how work becomes discoverable.

## Two Layers

### Authoritative State

This layer pays, verifies, and mutates the world:

- `contract_t`
- station ledgers
- station chain logs
- cargo manifests
- cargo receipt chains
- delivery shipment ledgers

Authoritative state is exact, signed where needed, and resolved at stations.

### Gossip State

This layer discovers, scores, and routes work:

- `knowledge_item_t`
- `knowledge_view_t`
- compatibility `known_contracts[]`
- holographic station/pilot memory bundles

Gossip state is bounded, local, portable, and lossy. It may be stale. It may be
incomplete. Actors use it to decide what to attempt, then resolve the attempt
against authoritative state.

## Transport Rule

Information speed equals ship speed.

Stations do not broadcast perfect station-to-station truth. A station can know:

- its own authored contracts
- memories carried in by docked ships
- memories exchanged through nearby contacts
- receipts or proofs physically delivered to it

Ships are couriers of both goods and information.

## Memory As Cargo

The gossip network should feel like logistics, not chat.

A contract creates pressure at one station. A receipt proves work happened at a
station. A ship carries those impressions somewhere else. The result is a
market that learns by moving:

```text
station need
  -> structured memory
  -> ship/station exchange
  -> worker job pressure
  -> physical attempt
  -> receipt, failure, or route memory
  -> reinforced or decayed memory
```

This is the live bridge between the moment-to-moment game and Signal's larger
institutional memory. Long-lived station history remains exact in chain logs and
receipts. Short-lived economic attention lives in gossip and fades unless the
world keeps proving it matters.

Holographic storage is the compressed version of the same idea. A ship can
carry structured memories for inspection and also carry a bounded holographic
trace of repeated market pressure. The structured memory says what was heard.
The hologram says what pattern has become familiar. Both move through physical
contact: docked station exchange or local ship-to-ship contact. Neither
replaces station authority.

This makes holograms a kind of cognitive cargo. They can be picked up at a
dock, blended into a station's local market trace, transported by another
worker, and forgotten through normal decay/interference. That is the exact
failure mode the economy wants: old opportunities fade unless traffic keeps
proving them.

## Memory Kinds

The first implementation layer uses structured market memories inside
`knowledge_item_t`.

Core payload families:

- `KNOW_CONTRACT`: exact-ish summary of an authored contract
- `KNOW_MARKET`: fuzzy supply/demand/route/proof memory

Current `KNOW_MARKET` subkinds:

- station supply
- station demand
- route danger
- route success
- delivery receipt
- route reputation
- route risk
- station trust
- station risk/default

Future subkinds can specialize further if useful:

- pilot reputation

## Market Memory Semantics

A market memory is not a contract. It is an impression:

```text
kind: demand
station_a: Helios
station_b: Prospect
commodity: ferrite ingots
quantity_hint: medium
value_hint: good
confidence: strong
salience: high
hops: 1
```

Meanings:

- `confidence`: how trustworthy this memory is
- `salience`: how strongly actors should care right now
- `hops`: how many exchanges have relayed it
- `observed_tick`: when the source observed the fact
- `learned_tick`: when this actor learned it
- `subject_hash`: stable identity for deduplication
- `source_hash`: immediate carrier/source
- `witness_hash`: original witness, if known
- `chain_anchor`: exact receipt/event hash if this memory is backed by proof

## Decay

Gossip should fade unless reinforced.

Baseline decay:

- confidence decays slowly
- salience decays faster
- hop count penalizes exchange
- station-authored or receipt-backed memories decay slower
- repeated matching memories reinforce through replacement or later holographic
  bundling
- completion receipts collapse matching demand memories
- station and worker HNN market traces lose one effective experience per idle
  minute when dock/exchange applies no reinforcing market memory

Decay is a gameplay feature: old rumors become less compelling and fresh
routes form from current traffic.

## Holographic Encoding

Structured memory stays inspectable. Holographic memory makes the neural layer
compact and fuzzy.

The intended encoding is:

```text
market_key =
  bind(KIND, market_kind)
  bundle bind(STATION_A, station_a)
  bundle bind(STATION_B, station_b)
  bundle bind(COMMODITY, commodity)
  bundle bind(CONFIDENCE, bucket)
  bundle bind(SALIENCE, bucket)
```

The target value side encodes action pressure:

```text
JOB_HAUL
JOB_MINE
JOB_TOW
JOB_SCOUT
JOB_DELIVER_PROOF
JOB_REPAIR
```

Repeated compatible memories reinforce. Conflicting memories interfere. The
structured layer remains the audit trail and UI source.

Current implementation note: the job vocabulary includes haul, mine, tow,
scout, delivery-proof, and repair vectors. Worker-local market-memory storage
now maps ordinary tractor demand to haul pressure, delivery demand and receipts
to delivery-proof pressure, fracture demand to scout pressure, repair-kit
supply to repair pressure, ore pressure to mine pressure, and scaffold pressure
to tow pressure. Stations also keep a runtime-only market HNN pool: docked
neural workers can upload carried market traces and download the station's
aggregate trace, so holographic economic attention can physically move from
one dock to another. Those HNN traces are advisory scoring inputs only. The
current station-pool policy downweights older traces as new market holograms
arrive, caps effective market experience so resonance stays bounded, and
decays unreinforced station/worker traces over idle exchange time. Richer
player-facing resonance explanations remain future work.

Holographic storage is allowed to be fuzzy because it is advisory. It can bias a
worker toward a remembered kind of work, compress repeated route experience, and
decay naturally through interference. It must not pay, verify, or mutate station
authority. Any player-facing claim still needs a structured memory, contract,
receipt, or chain-log anchor.

Product contract:

- HNN bundles may suggest or bias work
- HNN bundles may compress repeated memories
- HNN bundles may decay naturally through interference, replacement, and idle
  lack of reinforcement
- HNN bundles must never pay, verify, or mutate station ledgers
- every player-facing claim still needs a structured memory, contract, receipt,
  or chain-log anchor
- every HNN market scorer needs a structured-memory fallback or explanation
  path

## Holograms As Cargo

Holographic storage fits the gossip network because it behaves like carried
memory rather than a database. A hologram can be transported, blended with a
station's local trace, copied into another pilot, and allowed to decay through
normal interference. That makes it the right substrate for economic attention:

- a route that keeps paying becomes easier for workers to recognize
- a stale opportunity fades unless ships keep re-observing it
- contradictory memories reduce certainty instead of becoming permanent facts
- every useful fuzzy trace still points back to structured memories when the UI
  needs to explain the decision

The strongest product rule is: **holograms are portable familiarity, not
truth.** They should make workers curious about work; contracts, manifests,
receipts, and ledgers decide whether that work actually resolves.

Build contract:

- structured memory must exist for every player-facing claim
- HNN resonance may bias a worker only as an advisory score factor
- station HNN pools are transport pools, not station ledgers
- any worker action picked from resonance must still resolve against exact
  contracts, manifests, receipt chains, or station services
- resonance explanations should point back to the closest structured memory
  when one is locally available

## Worker Decision Loop

Every neural worker should eventually follow this shape:

```text
exchange knowledge when docked or in contact
decay local knowledge
score job offers from situated memory
choose the strongest job
specialize hull/capability for that job
fly with neural pilot
resolve against authoritative station state
emit receipt/failure/route memory
```

Role is an outcome, not an identity. A worker becomes a hauler because the best
memory-supported job is hauling. It becomes a miner because the best
memory-supported job is raw supply.

## UI Rule

The main contract menu should stay quiet and actionable.

Good:

- selected contract action line
- bottom navigation legend
- concise route/payout rows

Detailed gossip provenance belongs in inspect/debug surfaces:

- heard at station
- relayed by worker
- stale rumor
- route danger
- receipt confirmed
- confidence/salience

## Migration Path

1. Keep `known_contracts[]` as compatibility behavior.
2. Add structured `KNOW_MARKET` payloads. Status: demand, supply, basic route
   success/danger, delivery receipt, route reputation, and route risk memories
   are live. Positive station trust from completed work is live; station
   risk/default from failed remote pickup/stale supply, defaulted delivery
   shipments, and chronically stale active work is live. Richer source
   provenance and pilot reputation remain future work.
3. Add decay and inspect/debug visibility. Status: market confidence/salience
   decay, NPC scan diagnostics, docked station gossip rows, local ship-contact
   exchange, and compact worker
   job-choice diagnostics with value/demand/supply/route/freshness/capability/proof/
   hologram factor bytes are live. Worker inspect rows also show source/destination paths
   and explicit compact reason codes for known contracts, heard demand, remote
   supply, receipt proof, route memory, route risk, trust/risk, holographic
   resonance, ore pressure, and build plans. Market-memory-backed job rows also
   carry compact source station, hop count, observed-age metadata, proof/hash
   prefixes, and the full carried proof hash for job diagnostics. HNN-backed
   rows now attach the structured market memory that best explains the
   resonance when one is available, and otherwise label the generated pressure
   without pretending to have a proof hash. Inspect snapshots now prioritize
   the structured market-memory row that matches a selected job's carried proof
   hash and carry full subject, chain-anchor, source, and witness hashes for
   market diagnostic rows. When the scanned ship carries a receipt chain whose
   latest receipt hash matches a selected job proof, the matching cargo receipt
   row is prioritized in the same inspect snapshot, followed by per-link
   receipt diagnostic rows within the inspect row budget. If the scanned ship
   does not carry that chain, the inspect snapshot can also retrieve a matching
   chain from station-local receipt storage and mark it as a station chain.
   Compact HUD labels now distinguish heard memory, relay/source hash, witness
   hash, and proof anchor when that data is locally known. The inspect HUD also
   composes a compact selected-job detail block with job status, reason, top
   score factors, heard memory, proof relay, carried/station receipt status,
   explicit proof-anchor-only versus chain-not-local states, and a bounded list
   of locally visible signed receipt links when available. During an undocked
   scan, `[TAB]` opens and pages a focused receipt-relay view over those local
   links; `Shift+TAB` closes it. Remote retrieval for links that are only known
   by anchor remains future work.
4. Score side-by-side job offers from `knowledge_view_t`. Status: hauler
   assignment can use fuzzy demand, supply, route success/danger pressure, and
   receipt-backed route evidence, including current-position remote pickup legs
   from competing station supply memories; mining, scaffold tow,
   distress/fracture scout, repair, and delivery-proof offers now participate in
   the shared offer vocabulary.
   Scout/fracture, repair, and delivery-proof offers have safe execution paths.
   Delivery-proof workers can bind cargo into NPC-owned shipment ledger entries,
   deliver it to the destination, then return proof to the origin for debt
   clearing.
5. Convert worker assignment to knowledge-derived job offers. Status: hauling,
   mining, scaffold tow, scout/fracture, repair, and delivery-proof assignment
   pressure now flow through `npc_job_offer_t` with normalized cross-role
   scoring. The delivery shipment ledger now supports NPC-owned recourse
   shipments without exposing those worker debtors as player ledger entries.
6. Encode market memories into HNN bundles for neural resonance. Status: a
   bounded prototype encodes carried structured market memories into dedicated
   neural-worker market HNN traces at dock time and exposes advisory resonance
   to haul, mine, tow, scout/fracture, delivery-proof, and repair scoring.
   Stations keep a separate runtime market HNN pool that workers can
   upload/download during dock exchange, distinct from holographic
   flight-control experience. Holographic pilots can also upload/download
   aggregate flight experience at stations. Market HNN pools use weighted
   replacement plus an effective-experience cap; fuller resonance explanations
   remain future work.
7. Promote newly heard, distinct receipt-backed memories into runtime route
   reputation and station trust, then emit station-signed
   `CHAIN_EVT_ROUTE_HISTORY` summaries when evidence crosses the promotion
   threshold without making stale gossip permanent by accident. The docked
   HISTORY tab can now group those signed rows into aggregate cross-station
   route memory while preserving station-local recent event context.
8. Deepen starter traffic and no-omniscience soak coverage now that reset/load
   bootstrap seeds station-local pressure without peer-station broadcasts.
   `scripts/check_no_omniscience_soak.py` is the first named proof gate: it
   runs a broad fresh-world active-worker soak plus focused HNN worker
   specialization fixtures twice each, requires byte-identical replay output,
   and asserts local knowledge/HNN-memory transport plus route or useful worker
   outcomes.

## Acceptance Shape

The next evolution is ready when a no-omniscience soak can run from a fresh
world with these properties:

1. Stations initially know only their own authored pressure and local memory.
2. Workers transport structured market memories and HNN market traces by docking
   or contact.
3. Workers specialize through `npc_job_offer_t` pressure, not fixed identity.
4. Every completed job mutates only station authority, manifests, receipts, and
   ledgers.
5. Inspect/debug surfaces can explain the chosen job with exact work, heard
   memory, route evidence, receipt anchors, and HNN resonance where applicable.
6. Repeated receipt-backed patterns can first reinforce runtime route/station
   reputation and then promote into station-signed route-history events without
   making stale gossip permanent by accident.
7. Station chain-history queries, docked Contracts history rows, and the
   read-only station HISTORY tab can expose aggregate and recent route-history
   summaries as read-only context with human-readable route/action/evidence
   labels, never as payout or inventory authority.

Run the current no-omniscience proof gate with:

```sh
cmake --build build --target signal_replay --parallel
python3 scripts/check_no_omniscience_soak.py ./build/signal_replay
```

## Non-Goals

- Gossip does not pay players.
- Gossip does not mutate ledgers directly.
- Holographic memory does not replace signed receipts.
- Main contract UI should not expose every memory detail.

## Design Sentence

Stations sign truth. Ships transport memory. Holograms let the economy think
by moving through space.
