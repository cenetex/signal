#include "settlement_engine.h"
#include "sha256.h"
#include "../server/chain_log.h"
#include <string.h>
#include <stdlib.h>

static int cmp_u8_32(const void *a, const void *b) {
    return memcmp(a, b, 32);
}

static int cmp_cargo_pub(const void *a, const void *b) {
    const cargo_unit_t *cargo_a = (const cargo_unit_t *)a;
    const cargo_unit_t *cargo_b = (const cargo_unit_t *)b;
    return memcmp(cargo_a->pub, cargo_b->pub, 32);
}

static int cmp_construction_key(const void *a, const void *b) {
    const settl_construction_site_t *site_a =
        (const settl_construction_site_t *)a;
    const settl_construction_site_t *site_b =
        (const settl_construction_site_t *)b;
    int order = memcmp(
        site_a->station_pubkey,
        site_b->station_pubkey, 32);
    if (order != 0) return order;
    order = memcmp(
        site_a->scaffold_id,
        site_b->scaffold_id, 32);
    if (order != 0) return order;
    return memcmp(site_a, site_b, sizeof(*site_a));
}

static int cmp_death_key(const void *a, const void *b) {
    const settl_death_record_t *death_a =
        (const settl_death_record_t *)a;
    const settl_death_record_t *death_b =
        (const settl_death_record_t *)b;
    int order = memcmp(
        death_a->player_pubkey,
        death_b->player_pubkey, 32);
    if (order != 0) return order;
    if (death_a->death_tick < death_b->death_tick)
        return -1;
    if (death_a->death_tick > death_b->death_tick)
        return 1;
    return memcmp(death_a, death_b, sizeof(*death_a));
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
        qsort(sorted, mc, sizeof(cargo_unit_t), cmp_cargo_pub);
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
        qsort(srt, s->construction_site_count, sizeof(*srt),
              cmp_construction_key);
        for (uint16_t i = 0; i < s->construction_site_count; i++)
            sha256_update(&ctx, &srt[i], sizeof(*srt));
    }
    {
        settl_death_record_t srt[SETTL_MAX_DEATH_RECORDS];
        memcpy(srt, s->death_records, s->death_record_count * sizeof(*srt));
        qsort(srt, s->death_record_count, sizeof(*srt),
              cmp_death_key);
        for (uint16_t i = 0; i < s->death_record_count; i++)
            sha256_update(&ctx, &srt[i], sizeof(*srt));
    }

    sha256_final(&ctx, root_out);
}

/* ---- event application ---- */

enum {
    SETTL_TRANSFER_PAYLOAD_SIZE = 144,
    SETTL_TRADE_PAYLOAD_SIZE = 112,
    SETTL_TRADE_KIND_OFFSET = 104,
    SETTL_TRADE_DIRECTION_OFFSET = 105,
    SETTL_CONSTRUCTION_PAYLOAD_SIZE = 168,
    SETTL_CONSTRUCTION_INPUTS_OFFSET = 64,
    SETTL_CONSTRUCTION_INPUT_COUNT_OFFSET = 160,
    SETTL_CONSTRUCTION_INPUT_MAX = 3,
};

static settlement_apply_result_t settlement_result_default(
    settlement_apply_status_t status,
    uint32_t event_index,
    uint8_t cargo_index) {
    settlement_apply_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = status;
    result.event_index = event_index;
    result.cargo_index = cargo_index;
    result.cargo_trust.status =
        CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS;
    result.cargo_trust.chain_result =
        CARGO_RECEIPT_REJECT_EMPTY;
    return result;
}

static void settlement_publish_result(
    settlement_apply_result_t *out,
    settlement_apply_status_t status,
    uint32_t event_index,
    uint8_t cargo_index,
    const cargo_receipt_trust_result_t *cargo_trust) {
    if (!out) return;
    *out = settlement_result_default(
        status, event_index, cargo_index);
    if (cargo_trust) out->cargo_trust = *cargo_trust;
}

const char *settlement_apply_status_name(
    settlement_apply_status_t status) {
    switch (status) {
        case SETTLEMENT_APPLY_OK:
            return "ok";
        case SETTLEMENT_APPLY_REJECT_BAD_ARGUMENTS:
            return "reject_bad_arguments";
        case SETTLEMENT_APPLY_REJECT_PAYLOAD_HASH:
            return "reject_payload_hash";
        case SETTLEMENT_APPLY_REJECT_MISSING_TRUST_EVIDENCE:
            return "reject_missing_trust_evidence";
        case SETTLEMENT_APPLY_REJECT_TRUST_EVIDENCE_COUNT:
            return "reject_trust_evidence_count";
        case SETTLEMENT_APPLY_REJECT_CARGO_TRUST:
            return "reject_cargo_trust";
        case SETTLEMENT_APPLY_REJECT_RECEIPT_HOLDER:
            return "reject_receipt_holder";
        case SETTLEMENT_APPLY_REJECT_RESOURCE:
            return "reject_resource";
        case SETTLEMENT_APPLY_REJECT_EVENT:
            return "reject_event";
        case SETTLEMENT_APPLY_STATUS_COUNT:
            break;
    }
    return "unknown";
}

static bool settlement_payload_hash_matches(
    const chain_event_header_t *hdr,
    const uint8_t *payload,
    uint16_t payload_len) {
    if (!hdr || (!payload && payload_len != 0)) return false;
    uint8_t actual[32];
    sha256_bytes(payload, payload_len, actual);
    return memcmp(actual, hdr->payload_hash, sizeof(actual)) == 0;
}

static bool settlement_bytes_are_zero(
    const uint8_t *bytes, size_t count) {
    if (!bytes) return false;
    for (size_t i = 0; i < count; i++) {
        if (bytes[i] != 0) return false;
    }
    return true;
}

/*
 * Return the exact number of cargo proofs required by this event, zero for an
 * event that is not provenance-sensitive, and -1 for a malformed sensitive
 * payload.  Payload hashes are checked by the caller before these offsets are
 * inspected.
 */
static int settlement_required_cargo_evidence(
    const chain_event_header_t *hdr,
    const uint8_t *payload,
    uint16_t payload_len) {
    if (!hdr) return -1;
    switch (hdr->type) {
        case 0x04: /* TRANSFER_CARGO */
            return payload &&
                       payload_len == SETTL_TRANSFER_PAYLOAD_SIZE &&
                       settlement_bytes_are_zero(
                           payload + 137, 7)
                ? 1
                : -1;
        case 0x11: /* SELL_COMMODITY */
            if (!payload ||
                payload_len != SETTL_TRADE_PAYLOAD_SIZE ||
                payload[SETTL_TRADE_DIRECTION_OFFSET] != 1 ||
                !settlement_bytes_are_zero(
                    payload + 106, 6)) {
                return -1;
            }
            return 1;
        case 0x21: { /* DELIVER_CONSTRUCTION_INPUT */
            if (!payload ||
                payload_len != SETTL_CONSTRUCTION_PAYLOAD_SIZE ||
                !settlement_bytes_are_zero(
                    payload + 164, 4)) {
                return -1;
            }
            uint8_t count =
                payload[SETTL_CONSTRUCTION_INPUT_COUNT_OFFSET];
            if (count == 0 ||
                count > SETTL_CONSTRUCTION_INPUT_MAX) {
                return -1;
            }
            for (uint8_t i = count;
                 i < SETTL_CONSTRUCTION_INPUT_MAX; i++) {
                if (!settlement_bytes_are_zero(
                        payload +
                        SETTL_CONSTRUCTION_INPUTS_OFFSET +
                        (size_t)i * 32u,
                        32)) {
                    return -1;
                }
            }
            return count;
        }
        default:
            return 0;
    }
}

static const uint8_t *settlement_expected_cargo_pub(
    const chain_event_header_t *hdr,
    const uint8_t *payload,
    uint8_t cargo_index) {
    if (hdr->type == 0x21) {
        return payload + SETTL_CONSTRUCTION_INPUTS_OFFSET +
            (size_t)cargo_index * 32u;
    }
    return payload;
}

static const uint8_t *settlement_expected_receipt_holder(
    const chain_event_header_t *hdr,
    const uint8_t *payload) {
    switch (hdr->type) {
        case 0x04: /* TRANSFER_CARGO: current holder is from_pubkey. */
        case 0x11: /* SELL_COMMODITY: current holder is player_pubkey. */
        case 0x21: /* Construction manifest belongs to station_pubkey. */
            return payload + 32;
        default:
            return NULL;
    }
}

static bool settlement_cargo_trust_accepted(
    cargo_receipt_trust_status_t status) {
    return status == CARGO_RECEIPT_TRUST_VALID_TRUSTED ||
           status ==
               CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED;
}

static bool settlement_cargo_matches_origin(
    const cargo_unit_t *cargo,
    const cargo_receipt_origin_proof_t *origin) {
    if (!cargo || !origin) return false;
    const cargo_unit_t *produced =
        &origin->output_cargo;
    return memcmp(cargo->pub, produced->pub, 32) == 0 &&
           cargo->kind == produced->kind &&
           cargo->commodity == produced->commodity &&
           cargo->grade == produced->grade &&
           cargo->prefix_class ==
               produced->prefix_class &&
           cargo->recipe_id == produced->recipe_id &&
           cargo->quantity == produced->quantity &&
           cargo->mined_block == produced->mined_block;
}

static bool settlement_preflight_event(
    const chain_event_header_t *hdr,
    const uint8_t *payload,
    uint16_t payload_len,
    const settlement_event_trust_evidence_t *evidence,
    uint32_t event_index,
    settlement_apply_result_t *result_out) {
    if (!settlement_payload_hash_matches(
            hdr, payload, payload_len)) {
        settlement_publish_result(
            result_out,
            SETTLEMENT_APPLY_REJECT_PAYLOAD_HASH,
            event_index, UINT8_MAX, NULL);
        return false;
    }

    int required = settlement_required_cargo_evidence(
        hdr, payload, payload_len);
    if (required < 0) {
        settlement_publish_result(
            result_out, SETTLEMENT_APPLY_REJECT_EVENT,
            event_index, UINT8_MAX, NULL);
        return false;
    }
    if (required == 0) {
        if (evidence && evidence->cargo_count != 0) {
            settlement_publish_result(
                result_out,
                SETTLEMENT_APPLY_REJECT_TRUST_EVIDENCE_COUNT,
                event_index, UINT8_MAX, NULL);
            return false;
        }
        return true;
    }
    if (!evidence || evidence->cargo_count == 0 ||
        !evidence->cargo) {
        settlement_publish_result(
            result_out,
            SETTLEMENT_APPLY_REJECT_MISSING_TRUST_EVIDENCE,
            event_index, UINT8_MAX, NULL);
        return false;
    }
    if (evidence->cargo_count != (uint8_t)required) {
        settlement_publish_result(
            result_out,
            SETTLEMENT_APPLY_REJECT_TRUST_EVIDENCE_COUNT,
            event_index, UINT8_MAX, NULL);
        return false;
    }

    for (uint8_t i = 0; i < evidence->cargo_count; i++) {
        const settlement_cargo_trust_evidence_t *cargo =
            &evidence->cargo[i];
        cargo_receipt_trust_result_t trust =
            cargo_receipt_trust_verify(
                cargo->receipt_chain,
                cargo->receipt_count,
                settlement_expected_cargo_pub(
                    hdr, payload, i),
                cargo->origin,
                cargo->authority_trust);
        if (!settlement_cargo_trust_accepted(
                trust.status)) {
            settlement_publish_result(
                result_out,
                SETTLEMENT_APPLY_REJECT_CARGO_TRUST,
                event_index, i, &trust);
            return false;
        }
        const uint8_t *expected_holder =
            settlement_expected_receipt_holder(
                hdr, payload);
        const cargo_receipt_t *last =
            &cargo->receipt_chain[
                cargo->receipt_count - 1u];
        if (!expected_holder ||
            memcmp(last->recipient_pubkey,
                   expected_holder, 32) != 0) {
            settlement_publish_result(
                result_out,
                SETTLEMENT_APPLY_REJECT_RECEIPT_HOLDER,
                event_index, i, &trust);
            return false;
        }
    }
    return true;
}

static bool settlement_apply_event_validated(
                            settlement_state_t *s,
                            const chain_event_header_t *hdr,
                            const uint8_t *payload,
                            uint16_t payload_len,
                            const settlement_event_trust_evidence_t *evidence) {
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

    case 0x04: { /* TRANSFER_CARGO */
        if (payload_len != SETTL_TRANSFER_PAYLOAD_SIZE ||
            !evidence || evidence->cargo_count != 1) {
            return false;
        }
        int idx = find_manifest_unit(manifest, *mc, payload);
        if (idx < 0 ||
            payload[96] >= CARGO_KIND_COUNT ||
            manifest[idx].kind != payload[96] ||
            !evidence->cargo ||
            !evidence->cargo[0].origin ||
            evidence->cargo[0].origin->output_cargo.kind !=
                payload[96] ||
            !settlement_cargo_matches_origin(
                &manifest[idx],
                evidence->cargo[0].origin)) {
            return false;
        }
        cargo_unit_t unit = manifest[idx];
        settlement_manifest_remove(manifest, mc, (uint16_t)idx);
        /* Add to dest (same station for now) */
        if (*mc >= SETTL_MAX_MANIFEST_UNITS) return false;
        manifest[(*mc)++] = unit;
        return true;
    }

    case 0x10: /* BUY */
    case 0x11: { /* SELL */
        if (payload_len != SETTL_TRADE_PAYLOAD_SIZE)
            return false;
        int64_t delta;
        memcpy(&delta, payload + 96, 8);
        uint8_t kind = payload[SETTL_TRADE_KIND_OFFSET];
        uint8_t dir =
            payload[SETTL_TRADE_DIRECTION_OFFSET];
        if (kind >= CARGO_KIND_COUNT ||
            (hdr->type == 0x10 && dir != 0) ||
            (hdr->type == 0x11 && dir != 1) ||
            !settlement_bytes_are_zero(
                payload + 106, 6)) {
            return false;
        }

        if (dir == 0) { /* BUY: station→player, remove from manifest */
            int idx = find_manifest_unit(manifest, *mc, payload);
            if (idx < 0 ||
                manifest[idx].kind != kind) {
                return false;
            }
            settlement_manifest_remove(manifest, mc, (uint16_t)idx);
            int li = find_ledger_entry(ledger, *lc, payload + 32);
            if (li >= 0) ledger[li].balance += (float)delta;
        } else { /* SELL: player→station, add to manifest */
            if (*mc >= SETTL_MAX_MANIFEST_UNITS ||
                find_manifest_unit(manifest, *mc, payload) >= 0 ||
                !evidence || evidence->cargo_count != 1 ||
                !evidence->cargo ||
                !evidence->cargo[0].origin) {
                return false;
            }
            cargo_unit_t *cu = &manifest[(*mc)++];
            *cu = evidence->cargo[0].origin->output_cargo;
            if (memcmp(cu->pub, payload, 32) != 0 ||
                cu->kind != kind) {
                (*mc)--;
                return false;
            }
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

    case 0x21: { /* DELIVER_CONSTRUCTION_INPUT */
        if (payload_len != SETTL_CONSTRUCTION_PAYLOAD_SIZE ||
            !evidence || !evidence->cargo) {
            return false;
        }
        uint8_t input_count =
            payload[SETTL_CONSTRUCTION_INPUT_COUNT_OFFSET];
        if (input_count == 0 ||
            input_count > SETTL_CONSTRUCTION_INPUT_MAX ||
            evidence->cargo_count != input_count) {
            return false;
        }
        int site_idx = -1;
        for (uint16_t i = 0;
             i < s->construction_site_count; i++) {
            if (memcmp(s->construction_sites[i].scaffold_id,
                       payload, 32) == 0 &&
                memcmp(s->construction_sites[i].station_pubkey,
                       payload + 32, 32) == 0 &&
                s->construction_sites[i].active) {
                site_idx = (int)i;
                break;
            }
        }
        if (site_idx < 0) return false;

        /*
         * Verify every named unit exists before consuming any of them.  The
         * segment snapshot is still the final atomicity guard, but this keeps
         * direct internal event application free of partial construction
         * consumption too.
         */
        for (uint8_t j = 0; j < input_count; j++) {
            const uint8_t *pub =
                payload + SETTL_CONSTRUCTION_INPUTS_OFFSET +
                (size_t)j * 32u;
            int idx = find_manifest_unit(manifest, *mc, pub);
            if (idx < 0 ||
                !settlement_cargo_matches_origin(
                    &manifest[idx],
                    evidence->cargo[j].origin)) {
                return false;
            }
            for (uint8_t prior = 0; prior < j; prior++) {
                const uint8_t *prior_pub =
                    payload +
                    SETTL_CONSTRUCTION_INPUTS_OFFSET +
                    (size_t)prior * 32u;
                if (memcmp(pub, prior_pub, 32) == 0)
                    return false;
            }
        }
        for (uint8_t j = 0; j < input_count; j++) {
            const uint8_t *pub =
                payload + SETTL_CONSTRUCTION_INPUTS_OFFSET +
                (size_t)j * 32u;
            int idx = find_manifest_unit(manifest, *mc, pub);
            if (idx < 0) return false;
            settlement_manifest_remove(
                manifest, mc, (uint16_t)idx);
        }

        settl_construction_site_t *site =
            &s->construction_sites[site_idx];
        site->build_progress += 0.1f * input_count;
        if (site->build_progress > 1.0f)
            site->build_progress = 1.0f;
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
    const chain_event_header_t *hdr =
        (const chain_event_header_t *)hdr_arg;
    if (!s || !hdr || (!payload && payload_len != 0)) {
        return false;
    }
    if (!settlement_preflight_event(
            hdr, payload, payload_len, NULL, 0, NULL)) {
        return false;
    }
    return settlement_apply_event_validated(
        s, hdr, payload, payload_len, NULL);
}

bool settlement_apply_segment_trusted(
    settlement_state_t *s,
    const void *events_arg,
    const uint8_t **payloads,
    const uint16_t *payload_lens,
    const settlement_event_trust_evidence_t *trust_evidence,
    uint32_t event_count,
    settlement_checkpoint_t *cp_out,
    settlement_apply_result_t *result_out) {
    if (!s || !cp_out ||
        (event_count != 0 &&
         (!events_arg || !payloads || !payload_lens))) {
        settlement_publish_result(
            result_out,
            SETTLEMENT_APPLY_REJECT_BAD_ARGUMENTS,
            UINT32_MAX, UINT8_MAX, NULL);
        return false;
    }
    const chain_event_header_t *events =
        (const chain_event_header_t *)events_arg;

    /*
     * Trust and payload preflight is deliberately complete before the first
     * mutation.  A valid proof cannot smuggle a different payload because the
     * signed header hash is checked first, and each proof is then bound to the
     * exact cargo pubkey encoded by that payload.
     */
    for (uint32_t i = 0; i < event_count; i++) {
        if (!payloads[i] && payload_lens[i] != 0) {
            settlement_publish_result(
                result_out,
                SETTLEMENT_APPLY_REJECT_BAD_ARGUMENTS,
                i, UINT8_MAX, NULL);
            return false;
        }
        const settlement_event_trust_evidence_t *event_evidence =
            trust_evidence ? &trust_evidence[i] : NULL;
        if (!settlement_preflight_event(
                &events[i], payloads[i],
                payload_lens[i], event_evidence,
                i, result_out)) {
            return false;
        }
    }

    settlement_state_t *next =
        (settlement_state_t *)malloc(sizeof(*next));
    if (!next) {
        settlement_publish_result(
            result_out,
            SETTLEMENT_APPLY_REJECT_RESOURCE,
            UINT32_MAX, UINT8_MAX, NULL);
        return false;
    }
    *next = *s;
    uint32_t applied = 0;

    for (uint32_t i = 0; i < event_count; i++) {
        const settlement_event_trust_evidence_t *event_evidence =
            trust_evidence ? &trust_evidence[i] : NULL;
        if (!settlement_apply_event_validated(
                next, &events[i], payloads[i],
                payload_lens[i], event_evidence)) {
            free(next);
            settlement_publish_result(
                result_out, SETTLEMENT_APPLY_REJECT_EVENT,
                i, UINT8_MAX, NULL);
            return false;
        }
        applied++;
        next->last_event_id = events[i].event_id;
    }

    next->segment_index++;
    settlement_checkpoint_t checkpoint;
    memset(&checkpoint, 0, sizeof(checkpoint));
    checkpoint.segment_index = next->segment_index;
    checkpoint.event_count = applied;
    checkpoint.last_event_id = next->last_event_id;
    settlement_compute_root(
        s, checkpoint.prev_segment_root);
    settlement_compute_root(next, checkpoint.state_root);
    *s = *next;
    free(next);
    *cp_out = checkpoint;
    settlement_publish_result(
        result_out, SETTLEMENT_APPLY_OK,
        UINT32_MAX, UINT8_MAX, NULL);
    return true;
}

bool settlement_apply_segment(settlement_state_t *s,
                              const void *events_arg,
                              const uint8_t **payloads,
                              const uint16_t *payload_lens,
                              uint32_t event_count,
                              settlement_checkpoint_t *cp_out) {
    return settlement_apply_segment_trusted(
        s, events_arg, payloads, payload_lens,
        NULL, event_count, cp_out, NULL);
}
