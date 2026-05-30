# Pipeline Architecture: Contract Blocks, Identity Stamps, Chain Release

**Status:** architecture decision — v3

## The flow

```
SIGNAL                    RATi                      TREBUCHET
───────────────────────────────────────────────────────────────

Mine asteroid             
  → smelt ingot           
  → chain log:            
    CHAIN_EVT_SMELT       
    signed by station     

packnft CLI:              
  chain log proof         
  → signed Solana tx      
  → JSON contract block   
       │                  
       │  {               
       │    "contract": { 
       │      "type": "mint-ingot",
       │      "cargo_pub": "3f8a...",
       │      "parent_merkle": "a2c1...",
       │      "station_pubkey": "...",
       │      "smelt_epoch": 1847392,
       │      "chain_log_proof": "..."
       │    },
       │    "transaction": "base64...",
       │    "signatures": [...]
       │  }
       │                  
       └──────────────────►  Validate & stamp:
                              ├─ cargo_pub in registry?
                              ├─ program ID matches manifest?
                              ├─ destination mint canonical?
                              └─ STAMP: source registered,
                                 rate mode fixed-ratio,
                                 identity confirmed
                                   │
                                   │  {
                                   │    ...original contract,
                                   │    "stamp": {
                                   │      "registry_version": "v1",
                                   │      "canonical_mint": "G1NJ...RATi",
                                   │      "program_id": "2q5x...RATi",
                                   │      "source_config": "ruby-pump-current",
                                   │      "rate_mode": "fixed-ratio",
                                   │      "stamped_at": "...",
                                   │      "registry_hash": "..."
                                   │    }
                                   │  }
                                   │
                                   └──────────────►  Release to chain:
                                                       ├─ Submit tx to Solana RPC
                                                       ├─ Confirm
                                                       ├─ Generate launch report
                                                       └─ [future] Bridge to EVM,
                                                          submit on other chains

```

## The three roles

### Signal — produces signed contract blocks

Signal's job: take proven game labor and produce a signed Solana transaction that mints the corresponding on-chain asset. Output is a JSON contract block.

A contract block is self-contained. It includes:
- The operation type (mint-ingot, compose-frame, mint-pack-card, burn-to-mint-migrate)
- The provenance data (cargo_pub, parent_merkle, station_pubkey, smelt_epoch, chain log proof)
- The signed Solana transaction (base64)
- The transaction signatures

Anyone can verify the block: check the chain log proof against the station pubkey, verify the cargo_pub is deterministic from the fragment, confirm the transaction's instructions match the operation type. The block does not need RATi or Trebuchet to be meaningful — it's a provable claim about game labor.

### RATi — stamps contract blocks with identity

RATi's job: take a contract block and confirm it operates on canonical assets. Output is the same block with a stamp attached.

A stamp says:
- This cargo_pub is registered as a valid source
- This program ID matches the canonical manifest
- This destination mint is the real RATi (or Kyro, or Ruby)
- The rate mode is correct (fixed-ratio for v0→v1 migration, bonding-curve for new issuance)
- The registry version and hash at time of stamping

The stamp does NOT say "this transaction is valid" — that's Signal's job. The stamp says "this transaction is about the real token." RATi is the oracle of what is real, not the validator of what is correct.

A stamped block can be released by anyone — Trebuchet, a CLI, a bot, a different tool entirely. The stamp is portable. The block + stamp is the release artifact.

### Trebuchet — releases stamped blocks to chains

Trebuchet's job: take a stamped contract block and put it on-chain. Output is a launch report with the confirmed transaction signature.

Trebuchet handles:
- RPC submission (Solana mainnet, devnet, or custom endpoint)
- Confirmation polling with retry
- Fee estimation and priority fee management
- Multi-chain release (future: same stamped block, different chain adapter)
- Launch report generation (addresses, signatures, lock state, provenance)

Trebuchet does not validate the transaction (Signal did that) and does not verify the identity (RATi did that). It releases. It is the final mechanical step — submit, confirm, record.

## What this means for each project

### Signal owns

- Game simulation and chain log
- packnft CLI: chain log proof → signed Solana transaction
- burn-to-mint program SBF
- yield-split program SBF
- All C code, all crypto, all types

Signal produces: `.sbf` binaries, `packnft` CLI binary, JSON contract blocks on stdout.

### RATi owns

- Token registry (canonical mints, program IDs, authority state)
- Address manifest (PDA seeds, nonces, bumps)
- Source-mint registry (enabled sources, rate modes, verification status)
- Scam-token warnings
- Deployment runbooks (key ceremony, devnet RC, mainnet deploy)
- Validation scripts (`npm run check:registry`, `npm run check:launch-evidence`)
- The stamp service (validates a contract block against the registry, attaches identity stamp)

RATi produces: stamped contract blocks. Input is a Signal contract block. Output is the same block with an identity stamp.

### Trebuchet owns

- Launch wizard UI (steps 1-6)
- Pool deployment (CLMM creation, Burn & Earn locking)
- RPC orchestration (submit, confirm, retry)
- Yield tab (harvest, claim, compose UI)
- NFT pack panel (mint, reveal, burn UI)
- Launch report generation
- Multi-chain release adapter (future)

Trebuchet produces: confirmed on-chain transactions and launch reports. Input is a stamped contract block from RATi. Output is a confirmed transaction signature and a launch report.

## API between the three

### Signal → RATi: Contract block (JSON on stdout)

```json
{
  "contract": {
    "version": "signal-contract-v1",
    "type": "mint-ingot",
    "cargo_pub": "3f8a2c1...",
    "parent_merkle": "a2c1d3e...",
    "commodity": 0,
    "grade": 1,
    "station_pubkey": "ProspectPubkey...",
    "smelt_epoch": 1847392,
    "chain_log_proof": {
      "event_type": "CHAIN_EVT_SMELT",
      "station_pubkey": "ProspectPubkey...",
      "event_signature": "ed25519_sig...",
      "prev_hash": "sha256...",
      "payload_hash": "sha256...",
      "fragment_pub": "a2c1d3e...",
      "segment_id": 0,
      "event_id": 42
    }
  },
  "transaction": {
    "message": "base64_encoded_solana_message...",
    "signatures": ["base64_sig_1", "base64_sig_2"],
    "recent_blockhash": "..."
  },
  "signed_transaction_base64": "base64_full_signed_tx..."
}
```

### RATi → Trebuchet: Stamped contract block

```json
{
  "contract": { "...original contract block..." },
  "transaction": { "...original transaction..." },
  "stamp": {
    "version": "rati-stamp-v1",
    "registry_version": "v1.0",
    "registry_hash": "sha256_of_registry_json...",
    "canonical_destination_mint": "G1NJuxZQihpk6Bc9XLxjFpeiuwMiAPoRKjcBmqL1RATi",
    "program_id": "2q5xELTGky988Lz1oLZLpBoQv7DzB7bBxoUdQGRmRATi",
    "source_config": {
      "source_mint": "ABHQGzXNoRbJ1sjUsCJ2TmTAo1uMx4EUpV1qYiSVpump",
      "enabled": true,
      "rate_mode": "fixed-ratio",
      "source_token_id": "ruby-pump-current",
      "destination_token_id": "ruby"
    },
    "authority_state": {
      "mint_authority": "pda",
      "freeze_authority": null,
      "upgrade_authority": "multisig",
      "paused": false,
      "finalized": false
    },
    "warnings": [],
    "stamped_at": "2026-05-29T22:00:00Z",
    "stamp_signature": "ed25519_stamp_sig..."
  }
}
```

### Trebuchet → Solana: RPC submission

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "sendTransaction",
  "params": [
    "<signed_transaction_base64>",
    { "encoding": "base64", "maxRetries": 3 }
  ]
}
```

## Contract block types

Signal's packnft CLI supports these contract block types:

| Contract type | What it mints/burns | Trigger |
|---------------|---------------------|---------|
| `mint-ingot` | Mints yield-split ingot NFT | Fragment smelted in Signal, #480 proof submitted |
| `compose-frame` | Burns N ingot NFTs, mints 1 frame NFT | Player composes ingots |
| `compose-module` | Burns N frame NFTs, mints 1 module NFT | Player composes frames |
| `harvest-yield` | Claims CLMM fees, burns token side, deposits quote | Crank or self-harvest |
| `claim-yield` | Transfers 1/N of harvested quote to NFT holder | Holder claims |
| `mint-pack` | Mints unopened pack NFT | Player purchases pack |
| `reveal-pack` | Reveals pack contents, mints face-down card slots | Player opens pack |
| `mint-card` | Mints individual card NFT | Deterministic reveal per slot |
| `burn-card` | Burns card NFT | Player burns card for Hall Passes |
| `migrate-tokens` | Burns source tokens, mints canonical destination tokens | Player migrates v0 → v1 |

## Multi-chain future

The stamped contract block does not contain chain-specific submission details — that's Trebuchet's job. The same block can be released on multiple chains:

```
Stamped contract block
       │
       ├──► Trebuchet Solana adapter ──► Solana RPC ──► confirmed
       │
       ├──► Trebuchet EVM adapter ────► ETH RPC ─────► confirmed
       │
       └──► Trebuchet SVM adapter ────► Eclipse RPC ─► confirmed
```

The stamp says "this is canonical RATi." The contract says "mint this ingot NFT." Trebuchet says "here's how to submit that on Solana-formatted RPC, or EVM-formatted RPC, or SVM-formatted RPC." The stamp and contract are chain-agnostic. The release adapter is chain-specific.

## Validation surface

At each boundary, the receiving side validates:

### RATi validates Signal's contract block:
- `cargo_pub` is correctly derived from inputs (recompute hash)
- Chain log proof: signature valid against station pubkey, prev_hash linkage intact
- Transaction instructions match the declared contract type
- Transaction signatures valid

### Trebuchet validates RATi's stamp:
- Registry hash matches known registry version
- Stamp signature valid
- Destination mint is in the registry
- Source config is enabled
- No active warnings for this mint/program

### Solana validates Trebuchet's submission:
- Transaction signatures valid
- Account ownership correct
- PDA derivations correct
- Compute budget sufficient
- No conflicting state

## What goes away

| File | Lines | Replaced by |
|------|-------|-------------|
| Kyro `solana-program/kyro_token/` | ~884 Rust | Signal `programs/burn-to-mint/` (C SBF) |
| RATi `programs/` | C SBF track + spec | Signal `programs/burn-to-mint/` |
| Ruby High `core-pack-nfts.ts` | 1,463 | packnft CLI `mint-pack`, `reveal-pack` |
| Ruby High `hall-pass-nfts.ts` | 1,694 | packnft CLI `mint-card`, `burn-card` |
| Ruby High `hall-pass-card-catalog.ts` | 508 | packnft `catalog.c` |
| Ruby High `hall-pass-reveal-provenance.ts` | 161 | packnft `reveal.c` |
| Ruby High `nft-metadata-storage.ts` | 351 | packnft `metadata.c` |
| Ruby High `nft-arweave-assets.ts` | 60 | packnft `metadata.c` |
| Trebuchet `packnft/` (WIP) | — | Signal `tools/packnft/` (all C lives in Signal) |

## Directory layout after migration

```
signal/
├── shared/               types, crypto, sha256, base58, manifest
├── server/               game sim, chain log, station authority
├── client/               game client
├── programs/
│   ├── burn-to-mint/     onchain-c/ + onchain-rs/ + PROGRAM_SPEC.md
│   └── yield-split/      onchain-c/ + PROGRAM_SPEC.md
├── tools/
│   ├── signal_verify     existing
│   ├── signal_chain_assets existing
│   └── packnft/          CLI: packnft.c, catalog.c, reveal.c, metadata.c, txn.c, cli.c, test.c
└── CMakeLists.txt

rati/
├── registry/             rati-token-registry.v1.json
├── addresses/            rati-address-manifest.v1.json
├── deployment/           runbooks, key ceremony, launch evidence
├── tokens/               metadata drafts
├── stamp/                [NEW] stamp service — validates contract blocks, attaches identity stamps
└── package.json          npm run check, npm run stamp

trebuchet/
├── server.js             Express API + RPC orchestration
├── public/               Launch wizard, yield tab, NFT panel
├── docs/                 thesis.md, yield-split-design.md, packnft-architecture.md
└── package.json
```
