/*
 * raticross.c — encode what raticross.h documents. Nothing else.
 *
 * The encoder writes fields at published offsets. It never memcpy's a
 * host rc_header onto the wire. Signatures cover those encoded bytes,
 * never the in-memory struct.
 */

#include "raticross.h"

#include <string.h>

#include "tweetnacl.h"

/* ---- tiny helpers ----------------------------------------------------- */

static void wr_u8(uint8_t *p, uint8_t v) { p[0] = v; }

static void wr_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void wr_u64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v & 0xffu);
        v >>= 8;
    }
}

static uint16_t rd_u16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint64_t rd_u64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

int rc_is_zero(const uint8_t *p, size_t n) {
    uint8_t acc = 0;
    if (p == NULL) {
        return 1;
    }
    for (size_t i = 0; i < n; i++) {
        acc |= p[i];
    }
    return acc == 0;
}

static int is_actor_kind(uint8_t k) {
    return k == RC_KIND_AVATAR || k == RC_KIND_AGENT || k == RC_KIND_SYSTEM;
}

static int pubkey_nonzero(const uint8_t p[RC_PUBKEY_SIZE]) {
    return !rc_is_zero(p, RC_PUBKEY_SIZE);
}

void rc_header_clear(rc_header *h) {
    if (h != NULL) {
        memset(h, 0, sizeof(*h));
        h->version = RC_VERSION_V1;
    }
}

const char *rc_status_str(rc_status s) {
    switch (s) {
    case RC_OK:                return "ok";
    case RC_ERR_NULL:          return "null";
    case RC_ERR_VERSION:       return "version";
    case RC_ERR_KIND:          return "kind";
    case RC_ERR_RESERVED:      return "reserved";
    case RC_ERR_FLAG:          return "flag";
    case RC_ERR_PUBKEY:        return "pubkey";
    case RC_ERR_LEASE:         return "lease";
    case RC_ERR_SESSION:       return "session";
    case RC_ERR_IMPERSONATION: return "impersonation";
    case RC_ERR_RANGE:         return "range";
    case RC_ERR_HASH:          return "hash";
    case RC_ERR_SHORT:         return "short";
    case RC_ERR_TRAILING:      return "trailing";
    case RC_ERR_SIGN:          return "sign";
    case RC_ERR_VERIFY:        return "verify";
    default:                   return "unknown";
    }
}

/* ---- Contract B on the header ---------------------------------------- */

rc_status rc_header_validate(const rc_header *h) {
    if (h == NULL) {
        return RC_ERR_NULL;
    }
    if (h->version != RC_VERSION_V1) {
        return RC_ERR_VERSION;
    }
    if (!is_actor_kind(h->as_kind) || !is_actor_kind(h->to_kind)) {
        return RC_ERR_KIND;
    }
    if (!pubkey_nonzero(h->as_pubkey) || !pubkey_nonzero(h->to_pubkey)) {
        return RC_ERR_PUBKEY;
    }
    if (h->flags & ~(uint8_t)RC_FLAG_HAS_LEASE) {
        return RC_ERR_FLAG;
    }

    if (h->from_kind == RC_KIND_SESSION) {
        if ((h->flags & RC_FLAG_HAS_LEASE) == 0) {
            return RC_ERR_LEASE;
        }
        if (rc_is_zero(h->lease_id, RC_ID_SIZE)) {
            return RC_ERR_LEASE;
        }
        if (!rc_is_zero(h->from_pubkey, RC_PUBKEY_SIZE)) {
            return RC_ERR_SESSION;
        }
        return RC_OK;
    }

    if (!is_actor_kind(h->from_kind)) {
        return RC_ERR_KIND;
    }
    if (h->from_kind != h->as_kind) {
        return RC_ERR_IMPERSONATION;
    }
    if (memcmp(h->from_pubkey, h->as_pubkey, RC_PUBKEY_SIZE) != 0) {
        return RC_ERR_IMPERSONATION;
    }
    if (!pubkey_nonzero(h->from_pubkey)) {
        return RC_ERR_PUBKEY;
    }
    if (h->flags & RC_FLAG_HAS_LEASE) {
        return RC_ERR_LEASE;
    }
    if (!rc_is_zero(h->lease_id, RC_ID_SIZE)) {
        return RC_ERR_LEASE;
    }
    return RC_OK;
}

/* ---- encode / decode: field by field --------------------------------- */

rc_status rc_header_encode(const rc_header *h, uint8_t out[RC_HEADER_V1_SIZE]) {
    rc_status st;

    if (h == NULL || out == NULL) {
        return RC_ERR_NULL;
    }
    st = rc_header_validate(h);
    if (st != RC_OK) {
        return st;
    }

    memset(out, 0, RC_HEADER_V1_SIZE);
    wr_u8(out + RC_OFF_VERSION, h->version);
    wr_u8(out + RC_OFF_FROM_KIND, h->from_kind);
    wr_u8(out + RC_OFF_AS_KIND, h->as_kind);
    wr_u8(out + RC_OFF_TO_KIND, h->to_kind);
    wr_u8(out + RC_OFF_FLAGS, h->flags);
    memcpy(out + RC_OFF_AS_PUBKEY, h->as_pubkey, RC_PUBKEY_SIZE);
    memcpy(out + RC_OFF_TO_PUBKEY, h->to_pubkey, RC_PUBKEY_SIZE);
    memcpy(out + RC_OFF_FROM_PUBKEY, h->from_pubkey, RC_PUBKEY_SIZE);
    memcpy(out + RC_OFF_ENVELOPE_ID, h->envelope_id, RC_ID_SIZE);
    wr_u64le(out + RC_OFF_TS_MS, h->ts_ms);
    memcpy(out + RC_OFF_LEASE_ID, h->lease_id, RC_ID_SIZE);
    memcpy(out + RC_OFF_PAYLOAD_SHA256, h->payload_sha256, RC_HASH_SIZE);
    wr_u16le(out + RC_OFF_PAYLOAD_LEN, h->payload_len);
    return RC_OK;
}

rc_status rc_header_decode(const uint8_t in[RC_HEADER_V1_SIZE], rc_header *h) {
    size_t i;

    if (in == NULL || h == NULL) {
        return RC_ERR_NULL;
    }
    if (in[RC_OFF_VERSION] != RC_VERSION_V1) {
        return RC_ERR_VERSION;
    }
    for (i = 0; i < RC_RESERVED0_SIZE; i++) {
        if (in[RC_OFF_RESERVED0 + i] != 0) {
            return RC_ERR_RESERVED;
        }
    }
    for (i = 0; i < RC_RESERVED1_SIZE; i++) {
        if (in[RC_OFF_RESERVED1 + i] != 0) {
            return RC_ERR_RESERVED;
        }
    }

    rc_header_clear(h);
    h->version = in[RC_OFF_VERSION];
    h->from_kind = in[RC_OFF_FROM_KIND];
    h->as_kind = in[RC_OFF_AS_KIND];
    h->to_kind = in[RC_OFF_TO_KIND];
    h->flags = in[RC_OFF_FLAGS];
    memcpy(h->as_pubkey, in + RC_OFF_AS_PUBKEY, RC_PUBKEY_SIZE);
    memcpy(h->to_pubkey, in + RC_OFF_TO_PUBKEY, RC_PUBKEY_SIZE);
    memcpy(h->from_pubkey, in + RC_OFF_FROM_PUBKEY, RC_PUBKEY_SIZE);
    memcpy(h->envelope_id, in + RC_OFF_ENVELOPE_ID, RC_ID_SIZE);
    h->ts_ms = rd_u64le(in + RC_OFF_TS_MS);
    memcpy(h->lease_id, in + RC_OFF_LEASE_ID, RC_ID_SIZE);
    memcpy(h->payload_sha256, in + RC_OFF_PAYLOAD_SHA256, RC_HASH_SIZE);
    h->payload_len = rd_u16le(in + RC_OFF_PAYLOAD_LEN);
    return rc_header_validate(h);
}

/* ---- SHA-256 (FIPS 180-4). Public domain style, no tables beyond K. -- */

static uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
        0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
        0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
        0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, hh;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    hh = state[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += hh;
}

void rc_sha256(const uint8_t *data, size_t len, uint8_t out[RC_HASH_SIZE]) {
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    uint8_t block[64];
    uint64_t bitlen;
    size_t n = 0;
    size_t i;

    if (out == NULL) {
        return;
    }
    if (data == NULL && len != 0) {
        memset(out, 0, RC_HASH_SIZE);
        return;
    }

    while (len - n >= 64) {
        sha256_compress(state, data + n);
        n += 64;
    }

    memset(block, 0, sizeof(block));
    if (len - n) {
        memcpy(block, data + n, len - n);
    }
    block[len - n] = 0x80;

    bitlen = (uint64_t)len * 8u;
    if (len - n >= 56) {
        sha256_compress(state, block);
        memset(block, 0, sizeof(block));
    }
    for (i = 0; i < 8; i++) {
        block[63 - i] = (uint8_t)(bitlen >> (8 * i));
    }
    sha256_compress(state, block);

    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)state[i];
    }
}

rc_status rc_bind_payload(rc_header *h, const uint8_t *payload, size_t len) {
    if (h == NULL) {
        return RC_ERR_NULL;
    }
    if (len > RC_MAX_PAYLOAD) {
        return RC_ERR_RANGE;
    }
    if (len > 0 && payload == NULL) {
        return RC_ERR_NULL;
    }
    h->payload_len = (uint16_t)len;
    rc_sha256(payload, len, h->payload_sha256);
    return RC_OK;
}

/* ---- framed message -------------------------------------------------- */

size_t rc_message_size(uint16_t payload_len) {
    return (size_t)RC_HEADER_V1_SIZE + (size_t)payload_len + (size_t)RC_SIG_SIZE;
}

rc_status rc_message_encode(
    const rc_header *h,
    const uint8_t *payload,
    const uint8_t sig[RC_SIG_SIZE],
    uint8_t *out,
    size_t out_cap,
    size_t *out_len) {
    rc_status st;
    size_t need;

    if (h == NULL || out == NULL || sig == NULL || out_len == NULL) {
        return RC_ERR_NULL;
    }
    if (h->payload_len > 0 && payload == NULL) {
        return RC_ERR_NULL;
    }
    need = rc_message_size(h->payload_len);
    if (out_cap < need) {
        return RC_ERR_SHORT;
    }
    st = rc_header_encode(h, out);
    if (st != RC_OK) {
        return st;
    }
    if (h->payload_len > 0) {
        uint8_t digest[RC_HASH_SIZE];
        rc_sha256(payload, h->payload_len, digest);
        if (memcmp(digest, h->payload_sha256, RC_HASH_SIZE) != 0) {
            return RC_ERR_HASH;
        }
        memcpy(out + RC_HEADER_V1_SIZE, payload, h->payload_len);
    }
    memcpy(out + RC_HEADER_V1_SIZE + h->payload_len, sig, RC_SIG_SIZE);
    *out_len = need;
    return RC_OK;
}

rc_status rc_message_decode(
    const uint8_t *in,
    size_t in_len,
    rc_header *h,
    const uint8_t **payload,
    uint16_t *payload_len,
    const uint8_t **sig) {
    rc_status st;
    uint8_t digest[RC_HASH_SIZE];
    size_t need;

    if (in == NULL || h == NULL || payload == NULL || payload_len == NULL || sig == NULL) {
        return RC_ERR_NULL;
    }
    if (in_len < RC_HEADER_V1_SIZE + RC_SIG_SIZE) {
        return RC_ERR_SHORT;
    }
    st = rc_header_decode(in, h);
    if (st != RC_OK) {
        return st;
    }
    need = rc_message_size(h->payload_len);
    if (in_len < need) {
        return RC_ERR_SHORT;
    }
    if (in_len > need) {
        return RC_ERR_TRAILING;
    }
    *payload = (h->payload_len == 0) ? NULL : (in + RC_HEADER_V1_SIZE);
    *payload_len = h->payload_len;
    *sig = in + RC_HEADER_V1_SIZE + h->payload_len;
    if (h->payload_len > 0) {
        rc_sha256(*payload, h->payload_len, digest);
        if (memcmp(digest, h->payload_sha256, RC_HASH_SIZE) != 0) {
            return RC_ERR_HASH;
        }
    }
    return RC_OK;
}

/* ---- Ed25519 over the encoded header --------------------------------- */

rc_status rc_keypair(uint8_t pk[RC_PUBKEY_SIZE], uint8_t sk[RC_SECRET_SIZE]) {
    if (pk == NULL || sk == NULL) {
        return RC_ERR_NULL;
    }
    if (crypto_sign_keypair(pk, sk) != 0) {
        return RC_ERR_SIGN;
    }
    return RC_OK;
}

rc_status rc_keypair_from_seed(
    const uint8_t seed[32],
    uint8_t pk[RC_PUBKEY_SIZE],
    uint8_t sk[RC_SECRET_SIZE]) {
    if (seed == NULL || pk == NULL || sk == NULL) {
        return RC_ERR_NULL;
    }
    if (crypto_sign_keypair_from_seed(pk, sk, seed) != 0) {
        return RC_ERR_SIGN;
    }
    return RC_OK;
}

static int actor_kind_ok(rc_kind k) {
    return k == RC_KIND_AVATAR || k == RC_KIND_AGENT || k == RC_KIND_SYSTEM;
}

rc_status rc_header_actor(
    rc_header *h,
    rc_kind kind,
    const uint8_t as_pk[RC_PUBKEY_SIZE],
    rc_kind to_kind,
    const uint8_t to_pk[RC_PUBKEY_SIZE],
    const uint8_t envelope_id[RC_ID_SIZE],
    uint64_t ts_ms) {
    if (h == NULL || as_pk == NULL || to_pk == NULL || envelope_id == NULL) {
        return RC_ERR_NULL;
    }
    if (!actor_kind_ok(kind) || !actor_kind_ok(to_kind)) {
        return RC_ERR_KIND;
    }
    rc_header_clear(h);
    h->from_kind = (uint8_t)kind;
    h->as_kind = (uint8_t)kind;
    h->to_kind = (uint8_t)to_kind;
    memcpy(h->as_pubkey, as_pk, RC_PUBKEY_SIZE);
    memcpy(h->from_pubkey, as_pk, RC_PUBKEY_SIZE);
    memcpy(h->to_pubkey, to_pk, RC_PUBKEY_SIZE);
    memcpy(h->envelope_id, envelope_id, RC_ID_SIZE);
    h->ts_ms = ts_ms;
    return rc_header_validate(h);
}

rc_status rc_header_session(
    rc_header *h,
    rc_kind as_kind,
    const uint8_t as_pk[RC_PUBKEY_SIZE],
    rc_kind to_kind,
    const uint8_t to_pk[RC_PUBKEY_SIZE],
    const uint8_t envelope_id[RC_ID_SIZE],
    const uint8_t lease_id[RC_ID_SIZE],
    uint64_t ts_ms) {
    if (h == NULL || as_pk == NULL || to_pk == NULL || envelope_id == NULL || lease_id == NULL) {
        return RC_ERR_NULL;
    }
    if (!actor_kind_ok(as_kind) || !actor_kind_ok(to_kind)) {
        return RC_ERR_KIND;
    }
    rc_header_clear(h);
    h->from_kind = RC_KIND_SESSION;
    h->as_kind = (uint8_t)as_kind;
    h->to_kind = (uint8_t)to_kind;
    h->flags = RC_FLAG_HAS_LEASE;
    memcpy(h->as_pubkey, as_pk, RC_PUBKEY_SIZE);
    memset(h->from_pubkey, 0, RC_PUBKEY_SIZE);
    memcpy(h->to_pubkey, to_pk, RC_PUBKEY_SIZE);
    memcpy(h->envelope_id, envelope_id, RC_ID_SIZE);
    memcpy(h->lease_id, lease_id, RC_ID_SIZE);
    h->ts_ms = ts_ms;
    return rc_header_validate(h);
}

static rc_status secret_matches_as(const rc_header *h, const uint8_t sk[RC_SECRET_SIZE]) {
    if (memcmp(sk + RC_PUBKEY_SIZE, h->as_pubkey, RC_PUBKEY_SIZE) != 0) {
        return RC_ERR_PUBKEY;
    }
    return RC_OK;
}

rc_status rc_sign_header(
    const rc_header *h,
    const uint8_t sk[RC_SECRET_SIZE],
    uint8_t sig[RC_SIG_SIZE]) {
    uint8_t wire[RC_HEADER_V1_SIZE];
    uint8_t sm[RC_HEADER_V1_SIZE + RC_SIG_SIZE];
    unsigned long long smlen = 0;
    rc_status st;

    if (h == NULL || sk == NULL || sig == NULL) {
        return RC_ERR_NULL;
    }
    st = secret_matches_as(h, sk);
    if (st != RC_OK) {
        return st;
    }
    st = rc_header_encode(h, wire);
    if (st != RC_OK) {
        return st;
    }
    if (crypto_sign(sm, &smlen, wire, RC_HEADER_V1_SIZE, sk) != 0) {
        return RC_ERR_SIGN;
    }
    if (smlen != (unsigned long long)(RC_HEADER_V1_SIZE + RC_SIG_SIZE)) {
        return RC_ERR_SIGN;
    }
    memcpy(sig, sm, RC_SIG_SIZE);
    return RC_OK;
}

rc_status rc_verify_header(
    const uint8_t wire[RC_HEADER_V1_SIZE],
    const uint8_t pk[RC_PUBKEY_SIZE],
    const uint8_t sig[RC_SIG_SIZE]) {
    uint8_t sm[RC_HEADER_V1_SIZE + RC_SIG_SIZE];
    uint8_t m[RC_HEADER_V1_SIZE + RC_SIG_SIZE];
    unsigned long long mlen = 0;

    if (wire == NULL || pk == NULL || sig == NULL) {
        return RC_ERR_NULL;
    }
    memcpy(sm, sig, RC_SIG_SIZE);
    memcpy(sm + RC_SIG_SIZE, wire, RC_HEADER_V1_SIZE);
    if (crypto_sign_open(m, &mlen, sm, RC_HEADER_V1_SIZE + RC_SIG_SIZE, pk) != 0) {
        return RC_ERR_VERIFY;
    }
    return RC_OK;
}

rc_status rc_message_seal(
    rc_header *h,
    const uint8_t *payload,
    size_t payload_len,
    const uint8_t sk[RC_SECRET_SIZE],
    uint8_t *out,
    size_t out_cap,
    size_t *out_len) {
    uint8_t sig[RC_SIG_SIZE];
    rc_status st;

    if (h == NULL || sk == NULL || out == NULL || out_len == NULL) {
        return RC_ERR_NULL;
    }
    st = rc_bind_payload(h, payload, payload_len);
    if (st != RC_OK) {
        return st;
    }
    st = rc_sign_header(h, sk, sig);
    if (st != RC_OK) {
        return st;
    }
    return rc_message_encode(h, payload, sig, out, out_cap, out_len);
}

rc_status rc_message_open(
    const uint8_t *in,
    size_t in_len,
    rc_header *h,
    const uint8_t **payload,
    uint16_t *payload_len) {
    const uint8_t *sig = NULL;
    rc_status st;

    st = rc_message_decode(in, in_len, h, payload, payload_len, &sig);
    if (st != RC_OK) {
        return st;
    }
    return rc_verify_header(in, h->as_pubkey, sig);
}
