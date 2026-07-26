#include "settlement_engine.h"
#include "sha256.h"
#include "../server/chain_log.h"
#include <string.h>
#include <stdlib.h>

enum {
    SETTL_EVT_TRANSFER_CARGO = 0x04,
    SETTL_EVT_SELL = 0x11,
    SETTL_EVT_DELIVER_CONSTRUCTION_INPUT = 0x21,
    SETTL_TRANSFER_PAYLOAD_SIZE = 144,
    SETTL_TRADE_PAYLOAD_SIZE = 112,
    SETTL_TRADE_DIRECTION_OFFSET = 105,
    SETTL_MAX_CONSTRUCTION_INPUTS = 3,
    SETTL_CONSTRUCTION_INPUT_COUNT_OFFSET =
        32 + 32 + SETTL_MAX_CONSTRUCTION_INPUTS * 32,
    SETTL_CONSTRUCTION_INPUT_PAYLOAD_SIZE =
        SETTL_CONSTRUCTION_INPUT_COUNT_OFFSET + 8
};

static int cmp_u8_32(const void *a, const void *b) {
    return memcmp(a, b, 32);
}

static int find_manifest_unit(const cargo_unit_t *m, uint16_t count,
                               const uint8_t pub[32]) {
    for (uint16_t i = 0; i < count; i++)
        if (memcmp(m[i].pub, pub, 32) == 0) return (int)i;
    return -1;
}

static void settlement_manifest_remove(cargo_unit_t *m, uint16_t *count,
                                       uint16_t idx) {
    if (idx < *count - 1) m[idx] = m[*count - 1];
    (*count)--;
}

static int find_ledger_entry(const settl_ledger_entry_t *ledger,
                              uint8_t count, const uint8_t pubkey[32]) {
    for (uint8_t i = 0; i < count; i++)
        if (memcmp(ledger[i].player_pubkey, pubkey, 32) == 0) return (int)i;
    return -1;
}

void settlement_state_init(settlement_state_t *s) {
    memset(s, 0, sizeof(*s));
}

const char *settlement_import_status_name(settlement_import_status_t status) {
    switch (status) {
    case SETTL_IMPORT_OK: return "ok";
    case SETTL_IMPORT_REJECT_BAD_ARGUMENTS: return "reject_bad_arguments";
    case SETTL_IMPORT_REJECT_PAYLOAD_HASH: return "reject_payload_hash";
    case SETTL_IMPORT_REJECT_MISSING_EVIDENCE: return "reject_missing_evidence";
    case SETTL_IMPORT_REJECT_EVIDENCE_COUNT: return "reject_evidence_count";
    case SETTL_IMPORT_REJECT_EVIDENCE_EVENT: return "reject_evidence_event";
    case SETTL_IMPORT_REJECT_EVIDENCE_CARGO: return "reject_evidence_cargo";
    case SETTL_IMPORT_REJECT_TRUST_BAD_ARGUMENTS:
        return "reject_trust_bad_arguments";
    case SETTL_IMPORT_REJECT_TRUST_CHAIN: return "reject_trust_chain";
    case SETTL_IMPORT_REJECT_TRUST_MISSING_ORIGIN:
        return "reject_trust_missing_origin";
    case SETTL_IMPORT_REJECT_TRUST_ORIGIN_EVENT_TYPE:
        return "reject_trust_origin_event_type";
    case SETTL_IMPORT_REJECT_TRUST_ORIGIN_CARGO:
        return "reject_trust_origin_cargo";
    case SETTL_IMPORT_REJECT_TRUST_ORIGIN_PIN:
        return "reject_trust_origin_pin";
    case SETTL_IMPORT_REJECT_TRUST_ORIGIN_AUTHORITY:
        return "reject_trust_origin_authority";
    case SETTL_IMPORT_REJECT_TRUST_UNKNOWN_AUTHORITY:
        return "reject_trust_unknown_authority";
    case SETTL_IMPORT_REJECT_TRUST_UNTRUSTED_AUTHORITY:
        return "reject_trust_untrusted_authority";
    case SETTL_IMPORT_REJECT_TRUST_REVOKED_AUTHORITY:
        return "reject_trust_revoked_authority";
    case SETTL_IMPORT_REJECT_EVENT: return "reject_event";
    default: return "unknown";
    }
}

/* ---- state root ---- */

static void hash_u32(sha256_ctx_t *ctx, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v};
    sha256_update(ctx, b, 4);
}

static void hash_u64(sha256_ctx_t *ctx, uint64_t v) {
    uint8_t b[8] = {(uint8_t)(v>>56),(uint8_t)(v>>48),(uint8_t)(v>>40),(uint8_t)(v>>32),
                    (uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v};
    sha256_update(ctx, b, 8);
}

void settlement_compute_root(const settlement_state_t *s, uint8_t root_out[32]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    hash_u32(&ctx, s->segment_index);
    hash_u64(&ctx, s->last_event_id);

    for (int st = 0; st < MAX_STATIONS; st++) {
        uint16_t mc = s->station_manifest_counts[st];
        if (mc == 0) continue;
        hash_u32(&ctx, (uint32_t)st);
        cargo_unit_t sorted[SETTL_MAX_MANIFEST_UNITS];
        memcpy(sorted, s->station_manifests[st], mc * sizeof(cargo_unit_t));
        qsort(sorted, mc, sizeof(cargo_unit_t), cmp_u8_32);
        for (uint16_t i = 0; i < mc; i++)
            sha256_update(&ctx, &sorted[i], sizeof(cargo_unit_t));
    }

    for (int st = 0; st < MAX_STATIONS; st++) {
        uint8_t lc = s->station_ledger_counts[st];
        if (lc == 0) continue;
        hash_u32(&ctx, (uint32_t)st);
        settl_ledger_entry_t sorted[SETTL_MAX_LEDGER_ENTRIES];
        memcpy(sorted, s->station_ledgers[st], lc * sizeof(settl_ledger_entry_t));
        qsort(sorted, lc, sizeof(settl_ledger_entry_t), cmp_u8_32);
        for (uint8_t i = 0; i < lc; i++) {
            sha256_update(&ctx, sorted[i].player_pubkey, 32);
            sha256_update(&ctx, &sorted[i].balance, sizeof(float));
            sha256_update(&ctx, &sorted[i].lifetime_supply, sizeof(float));
        }
    }

    {
        settl_fragment_owner_t srt[SETTL_MAX_FRAGMENT_OWNERS];
        memcpy(srt, s->fragment_owners, s->fragment_owner_count * sizeof(*srt));
        qsort(srt, s->fragment_owner_count, sizeof(*srt), cmp_u8_32);
        for (uint16_t i = 0; i < s->fragment_owner_count; i++)
            sha256_update(&ctx, &srt[i], sizeof(*srt));
    }
    {
        settl_credit_note_t srt[SETTL_MAX_CREDIT_NOTES];
        memcpy(srt, s->credit_notes, s->credit_note_count * sizeof(*srt));
        qsort(srt, s->credit_note_count, sizeof(*srt), cmp_u8_32);
        for (uint16_t i = 0; i < s->credit_note_count; i++)
            sha256_update(&ctx, &srt[i], sizeof(*srt));
    }
    {
        settl_construction_site_t srt[SETTL_MAX_CONSTRUCTION_SITES];
        memcpy(srt, s->construction_sites, s->construction_site_count * sizeof(*srt));
        qsort(srt, s->construction_site_count, sizeof(*srt), cmp_u8_32);
        for (uint16_t i = 0; i < s->construction_site_count; i++)
            sha256_update(&ctx, &srt[i], sizeof(*srt));
    }
    {
        settl_death_record_t srt[SETTL_MAX_DEATH_RECORDS];
        memcpy(srt, s->death_records, s->death_record_count * sizeof(*srt));
        qsort(srt, s->death_record_count, sizeof(*srt), cmp_u8_32);
        for (uint16_t i = 0; i < s->death_record_count; i++)
            sha256_update(&ctx, &srt[i], sizeof(*srt));
    }

    sha256_final(&ctx, root_out);
}

/* ---- event application ---- */

static bool settlement_payload_hash_matches(const chain_event_header_t *hdr,
                                            const uint8_t *payload,
                                            uint16_t payload_len) {
    if (!hdr || !payload) return false;
    uint8_t hash[32];
    sha256_bytes(payload, payload_len, hash);
    return memcmp(hash, hdr->payload_hash, sizeof(hash)) == 0;
}

static bool settlement_event_requires_trust(uint8_t type) {
    return type == SETTL_EVT_TRANSFER_CARGO ||
           type == SETTL_EVT_SELL ||
           type == SETTL_EVT_DELIVER_CONSTRUCTION_INPUT;
}

static bool settlement_apply_event_preflighted(settlement_state_t *s,
                                               const void *hdr_arg,
                                               const uint8_t *payload,
                                               uint16_t payload_len) {
    const chain_event_header_t *hdr = (const chain_event_header_t *)hdr_arg;
    if (!s || !settlement_payload_hash_matches(hdr, payload, payload_len))
        return false;
    int st = 0; /* single-station for now — extended in multi-station world */
    cargo_unit_t *manifest = s->station_manifests[st];
    uint16_t *mc = &s->station_manifest_counts[st];
    settl_ledger_entry_t *ledger = s->station_ledgers[st];
    uint8_t *lc = &s->station_ledger_counts[st];

    switch (hdr->type) {

    case 0x01: { /* CLAIM_FRAGMENT */
        if (payload_len < 112 || s->fragment_owner_count >= SETTL_MAX_FRAGMENT_OWNERS)
            return false;
        for (uint16_t i = 0; i < s->fragment_owner_count; i++)
            if (memcmp(s->fragment_owners[i].fragment_pub, payload, 32) == 0)
                return false;
        settl_fragment_owner_t *fo = &s->fragment_owners[s->fragment_owner_count++];
        memcpy(fo->fragment_pub, payload, 32);
        memcpy(fo->winner_pubkey, payload + 32, 32);
        memcpy(fo->rock_pub, payload + 64, 32);
        fo->grade = payload[96];
        memcpy(&fo->mined_block, payload + 104, 8);
        return true;
    }

    case 0x02: { /* SMELT_INGOT */
        if (payload_len < 80 || *mc >= SETTL_MAX_MANIFEST_UNITS) return false;
        /* Remove fragment owner */
        for (uint16_t i = 0; i < s->fragment_owner_count; i++) {
            if (memcmp(s->fragment_owners[i].fragment_pub, payload, 32) == 0) {
                s->fragment_owners[i] = s->fragment_owners[--s->fragment_owner_count];
                break;
            }
        }
        cargo_unit_t *cu = &manifest[(*mc)++];
        memset(cu, 0, sizeof(*cu));
        memcpy(cu->pub, payload + 32, 32);
        cu->kind = CARGO_KIND_INGOT;
        cu->commodity = COMMODITY_FERRITE_ORE;
        cu->prefix_class = payload[64];
        memcpy(&cu->mined_block, payload + 72, 8);
        cu->quantity = 1;
        return true;
    }

    case 0x03: { /* PRODUCE_OUTPUT */
        if (payload_len < 12) return false;
        uint8_t ic = payload[2], oc = payload[3];
        if (payload_len < 12 + (ic + oc) * 32) return false;
        for (uint8_t j = 0; j < ic; j++) {
            int idx = find_manifest_unit(manifest, *mc, payload + 12 + j * 32);
            if (idx < 0) return false;
            settlement_manifest_remove(manifest, mc, (uint16_t)idx);
        }
        for (uint8_t j = 0; j < oc; j++) {
            if (*mc >= SETTL_MAX_MANIFEST_UNITS) return false;
            cargo_unit_t *cu = &manifest[(*mc)++];
            memset(cu, 0, sizeof(*cu));
            memcpy(cu->pub, payload + 12 + ic * 32 + j * 32, 32);
            cu->kind = CARGO_KIND_FRAME; /* generic finished good */
            cu->quantity = 1;
        }
        return true;
    }

    case SETTL_EVT_TRANSFER_CARGO: { /* TRANSFER_CARGO */
        if (payload_len < SETTL_TRANSFER_PAYLOAD_SIZE) return false;
        int idx = find_manifest_unit(manifest, *mc, payload);
        if (idx < 0) return false;
        cargo_unit_t unit = manifest[idx];
        settlement_manifest_remove(manifest, mc, (uint16_t)idx);
        /* Add to dest (same station for now) */
        if (*mc >= SETTL_MAX_MANIFEST_UNITS) return false;
        manifest[(*mc)++] = unit;
        return true;
    }

    case 0x10: /* BUY */
    case SETTL_EVT_SELL: { /* SELL */
        if (payload_len < SETTL_TRADE_PAYLOAD_SIZE) return false;
        int64_t delta;
        memcpy(&delta, payload + 96, 8);
        uint8_t dir = payload[SETTL_TRADE_DIRECTION_OFFSET];
        if ((hdr->type == 0x10 && dir != 0) ||
            (hdr->type == SETTL_EVT_SELL && dir != 1))
            return false;

        if (dir == 0) { /* BUY: station→player, remove from manifest */
            int idx = find_manifest_unit(manifest, *mc, payload);
            if (idx < 0) return false;
            settlement_manifest_remove(manifest, mc, (uint16_t)idx);
            int li = find_ledger_entry(ledger, *lc, payload + 32);
            if (li >= 0) ledger[li].balance += (float)delta;
        } else { /* SELL: player→station, add to manifest */
            if (*mc >= SETTL_MAX_MANIFEST_UNITS) return false;
            cargo_unit_t *cu = &manifest[(*mc)++];
            memset(cu, 0, sizeof(*cu));
            memcpy(cu->pub, payload, 32);
            cu->quantity = 1;
            int li = find_ledger_entry(ledger, *lc, payload + 32);
            if (li >= 0) {
                ledger[li].balance += (float)delta;
            } else if (*lc < SETTL_MAX_LEDGER_ENTRIES) {
                memcpy(ledger[*lc].player_pubkey, payload + 32, 32);
                ledger[*lc].balance = (float)delta;
                ledger[*lc].lifetime_supply = 0;
                (*lc)++;
            } else return false;
        }
        return true;
    }

    case 0x12: { /* ISSUE_CREDIT_NOTE */
        if (payload_len < 120 || s->credit_note_count >= SETTL_MAX_CREDIT_NOTES)
            return false;
        settl_credit_note_t *cn = &s->credit_notes[s->credit_note_count++];
        memcpy(cn->note_id, payload, 32);
        memcpy(cn->station_pubkey, payload + 32, 32);
        memcpy(cn->player_pubkey, payload + 64, 32);
        memcpy(&cn->amount, payload + 96, 8);
        memcpy(&cn->nonce, payload + 104, 8);
        memcpy(&cn->expiry_tick, payload + 112, 8);
        cn->redeemed = false;
        return true;
    }

    case 0x13: { /* REDEEM_CREDIT_NOTE */
        if (payload_len < 32) return false;
        for (uint16_t i = 0; i < s->credit_note_count; i++)
            if (memcmp(s->credit_notes[i].note_id, payload, 32) == 0) {
                if (s->credit_notes[i].redeemed) return false;
                s->credit_notes[i].redeemed = true;
                return true;
            }
        return false;
    }

    case 0x20: { /* START_STATION_SITE */
        if (payload_len < 72 || s->construction_site_count >= SETTL_MAX_CONSTRUCTION_SITES)
            return false;
        settl_construction_site_t *cs = &s->construction_sites[s->construction_site_count++];
        memcpy(cs->scaffold_id, payload, 32);
        memcpy(cs->station_pubkey, payload + 32, 32);
        cs->module_type = payload[64];
        cs->build_progress = 0.0f;
        cs->active = true;
        return true;
    }

    case SETTL_EVT_DELIVER_CONSTRUCTION_INPUT: {
        /* DELIVER_CONSTRUCTION_INPUT */
        if (payload_len < SETTL_CONSTRUCTION_INPUT_PAYLOAD_SIZE) return false;
        uint8_t input_count = payload[SETTL_CONSTRUCTION_INPUT_COUNT_OFFSET];
        if (input_count == 0 || input_count > SETTL_MAX_CONSTRUCTION_INPUTS)
            return false;

        int site_idx = -1;
        for (uint16_t i = 0; i < s->construction_site_count; i++) {
            if (memcmp(s->construction_sites[i].scaffold_id, payload, 32) == 0 &&
                s->construction_sites[i].active) {
                site_idx = (int)i;
                break;
            }
        }
        if (site_idx < 0) return false;

        /*
         * Validate the whole input set before removal. Duplicate pubs would
         * otherwise make a direct event application partially consume cargo.
         */
        for (uint8_t i = 0; i < input_count; i++) {
            const uint8_t *pub = payload + 64 + i * 32;
            if (find_manifest_unit(manifest, *mc, pub) < 0) return false;
            for (uint8_t j = 0; j < i; j++)
                if (memcmp(pub, payload + 64 + j * 32, 32) == 0)
                    return false;
        }
        for (uint8_t i = 0; i < input_count; i++) {
            int idx = find_manifest_unit(
                manifest, *mc, payload + 64 + i * 32);
            settlement_manifest_remove(manifest, mc, (uint16_t)idx);
        }

        settl_construction_site_t *site = &s->construction_sites[site_idx];
        site->build_progress += 0.1f * (float)input_count;
        if (site->build_progress > 1.0f) site->build_progress = 1.0f;
        return true;
    }

    case 0x22: { /* COMPLETE_STATION_MODULE */
        if (payload_len < 80) return false;
        for (uint16_t i = 0; i < s->construction_site_count; i++)
            if (memcmp(s->construction_sites[i].scaffold_id, payload, 32) == 0) {
                s->construction_sites[i].active = false;
                return true;
            }
        return false;
    }

    case 0x40: { /* PLAYER_DEATH */
        if (payload_len < 72 || s->death_record_count >= SETTL_MAX_DEATH_RECORDS)
            return false;
        settl_death_record_t *dr = &s->death_records[s->death_record_count++];
        memcpy(dr->player_pubkey, payload, 32);
        memcpy(dr->player_callsign, payload + 32, 8);
        dr->cause = payload[40];
        memcpy(&dr->death_tick, payload + 48, 8);
        memcpy(&dr->credits_earned, payload + 56, 4);
        memcpy(&dr->credits_spent, payload + 60, 4);
        memcpy(&dr->ore_mined, payload + 64, 4);
        memcpy(&dr->asteroids_fractured, payload + 68, 4);
        return true;
    }

    case 0x30: /* POST_SIGNAL_ANCHOR */
    case 0x31: /* OPERATOR_POST */
        return true;

    default:
        return false;
    }
}

bool settlement_apply_event(settlement_state_t *s,
                            const void *hdr_arg,
                            const uint8_t *payload,
                            uint16_t payload_len) {
    const chain_event_header_t *hdr = (const chain_event_header_t *)hdr_arg;
    if (!hdr || settlement_event_requires_trust(hdr->type)) return false;
    return settlement_apply_event_preflighted(
        s, hdr_arg, payload, payload_len);
}

static settlement_import_status_t settlement_status_from_trust(
    cargo_receipt_trust_status_t status) {
    switch (status) {
    case CARGO_RECEIPT_TRUST_VALID_TRUSTED:
    case CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED:
        return SETTL_IMPORT_OK;
    case CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS:
        return SETTL_IMPORT_REJECT_TRUST_BAD_ARGUMENTS;
    case CARGO_RECEIPT_TRUST_REJECT_CHAIN:
        return SETTL_IMPORT_REJECT_TRUST_CHAIN;
    case CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN:
        return SETTL_IMPORT_REJECT_TRUST_MISSING_ORIGIN;
    case CARGO_RECEIPT_TRUST_REJECT_ORIGIN_EVENT_TYPE:
        return SETTL_IMPORT_REJECT_TRUST_ORIGIN_EVENT_TYPE;
    case CARGO_RECEIPT_TRUST_REJECT_ORIGIN_CARGO:
        return SETTL_IMPORT_REJECT_TRUST_ORIGIN_CARGO;
    case CARGO_RECEIPT_TRUST_REJECT_ORIGIN_PIN:
        return SETTL_IMPORT_REJECT_TRUST_ORIGIN_PIN;
    case CARGO_RECEIPT_TRUST_REJECT_ORIGIN_AUTHORITY:
        return SETTL_IMPORT_REJECT_TRUST_ORIGIN_AUTHORITY;
    case CARGO_RECEIPT_TRUST_REJECT_UNKNOWN_AUTHORITY:
        return SETTL_IMPORT_REJECT_TRUST_UNKNOWN_AUTHORITY;
    case CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY:
        return SETTL_IMPORT_REJECT_TRUST_UNTRUSTED_AUTHORITY;
    case CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY:
        return SETTL_IMPORT_REJECT_TRUST_REVOKED_AUTHORITY;
    default:
        return SETTL_IMPORT_REJECT_TRUST_BAD_ARGUMENTS;
    }
}

static void settlement_result_set(settlement_import_result_t *result,
                                  settlement_import_status_t status,
                                  uint32_t event_index,
                                  uint8_t cargo_index,
                                  cargo_receipt_result_t chain_result) {
    if (!result) return;
    result->status = status;
    result->event_index = event_index;
    result->cargo_index = cargo_index;
    result->chain_result = chain_result;
}

static bool settlement_preflight_event(
    const chain_event_header_t *hdr,
    const uint8_t *payload,
    uint16_t payload_len,
    const settlement_event_trust_evidence_t *evidence,
    uint32_t event_index,
    settlement_import_result_t *result) {
    if (!settlement_payload_hash_matches(hdr, payload, payload_len)) {
        settlement_result_set(result, SETTL_IMPORT_REJECT_PAYLOAD_HASH,
                              event_index, UINT8_MAX, CARGO_RECEIPT_OK);
        return false;
    }

    const uint8_t *cargo_pubs[SETTL_MAX_CONSTRUCTION_INPUTS] = {0};
    uint8_t cargo_count = 0;
    switch (hdr->type) {
    case SETTL_EVT_TRANSFER_CARGO:
        if (payload_len < SETTL_TRANSFER_PAYLOAD_SIZE) {
            settlement_result_set(result, SETTL_IMPORT_REJECT_EVENT,
                                  event_index, UINT8_MAX, CARGO_RECEIPT_OK);
            return false;
        }
        cargo_pubs[cargo_count++] = payload;
        break;
    case SETTL_EVT_SELL:
        if (payload_len < SETTL_TRADE_PAYLOAD_SIZE) {
            settlement_result_set(result, SETTL_IMPORT_REJECT_EVENT,
                                  event_index, UINT8_MAX, CARGO_RECEIPT_OK);
            return false;
        }
        cargo_pubs[cargo_count++] = payload;
        break;
    case SETTL_EVT_DELIVER_CONSTRUCTION_INPUT:
        if (payload_len < SETTL_CONSTRUCTION_INPUT_PAYLOAD_SIZE) {
            settlement_result_set(result, SETTL_IMPORT_REJECT_EVENT,
                                  event_index, UINT8_MAX, CARGO_RECEIPT_OK);
            return false;
        }
        cargo_count = payload[SETTL_CONSTRUCTION_INPUT_COUNT_OFFSET];
        if (cargo_count == 0 ||
            cargo_count > SETTL_MAX_CONSTRUCTION_INPUTS) {
            settlement_result_set(result, SETTL_IMPORT_REJECT_EVENT,
                                  event_index, UINT8_MAX, CARGO_RECEIPT_OK);
            return false;
        }
        for (uint8_t i = 0; i < cargo_count; i++)
            cargo_pubs[i] = payload + 64 + i * 32;
        break;
    default:
        if (evidence && evidence->cargo_count != 0) {
            settlement_result_set(result, SETTL_IMPORT_REJECT_EVIDENCE_COUNT,
                                  event_index, UINT8_MAX, CARGO_RECEIPT_OK);
            return false;
        }
        return true;
    }

    if (!evidence || evidence->cargo_count == 0 || !evidence->cargo) {
        settlement_result_set(result, SETTL_IMPORT_REJECT_MISSING_EVIDENCE,
                              event_index, UINT8_MAX, CARGO_RECEIPT_OK);
        return false;
    }
    if (evidence->cargo_count != cargo_count) {
        settlement_result_set(result, SETTL_IMPORT_REJECT_EVIDENCE_COUNT,
                              event_index, UINT8_MAX, CARGO_RECEIPT_OK);
        return false;
    }

    for (uint8_t i = 0; i < cargo_count; i++) {
        const settlement_cargo_trust_evidence_t *item =
            &evidence->cargo[i];
        if (item->event_id != hdr->event_id) {
            settlement_result_set(result, SETTL_IMPORT_REJECT_EVIDENCE_EVENT,
                                  event_index, i, CARGO_RECEIPT_OK);
            return false;
        }
        if (memcmp(item->cargo_pub, cargo_pubs[i], 32) != 0) {
            settlement_result_set(result, SETTL_IMPORT_REJECT_EVIDENCE_CARGO,
                                  event_index, i, CARGO_RECEIPT_OK);
            return false;
        }

        cargo_receipt_trust_result_t trust = cargo_receipt_trust_verify(
            item->receipt_chain, item->receipt_count, cargo_pubs[i],
            item->origin_present ? &item->origin : NULL,
            item->authority_trust);
        settlement_import_status_t status =
            settlement_status_from_trust(trust.status);
        if (status != SETTL_IMPORT_OK) {
            settlement_result_set(result, status, event_index, i,
                                  trust.chain_result);
            return false;
        }
    }
    return true;
}

bool settlement_apply_segment_trusted(
    settlement_state_t *s,
    const void *events_arg,
    const uint8_t **payloads,
    const uint16_t *payload_lens,
    const settlement_event_trust_evidence_t *evidence,
    uint32_t event_count,
    settlement_checkpoint_t *cp_out,
    settlement_import_result_t *result_out) {
    const chain_event_header_t *events = (const chain_event_header_t *)events_arg;
    settlement_result_set(result_out, SETTL_IMPORT_OK, UINT32_MAX,
                          UINT8_MAX, CARGO_RECEIPT_OK);
    if (!s || !cp_out ||
        (event_count > 0 && (!events || !payloads || !payload_lens))) {
        settlement_result_set(result_out, SETTL_IMPORT_REJECT_BAD_ARGUMENTS,
                              UINT32_MAX, UINT8_MAX, CARGO_RECEIPT_OK);
        return false;
    }

    for (uint32_t i = 0; i < event_count; i++) {
        const settlement_event_trust_evidence_t *event_evidence =
            evidence ? &evidence[i] : NULL;
        if (!settlement_preflight_event(
                &events[i], payloads[i], payload_lens[i], event_evidence,
                i, result_out))
            return false;
    }

    settlement_state_t next = *s;
    for (uint32_t i = 0; i < event_count; i++) {
        if (!settlement_apply_event_preflighted(
                &next, &events[i], payloads[i], payload_lens[i])) {
            settlement_result_set(result_out, SETTL_IMPORT_REJECT_EVENT,
                                  i, UINT8_MAX, CARGO_RECEIPT_OK);
            return false;
        }
        next.last_event_id = events[i].event_id;
    }

    settlement_checkpoint_t next_cp = {0};
    settlement_compute_root(s, next_cp.prev_segment_root);
    next.segment_index++;
    next_cp.segment_index = next.segment_index;
    next_cp.event_count = event_count;
    next_cp.last_event_id = next.last_event_id;
    settlement_compute_root(&next, next_cp.state_root);

    *s = next;
    *cp_out = next_cp;
    return true;
}

bool settlement_apply_segment(settlement_state_t *s,
                              const void *events_arg,
                              const uint8_t **payloads,
                              const uint16_t *payload_lens,
                              uint32_t event_count,
                              settlement_checkpoint_t *cp_out) {
    return settlement_apply_segment_trusted(
        s, events_arg, payloads, payload_lens, NULL, event_count, cp_out, NULL);
}
