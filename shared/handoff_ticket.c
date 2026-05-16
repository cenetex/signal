/*
 * handoff_ticket.c -- Signed cross-zone ship handoff envelope.
 */
#include "handoff_ticket.h"

#include "cargo_receipt.h"
#include "manifest.h"
#include "sha256.h"
#include "signal_crypto.h"

#include <string.h>

static const char HANDOFF_SHIP_DOMAIN[] = "signal-handoff-ship-v1";
static const char HANDOFF_CARGO_DOMAIN[] = "signal-handoff-cargo-v1";

static void write_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void write_u64_le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t read_u64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

static void write_f32_le(uint8_t *p, float f) {
    union { float f; uint32_t u; } conv;
    conv.f = f;
    write_u32_le(p, conv.u);
}

static void sha_update_u16(sha256_ctx_t *c, uint16_t v) {
    uint8_t b[2];
    write_u16_le(b, v);
    sha256_update(c, b, sizeof(b));
}

static void sha_update_u32(sha256_ctx_t *c, uint32_t v) {
    uint8_t b[4];
    write_u32_le(b, v);
    sha256_update(c, b, sizeof(b));
}

static void sha_update_i32(sha256_ctx_t *c, int32_t v) {
    sha_update_u32(c, (uint32_t)v);
}

static void sha_update_f32(sha256_ctx_t *c, float f) {
    uint8_t b[4];
    write_f32_le(b, f);
    sha256_update(c, b, sizeof(b));
}

static bool is_zero32(const uint8_t p[32]) {
    static const uint8_t zero32[32] = {0};
    return !p || memcmp(p, zero32, 32) == 0;
}

static void cargo_unit_pack_for_handoff(const cargo_unit_t *u, uint8_t out[80]) {
    memset(out, 0, 80);
    if (!u) return;
    out[0] = u->kind;
    out[1] = u->commodity;
    out[2] = u->grade;
    out[3] = u->prefix_class;
    write_u16_le(&out[4], u->recipe_id);
    out[6] = u->origin_station;
    out[7] = u->quantity;
    write_u64_le(&out[8], u->mined_block);
    memcpy(&out[16], u->pub, 32);
    memcpy(&out[48], u->parent_merkle, 32);
}

void handoff_ticket_unsigned_pack(
    const handoff_ticket_t *ticket,
    uint8_t out[HANDOFF_TICKET_UNSIGNED_SIZE]) {
    size_t off = 0;
    memset(out, 0, HANDOFF_TICKET_UNSIGNED_SIZE);
    if (!ticket) return;
    memcpy(&out[off], ticket->source_authority, 32); off += 32;
    memcpy(&out[off], ticket->dest_authority, 32);   off += 32;
    memcpy(&out[off], ticket->player_pubkey, 32);    off += 32;
    write_u64_le(&out[off], ticket->issued_tick);    off += 8;
    write_u64_le(&out[off], ticket->expires_tick);   off += 8;
    write_u32_le(&out[off], ticket->source_zone);    off += 4;
    write_u32_le(&out[off], ticket->dest_zone);      off += 4;
    write_u16_le(&out[off], ticket->cargo_count);    off += 2;
    write_u16_le(&out[off], ticket->flags);          off += 2;
    memcpy(&out[off], ticket->ship_state_hash, 32);  off += 32;
    memcpy(&out[off], ticket->cargo_root, 32);       off += 32;
    (void)off;
}

void handoff_ticket_pack(const handoff_ticket_t *ticket,
                         uint8_t out[HANDOFF_TICKET_SIZE]) {
    handoff_ticket_unsigned_pack(ticket, out);
    if (ticket)
        memcpy(&out[HANDOFF_TICKET_UNSIGNED_SIZE], ticket->signature, 64);
    else
        memset(&out[HANDOFF_TICKET_UNSIGNED_SIZE], 0, 64);
}

bool handoff_ticket_unpack(const uint8_t in[HANDOFF_TICKET_SIZE],
                           handoff_ticket_t *out) {
    if (!in || !out) return false;
    size_t off = 0;
    memset(out, 0, sizeof(*out));
    memcpy(out->source_authority, &in[off], 32); off += 32;
    memcpy(out->dest_authority, &in[off], 32);   off += 32;
    memcpy(out->player_pubkey, &in[off], 32);    off += 32;
    out->issued_tick = read_u64_le(&in[off]);    off += 8;
    out->expires_tick = read_u64_le(&in[off]);   off += 8;
    out->source_zone = read_u32_le(&in[off]);    off += 4;
    out->dest_zone = read_u32_le(&in[off]);      off += 4;
    out->cargo_count = read_u16_le(&in[off]);    off += 2;
    out->flags = read_u16_le(&in[off]);          off += 2;
    memcpy(out->ship_state_hash, &in[off], 32);  off += 32;
    memcpy(out->cargo_root, &in[off], 32);       off += 32;
    memcpy(out->signature, &in[off], 64);
    return true;
}

void handoff_ticket_ship_state_hash(const ship_t *ship, uint8_t out[32]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, HANDOFF_SHIP_DOMAIN, sizeof(HANDOFF_SHIP_DOMAIN) - 1);
    if (!ship) {
        sha256_final(&c, out);
        return;
    }
    sha_update_f32(&c, ship->pos.x);
    sha_update_f32(&c, ship->pos.y);
    sha_update_f32(&c, ship->vel.x);
    sha_update_f32(&c, ship->vel.y);
    sha_update_f32(&c, ship->angle);
    sha_update_f32(&c, ship->hull);
    for (int i = 0; i < COMMODITY_COUNT; i++)
        sha_update_f32(&c, ship->cargo[i]);
    sha_update_i32(&c, (int32_t)ship->hull_class);
    sha_update_i32(&c, (int32_t)ship->mining_level);
    sha_update_i32(&c, (int32_t)ship->hold_level);
    sha_update_i32(&c, (int32_t)ship->tractor_level);
    sha256_update(&c, &ship->towed_count, 1);
    for (int i = 0; i < 10; i++)
        sha_update_u16(&c, (uint16_t)ship->towed_fragments[i]);
    sha_update_u16(&c, (uint16_t)ship->towed_scaffold);
    sha_update_f32(&c, ship->comm_range);
    sha_update_u32(&c, ship->unlocked_modules);
    sha256_final(&c, out);
}

void handoff_ticket_cargo_root(const ship_t *ship, uint8_t out[32]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, HANDOFF_CARGO_DOMAIN, sizeof(HANDOFF_CARGO_DOMAIN) - 1);
    if (!ship || !ship->manifest.units) {
        sha_update_u16(&c, 0);
        sha256_final(&c, out);
        return;
    }
    sha_update_u16(&c, ship->manifest.count);
    const ship_receipts_t *receipts = ship_get_receipts_const(ship);
    for (uint16_t i = 0; i < ship->manifest.count; i++) {
        uint8_t cargo_buf[80];
        cargo_unit_pack_for_handoff(&ship->manifest.units[i], cargo_buf);
        sha256_update(&c, cargo_buf, sizeof(cargo_buf));
        uint8_t len = 0;
        if (receipts && i < receipts->count)
            len = receipts->chains[i].len;
        if (len > CARGO_RECEIPT_CHAIN_MAX_LEN)
            len = CARGO_RECEIPT_CHAIN_MAX_LEN;
        sha256_update(&c, &len, 1);
        for (uint8_t j = 0; j < len; j++) {
            uint8_t receipt_buf[CARGO_RECEIPT_SIZE];
            cargo_receipt_pack(&receipts->chains[i].links[j], receipt_buf);
            sha256_update(&c, receipt_buf, sizeof(receipt_buf));
        }
    }
    sha256_final(&c, out);
}

void handoff_ticket_hash(const handoff_ticket_t *ticket, uint8_t out[32]) {
    uint8_t packed[HANDOFF_TICKET_SIZE];
    handoff_ticket_pack(ticket, packed);
    sha256_bytes(packed, sizeof(packed), out);
}

bool handoff_ticket_issue_for_ship(
    const uint8_t source_authority[32],
    const uint8_t source_secret[64],
    const uint8_t dest_authority[32],
    const uint8_t player_pubkey[32],
    uint32_t source_zone,
    uint32_t dest_zone,
    uint64_t issued_tick,
    uint64_t expires_tick,
    const ship_t *ship,
    handoff_ticket_t *out) {
    if (!source_authority || !source_secret || !dest_authority ||
        !player_pubkey || !ship || !out || expires_tick < issued_tick ||
        is_zero32(source_authority) || is_zero32(dest_authority) ||
        is_zero32(player_pubkey)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->source_authority, source_authority, 32);
    memcpy(out->dest_authority, dest_authority, 32);
    memcpy(out->player_pubkey, player_pubkey, 32);
    out->issued_tick = issued_tick;
    out->expires_tick = expires_tick;
    out->source_zone = source_zone;
    out->dest_zone = dest_zone;
    out->cargo_count = ship->manifest.count;
    handoff_ticket_ship_state_hash(ship, out->ship_state_hash);
    handoff_ticket_cargo_root(ship, out->cargo_root);
    uint8_t blob[HANDOFF_TICKET_UNSIGNED_SIZE];
    handoff_ticket_unsigned_pack(out, blob);
    signal_crypto_sign(out->signature, blob, sizeof(blob), source_secret);
    return true;
}

handoff_ticket_result_t handoff_ticket_verify_hashes(
    const handoff_ticket_t *ticket,
    uint64_t now_tick,
    const uint8_t expected_source_authority[32],
    const uint8_t expected_dest_authority[32],
    const uint8_t expected_player_pubkey[32],
    const uint8_t expected_ship_state_hash[32],
    const uint8_t expected_cargo_root[32]) {
    if (!ticket || ticket->expires_tick < ticket->issued_tick)
        return HANDOFF_TICKET_REJECT_BAD_ARGS;
    if (is_zero32(ticket->source_authority) || is_zero32(ticket->dest_authority))
        return HANDOFF_TICKET_REJECT_ZERO_AUTHORITY;
    if (now_tick > ticket->expires_tick)
        return HANDOFF_TICKET_REJECT_EXPIRED;
    if (expected_source_authority &&
        memcmp(ticket->source_authority, expected_source_authority, 32) != 0)
        return HANDOFF_TICKET_REJECT_SOURCE;
    if (expected_dest_authority &&
        memcmp(ticket->dest_authority, expected_dest_authority, 32) != 0)
        return HANDOFF_TICKET_REJECT_DEST;
    if (expected_player_pubkey &&
        memcmp(ticket->player_pubkey, expected_player_pubkey, 32) != 0)
        return HANDOFF_TICKET_REJECT_PLAYER;
    if (expected_ship_state_hash &&
        memcmp(ticket->ship_state_hash, expected_ship_state_hash, 32) != 0)
        return HANDOFF_TICKET_REJECT_SHIP_STATE;
    if (expected_cargo_root &&
        memcmp(ticket->cargo_root, expected_cargo_root, 32) != 0)
        return HANDOFF_TICKET_REJECT_CARGO_ROOT;

    uint8_t blob[HANDOFF_TICKET_UNSIGNED_SIZE];
    handoff_ticket_unsigned_pack(ticket, blob);
    if (!signal_crypto_verify(ticket->signature, blob, sizeof(blob),
                              ticket->source_authority)) {
        return HANDOFF_TICKET_REJECT_BAD_SIGNATURE;
    }
    return HANDOFF_TICKET_OK;
}

handoff_ticket_result_t handoff_ticket_verify_for_ship(
    const handoff_ticket_t *ticket,
    uint64_t now_tick,
    const uint8_t expected_source_authority[32],
    const uint8_t expected_dest_authority[32],
    const uint8_t expected_player_pubkey[32],
    const ship_t *ship) {
    if (!ship) return HANDOFF_TICKET_REJECT_BAD_ARGS;
    uint8_t ship_hash[32];
    uint8_t cargo_root[32];
    handoff_ticket_ship_state_hash(ship, ship_hash);
    handoff_ticket_cargo_root(ship, cargo_root);
    return handoff_ticket_verify_hashes(ticket, now_tick,
                                        expected_source_authority,
                                        expected_dest_authority,
                                        expected_player_pubkey,
                                        ship_hash, cargo_root);
}

const char *handoff_ticket_result_name(handoff_ticket_result_t result) {
    switch (result) {
    case HANDOFF_TICKET_OK: return "ok";
    case HANDOFF_TICKET_REJECT_BAD_ARGS: return "bad-args";
    case HANDOFF_TICKET_REJECT_EXPIRED: return "expired";
    case HANDOFF_TICKET_REJECT_ZERO_AUTHORITY: return "zero-authority";
    case HANDOFF_TICKET_REJECT_SOURCE: return "source";
    case HANDOFF_TICKET_REJECT_DEST: return "dest";
    case HANDOFF_TICKET_REJECT_PLAYER: return "player";
    case HANDOFF_TICKET_REJECT_SHIP_STATE: return "ship-state";
    case HANDOFF_TICKET_REJECT_CARGO_ROOT: return "cargo-root";
    case HANDOFF_TICKET_REJECT_BAD_SIGNATURE: return "bad-signature";
    default: return "unknown";
    }
}
