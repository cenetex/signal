/*
 * station_authority.h -- Per-station Ed25519 identity primitives.
 *
 * Layer B of the off-chain decentralization roadmap (#479). Each
 * station has its own deterministic keypair so events authored within
 * its signal range can be signed by *the station*, not by the server-
 * as-a-whole — the cornerstone of per-zone federation.
 *
 * Seed derivation:
 *   - Seeded stations (indices 0/1/2): seed =
 *       SHA256("signal-station-v1" || world_seed_u32 || station_index_u32)
 *     so all servers running with the same world seed agree on every
 *     seeded station's pubkey.
 *   - Player-planted outposts (indices 3+): seed =
 *       SHA256("signal-outpost-v1" || founder_pub[32]
 *              || station_name[16] || planted_tick_u64)
 *     reproducible by any auditor with the world state + founding
 *     event. The server runs the station, so it holds the private key.
 *
 * The private key is rederivable on demand from the world seed (and
 * for outposts, the saved founder + name + tick). It is therefore
 * deliberately omitted from the wire format and from every save —
 * losing the disk does not leak any station's signing key.
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

/* Derive the seeded-station seed for index 0/1/2 from the world seed.
 * Output is the 32-byte Ed25519 seed; feed it to
 * signal_crypto_keypair_from_seed to get the full keypair. */
void station_authority_seeded_seed(uint32_t world_seed,
                                   uint32_t station_index,
                                   uint8_t out_seed[32]);

/* Derive the outpost seed from (founder_pubkey || station_name ||
 * planted_tick). station_name is read up to its NUL terminator and
 * truncated/zero-padded to 16 bytes for hashing — fixed-length input
 * keeps the seed reproducible regardless of name length quirks. */
void station_authority_outpost_seed(const uint8_t founder_pub[32],
                                    const char *station_name,
                                    uint64_t planted_tick,
                                    uint8_t out_seed[32]);

/* Populate s->station_pubkey + s->station_secret for a seeded station.
 * Idempotent: same world_seed + index always produces the same key. */
void station_authority_init_seeded(station_t *s,
                                   uint32_t world_seed,
                                   uint32_t station_index);

/* Populate s->station_pubkey + s->station_secret for a player-planted
 * outpost. Also stamps s->outpost_founder_pubkey + s->outpost_planted_tick
 * so the keypair can be rederived on save/load. */
void station_authority_init_outpost(station_t *s,
                                    const uint8_t founder_pub[32],
                                    uint64_t planted_tick);

/* Re-populate s->station_secret from already-set identity material
 * (s->station_pubkey + outpost provenance, or world seed for seeded
 * stations). Called by the world loader so the secret never has to
 * be written to disk. */
void station_authority_rederive_secret(station_t *s,
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
