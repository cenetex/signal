#include "cargo_receipt_trust.h"

#include "manifest.h"
#include "station_authority.h"

#include <string.h>

typedef struct {
    cargo_receipt_authority_trust_t trust;
    int station_index;
} authority_resolution_t;

static bool pubkey_is_zero(const uint8_t pubkey[32]) {
    static const uint8_t zero[32] = {0};
    return !pubkey || memcmp(pubkey, zero, sizeof(zero)) == 0;
}

static bool cargo_origin_metadata_matches(
    const cargo_receipt_origin_proof_t *origin,
    int origin_station,
    const cargo_unit_t *unit) {
    static const uint8_t zero32[32] = {0};
    if (!origin || !unit ||
        origin_station < 0 || origin_station > UINT8_MAX ||
        origin->output_semantics_version !=
            CARGO_RECEIPT_ORIGIN_SEMANTICS_V1 ||
        memcmp(origin->output_cargo_pub,
               origin->output_cargo.pub, 32) != 0 ||
        memcmp(origin->output_cargo.pub, zero32, 32) == 0 ||
        memcmp(origin->output_cargo.pub, unit->pub, 32) != 0 ||
        (unsigned)origin->output_cargo.grade >=
            (unsigned)MINING_GRADE_COUNT ||
        origin->output_cargo.quantity == 0u) {
        return false;
    }

    const cargo_unit_t *signed_output = &origin->output_cargo;
    cargo_kind_t commodity_kind;
    if (!cargo_kind_for_commodity(
            (commodity_t)signed_output->commodity,
            &commodity_kind) ||
        commodity_kind != (cargo_kind_t)signed_output->kind) {
        return false;
    }
    if (origin->event_type == CARGO_RECEIPT_ORIGIN_EVENT_SMELT) {
        if (signed_output->kind != (uint8_t)CARGO_KIND_INGOT ||
            signed_output->recipe_id != (uint16_t)RECIPE_SMELT ||
            signed_output->quantity != 1u ||
            (unsigned)signed_output->prefix_class >=
                (unsigned)INGOT_PREFIX_COUNT ||
            signed_output->prefix_class !=
                (uint8_t)mining_pubkey_class(signed_output->pub) ||
            memcmp(signed_output->parent_merkle,
                   zero32, 32) == 0) {
            return false;
        }
    } else if (origin->event_type ==
               CARGO_RECEIPT_ORIGIN_EVENT_CRAFT) {
        if (signed_output->prefix_class !=
                (uint8_t)INGOT_PREFIX_ANONYMOUS ||
            signed_output->mined_block != 0u ||
            signed_output->recipe_id !=
                origin->craft_recipe_id) {
            return false;
        }
        if (signed_output->recipe_id ==
            (uint16_t)RECIPE_LEGACY_MIGRATE) {
            if (origin->craft_input_count != 0u ||
                memcmp(signed_output->parent_merkle,
                       zero32, 32) != 0) {
                return false;
            }
        } else {
            const recipe_def_t *recipe =
                recipe_get(
                    (recipe_id_t)signed_output->recipe_id);
            if (!recipe ||
                origin->craft_input_count !=
                    recipe->input_count ||
                signed_output->kind !=
                    (uint8_t)recipe->output_kind ||
                signed_output->commodity !=
                    (uint8_t)recipe->output_commodity ||
                memcmp(signed_output->parent_merkle,
                       zero32, 32) == 0) {
                return false;
            }
        }
    } else {
        return false;
    }

    cargo_unit_t expected = *signed_output;
    expected.origin_station = (uint8_t)origin_station;
    uint8_t expected_wire[CARGO_UNIT_WIRE_SIZE];
    uint8_t presented_wire[CARGO_UNIT_WIRE_SIZE];
    cargo_unit_wire_pack(&expected, expected_wire);
    cargo_unit_wire_pack(unit, presented_wire);
    return memcmp(expected_wire, presented_wire,
                  sizeof(expected_wire)) == 0;
}

static void reject_origin_metadata(
    cargo_receipt_station_evaluation_t *out,
    bool has_receipt) {
    if (!out) return;
    out->trust.status =
        CARGO_RECEIPT_TRUST_REJECT_ORIGIN_METADATA;
    out->legality.reasons |=
        CARGO_LEGALITY_REASON_RECEIPT_REJECTED;
    out->legality.status = CARGO_LEGALITY_CONTRABAND;
    if (has_receipt) out->first_rejected_link = 0;
}

static void mark_craft_provenance(
    cargo_receipt_station_evaluation_t *out,
    const cargo_receipt_origin_proof_t *origin) {
    if (!out || !origin ||
        origin->event_type !=
            CARGO_RECEIPT_ORIGIN_EVENT_CRAFT ||
        origin->output_semantics_version !=
            CARGO_RECEIPT_ORIGIN_SEMANTICS_V1) {
        return;
    }
    out->craft_provenance =
        CARGO_CRAFT_PROVENANCE_STATION_ATTESTED_V1;
    /*
     * CRAFT V1 has no origin/custody/consumption proof for its inputs.
     * Keep both fields explicit even though zero-initialization would also
     * make them false.
     */
    out->craft_input_lineage_proven = false;
    out->craft_conservation_proven = false;
}

static bool registry_contains_authority(
    const station_t *station,
    const uint8_t pubkey[32],
    bool *out_has_lifecycle) {
    if (out_has_lifecycle) *out_has_lifecycle = false;
    if (!station || !pubkey ||
        station->authority_registry_count >
            STATION_AUTHORITY_REGISTRY_CAP) {
        return false;
    }
    for (uint8_t i = 0; i < station->authority_registry_count; i++) {
        const station_authority_record_t *record =
            &station->authority_registry[i];
        if (memcmp(record->pubkey, pubkey, 32) != 0) continue;
        if (out_has_lifecycle) {
            *out_has_lifecycle =
                record->lifecycle !=
                STATION_AUTHORITY_LIFECYCLE_UNSPECIFIED;
        }
        return true;
    }
    return false;
}

/*
 * Resolve ownership and policy separately. The evaluating station's explicit
 * deny rows override federation discovery. A key with lifecycle may belong to
 * only one station; duplicate ownership or a matching malformed registry is
 * treated as revoked rather than guessed.
 */
static authority_resolution_t resolve_authority(
    const world_t *world,
    int evaluating_station,
    const uint8_t pubkey[32]) {
    authority_resolution_t out = {
        .trust = CARGO_RECEIPT_AUTHORITY_UNKNOWN,
        .station_index = -1,
    };
    if (!world || pubkey_is_zero(pubkey) ||
        evaluating_station < 0 ||
        evaluating_station >= world->station_count ||
        evaluating_station >= MAX_STATIONS) {
        return out;
    }

    const station_t *viewer = &world->stations[evaluating_station];
    cargo_receipt_authority_trust_t viewer_trust =
        station_authority_trust_for_pubkey(viewer, pubkey);
    bool viewer_override =
        viewer_trust == CARGO_RECEIPT_AUTHORITY_REVOKED ||
        viewer_trust == CARGO_RECEIPT_AUTHORITY_UNTRUSTED;

    int station_count = world->station_count;
    if (station_count > MAX_STATIONS) station_count = MAX_STATIONS;
    for (int i = 0; i < station_count; i++) {
        const station_t *candidate = &world->stations[i];
        if (!station_exists(candidate)) continue;
        bool has_lifecycle = false;
        bool contains = registry_contains_authority(
            candidate, pubkey, &has_lifecycle);
        if (!contains) continue;
        if (!station_authority_registry_validate(candidate)) {
            out.trust = CARGO_RECEIPT_AUTHORITY_REVOKED;
            out.station_index = -1;
            return out;
        }
        /* Deny-only rows do not assert ownership. */
        if (!has_lifecycle) continue;
        if (out.station_index >= 0 && out.station_index != i) {
            out.trust = CARGO_RECEIPT_AUTHORITY_REVOKED;
            out.station_index = -1;
            return out;
        }
        out.station_index = i;
        out.trust = station_authority_trust_for_pubkey(
            candidate, pubkey);
    }
    if (viewer_override) out.trust = viewer_trust;
    return out;
}

static cargo_receipt_trust_result_t empty_chain_trust_result(void) {
    return (cargo_receipt_trust_result_t){
        .status = CARGO_RECEIPT_TRUST_REJECT_CHAIN,
        .chain_checked = false,
        .chain_result = CARGO_RECEIPT_REJECT_EMPTY,
        .origin_event = CARGO_RECEIPT_ORIGIN_EVENT_NONE,
        .authority_lifecycle =
            CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED,
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

static cargo_receipt_trust_status_t valid_status_for_authority(
    cargo_receipt_authority_trust_t trust) {
    if (trust == CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT)
        return CARGO_RECEIPT_TRUST_VALID_TRUSTED;
    if (trust == CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED)
        return CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED;
    return rejection_for_authority(trust);
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
        if (legality->status != CARGO_LEGALITY_CONTRABAND) {
            legality->status = screens ? CARGO_LEGALITY_CONTRABAND
                                       : CARGO_LEGALITY_SUSPICIOUS;
        }
    } else if (trust == CARGO_RECEIPT_AUTHORITY_REVOKED) {
        legality->reasons |= CARGO_LEGALITY_REASON_RECEIPT_REJECTED;
        legality->status = CARGO_LEGALITY_CONTRABAND;
    }
}

static void set_resolved_chainless_trust(
    cargo_receipt_station_evaluation_t *out,
    const cargo_receipt_origin_proof_t *origin,
    authority_resolution_t authority) {
    out->trust.status = valid_status_for_authority(authority.trust);
    out->trust.chain_checked = false;
    out->trust.chain_result = CARGO_RECEIPT_REJECT_EMPTY;
    out->trust.origin_event = origin->event_type;
    out->trust.authority_lifecycle = origin->authority_lifecycle;
    out->trust.authority_trust = authority.trust;
}

static cargo_receipt_station_evaluation_t
cargo_receipt_station_evaluation_default(void) {
    return (cargo_receipt_station_evaluation_t){
        .accepted = false,
        .local_origin_without_receipt = false,
        .trust = {
            .status = CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS,
            .chain_checked = false,
            .chain_result = CARGO_RECEIPT_REJECT_EMPTY,
            .origin_event = CARGO_RECEIPT_ORIGIN_EVENT_NONE,
            .authority_lifecycle =
                CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED,
            .authority_trust = CARGO_RECEIPT_AUTHORITY_UNKNOWN,
        },
        .origin_status = CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS,
        .legality = {
            .status = CARGO_LEGALITY_SUSPICIOUS,
            .receipt_result = CARGO_RECEIPT_REJECT_EMPTY,
            .origin_station = -1,
            .black_market_station = -1,
        },
        .origin_station = -1,
        .first_rejected_link = -1,
        .craft_provenance =
            CARGO_CRAFT_PROVENANCE_NOT_CRAFT,
        .craft_input_lineage_proven = false,
        .craft_conservation_proven = false,
    };
}

cargo_receipt_station_evaluation_t cargo_receipt_evaluate_at_station(
    const world_t *world,
    int evaluating_station,
    const cargo_unit_t *unit,
    const cargo_receipt_chain_t *chain) {
    cargo_receipt_station_evaluation_t out =
        cargo_receipt_station_evaluation_default();
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
        out.origin_status = cargo_receipt_resolve_local_origin(
            viewer, unit->pub, &origin);
        if (out.origin_status ==
            CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED) {
            authority_resolution_t authority = resolve_authority(
                world, evaluating_station, origin.authority);
            out.local_origin_without_receipt = true;
            out.origin_station = evaluating_station;
            out.legality.origin_station = evaluating_station;
            set_resolved_chainless_trust(&out, &origin, authority);
            if (!cargo_origin_metadata_matches(
                    &origin, evaluating_station, unit)) {
                out.local_origin_without_receipt = false;
                reject_origin_metadata(&out, false);
                return out;
            }
            mark_craft_provenance(&out, &origin);
            mark_authority_legality(
                &out.legality, authority.trust, screens);
            if (!authority_policy_accepts(
                    authority.trust, screens, tolerates)) {
                return out;
            }
        } else {
            out.trust.status =
                CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN;
            out.legality.reasons |=
                CARGO_LEGALITY_REASON_MISSING_RECEIPT;
            out.legality.status = CARGO_LEGALITY_CONTRABAND;
            return out;
        }
    } else {
        const cargo_receipt_t *origin_receipt = &chain->links[0];
        cargo_receipt_origin_proof_t origin = {0};
        authority_resolution_t origin_authority = resolve_authority(
            world, evaluating_station,
            origin_receipt->authoring_station);
        out.origin_station = origin_authority.station_index;
        out.legality.origin_station = origin_authority.station_index;
        if (origin_authority.station_index >= 0) {
            out.origin_status =
                cargo_receipt_resolve_origin_for_authority_pinned(
                    &world->stations[origin_authority.station_index],
                    origin_receipt->authoring_station,
                    unit->pub,
                    origin_receipt->prev_receipt_hash,
                    &origin);
        } else {
            out.origin_status =
                CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND;
        }
        out.trust = cargo_receipt_trust_verify(
            chain->links, chain->len, unit->pub,
            out.origin_status == CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED
                ? &origin : NULL,
            origin_authority.trust);
        out.legality.receipt_result = out.trust.chain_result;
        if (out.trust.chain_result != CARGO_RECEIPT_OK) {
            out.legality.reasons |=
                cargo_legality_reason_from_receipt_result(
                    out.trust.chain_result);
            out.legality.status = CARGO_LEGALITY_CONTRABAND;
            return out;
        }
        if (out.origin_status !=
            CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED) {
            out.legality.reasons |=
                CARGO_LEGALITY_REASON_RECEIPT_REJECTED;
            out.legality.status = CARGO_LEGALITY_CONTRABAND;
            return out;
        }
        if (!cargo_origin_metadata_matches(
                &origin, origin_authority.station_index,
                unit)) {
            reject_origin_metadata(&out, true);
            return out;
        }
        mark_craft_provenance(&out, &origin);
        mark_authority_legality(
            &out.legality, origin_authority.trust, screens);
        if (!authority_policy_accepts(
                origin_authority.trust, screens, tolerates)) {
            out.trust.status =
                rejection_for_authority(origin_authority.trust);
            out.first_rejected_link = 0;
            return out;
        }

        for (uint8_t i = 0; i < chain->len; i++) {
            authority_resolution_t author = resolve_authority(
                world, evaluating_station,
                chain->links[i].authoring_station);
            mark_authority_legality(
                &out.legality, author.trust, screens);
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
            if (!authority_policy_accepts(
                    author.trust, screens, tolerates)) {
                out.trust.status =
                    rejection_for_authority(author.trust);
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
            out.legality.reasons |=
                CARGO_LEGALITY_REASON_BANNED_ORIGIN;
            out.legality.status = CARGO_LEGALITY_CONTRABAND;
        }
    }
    /*
     * Cryptographic/origin failures returned above. At this point only local
     * station policy may decide whether suspicious/contraband authority or
     * origin attributes are tolerated.
     */
    out.accepted = cargo_legality_station_accepts(out.legality);
    return out;
}

cargo_receipt_station_evaluation_t
cargo_receipt_evaluate_physical_origin_at_station(
    const world_t *world,
    int evaluating_station,
    const cargo_unit_t *unit) {
    cargo_receipt_station_evaluation_t out =
        cargo_receipt_station_evaluation_default();
    if (!world || !unit || evaluating_station < 0 ||
        evaluating_station >= world->station_count ||
        evaluating_station >= MAX_STATIONS ||
        unit->origin_station >= world->station_count ||
        unit->origin_station >= MAX_STATIONS ||
        !station_exists(&world->stations[evaluating_station]) ||
        !station_exists(&world->stations[unit->origin_station])) {
        return out;
    }

    const station_t *viewer = &world->stations[evaluating_station];
    const int origin_station = (int)unit->origin_station;
    const station_t *origin_station_state =
        &world->stations[origin_station];
    const bool screens = cargo_legality_station_screens(
        viewer, evaluating_station);
    const bool tolerates = cargo_legality_station_tolerates_contraband(
        viewer, evaluating_station);
    out.legality = legality_base(viewer, evaluating_station);
    out.origin_station = origin_station;
    out.legality.origin_station = origin_station;
    out.local_origin_without_receipt =
        origin_station == evaluating_station;

    cargo_receipt_origin_proof_t origin = {0};
    out.trust = empty_chain_trust_result();
    out.origin_status = cargo_receipt_resolve_local_origin(
        origin_station_state, unit->pub, &origin);
    if (out.origin_status !=
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED) {
        out.local_origin_without_receipt = false;
        out.trust.status =
            CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN;
        out.legality.reasons |=
            CARGO_LEGALITY_REASON_MISSING_RECEIPT;
        out.legality.status = CARGO_LEGALITY_CONTRABAND;
        return out;
    }

    authority_resolution_t authority = resolve_authority(
        world, evaluating_station, origin.authority);
    set_resolved_chainless_trust(&out, &origin, authority);
    if (!cargo_origin_metadata_matches(
            &origin, origin_station, unit)) {
        out.local_origin_without_receipt = false;
        reject_origin_metadata(&out, false);
        return out;
    }
    mark_craft_provenance(&out, &origin);
    mark_authority_legality(
        &out.legality, authority.trust, screens);
    if (!authority_policy_accepts(
            authority.trust, screens, tolerates)) {
        return out;
    }

    if (cargo_legality_station_tolerates_contraband(
            origin_station_state, origin_station)) {
        out.legality.reasons |=
            CARGO_LEGALITY_REASON_BLACK_MARKET_AUTHORITY;
        out.legality.black_market_station = origin_station;
        out.legality.status = CARGO_LEGALITY_CONTRABAND;
    }

    if (origin_station < 64) {
        uint64_t forbidden =
            station_policy_forbidden_origin_mask_for_station(
                viewer, evaluating_station,
                (commodity_t)unit->commodity);
        if ((forbidden & (1ULL << origin_station)) != 0) {
            out.legality.reasons |=
                CARGO_LEGALITY_REASON_BANNED_ORIGIN;
            out.legality.status = CARGO_LEGALITY_CONTRABAND;
        }
    }
    out.accepted = cargo_legality_station_accepts(out.legality);
    return out;
}
