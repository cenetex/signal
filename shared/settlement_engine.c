#include "settlement_engine.h"
#include "sha256.h"
#include "../server/chain_log.h"
#include <string.h>
#include <stdlib.h>

static int cmp_u8_32(const void *a, const void *b) {
    return memcmp(a, b, 32);
}

static int find_manifest_unit(const cargo_unit_t *m, uint16_t count,
                               const uint8_t pub[32]) {
    for (uint16_t i = 0; i < count; i++)
        if (memcmp(m[i].pub, pub, 32) == 0) return (int)i;
    return -1;
}

static void manifest_remove(cargo_unit_t *m, uint16_t *count, uint16_t idx) {
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

bool settlement_apply_event(settlement_state_t *s,
                            const void *hdr_arg,
                            const uint8_t *payload,
                            uint16_t payload_len) {
    const chain_event_header_t *hdr = (const chain_event_header_t *)hdr_arg;
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
            manifest_remove(manifest, mc, (uint16_t)idx);
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
        if (payload_len < 96) return false;
        int idx = find_manifest_unit(manifest, *mc, payload);
        if (idx < 0) return false;
        cargo_unit_t unit = manifest[idx];
        manifest_remove(manifest, mc, (uint16_t)idx);
        /* Add to dest (same station for now) */
        if (*mc >= SETTL_MAX_MANIFEST_UNITS) return false;
        manifest[(*mc)++] = unit;
        return true;
    }

    case 0x10: /* BUY */
    case 0x11: { /* SELL */
        if (payload_len < 112) return false;
        int64_t delta;
        memcpy(&delta, payload + 96, 8);
        uint8_t dir = payload[104];

        if (dir == 0) { /* BUY: station→player, remove from manifest */
            int idx = find_manifest_unit(manifest, *mc, payload);
            if (idx < 0) return false;
            manifest_remove(manifest, mc, (uint16_t)idx);
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

    case 0x21: { /* DELIVER_CONSTRUCTION_INPUT */
        if (payload_len < 168) return false;
        for (uint16_t i = 0; i < s->construction_site_count; i++)
            if (memcmp(s->construction_sites[i].scaffold_id, payload, 32) == 0) {
                s->construction_sites[i].build_progress += 0.1f;
                if (s->construction_sites[i].build_progress > 1.0f)
                    s->construction_sites[i].build_progress = 1.0f;
                return true;
            }
        return false;
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

bool settlement_apply_segment(settlement_state_t *s,
                              const void *events_arg,
                              const uint8_t **payloads,
                              const uint16_t *payload_lens,
                              uint32_t event_count,
                              settlement_checkpoint_t *cp_out) {
    const chain_event_header_t *events = (const chain_event_header_t *)events_arg;
    settlement_state_t snapshot = *s;
    uint32_t applied = 0;

    for (uint32_t i = 0; i < event_count; i++) {
        if (!settlement_apply_event(s, &events[i], payloads[i], payload_lens[i])) {
            *s = snapshot;
            return false;
        }
        applied++;
        s->last_event_id = events[i].event_id;
    }

    s->segment_index++;
    memset(cp_out, 0, sizeof(*cp_out));
    cp_out->segment_index = s->segment_index;
    cp_out->event_count = applied;
    cp_out->last_event_id = s->last_event_id;
    settlement_compute_root(s, cp_out->state_root);
    return true;
}
