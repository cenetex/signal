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

/* Canonical grade palette — single source of truth for every UI surface
 * that shows a grade (world rock dot, tow tether, sell popup, hint-bar
 * batch, station market/contracts, manifest). Progression reads rarer
 * → warmer: white → cyan → violet → gold → bright gold. Kept with the
 * grade definition + label + multiplier so UI code doesn't need to pull
 * in world-rendering headers just to tint grade text. */
static inline void mining_grade_rgb(mining_grade_t grade,
                                    uint8_t *r, uint8_t *g, uint8_t *b) {
    switch (grade) {
    case MINING_GRADE_FINE:         *r = 140; *g = 220; *b = 255; break; /* light cyan */
    case MINING_GRADE_RARE:         *r = 190; *g = 130; *b = 255; break; /* violet */
    case MINING_GRADE_RATI:         *r = 255; *g = 200; *b =  90; break; /* warm gold */
    case MINING_GRADE_COMMISSIONED: *r = 255; *g = 240; *b = 130; break; /* bright gold */
    case MINING_GRADE_COMMON:
    default:                        *r = 200; *g = 220; *b = 230; break; /* cool white */
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

/* Canonical fragment identity once a fracture claim resolves:
 *   sha256("SIGNALv1" || "FRAG" || fracture_seed || winner_player_pub || nonce_le)
 *
 * Terminology note: `winner_player_pub` is a 32-byte opaque identifier
 * (sha256 of the session_token), NOT a secp256k1 / ed25519 public key.
 * It cannot be used for signature verification — it just deterministically
 * names the session that won the claim race. Real keypair support
 * (signed transfers, signed claims) would layer on top; this field is
 * the proof-of-race, not a proof-of-key. */
static inline void mining_fragment_pub_compute(
    const uint8_t fracture_seed[MINING_FRACTURE_SEED_BYTES],
    const uint8_t winner_player_pub[MINING_PUBKEY_BYTES],
    uint32_t burst_nonce,
    uint8_t out_pub[MINING_PUBKEY_BYTES]) {
    uint8_t buf[8 + 4 + MINING_FRACTURE_SEED_BYTES + MINING_PUBKEY_BYTES + 4];
    size_t o = 0;
    static const uint8_t domain[8] = { 'S','I','G','N','A','L','v','1' };
    memcpy(&buf[o], domain, sizeof(domain));
    o += sizeof(domain);
    buf[o++] = 'F';
    buf[o++] = 'R';
    buf[o++] = 'A';
    buf[o++] = 'G';
    memcpy(&buf[o], fracture_seed, MINING_FRACTURE_SEED_BYTES);
    o += MINING_FRACTURE_SEED_BYTES;
    memcpy(&buf[o], winner_player_pub, MINING_PUBKEY_BYTES);
    o += MINING_PUBKEY_BYTES;
    buf[o++] = (uint8_t)(burst_nonce);
    buf[o++] = (uint8_t)(burst_nonce >> 8);
    buf[o++] = (uint8_t)(burst_nonce >> 16);
    buf[o++] = (uint8_t)(burst_nonce >> 24);
    sha256_bytes(buf, o, out_pub);
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

/* Class prefix indices — must mirror ingot_prefix_t in shared/types.h.
 * Kept as plain integers here so this header has no types.h dependency.
 * Single source of truth lives in types.h; v2 callers should compare
 * against the typed enum names. */
enum {
    MINING_CLASS_ANONYMOUS    = 0,
    MINING_CLASS_M            = 1,
    MINING_CLASS_H            = 2,
    MINING_CLASS_T            = 3,
    MINING_CLASS_S            = 4,
    MINING_CLASS_F            = 5,
    MINING_CLASS_K            = 6,
    MINING_CLASS_RATI         = 7,
    MINING_CLASS_COMMISSIONED = 8,
};

/* Inspect the leading character(s) of base58(pubkey) to determine
 * which hull class an ingot can mint. Returns one of MINING_CLASS_*. */
static inline int mining_pubkey_class(const uint8_t pub[MINING_PUBKEY_BYTES]) {
    char b58[MINING_BASE58_CAP];
    size_t n = base58_encode(pub, MINING_PUBKEY_BYTES, b58, sizeof(b58));
    if (n < 4) return MINING_CLASS_ANONYMOUS;
    if (b58[0]=='R' && b58[1]=='A' && b58[2]=='T' && b58[3]=='i')
        return MINING_CLASS_RATI;
    switch (b58[0]) {
    case 'M': return MINING_CLASS_M;
    case 'H': return MINING_CLASS_H;
    case 'T': return MINING_CLASS_T;
    case 'S': return MINING_CLASS_S;
    case 'F': return MINING_CLASS_F;
    case 'K': return MINING_CLASS_K;
    /* R/A/T/i alone are reserved (RATi disambiguation) — anonymous. */
    default:  return MINING_CLASS_ANONYMOUS;
    }
}

/* Render a pubkey as its display callsign, with the class-prefix dash
 * inserted at the boundary:
 *   M-class:    "M-ABCDEF"   (8 chars + null = 9)
 *   RATi-class: "RATi-XYZ"   (8 chars + null = 9)
 *   anonymous:  "ABCDEFG"    (7 chars + null = 8)
 * Caller buffer must be at least 12 bytes. */
static inline void mining_render_callsign(const uint8_t pub[MINING_PUBKEY_BYTES],
                                          char out[12]) {
    char b58[MINING_BASE58_CAP];
    size_t n = base58_encode(pub, MINING_PUBKEY_BYTES, b58, sizeof(b58));
    if (n < 7) {
        /* Should never happen with a 32B pubkey, but be defensive. */
        size_t i;
        for (i = 0; i < n && i < 11; i++) out[i] = b58[i];
        out[i] = '\0';
        return;
    }
    int cls = mining_pubkey_class(pub);
    if (cls == MINING_CLASS_RATI) {
        /* Skip the 4 RATi chars, render 3 of the body chars. */
        out[0]='R'; out[1]='A'; out[2]='T'; out[3]='i'; out[4]='-';
        out[5]=b58[4]; out[6]=b58[5]; out[7]=b58[6];
        out[8]='\0';
    } else if (cls != MINING_CLASS_ANONYMOUS) {
        out[0] = b58[0];
        out[1] = '-';
        out[2] = b58[1]; out[3] = b58[2]; out[4] = b58[3];
        out[5] = b58[4]; out[6] = b58[5]; out[7] = b58[6];
        out[8] = '\0';
    } else {
        memcpy(out, b58, 7);
        out[7] = '\0';
    }
}

/* Render a pubkey as an alphanumeric callsign in `XXX-XXX` style for
 * player display. Deterministic; same pubkey always produces the same
 * callsign across machines / restarts.
 *
 * Format: 6 chars from the alphanumeric alphabet (no I, O, l, 0 to keep
 * it visually unambiguous — 32 chars total), with a dash inserted at a
 * deterministic position 1..5. Output is always exactly 7 chars + null.
 *
 * Class-prefix is intentionally NOT preserved here: that signal already
 * lives in mining_render_callsign() (RATi-/M-/H-/etc. for cargo lineage).
 * Player-display callsigns are pure alphanumeric noise — easy to type,
 * easy to read, no character-set surprises. Collisions in 32^6 ≈ 1.07B
 * are vanishingly rare for a single-server universe.
 *
 * Caller buffer must be at least 8 bytes. */
static inline void mining_alphanumeric_callsign(const uint8_t pub[MINING_PUBKEY_BYTES],
                                                char out[8]) {
    /* 32-char alphabet — A-Z minus I/O, plus digits 2-9 (no 0/1).
     * Power of two so byte→index is just a mask. Declared without
     * room for the trailing null so the array stays exactly 32. */
    static const char ALNUM[32] = {
        'A','B','C','D','E','F','G','H','J','K','L','M','N','P','Q','R',
        'S','T','U','V','W','X','Y','Z','2','3','4','5','6','7','8','9'
    };
    char body[6];
    for (int i = 0; i < 6; i++)
        body[i] = ALNUM[pub[i] & 0x1F];
    /* Dash position 1..5, derived from a different pub byte so it
     * doesn't correlate with the first body char. */
    int dash = 1 + (pub[6] % 5);
    int ci = 0;
    for (int i = 0; i < 7; i++) {
        if (i == dash) out[i] = '-';
        else           out[i] = body[ci++];
    }
    out[7] = '\0';
}

#endif /* SHARED_MINING_H */
