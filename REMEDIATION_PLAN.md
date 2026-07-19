# Signal Remediation Plan

Derived from full-codebase reviews on 2026-05-29 and 2026-06-09, then
re-groomed on 2026-06-13 around the metaproduct thesis in
[`docs/metaproduct.md`](docs/metaproduct.md).

Items are ordered by leverage against Signal's core product stack:

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

---

## 0. Metaproduct Alignment

**Problem:** Product, engineering, decentralization, bridge, and remediation
docs have described adjacent futures: server federation, pure P2P, Solana
bridging, Arweave deployment, provenance, RATi identity, and station
civilization. Without one hierarchy, backlog items look like competing
products.

**Fix:** Treat [`docs/metaproduct.md`](docs/metaproduct.md), [`PRD.md`](PRD.md),
and [`ENG.md`](ENG.md) as the canonical framing:

- lead with the rock game
- preserve the rule that large social structures come from physical actions
- make object history visible
- keep station sovereignty local
- verify station history offline
- anchor portable artifacts to Arweave/permaweb
- keep Solana-style wrapping as an adapter over native Signal history

---

## 1. Determinism Acceptance (#588)

**Problem:** P2P and quorum authority require cross-platform deterministic state.
The original #588 calls for replacing float sim state with `q32.32`, but that
rewrite is not what is currently built. Recent work added a stricter replay
ratchet instead: exact IEEE-754 bit hashing, native↔WASM replay gates,
fracture/thrown-rock coverage, and long-horizon probes.

**Fix:** Decide and document the acceptance path:

- **Durable path:** migrate settlement-critical sim state and step functions to
  `q32.32`.
- **Staged path:** promote strict native↔WASM replay gates as the acceptance
  criterion, then add Linux x86, PR-triggered gates, and broader replay
  scenarios before P2P work depends on it.

Open coverage gap: the current long-horizon replay setup disables NPC ships
after world construction, so it stresses flight, physics, production, and
settlement accumulation but not the neural-worker/gossip/HNN economy over
10k-100k ticks. Add a long-horizon scenario with active neural workers,
contract gossip, and HNN exchange before treating the staged path as complete.

Either path must keep render-only math separate from authoritative sim math.

---

## 2. Manifest Authority (#340 / #339)

**Problem:** The metaproduct depends on concrete cargo identity, but some
economy paths still treat finished goods as aggregate floats. That weakens the
claim that value moves as physical/provenanced matter.

**Fix:** Make buy, sell, deliver, production, and NPC hauling move concrete
`cargo_unit_t` rows by default. Retire finished-goods float authority once
compatibility migrations are complete.

Acceptance:

- trades preserve `cargo_unit_t.pub`, `parent_merkle`, origin, and receipt state
- delivery consumes the exact unit it validated
- station stock and player inventory can be audited from manifests
- aggregate floats remain only for raw ore/bulk compatibility

---

## 3. Lineage View

**Problem:** The substrate can already remember objects, receipts, and signed
events, and `signal_chain_assets --lineage=<cargo_pub>` can now print a
command-line cargo tree. The player-facing killer demo is still not yet one
inspectable in-game artifact. Signal needs to show the ladder from one rock to
shared infrastructure.

**Fix:** Add a lineage query/view that can walk:

```text
rock -> fragment -> ingot -> frame -> outpost/module/gate contribution
```

Acceptance:

- cargo inspection can show parent fragment/rock identity where available
- crafted products expose input cargo roots
- construction contributions preserve consumed frame/product pubkeys
- verifier/export tooling can emit a human-readable lineage tree (shipped for
  cargo SMELT/CRAFT chains)
- docked UI can show the same story without requiring command-line tools

---

## 4. Typed Provenance Contracts (#587)

**Problem:** Contracts can reference provenance classes, but the next product
step is contracts that price explicit witnessed events: this fragment, this
death, this fracture, this vessel, this route.

**Fix:** Add a typed contract target surface:

- explicit 32-byte target pubkey
- contract type enum for cargo, fracture, death, vessel, construction, and route
- fulfillment paths for fracture/death events
- UI row classification so players can read why a contract accepts or rejects
  their cargo/event

---

## 5. Settlement Event Bridge (#354 / #355 / #356)

**Problem:** Chain logs and the settlement engine exist, but not every validated
gameplay fact is distilled into canonical settlement events.

**Fix:** Bridge authoritative sim validation into settlement facts:

- smelt/craft/transfer/delivery events
- construction and outpost milestones
- station-authored hail/work signal roots
- fracture/death facts where they affect contracts or identity
- checkpoint roots that can be verified forward from the prior checkpoint

Non-goal: settlement does not track every frame of ship movement.

---

## 6. Player-Facing Lineage

**Problem:** The provenance substrate is strong, but it is still too invisible
to normal players. The game should feel like a rock economy first, and the
history should appear naturally when the player sells, delivers, inspects, or
builds with cargo.

**Fix:** Add UI surfaces for:

- cargo provenance inspection
- station-local ledger/history rows
- contract lineage requirements and rejection reasons
- RATi-grade delivery context
- construction milestones as station-signed history

This pairs with #586, #242, #337, #243, and #575 first-session clarity work.

---

## 7. Institution Tools

**Problem:** The bigger game is cooperative infrastructure drama, but player
groups still lack mechanics that make them institutions rather than labels.

**Fix:** Add tools that make social work legible:

- shared contracts
- escrowed cargo
- signed delivery obligations
- station-endorsed bounties
- route health dashboards
- public construction manifests
- verified contribution ledgers

Each tool must attach to real station/cargo/receipt history rather than a
detached social menu.

---

## 8. Unified Ship/Controller Model (#294)

**Problem:** NPC haulers and players still have parallel state paths. Any
parallel cargo path risks provenance divergence.

**Fix:** Retire remaining `npc_ship_t` cargo authority in favor of the unified
`ship_t` + `character_t` substrate. NPCs and players should move manifests,
receipts, and cargo identities through the same mechanics.

---

## 9. Permaweb and P2P (#590 / #591 / #589)

**Problem:** Client Arweave reads, peer anchoring, and WebRTC mesh behavior are
well scoped but should not outrun determinism or canonical settlement events.

**Fix order:**

1. client reads discovery manifest, chain-log tip, and snapshot txids from
   Arweave
2. client verifies station pubkeys and chain-log continuity before bootstrap
3. peers compare state roots over WebRTC data channels
4. peer anchor service uploads events/snapshots and updates discovery manifests
5. quorum behavior replaces operator-only authority for selected shards

---

## 10. RATi Vessel Identity (#496)

**Problem:** Player identity persists today as local Ed25519 keys and saves.
The stronger lore/product frame is that RATi persists across worlds and a
RATi-bearing vessel is the local embodiment of that cross-world identity.

**Fix:** Implement substrate-attached vessel birth after manifest authority and
settlement events are canonical:

- station witnesses vessel birth
- physical materials fund the identity embodiment
- RATi namespace binds to the vessel event
- death can destroy embodiment without destroying cross-world identity
- verification tools can prove the vessel's lineage

---

## 11. Streaming Entity Pool (#285)

**Problem:** Hard caps still shape world scale and make some subsystem splits
awkward.

**Fix:** Lift protocol/entity caps after economic/provenance invariants are
stable. Then extract bounded subsystems from `server/game_sim.c`: signal grid,
docking/trading, scaffold lifecycle, cargo pods, and station logistics.

---

## 12. CI, Fuzzing, and Safety Breadth

**Problem:** The local harness is strong, but malformed protocol/save inputs and
platform drift need broader automated coverage.

**Fix:**

- keep replay gates on the blocking path
- add Linux x86 and Windows coverage for determinism-critical targets
- run ASan/UBSan and clang-tidy in CI
- ~~add libFuzzer harnesses~~ for protocol decode, save load, and chain-log
  parsing. **First harness shipped 2026-07:** `make fuzz-receipts` covers the
  untrusted receipt/handoff decode paths (`tests/fuzz/fuzz_cargo_receipt.c`:
  `cargo_receipt_unpack`/`_chain_verify`, `ship_receipts_*` store ops,
  `handoff_ticket_unpack`/`_verify_hashes`, `handoff_ship_snapshot_unpack`),
  with a minimized seed corpus tracked in `tests/fuzz/corpus/` and a
  standalone ASan replay target (`make fuzz-receipts-standalone`) for crash
  triage. Still open: full client-input/snapshot protocol decode (needs
  world/player shims), save-load parsing, chain-log parsing, and a
  time-bounded CI job so the harness runs on PRs instead of only locally.

---

## 13. Maintenance Backlog

These are still valuable, but no longer outrank the metaproduct substrate:

- `sim_ai.c` frontier director extraction
- `hud.c` decomposition
- focused unit tests for scaffold, signal grid, trade paging, and frontier
  planning
- A* nav graph overflow visibility/fallback
- memory allocator strategy for hot-path pools and frame arenas
