/*
 * station_authority.h -- Per-station Ed25519 identity primitives.
 *
 * Layer B of the off-chain decentralization roadmap (#479). Each
 * station has its own operator-secret-derived keypair so events authored
 * within its signal range can be signed by *the station*, not by the
 * server-as-a-whole — the cornerstone of per-zone federation.
 *
 * Seed derivation:
 *   - Seeded stations (indices 0/1/2): seed =
 *       SHA256("signal-station-v1" || operator_secret || world_seed_u32
 *              || station_index_u32)
 *   - Player-planted outposts (indices 3+): seed =
 *       SHA256("signal-outpost-v1" || operator_secret || founder_pub[32]
 *              || station_name[16] || planted_tick_u64)
 *
 * The private key is rederivable on demand from operator-held secret
 * material plus the public world/provenance data. It is deliberately
 * omitted from the wire format and from every save — losing the disk
 * does not leak any station's signing key.
 *
 * In this layer, no sim events are signed yet (that's Layer C). This
 * file only establishes the identity infrastructure: derive, store,
 * sign, verify.
 */
#ifndef SERVER_STATION_AUTHORITY_H
#define SERVER_STATION_AUTHORITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the operator-held secret mixed into every station key
 * derivation. The input may be any non-empty operator secret; this
 * module hashes it down to a fixed 32-byte root. Returns false for
 * NULL/empty input and leaves the previous secret unchanged. */
bool station_authority_configure_secret(const char *secret);

/* Reset to the deterministic development secret used by local tests and
 * non-production local runs. Production servers should configure a real
 * secret before world_reset/world_load. */
void station_authority_use_dev_secret(void);

/* Derive the seeded-station seed for index 0/1/2 from the operator
 * secret, world seed, and station index.
 * Output is the 32-byte Ed25519 seed; feed it to
 * signal_crypto_keypair_from_seed to get the full keypair. */
void station_authority_seeded_seed(uint32_t world_seed,
                                   uint32_t station_index,
                                   uint8_t out_seed[32]);

/* Derive the outpost seed from (operator_secret || founder_pubkey ||
 * station_name || planted_tick). station_name is read up to its NUL
 * terminator and truncated/zero-padded to 16 bytes for hashing —
 * fixed-length input keeps the seed reproducible regardless of name
 * length quirks. */
void station_authority_outpost_seed(const uint8_t founder_pub[32],
                                    const char *station_name,
                                    uint64_t planted_tick,
                                    uint8_t out_seed[32]);

/* Populate s->station_pubkey + s->station_secret for a seeded station.
 * Idempotent: same operator secret + world_seed + index always produces
 * the same key. */
void station_authority_init_seeded(station_t *s,
                                   uint32_t world_seed,
                                   uint32_t station_index);

/* Populate s->station_pubkey + s->station_secret for a player-planted
 * outpost. Also stamps s->outpost_founder_pubkey + s->outpost_planted_tick
 * so the keypair can be rederived on save/load. */
void station_authority_init_outpost(station_t *s,
                                    const uint8_t founder_pub[32],
                                    uint64_t planted_tick);

/* Re-populate s->station_secret from operator secret + already-set
 * identity material. Called by the world loader so the secret never
 * has to be written to disk. Returns true when a non-zero saved pubkey
 * did not match the currently configured operator secret and was
 * replaced with the derived pubkey; callers should treat that as an
 * intentional station rekey and start a fresh chain head. */
bool station_authority_rederive_secret(station_t *s,
                                       uint32_t world_seed,
                                       int station_index);

/* Sign msg[0..len) with station s's private key. Writes 64 bytes to
 * sig. The station must have a populated secret — pass through the
 * init/rederive helpers above before calling this. */
void station_sign(const station_t *s, const uint8_t *msg, size_t len,
                  uint8_t sig[64]);

/* Verify sig was produced by station s over msg[0..len). Returns true
 * iff the signature is valid. */
bool station_verify(const station_t *s, const uint8_t *msg, size_t len,
                    const uint8_t sig[64]);

/* Pretty-print the leading bytes of station s's pubkey as a base58
 * prefix into out (e.g. "Ax7q9aBc..."). out must be at least 16 bytes;
 * the result is always NUL-terminated. */
void station_pubkey_b58_prefix(const station_t *s, char out[16]);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_STATION_AUTHORITY_H */
