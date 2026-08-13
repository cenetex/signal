/*
 * raticross.h — the envelope.
 *
 * This header is the specification. The C encoder is the notary.
 * Later TypeScript / Rust / WASM bindings live in other repositories.
 * If a binding disagrees with the bytes this file describes, the
 * binding is wrong.
 *
 * Opposite of an implicit, shifting chain-account layout:
 *   - every field has an offset, a size, and a meaning in this comment
 *   - v1 never moves a field; new meaning is a new version byte
 *   - reserved bytes MUST be written zero and MUST be rejected if not
 *   - multi-byte integers are little-endian on the wire, always
 *   - encode writes bytes; it never casts a host struct onto the wire
 *   - sizeof(rc_header) is a host accident, never a wire length
 *   - a session handle never appears in signed bytes
 *   - the signature is over the encoded 192-byte header, not rc_header
 *
 * Wire message, version 1:
 *
 *     [ header 192 bytes ][ payload N bytes ][ signature 64 bytes ]
 *
 * The signature is detached Ed25519 over the 192-byte header only.
 * The header carries SHA-256(payload), so the payload is bound without
 * being parsed. Raticross does not interpret payload. Worlds do.
 *
 * Header v1 offsets (decimal). Total size is RC_HEADER_V1_SIZE (192).
 *
 *   off  sz  field
 *     0   1  version                 = 1
 *     1   1  from_kind               rc_kind
 *     2   1  as_kind                 rc_kind, never SESSION
 *     3   1  to_kind                 rc_kind, never SESSION
 *     4   1  flags                   RC_FLAG_*
 *     5   3  reserved0               MUST be 0
 *     8  32  as_pubkey               speaker citizen
 *    40  32  to_pubkey               addressee citizen
 *    72  32  from_pubkey             zero iff from_kind == SESSION
 *   104  16  envelope_id             caller-chosen; uniqueness is host policy
 *   120   8  ts_ms                   Unix ms, uint64 LE; not a replay clock
 *   128  16  lease_id                zero iff no lease
 *   144  32  payload_sha256          SHA-256 of the payload bytes
 *   176   2  payload_len             uint16 LE, N
 *   178  14  reserved1               MUST be 0
 *
 * Contract B, encoded:
 *   SESSION may speak only with RC_FLAG_HAS_LEASE, a nonzero lease_id,
 *   and a zero from_pubkey. The adapter-local session handle is not here.
 *   A durable actor speaks as itself: from_kind == as_kind, from_pubkey
 *   equals as_pubkey, no lease flag, lease_id zero.
 *   The signature is always the speaking citizen's (as_pubkey).
 *
 * Versioning:
 *   Readers that do not know `version` MUST reject the message.
 *   Do not reuse reserved bytes in v1. v2 is a new layout behind
 *   version == 2. Old verifiers fail closed.
 *
 * Endianness:
 *   Hosts may be anything. The wire is little-endian. rc_header_encode
 *   and rc_header_decode are the only functions allowed to touch that.
 *
 * Keys:
 *   Public key is 32 raw Ed25519 bytes. Secret key is 64 bytes
 *   (seed || public), NaCl convention. A 32-byte seed is not accepted
 *   as a secret key. Display encodings (base58) are not in this header.
 *
 * What this library does not do:
 *   replay windows, clocks, routing, human identity, chain RPC,
 *   payload schemas, session-handle storage.
 */

#ifndef RATICROSS_H
#define RATICROSS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RC_VERSION_V1          1u
#define RC_PUBKEY_SIZE         32u
#define RC_SECRET_SIZE         64u
#define RC_ID_SIZE             16u
#define RC_HASH_SIZE           32u
#define RC_SIG_SIZE            64u
#define RC_HEADER_V1_SIZE      192u
#define RC_MAX_PAYLOAD         65535u

#define RC_FLAG_HAS_LEASE      (1u << 0)

/* Kinds on the wire. Zero is weather. Everything else is a citizen. */
typedef enum rc_kind {
    RC_KIND_SESSION = 0,
    RC_KIND_AVATAR  = 1,
    RC_KIND_AGENT   = 2,
    RC_KIND_SYSTEM  = 3
} rc_kind;

typedef enum rc_status {
    RC_OK                  = 0,
    RC_ERR_NULL            = 1,
    RC_ERR_VERSION         = 2,
    RC_ERR_KIND            = 3,
    RC_ERR_RESERVED        = 4,
    RC_ERR_FLAG            = 5,
    RC_ERR_PUBKEY          = 6,
    RC_ERR_LEASE           = 7,
    RC_ERR_SESSION         = 8,
    RC_ERR_IMPERSONATION   = 9,
    RC_ERR_RANGE           = 10,
    RC_ERR_HASH            = 11,
    RC_ERR_SHORT           = 12,
    RC_ERR_TRAILING        = 13,
    RC_ERR_SIGN            = 14,
    RC_ERR_VERIFY          = 15
} rc_status;

/*
 * Host-side view of a header. Field order here is for C callers.
 * It is NOT the wire layout: the compiler may pad ts_ms. Never
 * memcpy this struct, never use sizeof(rc_header) as a length.
 */
typedef struct rc_header {
    uint8_t  version;
    uint8_t  from_kind;
    uint8_t  as_kind;
    uint8_t  to_kind;
    uint8_t  flags;
    uint8_t  as_pubkey[RC_PUBKEY_SIZE];
    uint8_t  to_pubkey[RC_PUBKEY_SIZE];
    uint8_t  from_pubkey[RC_PUBKEY_SIZE];
    uint8_t  envelope_id[RC_ID_SIZE];
    uint64_t ts_ms;
    uint8_t  lease_id[RC_ID_SIZE];
    uint8_t  payload_sha256[RC_HASH_SIZE];
    uint16_t payload_len;
} rc_header;

/*
 * Offsets exist so a reader can audit the encoder without reading C.
 * Bindings and hex dumps should cite these names, not magic numbers.
 */
#define RC_OFF_VERSION         0u
#define RC_OFF_FROM_KIND       1u
#define RC_OFF_AS_KIND         2u
#define RC_OFF_TO_KIND         3u
#define RC_OFF_FLAGS           4u
#define RC_OFF_RESERVED0       5u
#define RC_OFF_AS_PUBKEY       8u
#define RC_OFF_TO_PUBKEY       40u
#define RC_OFF_FROM_PUBKEY     72u
#define RC_OFF_ENVELOPE_ID     104u
#define RC_OFF_TS_MS           120u
#define RC_OFF_LEASE_ID        128u
#define RC_OFF_PAYLOAD_SHA256  144u
#define RC_OFF_PAYLOAD_LEN     176u
#define RC_OFF_RESERVED1       178u

#define RC_RESERVED0_SIZE      3u
#define RC_RESERVED1_SIZE      14u

void       rc_header_clear(rc_header *h);
rc_status  rc_header_validate(const rc_header *h);
rc_status  rc_header_encode(const rc_header *h, uint8_t out[RC_HEADER_V1_SIZE]);
rc_status  rc_header_decode(const uint8_t in[RC_HEADER_V1_SIZE], rc_header *h);

void       rc_sha256(const uint8_t *data, size_t len, uint8_t out[RC_HASH_SIZE]);
rc_status  rc_bind_payload(rc_header *h, const uint8_t *payload, size_t len);

size_t     rc_message_size(uint16_t payload_len);
rc_status  rc_message_encode(
                const rc_header *h,
                const uint8_t *payload,
                const uint8_t sig[RC_SIG_SIZE],
                uint8_t *out,
                size_t out_cap,
                size_t *out_len);
rc_status  rc_message_decode(
                const uint8_t *in,
                size_t in_len,
                rc_header *h,
                const uint8_t **payload,
                uint16_t *payload_len,
                const uint8_t **sig);

/* Ed25519. Secret is 64 bytes (seed || pk). Sign the encoded header. */
rc_status  rc_keypair(uint8_t pk[RC_PUBKEY_SIZE], uint8_t sk[RC_SECRET_SIZE]);
rc_status  rc_keypair_from_seed(
                const uint8_t seed[32],
                uint8_t pk[RC_PUBKEY_SIZE],
                uint8_t sk[RC_SECRET_SIZE]);

/* Fill a header. Session constructors leave from_pubkey zero. */
rc_status  rc_header_actor(
                rc_header *h,
                rc_kind kind,
                const uint8_t as_pk[RC_PUBKEY_SIZE],
                rc_kind to_kind,
                const uint8_t to_pk[RC_PUBKEY_SIZE],
                const uint8_t envelope_id[RC_ID_SIZE],
                uint64_t ts_ms);
rc_status  rc_header_session(
                rc_header *h,
                rc_kind as_kind,
                const uint8_t as_pk[RC_PUBKEY_SIZE],
                rc_kind to_kind,
                const uint8_t to_pk[RC_PUBKEY_SIZE],
                const uint8_t envelope_id[RC_ID_SIZE],
                const uint8_t lease_id[RC_ID_SIZE],
                uint64_t ts_ms);
rc_status  rc_sign_header(
                const rc_header *h,
                const uint8_t sk[RC_SECRET_SIZE],
                uint8_t sig[RC_SIG_SIZE]);
rc_status  rc_verify_header(
                const uint8_t wire[RC_HEADER_V1_SIZE],
                const uint8_t pk[RC_PUBKEY_SIZE],
                const uint8_t sig[RC_SIG_SIZE]);
rc_status  rc_message_seal(
                rc_header *h,
                const uint8_t *payload,
                size_t payload_len,
                const uint8_t sk[RC_SECRET_SIZE],
                uint8_t *out,
                size_t out_cap,
                size_t *out_len);
rc_status  rc_message_open(
                const uint8_t *in,
                size_t in_len,
                rc_header *h,
                const uint8_t **payload,
                uint16_t *payload_len);

int        rc_is_zero(const uint8_t *p, size_t n);
const char *rc_status_str(rc_status s);

#ifdef __cplusplus
}
#endif

#endif /* RATICROSS_H */
