# RATi Burn-To-Mint PDA Seeds

Status: draft v0.1
Date: 2026-05-21
Scope: canonical v1 PDA derivation plan.

All seed bytes are exact ASCII unless a field is explicitly described as a
public key or little-endian integer. Changing any seed changes public addresses,
so this file must be frozen before final program ID selection.

## Seed Table

| Account | Seeds |
| --- | --- |
| Config | `b"rati"`, `b"burn-to-mint"`, `b"config"`, `b"v1"` |
| Mint account | `b"rati"`, `b"mint"`, `b"v1"`, `token_id_ascii`, `vanity_nonce_u64_le` |
| Destination token config | `b"rati"`, `b"destination"`, `destination_mint_pubkey` |
| Source mint config | `b"rati"`, `b"source"`, `source_mint_pubkey`, `destination_mint_pubkey` |
| Mint authority | `b"rati"`, `b"mint-authority"`, `destination_mint_pubkey` |
| Receipt | `b"rati"`, `b"receipt"`, `user_pubkey`, `source_mint_pubkey`, `destination_mint_pubkey`, `user_nonce_u64_le` |

## Stability Rules

- The final program ID must be the same on devnet release-candidate and
  mainnet.
- The same final program ID plus the same seed bytes derives the same PDA
  public keys on both clusters.
- Token IDs are seed material for vanity mint PDAs.
- Token IDs are not seed material for authority PDAs; mint public keys are.
- User-provided strings are not seed material.
- Versioned seeds are allowed only at account-family boundaries.

## Vanity Mint PDA

Each destination token mint is created at a PDA with a public vanity nonce:

```text
find_program_address(
  [
    b"rati",
    b"mint",
    b"v1",
    token_id_ascii,
    vanity_nonce_u64_le
  ],
  burn_to_mint_program_id
)
```

The vanity nonce and bump are public manifest fields. The mint PDA has no private
key.

## Mint Authority PDA

Each destination mint has its own mint authority PDA:

```text
find_program_address(
  [
    b"rati",
    b"mint-authority",
    destination_mint_pubkey
  ],
  burn_to_mint_program_id
)
```

This keeps RATi, Kyro, and Ruby authority addresses isolated even though they
share one program.

## Receipt Nonce

Receipts use a user-supplied `u64` nonce encoded little-endian. The program must
reject duplicate receipt accounts. Receipts are used for auditability and user
support, not as a holder snapshot mechanism.

## Freeze Point

Before selecting the final burn-to-mint program ID:

- all seed families in this file must be final;
- account sizes must be final or safely versioned;
- the derivation script must match this table;
- devnet disposable tests must pass.
