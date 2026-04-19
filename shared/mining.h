/*
 * mining.h -- RATi keypair mining primitives (Phase 1).
 *
 * Shared between client + server so both sides derive keypairs
 * identically from fracture state. Anchors entropy in game events
 * (asteroid + ship position at fracture time) instead of local PRNGs,
 * which makes every find replayable / verifiable.
 *
 * Crypto posture for Phase 1
 * --------------------------
 * The "keypair" in this file is a pair of 32-byte SHA-256 outputs,
 * NOT a real ed25519 keypair. You cannot sign arbitrary messages with
 * this pseudokey — only the server can verify a submitted keypair by
 * re-deriving it. That's enough for Phase 1's in-game economy; V1.5
 * swaps in TweetNaCl ed25519 (same seed, same APIs, same wire — just
 * real crypto). When that lands, every Phase-1-mined keypair is still
 * valid: the PRF derivation carries over.
 */
#ifndef SHARED_MINING_H
#define SHARED_MINING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sha256.h"
#include "base58.h"

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define MINING_PUBKEY_BYTES        32
#define MINING_PRIVKEY_BYTES       32
#define MINING_FRACTURE_SEED_BYTES 32
#define MINING_BASE58_CAP          64   /* >= ceil(32 * 138/100) + terminator */

/* Burst size per fragment, fixed regardless of ore tonnage. Small
 * bursts keep "common" the modal outcome instead of best-of-N
 * inflating every fragment to fine/rare. With prefix-anchored
 * patterns this gives roughly: common 70%, fine 29%, rare 0.65%,
 * RATi 0.14%, commissioned <0.01%. Bigger asteroids still pay more
 * via the multiplier × ore_tons; the *quality* roll is fragment-flat. */
#define MINING_BURST_PER_FRAGMENT  20

/* ------------------------------------------------------------------ */
/* Grade ladder                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    MINING_GRADE_COMMON       = 0,  /* no pattern */
    MINING_GRADE_FINE         = 1,  /* any 2-char repeat somewhere */
    MINING_GRADE_RARE         = 2,  /* 3-in-a-row identical chars */
    MINING_GRADE_RATI         = 3,  /* "RATi" anywhere in the base58 */
    MINING_GRADE_COMMISSIONED = 4,  /* matches an open station commission */
    MINING_GRADE_COUNT        = 5
} mining_grade_t;

/* Payout multiplier applied at the refinery on top of the ore's base
 * price. Anchored at 1.0× for common so the baseline economy is
 * unchanged; higher grades are bonuses over the same ore delivery.
 * RATi IS the ore — a "strike" is a better-graded load of ore, not
 * a separate tradable asset. */
static inline float mining_payout_multiplier(mining_grade_t g) {
    switch (g) {
    case MINING_GRADE_COMMON:       return 1.0f;
    case MINING_GRADE_FINE:         return 1.2f;
    case MINING_GRADE_RARE:         return 2.0f;
    case MINING_GRADE_RATI:         return 10.0f;
    case MINING_GRADE_COMMISSIONED: return 100.0f;
    default: return 1.0f;
    }
}

/* Short human label for HUD / channel copy. */
static inline const char *mining_grade_label(mining_grade_t g) {
    switch (g) {
    case MINING_GRADE_COMMON:       return "common";
    case MINING_GRADE_FINE:         return "fine";
    case MINING_GRADE_RARE:         return "rare";
    case MINING_GRADE_RATI:         return "RATi";
    case MINING_GRADE_COMMISSIONED: return "commissioned";
    default: return "?";
    }
}

/* ------------------------------------------------------------------ */
/* Grade classification                                                */
/* ------------------------------------------------------------------ */

/* Classify a pubkey's base58 callsign. Pure inspection — anchored at
 * position 0 so each grade is geometrically harder than the last and
 * "best of N" can't inflate the ladder. With burst=20 per fragment
 * this gives a clean ~70/29/0.65/0.14/<0.01 split.
 *
 *   commissioned = exact "RATi" prefix      (the brand lottery)
 *   RATi         = 'R' + 'A' in first 4     (looser, reachable)
 *   rare         = first 3 chars identical  (anchored triple)
 *   fine         = first 2 chars identical  (anchored pair) */
static inline mining_grade_t mining_classify_base58(const char *s) {
    if (!s || !s[0]) return MINING_GRADE_COMMON;
    size_t n = strlen(s);

    /* Commissioned: exact RATi brand prefix. ~1 in 11M per candidate;
     * effectively a session-long lottery even with bursts. */
    if (n >= 4 && s[0] == 'R' && s[1] == 'A' && s[2] == 'T' && s[3] == 'i')
        return MINING_GRADE_COMMISSIONED;

    /* RATi: starts with 'R' and has 'A' in first 4 positions. Looser
     * than exact-match so it actually surfaces in a session. */
    if (n >= 4 && s[0] == 'R') {
        for (size_t i = 1; i < 4; i++) {
            if (s[i] == 'A') return MINING_GRADE_RATI;
        }
    }

    /* Rare: first three chars identical. */
    if (n >= 3 && s[0] == s[1] && s[1] == s[2])
        return MINING_GRADE_RARE;

    /* Fine: first two chars identical. */
    if (n >= 2 && s[0] == s[1])
        return MINING_GRADE_FINE;

    return MINING_GRADE_COMMON;
}

/* ------------------------------------------------------------------ */
/* Fracture seed — shared entropy for all observers                    */
/* ------------------------------------------------------------------ */

/* Inputs used at both client-mine time and server-verify time. All
 * quantized to integers so float rounding can't cause mismatch. */
typedef struct {
    uint16_t asteroid_id;
    int32_t  asteroid_pos_x_q;   /* round(pos * 100) */
    int32_t  asteroid_pos_y_q;
    int32_t  asteroid_rotation_q;/* round(rot * 1000) */
    int32_t  ship_pos_x_q;
    int32_t  ship_pos_y_q;
    int32_t  ship_angle_q;       /* round(angle * 1000) */
    int32_t  outward_dir_q;      /* round(angle * 1000) */
    uint64_t world_time_ms;      /* round(time * 1000) */
    uint8_t  fractured_by;
} mining_fracture_inputs_t;

/* Quantize the float state of a fracture into the inputs struct.
 * Defined as a macro of helper functions to keep the interface tight. */
static inline int32_t mining_q100_(float v) {
    /* round-half-away-from-zero to avoid bankers' rounding drift. */
    return (v >= 0.0f) ? (int32_t)(v * 100.0f + 0.5f)
                       : (int32_t)(v * 100.0f - 0.5f);
}
static inline int32_t mining_q1000_(float v) {
    return (v >= 0.0f) ? (int32_t)(v * 1000.0f + 0.5f)
                       : (int32_t)(v * 1000.0f - 0.5f);
}

/* SHA-256 over a canonical little-endian layout. Both client and
 * server must hit this same byte sequence. */
static inline void mining_fracture_seed_compute(const mining_fracture_inputs_t *in,
                                                uint8_t out[MINING_FRACTURE_SEED_BYTES]) {
    uint8_t buf[64];
    size_t o = 0;
    #define MINE_W8(v)  do { buf[o++] = (uint8_t)((v) & 0xFF); } while (0)
    #define MINE_W16(v) do { MINE_W8(v); MINE_W8((v) >> 8); } while (0)
    #define MINE_W32(v) do { MINE_W8(v); MINE_W8((v) >> 8); MINE_W8((v) >> 16); MINE_W8((v) >> 24); } while (0)
    #define MINE_W64(v) do { MINE_W32((v) & 0xFFFFFFFFu); MINE_W32((v) >> 32); } while (0)
    MINE_W16((uint16_t)in->asteroid_id);
    MINE_W32((uint32_t)in->asteroid_pos_x_q);
    MINE_W32((uint32_t)in->asteroid_pos_y_q);
    MINE_W32((uint32_t)in->asteroid_rotation_q);
    MINE_W32((uint32_t)in->ship_pos_x_q);
    MINE_W32((uint32_t)in->ship_pos_y_q);
    MINE_W32((uint32_t)in->ship_angle_q);
    MINE_W32((uint32_t)in->outward_dir_q);
    MINE_W64(in->world_time_ms);
    MINE_W8(in->fractured_by);
    #undef MINE_W8
    #undef MINE_W16
    #undef MINE_W32
    #undef MINE_W64
    sha256_bytes(buf, o, out);
}

/* ------------------------------------------------------------------ */
/* Pseudokey derivation — Phase 1                                      */
/* ------------------------------------------------------------------ */

/* V1 pseudokey: priv = sha256("P1" || combined), pub = sha256("p1" || combined).
 * V1.5 will replace this with ed25519_keygen_from_seed(combined) — same
 * signature, same call sites, same interop. */
typedef struct {
    uint8_t priv[MINING_PRIVKEY_BYTES];
    uint8_t pub[MINING_PUBKEY_BYTES];
} mining_keypair_t;

static inline void mining_keypair_derive(const uint8_t fracture_seed[MINING_FRACTURE_SEED_BYTES],
                                         const uint8_t player_pubkey[MINING_PUBKEY_BYTES],
                                         uint32_t burst_nonce,
                                         mining_keypair_t *out) {
    /* combined = sha256(fracture_seed || player_pubkey || burst_nonce_le) */
    uint8_t combined_in[MINING_FRACTURE_SEED_BYTES + MINING_PUBKEY_BYTES + 4];
    memcpy(combined_in, fracture_seed, MINING_FRACTURE_SEED_BYTES);
    memcpy(&combined_in[MINING_FRACTURE_SEED_BYTES], player_pubkey, MINING_PUBKEY_BYTES);
    combined_in[MINING_FRACTURE_SEED_BYTES + MINING_PUBKEY_BYTES    ] = (uint8_t)(burst_nonce);
    combined_in[MINING_FRACTURE_SEED_BYTES + MINING_PUBKEY_BYTES + 1] = (uint8_t)(burst_nonce >>  8);
    combined_in[MINING_FRACTURE_SEED_BYTES + MINING_PUBKEY_BYTES + 2] = (uint8_t)(burst_nonce >> 16);
    combined_in[MINING_FRACTURE_SEED_BYTES + MINING_PUBKEY_BYTES + 3] = (uint8_t)(burst_nonce >> 24);
    uint8_t combined[32];
    sha256_bytes(combined_in, sizeof(combined_in), combined);

    /* Domain-separated PRF for pub / priv — prevents the two halves
     * being the same value. V1.5 will drop both and call ed25519. */
    uint8_t priv_in[34]; priv_in[0]='P'; priv_in[1]='1'; memcpy(&priv_in[2], combined, 32);
    uint8_t pub_in [34]; pub_in [0]='p'; pub_in [1]='1'; memcpy(&pub_in [2], combined, 32);
    sha256_bytes(priv_in, sizeof(priv_in), out->priv);
    sha256_bytes(pub_in,  sizeof(pub_in),  out->pub);
}

/* Convenience: derive a standalone player keypair from a random seed
 * (used once at first launch, not from fracture state). */
static inline void mining_keypair_from_random_seed(const uint8_t seed[32],
                                                    mining_keypair_t *out) {
    uint8_t priv_in[34]; priv_in[0]='P'; priv_in[1]='1'; memcpy(&priv_in[2], seed, 32);
    uint8_t pub_in [34]; pub_in [0]='p'; pub_in [1]='1'; memcpy(&pub_in [2], seed, 32);
    sha256_bytes(priv_in, sizeof(priv_in), out->priv);
    sha256_bytes(pub_in,  sizeof(pub_in),  out->pub);
}

/* Derive the 7-char callsign from a pubkey. First 7 chars of base58. */
static inline void mining_callsign_from_pubkey(const uint8_t pub[MINING_PUBKEY_BYTES],
                                                char out[8]) {
    char buf[MINING_BASE58_CAP];
    size_t n = base58_encode(pub, MINING_PUBKEY_BYTES, buf, sizeof(buf));
    size_t take = (n > 7) ? 7 : n;
    memcpy(out, buf, take);
    out[take] = '\0';
    /* Pad with underscores if base58 underflowed (shouldn't with 32B). */
    while (take < 7) { out[take++] = '_'; }
    out[7] = '\0';
}

#endif /* SHARED_MINING_H */
