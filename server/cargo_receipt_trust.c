#include "cargo_receipt_trust.h"

#include "station_authority.h"

#include <string.h>

typedef struct {
    cargo_receipt_authority_trust_t trust;
    int station_index;
} authority_resolution_t;

static int trust_precedence(cargo_receipt_authority_trust_t trust) {
    switch (trust) {
        case CARGO_RECEIPT_AUTHORITY_REVOKED: return 5;
        case CARGO_RECEIPT_AUTHORITY_UNTRUSTED: return 4;
        case CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT: return 3;
        case CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED: return 2;
        case CARGO_RECEIPT_AUTHORITY_UNKNOWN: return 1;
        default: return 0;
    }
}

static authority_resolution_t resolve_authority(
    const world_t *world,
    int evaluating_station,
    const uint8_t pubkey[32]) {
    authority_resolution_t out = {
        .trust = CARGO_RECEIPT_AUTHORITY_UNKNOWN,
        .station_index = -1,
    };
    if (!world || !pubkey || evaluating_station < 0 ||
        evaluating_station >= MAX_STATIONS) {
        return out;
    }

    const station_t *viewer = &world->stations[evaluating_station];
    cargo_receipt_authority_trust_t local =
        station_authority_trust_for_pubkey(viewer, pubkey);
    if (local == CARGO_RECEIPT_AUTHORITY_REVOKED ||
        local == CARGO_RECEIPT_AUTHORITY_UNTRUSTED) {
        out.trust = local;
        return out;
    }

    int station_count = world->station_count;
    if (station_count < 0) station_count = 0;
    if (station_count > MAX_STATIONS) station_count = MAX_STATIONS;
    for (int i = 0; i < station_count; i++) {
        const station_t *candidate = &world->stations[i];
        if (!station_exists(candidate)) continue;
        cargo_receipt_authority_trust_t trust =
            station_authority_trust_for_pubkey(candidate, pubkey);
        if (trust_precedence(trust) > trust_precedence(out.trust)) {
            out.trust = trust;
            out.station_index = i;
        }
        if (trust == CARGO_RECEIPT_AUTHORITY_REVOKED) break;
    }
    return out;
}

static cargo_receipt_trust_result_t empty_chain_trust_result(void) {
    return (cargo_receipt_trust_result_t){
        .status = CARGO_RECEIPT_TRUST_REJECT_CHAIN,
        .chain_result = CARGO_RECEIPT_REJECT_EMPTY,
        .origin_event = CARGO_RECEIPT_ORIGIN_EVENT_NONE,
        .authority_trust = CARGO_RECEIPT_AUTHORITY_UNKNOWN,
    };
}

static bool authority_policy_accepts(
    cargo_receipt_authority_trust_t trust,
    bool screens,
    bool tolerates) {
    switch (trust) {
        case CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT:
        case CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED:
            return true;
        case CARGO_RECEIPT_AUTHORITY_UNKNOWN:
            return !screens || tolerates;
        case CARGO_RECEIPT_AUTHORITY_UNTRUSTED:
            return tolerates;
        case CARGO_RECEIPT_AUTHORITY_REVOKED:
        default:
            return false;
    }
}

static cargo_receipt_trust_status_t rejection_for_authority(
    cargo_receipt_authority_trust_t trust) {
    switch (trust) {
        case CARGO_RECEIPT_AUTHORITY_REVOKED:
            return CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY;
        case CARGO_RECEIPT_AUTHORITY_UNTRUSTED:
            return CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY;
        case CARGO_RECEIPT_AUTHORITY_UNKNOWN:
            return CARGO_RECEIPT_TRUST_REJECT_UNKNOWN_AUTHORITY;
        default:
            return CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS;
    }
}

static cargo_legality_result_t legality_base(
    const station_t *viewer,
    int evaluating_station) {
    cargo_legality_result_t out = {
        .status = CARGO_LEGALITY_CLEAN,
        .receipt_result = CARGO_RECEIPT_OK,
        .origin_station = -1,
        .black_market_station = -1,
    };
    if (cargo_legality_station_screens(viewer, evaluating_station))
        out.reasons |= CARGO_LEGALITY_REASON_POLICY_SCREENS;
    if (cargo_legality_station_tolerates_contraband(
            viewer, evaluating_station)) {
        out.reasons |= CARGO_LEGALITY_REASON_POLICY_TOLERATES;
    }
    return out;
}

static void mark_authority_legality(
    cargo_legality_result_t *legality,
    cargo_receipt_authority_trust_t trust,
    bool screens) {
    if (!legality) return;
    if (trust == CARGO_RECEIPT_AUTHORITY_UNKNOWN ||
        trust == CARGO_RECEIPT_AUTHORITY_UNTRUSTED) {
        legality->reasons |= CARGO_LEGALITY_REASON_UNKNOWN_AUTHORITY;
        legality->status = screens ? CARGO_LEGALITY_CONTRABAND
                                   : CARGO_LEGALITY_SUSPICIOUS;
    } else if (trust == CARGO_RECEIPT_AUTHORITY_REVOKED) {
        legality->reasons |= CARGO_LEGALITY_REASON_RECEIPT_REJECTED;
        legality->status = CARGO_LEGALITY_CONTRABAND;
    }
}

cargo_receipt_station_evaluation_t cargo_receipt_evaluate_at_station(
    const world_t *world,
    int evaluating_station,
    const cargo_unit_t *unit,
    const cargo_receipt_chain_t *chain) {
    cargo_receipt_station_evaluation_t out = {
        .accepted = false,
        .trust = {
            .status = CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS,
            .chain_result = CARGO_RECEIPT_OK,
            .origin_event = CARGO_RECEIPT_ORIGIN_EVENT_NONE,
            .authority_trust = CARGO_RECEIPT_AUTHORITY_UNKNOWN,
        },
        .origin_status = CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS,
        .legality = {
            .status = CARGO_LEGALITY_SUSPICIOUS,
            .receipt_result = CARGO_RECEIPT_OK,
            .origin_station = -1,
            .black_market_station = -1,
        },
        .origin_station = -1,
        .first_rejected_link = -1,
    };
    if (!world || !unit || evaluating_station < 0 ||
        evaluating_station >= world->station_count ||
        evaluating_station >= MAX_STATIONS ||
        !station_exists(&world->stations[evaluating_station])) {
        return out;
    }

    const station_t *viewer = &world->stations[evaluating_station];
    bool screens = cargo_legality_station_screens(
        viewer, evaluating_station);
    bool tolerates = cargo_legality_station_tolerates_contraband(
        viewer, evaluating_station);
    out.legality = legality_base(viewer, evaluating_station);

    if (!chain || chain->len == 0) {
        cargo_receipt_origin_proof_t origin = {0};
        out.trust = empty_chain_trust_result();
        /* Frozen save-migration cargo predates portable receipts and signed
         * production events by definition. Do not scan and re-verify a
         * station's growing history for a transform that this recipe cannot
         * have. Existing saves grandfather it at every station, while a
         * revoked hinted origin still fails closed. */
        if (unit->recipe_id == (uint16_t)RECIPE_LEGACY_MIGRATE) {
            cargo_receipt_authority_trust_t hinted_trust =
                CARGO_RECEIPT_AUTHORITY_UNKNOWN;
            out.origin_status =
                CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND;
            out.legality.reasons |=
                CARGO_LEGALITY_REASON_MISSING_RECEIPT |
                CARGO_LEGALITY_REASON_UNKNOWN_AUTHORITY;
            out.legality.status = screens
                ? CARGO_LEGALITY_CONTRABAND
                : CARGO_LEGALITY_SUSPICIOUS;
            if (unit->origin_station < world->station_count &&
                unit->origin_station < MAX_STATIONS &&
                station_exists(&world->stations[unit->origin_station])) {
                authority_resolution_t hinted = resolve_authority(
                    world, evaluating_station,
                    world->stations[unit->origin_station].station_pubkey);
                if (hinted.trust == CARGO_RECEIPT_AUTHORITY_REVOKED)
                    hinted_trust = hinted.trust;
                out.origin_station = unit->origin_station;
                out.legality.origin_station = unit->origin_station;
            }
            out.trust.status = rejection_for_authority(hinted_trust);
            out.trust.authority_trust = hinted_trust;
            out.accepted =
                hinted_trust != CARGO_RECEIPT_AUTHORITY_REVOKED;
            return out;
        }
        out.origin_status = cargo_receipt_resolve_local_origin(
            viewer, unit->pub, &origin);
        if (out.origin_status != CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED) {
            authority_resolution_t hinted = {
                .trust = CARGO_RECEIPT_AUTHORITY_UNKNOWN,
                .station_index = -1,
            };
            if (unit->origin_station < world->station_count &&
                unit->origin_station < MAX_STATIONS &&
                station_exists(&world->stations[unit->origin_station])) {
                const station_t *hinted_station =
                    &world->stations[unit->origin_station];
                out.origin_status =
                    cargo_receipt_resolve_origin_for_authority(
                        hinted_station->station_pubkey,
                        unit->pub, &origin);
                if (out.origin_status ==
                    CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED) {
                    hinted = resolve_authority(
                        world, evaluating_station,
                        hinted_station->station_pubkey);
                    out.local_origin_without_receipt = true;
                    out.origin_station = unit->origin_station;
                    out.legality.origin_station = unit->origin_station;
                    out.trust = (cargo_receipt_trust_result_t){
                        .status = hinted.trust ==
                                CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED
                            ? CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED
                            : hinted.trust ==
                                CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT
                                ? CARGO_RECEIPT_TRUST_VALID_TRUSTED
                                : rejection_for_authority(hinted.trust),
                        .chain_result = CARGO_RECEIPT_OK,
                        .origin_event = origin.event_type,
                        .authority_trust = hinted.trust,
                    };
                    out.legality.reasons |=
                        CARGO_LEGALITY_REASON_MISSING_RECEIPT;
                    out.legality.status = screens
                        ? CARGO_LEGALITY_CONTRABAND
                        : CARGO_LEGALITY_SUSPICIOUS;
                    mark_authority_legality(
                        &out.legality, hinted.trust, screens);
                    if (!authority_policy_accepts(
                            hinted.trust, screens, tolerates)) {
                        return out;
                    }
                    goto origin_resolved_without_receipt;
                }
            }
            out.trust.status =
                CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN;
            out.legality.reasons |= CARGO_LEGALITY_REASON_MISSING_RECEIPT;
            out.legality.status = screens
                ? CARGO_LEGALITY_CONTRABAND
                : CARGO_LEGALITY_SUSPICIOUS;
            return out;
        }
        out.local_origin_without_receipt = true;
        out.origin_station = evaluating_station;
        out.legality.origin_station = evaluating_station;
        out.trust = (cargo_receipt_trust_result_t){
            .status = CARGO_RECEIPT_TRUST_VALID_TRUSTED,
            .chain_result = CARGO_RECEIPT_OK,
            .origin_event = origin.event_type,
            .authority_trust =
                CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        };
origin_resolved_without_receipt:
        ;
    } else {
        const cargo_receipt_t *origin_receipt = &chain->links[0];
        cargo_receipt_origin_proof_t origin = {0};
        out.origin_status = cargo_receipt_resolve_origin_for_authority(
            origin_receipt->authoring_station, unit->pub, &origin);
        authority_resolution_t origin_authority = resolve_authority(
            world, evaluating_station, origin_receipt->authoring_station);
        out.origin_station = origin_authority.station_index;
        out.legality.origin_station = origin_authority.station_index;
        out.trust = cargo_receipt_trust_verify(
            chain->links, chain->len, unit->pub,
            out.origin_status == CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED
                ? &origin : NULL,
            origin_authority.trust);
        out.legality.receipt_result = out.trust.chain_result;
        if (out.trust.chain_result != CARGO_RECEIPT_OK) {
            out.legality.reasons |= cargo_legality_reason_from_receipt_result(
                out.trust.chain_result);
            out.legality.status = CARGO_LEGALITY_CONTRABAND;
            return out;
        }
        if (out.origin_status != CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED) {
            out.legality.reasons |= CARGO_LEGALITY_REASON_RECEIPT_REJECTED;
            out.legality.status = CARGO_LEGALITY_CONTRABAND;
            return out;
        }
        mark_authority_legality(&out.legality, origin_authority.trust,
                                screens);
        if (!authority_policy_accepts(origin_authority.trust,
                                      screens, tolerates)) {
            out.trust.status = rejection_for_authority(
                origin_authority.trust);
            out.first_rejected_link = 0;
            return out;
        }

        for (uint8_t i = 0; i < chain->len; i++) {
            authority_resolution_t author = resolve_authority(
                world, evaluating_station,
                chain->links[i].authoring_station);
            mark_authority_legality(&out.legality, author.trust, screens);
            if (author.station_index >= 0 &&
                cargo_legality_station_tolerates_contraband(
                    &world->stations[author.station_index],
                    author.station_index)) {
                out.legality.reasons |=
                    CARGO_LEGALITY_REASON_BLACK_MARKET_AUTHORITY;
                out.legality.black_market_station =
                    author.station_index;
                out.legality.status = CARGO_LEGALITY_CONTRABAND;
            }
            if (!authority_policy_accepts(author.trust,
                                          screens, tolerates)) {
                out.trust.status = rejection_for_authority(author.trust);
                out.trust.authority_trust = author.trust;
                out.first_rejected_link = (int)i;
                return out;
            }
        }
    }

    if (out.origin_station >= 0 && out.origin_station < 64) {
        uint64_t forbidden =
            station_policy_forbidden_origin_mask_for_station(
                viewer, evaluating_station,
                (commodity_t)unit->commodity);
        if ((forbidden & (1ULL << out.origin_station)) != 0) {
            out.legality.reasons |= CARGO_LEGALITY_REASON_BANNED_ORIGIN;
            out.legality.status = CARGO_LEGALITY_CONTRABAND;
        }
    }
    out.accepted = cargo_legality_station_accepts(out.legality);
    return out;
}
