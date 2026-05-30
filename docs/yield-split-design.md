# Yield-Split NFTs: Cargo Units as Fee Streams

**Status:** design draft

## The insight

Yield-split NFTs are not a new thing. They are exactly Signal's cargo units — ingots, frames, modules — with on-chain yield rights attached. The connection is structural, not metaphorical.

Signal's cargo architecture defines three states of matter:

```
FRAGMENT (in space)     →  no crate identity, has fragment_pub, physical
BULK FLOAT (at station) →  no identity at all, ephemeral working buffer
CRATE (anywhere)        →  cargo_unit_t with pub + parent_merkle, named, provenance-bearing
```

**Crate identity is born at the smelt boundary.** Below the boundary, matter is fragments and bulk float — characterized but not named. At the boundary, the furnace transforms a fragment into an ingot. The ingot gets a `cargo_unit.pub` (content hash) and `parent_merkle = fragment_pub`. This is the moment identity appears.

The yield-split NFT IS the ingot. When Signal's #480 on-chain anchoring lands, the smelt event recorded in the chain log becomes verifiable on Solana. The ingot — which already carries the fragment's provenance and the smelter station's signature — is minted as a Metaplex Core NFT. That NFT entitles its holder to a share of the LP fees from the Trebuchet-deployed pool that backs the token the ingot represents.

## The full pipeline

```
SIGNAL                              RATi                        TREBUCHET + CHAIN
─────────────────────────────────────────────────────────────────────────────────

1. Player mines asteroid
   ├─ Fracture → 100 fragments
   ├─ Tow fragment to station
   └─ Smelt fragment → ingot
      cargo_unit.pub = hash(commodity, grade, fragment_pub, idx)
      parent_merkle = fragment_pub
      Chain log: CHAIN_EVT_SMELT signed by station

2. Haul ingot, deliver contract
   ├─ CHAIN_EVT_TRANSFER
   └─ CHAIN_EVT_TRADE

                                  3. #480 anchors chain-tip hash
                                     to Solana

                                  4. RATi registry validates:
                                     - station pubkey
                                     - smelt event in chain log
                                     - fragment_pub lineage

                                  5. Burn-to-mint: canonical RATi
                                     minted to miner's wallet

                                                                6. Trebuchet deploys CLMM pool
                                                                   with locked LP (Burn & Earn)

                                                                7. Yield-split contract:
                                                                   ingot = NFT minted
                                                                   NFT holder = yield stream
                                                                   parent_merkle = fragment_pub

                                                                8. Yield accrues:
                                                                   Token side burned
                                                                   Quote side split per ingot
```

## The smelt boundary as the mint event

This is the key alignment. Signal's smelt boundary — where `cargo_unit.pub` is first computed and `parent_merkle` is set to `fragment_pub` — is exactly the moment a yield-split NFT should be minted.

| Signal concept | On-chain equivalent |
|----------------|---------------------|
| `fragment_pub` | Fragment provenance recorded in NFT metadata |
| `cargo_unit.pub` | The NFT's content identity (deterministic from inputs) |
| `parent_merkle` | The NFT's provenance root — what was consumed to create it |
| `CHAIN_EVT_SMELT` | The signed proof that the smelt happened at a specific station, epoch |
| `station_pubkey` | Which station performed the smelt — recorded in NFT attributes |
| `commodity` + `grade` | The ingot's material and quality — encoded in NFT traits |

**No new identity system is needed.** Signal already content-addresses every object. The NFT is just the on-chain representation of a cargo unit whose existence is already provable from the chain log.

## Composition: from ingots to frames to modules

Signal's cargo architecture already defines composition:

```
cargo_unit (ingot) → hash_product(recipe_id, sorted_input_pubs, idx) → cargo_unit (frame)
cargo_unit (frame) → hash_product(recipe_id, sorted_input_pubs, idx) → cargo_unit (module)
```

For yield-split, composition works the same way:
- Burn N ingot NFTs → mint 1 frame NFT
- The frame's `parent_merkle = merkle_root(sorted_ingot_pubs)`
- The frame NFT has a higher yield multiplier (it represents N ingots worth of yield)
- The provenance DAG is intact: walk from frame → each ingot → each fragment → each asteroid

### Yield multiplier and composition bonus

| Tier | Signal name | Chunks of yield | Composition bonus | Signal analogue |
|------|-------------|----------------|-------------------|-----------------|
| Fragment | raw fragment | Not an NFT yet | — | Matter before smelt boundary |
| Ingot | smelted ingot | 1× (baseline) | — | First crate identity |
| Frame | fabricated frame | 10× (from 10 ingots) | None (exact sum) | Hash of input ingots |
| Module | assembled module | 50× (from 5 frames) | None (exact sum) | Hash of input frames |

The yield multiplier is simply the number of base ingots the composed unit represents. A frame made from 10 ingots earns 10× the yield of one ingot — no bonus needed, because it IS 10 ingots, just consolidated into one NFT. The value proposition of composition is consolidation and higher-tier utility, not yield multiplication.

Alternatively, a composition bonus could be funded by the token-burn side of harvests: if the protocol burns 100 units of token-side fees this harvest cycle, it could allocate an extra 20 units worth of quote-token yield to composed holders. The bonus rewards consolidation without diluting fragment-level holders.

## The yield-split contract

### Account model

```
YieldVault (PDA ["yield-vault", pool_id])
  ├─ pool_id: Pubkey               // CLMM pool
  ├─ position_nft: Pubkey          // Burn & Earn position NFT
  ├─ token_mint: Pubkey            // Token side (RATi)
  ├─ quote_mint: Pubkey            // Quote side (SOL or USDC)
  ├─ total_yield_shares: u64       // Sum of all active ingot multipliers
  ├─ harvested_quote: u64          // Accumulated, unclaimed quote tokens
  ├─ total_token_burned: u64       // Lifetime token side burned
  ├─ last_harvest_slot: u64
  ├─ harvest_authority: Pubkey
  ├─ paused: bool
  └─ bump: u8

IngotRecord (PDA ["ingot", vault, cargo_pub])
  ├─ cargo_pub: [u8; 32]           // Matches Signal's cargo_unit.pub
  ├─ parent_merkle: [u8; 32]       // fragment_pub for base ingots, merkle_root for composed
  ├─ tier: u8                      // 1=ingot, 2=frame, 3=module
  ├─ multiplier: u64               // 1 for ingot, N for composed
  ├─ station_pubkey: [u8; 32]      // Signal station that smelted/fabricated
  ├─ smelt_epoch: u64              // Signal sim tick
  ├─ holder: Pubkey                // Current owner
  ├─ claimed_quote: u64            // Lifetime claimed
  ├─ source_ingots: [u8; 320]      // Up to 10 cargo_pubs (for composed units)
  └─ bump: u8
```

### Instructions

```
InitializeVault(pool_id, position_nft, token_mint, quote_mint)
  Creates YieldVault PDA.

MintIngot(cargo_pub[32], parent_merkle[32], station_pubkey[32], smelt_epoch, chain_log_proof)
  ├─ Validates chain-log proof against station pubkey
  ├─ Verifies cargo_pub = hash_ingot(commodity, grade, fragment_pub, idx)
  ├─ Mints Metaplex Core NFT to signer
  ├─ Creates IngotRecord
  ├─ total_yield_shares += 1
  └─ NFT metadata includes cargo_pub, parent_merkle, station, epoch

Compose(ingot_records[N], target_tier)
  ├─ Burns N input IngotRecords + their Core NFTs
  ├─ Computes new cargo_pub = hash_product(recipe_id, sorted_input_pubs, idx)
  ├─ Mints composed Core NFT
  ├─ Creates new IngotRecord with:
  │   multiplier = sum(input_multipliers)
  │   parent_merkle = merkle_root(sorted_input_cargo_pubs)
  │   tier = max(input_tiers) + 1
  └─ total_yield_shares unchanged (multiplier reflects N ingots)

Harvest()
  ├─ Claims accrued fees from CLMM position
  ├─ Burns token-side fees
  ├─ Deposits quote-side fees into YieldVault.harvested_quote
  └─ Updates total_token_burned, last_harvest_slot

ClaimYield(ingot_record)
  ├─ Computes share = harvested_quote * multiplier / total_yield_shares
  ├─ Transfers quote tokens to holder
  ├─ harvested_quote -= share
  ├─ Updates claimed_quote
  └─ Each ingot claims once per harvest cycle (tracked by last_claim_slot)
```

## Signal provenance in NFT metadata

Every ingot NFT carries the full provenance chain from Signal:

```json
{
  "name": "Ferrite Ingot — Prospect Refinery Epoch 1847392",
  "symbol": "RATi-INGOT",
  "attributes": [
    { "trait_type": "Material", "value": "Ferrite" },
    { "trait_type": "Grade", "value": "Standard" },
    { "trait_type": "Tier", "value": "Ingot" },
    { "trait_type": "Multiplier", "value": "1" },
    { "trait_type": "Station", "value": "Prospect Refinery" },
    { "trait_type": "Epoch", "value": "1847392" }
  ],
  "properties": {
    "provenance": {
      "cargo_pub": "3f8a...",
      "parent_merkle": "a2c1...",
      "fragment_pub": "a2c1...",
      "rock_pub": "7d1e...",
      "station_pubkey": "ProspectPubkey...",
      "chain_log_event": "CHAIN_EVT_SMELT",
      "chain_log_tx": "solana_tx_sig_from_480"
    }
  }
}
```

For a composed frame, `parent_merkle` is the merkle root of the input ingot `cargo_pub` values, and `source_ingots` lists each contributing ingot. Any auditor can walk the full DAG from frame → ingots → fragments → rocks.

## Trebuchet integration

Trebuchet's existing launch flow maps cleanly:

| Trebuchet step | With yield-split |
|----------------|------------------|
| Step 1 — Generate wallet | Unchanged |
| Step 2 — Configure token & pools | Add: "Enable Signal yield-split?" toggle. If on: vault config (quote token for yield, harvest authority, composition tiers enabled). Visualize shows ingot → frame → module tree alongside tokenomics donut. |
| Step 3 — Fund wallet | Unchanged |
| Step 4 — Create token | Unchanged |
| Step 5 — Create pools | After Burn & Earn lock: deploy YieldVault. Initialize with pool_id, position_nft. |
| Step 6 — Sweep | Vault authority transfers to destination wallet alongside Fee Keys. |

**Post-launch panel:** A "Yield" tab appears after launch. It shows:
- Total ingots minted / total yield shares
- Accumulated quote tokens available to claim
- Per-user: your ingots, their multipliers, claimable yield
- Composition UI: select ingots, preview frame/Module NFT, compose
- Harvest button (if self-crank)
- Full provenance tree for each ingot

## The NFT pack connection

Ruby High's card packs and this yield-split system share the same C library because they're the same operations:

| Operation | Ruby High | Yield-split |
|-----------|-----------|-------------|
| Mint NFT | Pack mint → 5 face-down cards | Ingest Chain-log proof → 1 ingot NFT |
| Reveal | Deterministic reveal from seed + commitment | Ingot identity is deterministic from cargo_pub (no reveal needed — it's already known) |
| Metadata | Card attributes (character, rarity, subject) | Ingot attributes (material, grade, station, epoch) |
| Compose | Not in Ruby High | Burn N ingot NFTs → 1 frame NFT |
| Burn | Card burn → Hall Pass credits | Composition consumes inputs |
| Provenance | Pack commitment + reveal seed | Fragment_pub + parent_merkle + chain log |

The C library provides: SHA-256, base58, deterministic identity, metadata JSON generation, and transaction building. Ruby High calls it for cards. The yield-split system calls it for ingots. Same library, different catalogs.

## What this means

This design closes the loop between all three projects:

- **Signal** produces the labor and the provenance that proves it happened. The game's economy — mining, smelting, hauling, composing — generates the objects.
- **RATi** validates the identity. The registry verifies station pubkeys. The burn-to-mint program converts game labor to canonical tokens.
- **Trebuchet** deploys the liquidity that makes yield possible. The CLMM pool earns fees. Burn & Earn locks the LP permanently. The yield-split contract divides the fee stream among the ingot holders.

The ingot NFT is the object that threads through all three. It was born in Signal's furnace. It was validated by RATi's registry. It earns yield from Trebuchet's liquidity. Its provenance is provable at every step.

And because Signal's composition system already exists — frames are made from ingots, modules are made from frames — the yield-split system can support higher-tier NFTs with exactly the same `parent_merkle` pattern. No new abstraction. Just the cargo unit, on-chain, earning fees.
