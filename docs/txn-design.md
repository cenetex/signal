# txn.c — Solana Transaction Builder

## Architecture

Five layers. Each layer depends only on the layer below. Each layer has its
own test suite with golden vectors.

```
Layer 5: CLI integration      cli.c (existing — gains txn operations)
Layer 4: Core instructions     core_ix.h / core_ix.c
Layer 3: Transaction            txn.h / txn.c
Layer 2: Message                msg.h / msg.c
Layer 1: Primitives             compact.h / base64.h
                                 (base58 already in shared/base58.h)
```

## Layer 1: Primitives

### Compact-u16

Solana uses a variable-length encoding for integers. 1-3 bytes.

```
Value range        Bytes  Encoding
0x00 – 0x7F        1      [value]
0x80 – 0x3FFF       2      [value & 0x7F | 0x80] [value >> 7]
0x4000 – 0x3FFFFF    3      [value & 0x7F | 0x80] [(value >> 7) & 0x7F | 0x80] [value >> 14]
```

```c
// compact.h
int compact_u16_size(uint16_t value);
int compact_u16_encode(uint16_t value, uint8_t out[3]);
int compact_u16_decode(const uint8_t *data, size_t data_len, uint16_t *value_out);
```

**Tests:** Known values from Solana mainnet transactions. Edge cases: 0, 0x7F, 0x80, 0x3FFF, 0x4000, 0xFFFF.

### Base64

Solana uses standard Base64 for serialized transactions (RPC `sendTransaction`).

```c
// base64.h
int base64_encode(const uint8_t *data, size_t len, char *out, size_t out_cap);
int base64_decode(const char *in, uint8_t *out, size_t out_cap);
```

**Tests:** Round-trip known transaction bytes. Known base64 strings from Solana RPC responses.

## Layer 2: Message

### Solana message layout

```
Offset  Size  Field
0       1     num_required_signatures
1       1     num_readonly_signed_accounts
2       1     num_readonly_unsigned_accounts
3       N*32  account_addresses (compact-u16 count, then addresses)
?       32    recent_blockhash
?       ...   instructions (compact-u16 count, then each instruction)
```

Each instruction:
```
Offset  Size  Field
0       1     program_id_index (index into account_addresses)
1       ...   account_indexes (compact-u16 count, then u8 indexes)
?       ...   data (compact-u16 length, then bytes)
```

### API

```c
// msg.h

#define SOLANA_PUBKEY_SIZE 32
#define SOLANA_BLOCKHASH_SIZE 32
#define SOLANA_MAX_ACCOUNTS 64
#define SOLANA_MAX_IX_DATA 1024
#define SOLANA_MAX_IX_ACCOUNTS 16
#define SOLANA_MAX_MESSAGE_SIZE 2048

typedef struct {
    uint8_t  program_id_index;
    uint8_t  account_indexes[SOLANA_MAX_IX_ACCOUNTS];
    uint8_t  account_count;
    uint8_t  data[SOLANA_MAX_IX_DATA];
    uint16_t data_len;
} solana_instruction_t;

typedef struct {
    uint8_t  num_required_signatures;
    uint8_t  num_readonly_signed;
    uint8_t  num_readonly_unsigned;
    uint8_t  account_addresses[SOLANA_MAX_ACCOUNTS][SOLANA_PUBKEY_SIZE];
    uint8_t  account_count;
    uint8_t  recent_blockhash[SOLANA_BLOCKHASH_SIZE];
    solana_instruction_t instructions[16];
    uint8_t  instruction_count;
    bool     built;  // true after accounts sorted, header computed
} solana_message_t;

void solana_message_init(solana_message_t *msg,
                         const uint8_t blockhash[SOLANA_BLOCKHASH_SIZE]);

int  solana_message_add_account(solana_message_t *msg,
                                const uint8_t pubkey[SOLANA_PUBKEY_SIZE],
                                bool is_signer, bool is_writable);

int  solana_message_add_instruction(solana_message_t *msg,
                                    const uint8_t program_id[SOLANA_PUBKEY_SIZE],
                                    const uint8_t *account_pubkeys[],  // array of pubkeys
                                    const bool     account_is_signer[],
                                    const bool     account_is_writable[],
                                    uint8_t        account_count,
                                    const uint8_t *ix_data,
                                    uint16_t       ix_data_len);

void solana_message_build(solana_message_t *msg);
// Sorts accounts (signers first, then writable, then readonly),
// computes header bytes, builds instruction account indexes.

int  solana_message_serialize(const solana_message_t *msg,
                              uint8_t *out, size_t out_cap);
// Returns number of bytes written, or -1 on overflow.
```

### Account ordering (after build)

Solana requires accounts in this order:
1. All signer accounts (fee payer first)
2. Writable non-signer accounts
3. Readonly non-signer accounts

Within each group, accounts appear in insertion order. The message header
records how many signers, how many readonly-signed, and how many
readonly-unsigned.

`solana_message_build()` sorts the internal account list and rebuilds
instruction account indexes to match.

### Test strategy

**Golden vector:** A known mainnet transaction. Decode it, verify every field.
Re-encode it, verify the bytes match exactly.

**Round-trip:** Build a message programmatically, serialize, deserialize,
verify all fields match.

**Account ordering:** Build messages with accounts in various orders, verify
the sorted output matches the Solana spec.

## Layer 3: Transaction

### Message signing

A Solana transaction is a list of signatures followed by the serialized message.

```
Offset  Size  Field
0       1     signature count (compact-u16)
1       N*64  signatures (64-byte Ed25519)
?       ...   serialized message
```

```c
// txn.h

#define SOLANA_SIGNATURE_SIZE 64
#define SOLANA_MAX_SIGNERS 16

typedef struct {
    uint8_t  signatures[SOLANA_MAX_SIGNERS][SOLANA_SIGNATURE_SIZE];
    uint8_t  signer_count;
    solana_message_t message;
} solana_transaction_t;

// Sign the message with one or more Ed25519 keypairs.
// Each keypair is 64 bytes: [secret (32)] [public (32)].
// The fee payer must be the first signer.
int solana_transaction_sign(solana_transaction_t *txn,
                            const uint8_t signer_keypairs[][64],
                            uint8_t signer_count);

// Serialize a signed transaction to bytes.
int solana_transaction_serialize(const solana_transaction_t *txn,
                                 uint8_t *out, size_t out_cap);

// Base64-encode a signed transaction (for RPC submission).
int solana_transaction_to_base64(const solana_transaction_t *txn,
                                 char *out, size_t out_cap);

// Deserialize a base64 transaction.
int solana_transaction_from_base64(const char *base64,
                                   solana_transaction_t *txn);
```

### Signing algorithm

```
message_bytes = serialize(message)
for each signer:
    signature = ed25519_sign(signer_secret, message_bytes)
    signatures[i] = signature
```

Signal's `signal_crypto.h` provides `signal_crypto_sign()`. The interface:

```c
// shared/signal_crypto.h
void signal_crypto_sign(uint8_t sig[64],
                        const uint8_t secret[32],
                        const uint8_t msg[], size_t msg_len);
```

### Test strategy

**Golden vector:** A known signed transaction from mainnet. Deserialize, verify
signatures against the message and known public keys. Re-serialize, verify
bytes match.

**Round-trip:** Sign a transaction with a known keypair, serialize, deserialize,
verify signatures validate.

**Signer order:** Verify the fee payer's signature comes first.

## Layer 4: Core Instructions

Metaplex Core uses Anchor-style instruction discriminators (8-byte SHA-256
hash of the global instruction name).

The discriminator for each instruction:

```
CreateV1          = sha256("global:create_v1")[..8]
CreateCollectionV1 = sha256("global:create_collection_v1")[..8]
BurnV1             = sha256("global:burn_v1")[..8]
UpdateV1           = sha256("global:update_v1")[..8]
```

We compute these once and hardcode them:

```c
// Core instruction discriminators (hardcoded from SHA-256 of names)
static const uint8_t CORE_CREATE_V1_DISC[8] = {
    0x??, 0x??, 0x??, 0x??, 0x??, 0x??, 0x??, 0x??
};
```

### CreateAsset instruction

```
Offset  Size  Field
0       8     discriminator
8       1     asset address present (bool)
9       32    asset address (if present, else zeroed)
41      1     name present (bool)
42      4     name length (u32 LE) — if present
46      ?     name bytes — if present
        ...   uri (same pattern: bool + u32 LE + bytes)
        ...   collection present (bool)
        ...   collection address (32 bytes) — if present
        ...   plugins present (bool, set to 0 for now)
```

For our use case, we always provide name and URI. Collection is optional
(for pack/card NFTs in a collection; for collection NFTs themselves, no
collection field). We don't use plugins yet.

### CreateCollection instruction

Same as CreateAsset but without a collection field and with a different
discriminator.

### Burn instruction

```
Offset  Size  Field
0       8     discriminator
8       1     compressed (bool, 0 for standard NFTs)
```

Simple — just the discriminator + a 0 byte.

### Update instruction

```
Offset  Size  Field
0       8     discriminator
8       1     new name present (bool)
9       4     new name length (u32 LE) — if present
13      ?     new name bytes — if present
        ...   new uri (same pattern: bool + u32 LE + bytes)
```

### API

```c
// core_ix.h
#include "msg.h"

// Core program ID on Solana
#define CORE_PROGRAM_ID_BYTES \
    {0xCo,0xRE,0xEN,0x...,0x...} // CoREENxT6tW1HoK8ypY1SxRMZTcVPm7R94rH4PZNhX7d

// Build a CreateAsset instruction
int core_create_asset_ix(
    const char *name,
    const char *uri,
    const uint8_t owner[32],       // destination wallet
    const uint8_t asset_signer[32], // ephemeral keypair for the asset PDA
    const uint8_t collection[32],   // set to all zeros if no collection
    bool has_collection,
    solana_instruction_t *ix_out
);

// Build a CreateCollection instruction
int core_create_collection_ix(
    const char *name,
    const char *uri,
    const uint8_t collection_signer[32],
    solana_instruction_t *ix_out
);

// Build a BurnAsset instruction
int core_burn_asset_ix(solana_instruction_t *ix_out);

// Build an UpdateAsset instruction (e.g., mark pack as opened)
int core_update_asset_ix(
    const char *new_name,  // NULL to keep existing
    const char *new_uri,   // NULL to keep existing
    solana_instruction_t *ix_out
);
```

### Account layout for Core instructions

**CreateAsset accounts:**
```
0. [writable, signer] asset_signer    — ephemeral keypair
1. [writable] owner                   — destination wallet (gets the NFT)
2. [ ] collection                     — collection address (optional, not signer, not writable)
3. [ ] system_program                 — 11111111111111111111111111111111
4. [ ] core_program                   — CoREENxT6tW1HoK8ypY1SxRMZTcVPm7R94rH4PZNhX7d
```

**CreateCollection accounts:**
```
0. [writable, signer] collection_signer
1. [writable, signer] fee_payer
2. [ ] system_program
3. [ ] core_program
```

**BurnAsset accounts:**
```
0. [writable] asset_address
1. [writable] collection_address
2. [signer] owner
3. [ ] core_program
```

**UpdateAsset accounts:**
```
0. [writable] asset_address
1. [ ] collection_address (optional)
2. [signer] authority
3. [ ] core_program
```

### Test strategy for Core instructions

**Golden vectors from SDK.** Use `@metaplex-foundation/mpl-core` in a Node
script to generate known CreateAsset/CreateCollection/Burn/Update transactions.
Capture the raw instruction data bytes. Verify our C builders produce the same
bytes.

**Create a Node script** that outputs golden vectors:

```javascript
// scripts/generate-core-vectors.mjs
import { generateSigner, create, createCollection } from '@metaplex-foundation/mpl-core';
import { createUmi } from '@metaplex-foundation/umi-bundle-defaults';

const umi = createUmi('http://localhost:8899');
const asset = generateSigner(umi);
const collection = generateSigner(umi);
const owner = generateSigner(umi);

const ix = create(umi, {
  asset,
  collection: collection.publicKey,
  owner: owner.publicKey,
  name: 'Test NFT',
  uri: 'https://example.com/metadata.json',
});

console.log('CreateAsset discriminant:', toHex(ix.data.slice(0, 8)));
console.log('Full ix data hex:', toHex(ix.data));
```

The golden vectors are committed to `tools/packnft/test_vectors/`. The C test
suite reads them and verifies our builders produce identical output.

## Layer 5: CLI integration

Add transaction-building operations to the existing packnft CLI:

```
build-pack-mint      → core_create_asset_ix + solana_transaction_sign → base64
build-card-mint      → core_create_asset_ix + solana_transaction_sign → base64
build-card-burn      → core_burn_asset_ix + solana_transaction_sign → base64
build-collection-create → core_create_collection_ix + solana_transaction_sign → base64
build-furnace-convert → SPL Token burn + burn-to-mint CPI → base64
build-furnace-boost   → SPL Token burn + memo → base64
```

Each operation:
1. Parses JSON input (authority secret, owner address, collection address, etc.)
2. Decodes base58 authority secret → 64-byte keypair
3. Builds the appropriate Core instruction
4. Builds a Solana message with the required accounts
5. Fetches a recent blockhash (from the caller — Trebuchet or Signal server provides this)
6. Signs with the authority keypair (and any other required signers)
7. Returns base64 signed transaction

The CLI does NOT submit to an RPC. It produces a signed transaction that the
caller submits.

## Test strategy summary

| Layer | Test type | How |
|-------|-----------|-----|
| compact-u16 | Unit + edge cases | 0, 0x7F, 0x80, 0x3FFF, 0x4000, 0xFFFF |
| base64 | Round-trip | Known bytes ↔ base64 |
| Message | Golden vector | Known mainnet tx, decode + re-encode |
| Message | Round-trip | Build, serialize, deserialize, compare |
| Message | Account order | Verify signer/writable/readonly ordering |
| Transaction | Golden vector | Known signed tx, deserialize, re-serialize |
| Transaction | Sign + verify | Sign with known keypair, verify sigs |
| Core ix | SDK golden vectors | Compare our bytes vs @metaplex-foundation/mpl-core |
| Core ix | Round-trip | Build ix, decode, verify fields |
| CLI | Integration | Full flow: JSON in → base64 tx out, submit to devnet |

## Implementation order

1. `compact.h/compact.c` + tests — ~60 lines
2. `base64.h/base64.c` + tests — ~60 lines
3. `msg.h/msg.c` + tests — ~200 lines (hardest: account ordering)
4. `txn.h/txn.c` + tests — ~120 lines
5. Generate golden vectors from Node SDK — script
6. `core_ix.h/core_ix.c` + tests — ~180 lines
7. CLI integration — ~150 lines
8. Devnet smoke test — submit a real Core NFT mint

Total: ~770 lines of C + tests. Each layer independently testable.

## Open questions

**Core instruction discriminators.** We need to compute the exact SHA-256
discriminators. I'll write a small C program that computes them at build time,
or hardcode them after computing once.

**Account metas for Core.** The exact set of accounts and their signer/writable
flags for each Core instruction needs to be verified against the on-chain
program. The account layouts above are my best understanding but must be
cross-checked.

**Compute budget.** Real transactions need a compute budget instruction
(SetComputeUnitLimit + SetComputeUnitPrice). We should add helpers for these
but make them optional — the caller can add them if needed.

**Recent blockhash.** We don't fetch it. The caller provides it. This keeps
txn.c network-free.
