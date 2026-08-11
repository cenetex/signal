/*
 * handoff_ticket.c -- Signed cross-zone ship handoff envelope.
 */
#include "handoff_ticket.h"

#include "cargo_receipt.h"
#include "commodity.h"
#include "manifest.h"
#include "sha256.h"
#include "signal_crypto.h"
#include "wire_codec.h"

#include <string.h>

static const char HANDOFF_SHIP_DOMAIN[] = "signal-handoff-ship-v1";
static const char HANDOFF_CARGO_DOMAIN[] = "signal-handoff-cargo-v1";

#define write_u16_le wire_write_u16_le
#define read_u16_le  wire_read_u16_le
#define write_u32_le wire_write_u32_le
#define read_u32_le  wire_read_u32_le
#define write_u64_le wire_write_u64_le
#define read_u64_le  wire_read_u64_le
#define write_f32_le wire_write_f32_le
#define read_f32_le  wire_read_f32_le

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
        sha_update_f32(&c, ship_cargo_amount(ship, (commodity_t)i));
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
        cargo_unit_wire_pack(&ship->manifest.units[i], cargo_buf);
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

size_t handoff_ship_snapshot_size(const ship_t *ship) {
    uint16_t count = 0;
    size_t total = HANDOFF_SHIP_SNAPSHOT_HEADER_SIZE;
    const ship_receipts_t *receipts = NULL;
    if (!ship) return 0;
    if (ship->manifest.units) count = ship->manifest.count;
    if (count > HANDOFF_SHIP_SNAPSHOT_MAX_CARGO) return 0;
    receipts = ship_get_receipts_const(ship);
    for (uint16_t i = 0; i < count; i++) {
        uint8_t len = 0;
        if (receipts && i < receipts->count) {
            len = receipts->chains[i].len;
            if (len > CARGO_RECEIPT_CHAIN_MAX_LEN) return 0;
        }
        total += HANDOFF_CARGO_UNIT_WIRE_SIZE + 1u +
                 (size_t)len * CARGO_RECEIPT_SIZE;
    }
    return total;
}

bool handoff_ship_snapshot_pack(const ship_t *ship, uint8_t *out, size_t cap,
                                size_t *out_len) {
    size_t need = handoff_ship_snapshot_size(ship);
    size_t off = 0;
    uint16_t count = 0;
    const ship_receipts_t *receipts = NULL;
    if (out_len) *out_len = 0;
    if (!ship || !out || need == 0 || cap < need) return false;

    write_f32_le(&out[off], ship->pos.x); off += 4;
    write_f32_le(&out[off], ship->pos.y); off += 4;
    write_f32_le(&out[off], ship->vel.x); off += 4;
    write_f32_le(&out[off], ship->vel.y); off += 4;
    write_f32_le(&out[off], ship->angle); off += 4;
    write_f32_le(&out[off], ship->hull); off += 4;
    for (int c = 0; c < COMMODITY_COUNT; c++) {
        write_f32_le(&out[off], ship_cargo_amount(ship, (commodity_t)c));
        off += 4;
    }
    write_u32_le(&out[off], (uint32_t)ship->hull_class); off += 4;
    write_u32_le(&out[off], (uint32_t)ship->mining_level); off += 4;
    write_u32_le(&out[off], (uint32_t)ship->hold_level); off += 4;
    write_u32_le(&out[off], (uint32_t)ship->tractor_level); off += 4;
    out[off++] = ship->towed_count;
    for (int i = 0; i < 10; i++) {
        write_u16_le(&out[off], (uint16_t)ship->towed_fragments[i]);
        off += 2;
    }
    write_u16_le(&out[off], (uint16_t)ship->towed_scaffold); off += 2;
    write_f32_le(&out[off], ship->comm_range); off += 4;
    write_u32_le(&out[off], ship->unlocked_modules); off += 4;

    if (ship->manifest.units) count = ship->manifest.count;
    write_u16_le(&out[off], count); off += 2;
    receipts = ship_get_receipts_const(ship);
    for (uint16_t i = 0; i < count; i++) {
        uint8_t len = 0;
        cargo_unit_wire_pack(&ship->manifest.units[i], &out[off]);
        off += HANDOFF_CARGO_UNIT_WIRE_SIZE;
        if (receipts && i < receipts->count) len = receipts->chains[i].len;
        out[off++] = len;
        for (uint8_t j = 0; j < len; j++) {
            cargo_receipt_pack(&receipts->chains[i].links[j], &out[off]);
            off += CARGO_RECEIPT_SIZE;
        }
    }
    if (out_len) *out_len = off;
    return off == need;
}

bool handoff_ship_snapshot_unpack(const uint8_t *data, size_t len,
                                  ship_t *out, size_t *consumed) {
    ship_t tmp;
    size_t off = 0;
    uint16_t count = 0;
    if (consumed) *consumed = 0;
    if (!data || !out || len < HANDOFF_SHIP_SNAPSHOT_HEADER_SIZE)
        return false;
    memset(&tmp, 0, sizeof(tmp));

    tmp.pos.x = read_f32_le(&data[off]); off += 4;
    tmp.pos.y = read_f32_le(&data[off]); off += 4;
    tmp.vel.x = read_f32_le(&data[off]); off += 4;
    tmp.vel.y = read_f32_le(&data[off]); off += 4;
    tmp.angle = read_f32_le(&data[off]); off += 4;
    tmp.hull = read_f32_le(&data[off]); off += 4;
    for (int c = 0; c < COMMODITY_COUNT; c++) {
        tmp.cargo[c] = read_f32_le(&data[off]);
        off += 4;
    }
    tmp.hull_class = (hull_class_t)read_u32_le(&data[off]); off += 4;
    tmp.mining_level = (int)read_u32_le(&data[off]); off += 4;
    tmp.hold_level = (int)read_u32_le(&data[off]); off += 4;
    tmp.tractor_level = (int)read_u32_le(&data[off]); off += 4;
    tmp.towed_count = data[off++];
    for (int i = 0; i < 10; i++) {
        tmp.towed_fragments[i] = (int16_t)read_u16_le(&data[off]);
        off += 2;
    }
    tmp.towed_scaffold = (int16_t)read_u16_le(&data[off]); off += 2;
    tmp.comm_range = read_f32_le(&data[off]); off += 4;
    tmp.unlocked_modules = read_u32_le(&data[off]); off += 4;

    count = read_u16_le(&data[off]); off += 2;
    if (count > HANDOFF_SHIP_SNAPSHOT_MAX_CARGO) return false;
    if (!ship_manifest_bootstrap(&tmp)) goto fail;

    for (uint16_t i = 0; i < count; i++) {
        cargo_unit_t unit;
        cargo_receipt_chain_t chain;
        uint8_t chain_len = 0;
        memset(&chain, 0, sizeof(chain));
        if (len - off < HANDOFF_CARGO_UNIT_WIRE_SIZE + 1u) goto fail;
        cargo_unit_wire_unpack(&data[off], &unit);
        off += HANDOFF_CARGO_UNIT_WIRE_SIZE;
        chain_len = data[off++];
        if (chain_len > CARGO_RECEIPT_CHAIN_MAX_LEN) goto fail;
        if (len - off < (size_t)chain_len * CARGO_RECEIPT_SIZE) goto fail;
        chain.len = chain_len;
        for (uint8_t j = 0; j < chain_len; j++) {
            if (!cargo_receipt_unpack(&data[off], &chain.links[j]))
                goto fail;
            off += CARGO_RECEIPT_SIZE;
        }
        if (!ship_manifest_push_with_chain(&tmp, &unit,
                                           chain_len > 0 ? &chain : NULL))
            goto fail;
    }

    *out = tmp;
    if (consumed) *consumed = off;
    return true;

fail:
    ship_cleanup(&tmp);
    return false;
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
    if (out) memset(out, 0, sizeof(*out));
    if (!source_authority || !source_secret || !dest_authority ||
        !player_pubkey || !ship || !out || expires_tick < issued_tick ||
        is_zero32(source_authority) || is_zero32(dest_authority) ||
        is_zero32(player_pubkey)) {
        return false;
    }
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
    if (!signal_crypto_verify(out->signature, blob, sizeof(blob),
                              source_authority)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
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
