#include "cargo_legacy_inventory.h"

#include "actor_principal_resolver.h"
#include "cargo_receipt_trust.h"
#include "contract_ownership.h"
#include "manifest.h"

#include <stdlib.h>
#include <string.h>

enum {
    CARGO_LEGACY_REASON_FLAG_REIDENTIFICATION =
        1u << CARGO_LEGACY_REASON_EXPLICIT_REIDENTIFICATION_REQUIRED,
    CARGO_LEGACY_REASON_FLAG_MALFORMED =
        1u << CARGO_LEGACY_REASON_MALFORMED_UNIT,
    CARGO_LEGACY_REASON_FLAG_DUPLICATE =
        1u << CARGO_LEGACY_REASON_DUPLICATE_IDENTITY,
    CARGO_LEGACY_REASON_FLAG_RECEIPT_MISMATCH =
        1u << CARGO_LEGACY_REASON_RECEIPT_SIDECAR_MISMATCH,
    CARGO_LEGACY_REASON_FLAG_RECEIPT_ABSENT =
        1u << CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT,
    CARGO_LEGACY_REASON_FLAG_CUSTODIAN =
        1u << CARGO_LEGACY_REASON_UNRESOLVED_CUSTODIAN,
};

typedef enum {
    CARGO_LEGACY_SIDECAR_VALID = 0,
    CARGO_LEGACY_SIDECAR_ABSENT,
    CARGO_LEGACY_SIDECAR_MISMATCH,
} cargo_legacy_sidecar_status_t;

typedef struct {
    uint8_t pub[32];
    uint32_t reason_flags;
    /*
     * A non-zero ID identifies two serialized views of the same logical
     * delivery row: the shipment envelope and its exact physical pod
     * materialization. Duplicate classification collapses that pair once.
     */
    uint32_t alias_id;
    uint8_t holder;
    uint32_t holder_index;
    uint32_t unit_index;
} cargo_legacy_inventory_candidate_t;

typedef struct {
    uint32_t alias_id;
    uint16_t manifest_index;
    uint8_t npc_index;
} cargo_legacy_npc_manifest_alias_t;

enum {
    CARGO_LEGACY_NPC_ALIAS_CAP =
        MAX_DELIVERY_SHIPMENTS * MAX_DELIVERY_BOUND_CARGO,
};

typedef struct {
    uint32_t pod_alias_id[MAX_CARGO_PODS][CARGO_POD_MANIFEST_CAP];
    cargo_legacy_sidecar_status_t
        pod_sidecar[MAX_CARGO_PODS][CARGO_POD_MANIFEST_CAP];
    bool pod_custodian_resolved[MAX_CARGO_PODS][CARGO_POD_MANIFEST_CAP];
    uint32_t shipment_alias_id
        [MAX_DELIVERY_SHIPMENTS][MAX_DELIVERY_BOUND_CARGO];
    cargo_legacy_npc_manifest_alias_t
        npc_manifest_aliases[CARGO_LEGACY_NPC_ALIAS_CAP];
    uint16_t npc_manifest_alias_count;
} cargo_legacy_delivery_alias_map_t;

typedef struct {
    const world_t *world;
    cargo_legacy_inventory_report_t *report;
    cargo_legacy_inventory_candidate_t *candidates;
    cargo_legacy_delivery_alias_map_t *delivery_aliases;
    uint32_t candidate_count;
} cargo_legacy_inventory_scan_t;

static bool bytes_are_zero(const uint8_t *bytes, size_t len) {
    uint8_t any = 0;
    if (!bytes) return false;
    for (size_t i = 0; i < len; i++) any |= bytes[i];
    return any == 0;
}

static bool legacy_unit_is_structurally_valid(const cargo_unit_t *unit) {
    cargo_kind_t expected_kind;
    return unit &&
           unit->recipe_id == (uint16_t)RECIPE_LEGACY_MIGRATE &&
           unit->commodity >= (uint8_t)COMMODITY_RAW_ORE_COUNT &&
           unit->commodity < (uint8_t)COMMODITY_COUNT &&
           cargo_kind_for_commodity(
               (commodity_t)unit->commodity, &expected_kind) &&
           unit->kind == (uint8_t)expected_kind &&
           unit->grade < (uint8_t)MINING_GRADE_COUNT &&
           unit->prefix_class == (uint8_t)INGOT_PREFIX_ANONYMOUS &&
           unit->quantity == 1u &&
           unit->origin_station < MAX_STATIONS &&
           unit->mined_block == 0u &&
           !bytes_are_zero(unit->pub, sizeof(unit->pub)) &&
           bytes_are_zero(
               unit->parent_merkle, sizeof(unit->parent_merkle));
}

static bool principal_is_resolved(const actor_principal_t *principal) {
    return actor_principal_is_canonical(principal) &&
           principal->kind != (uint8_t)ACTOR_PRINCIPAL_NONE &&
           principal->kind != (uint8_t)ACTOR_PRINCIPAL_UNATTRIBUTED;
}

static bool station_custodian_resolved(
    const world_t *world,
    int station_index) {
    actor_principal_t principal = actor_principal_none();
    return actor_principal_from_station(
               world, station_index, &principal) &&
           principal_is_resolved(&principal);
}

static bool player_custodian_resolved(
    const world_t *world,
    int player_index) {
    actor_principal_t principal = actor_principal_none();
    return world &&
           player_index >= 0 && player_index < MAX_PLAYERS &&
           actor_principal_from_verified_player(
               &world->players[player_index], &principal) &&
           principal_is_resolved(&principal);
}

static bool npc_custodian_resolved(
    const world_t *world,
    int npc_index) {
    actor_principal_t principal = actor_principal_none();
    bool token_conflict = false;
    return actor_principal_from_unique_npc_slot(
               world, npc_index, &principal, &token_conflict) &&
           !token_conflict &&
           principal_is_resolved(&principal);
}

static bool pod_custodian_resolved(
    const world_t *world,
    const cargo_pod_t *pod) {
    int station = cargo_pod_custody_station(pod);
    if (station >= 0)
        return station_custodian_resolved(world, station);
    int player = cargo_pod_player_tractor(pod);
    return player >= 0 &&
           player_custodian_resolved(world, player);
}

static bool manifest_sidecar_shape_valid(
    const manifest_t *manifest,
    const ship_receipts_t *receipts) {
    if (!manifest || manifest->count > manifest->cap ||
        (manifest->count > 0u && !manifest->units)) {
        return false;
    }
    if (!receipts || receipts->count != manifest->count ||
        receipts->count > receipts->cap ||
        (receipts->count > 0u && !receipts->chains)) {
        return false;
    }
    for (uint16_t i = 0; i < receipts->count; i++) {
        if (receipts->chains[i].len >
            CARGO_RECEIPT_CHAIN_MAX_LEN) {
            return false;
        }
    }
    return true;
}

static cargo_legacy_sidecar_status_t sidecar_status_for_chain(
    const world_t *world,
    const cargo_receipt_chain_t *chain,
    const cargo_unit_t *unit) {
    if (!chain || chain->len == 0u)
        return CARGO_LEGACY_SIDECAR_ABSENT;
    if (!world || !unit || world->station_count <= 0 ||
        chain->len > CARGO_RECEIPT_CHAIN_MAX_LEN ||
        (int)unit->origin_station >= world->station_count ||
        unit->origin_station >= MAX_STATIONS) {
        return CARGO_LEGACY_SIDECAR_MISMATCH;
    }
    cargo_receipt_station_evaluation_t evaluation =
        cargo_receipt_evaluate_at_station(
            world, (int)unit->origin_station, unit, chain);
    if (evaluation.origin_status !=
            CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED ||
        evaluation.trust.chain_result != CARGO_RECEIPT_OK ||
        (evaluation.trust.status !=
             CARGO_RECEIPT_TRUST_VALID_TRUSTED &&
         evaluation.trust.status !=
             CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED)) {
        return CARGO_LEGACY_SIDECAR_MISMATCH;
    }
    return CARGO_LEGACY_SIDECAR_VALID;
}

static void add_holder_error(cargo_legacy_inventory_scan_t *scan) {
    scan->report->reason_count[
        CARGO_LEGACY_REASON_HOLDER_BOUNDS_INVALID]++;
}

static uint32_t npc_manifest_alias_id(
    const cargo_legacy_inventory_scan_t *scan,
    uint32_t npc_index,
    uint32_t manifest_index) {
    if (!scan || !scan->delivery_aliases ||
        npc_index >= MAX_NPC_SHIPS ||
        manifest_index > UINT16_MAX) {
        return 0u;
    }
    const cargo_legacy_delivery_alias_map_t *aliases =
        scan->delivery_aliases;
    for (uint16_t i = 0;
         i < aliases->npc_manifest_alias_count; i++) {
        const cargo_legacy_npc_manifest_alias_t *alias =
            &aliases->npc_manifest_aliases[i];
        if (alias->npc_index == (uint8_t)npc_index &&
            alias->manifest_index == (uint16_t)manifest_index) {
            return alias->alias_id;
        }
    }
    return 0u;
}

static bool scan_one_unit(
    cargo_legacy_inventory_scan_t *scan,
    cargo_legacy_holder_t holder,
    uint32_t holder_index,
    uint32_t unit_index,
    const cargo_unit_t *unit,
    cargo_legacy_sidecar_status_t sidecar_status,
    uint32_t alias_id,
    bool custodian_resolved) {
    if (scan->report->units_examined >=
        (uint32_t)CARGO_LEGACY_INVENTORY_SCAN_LIMIT) {
        scan->report->truncated = true;
        return false;
    }
    scan->report->units_examined++;
    if (!unit ||
        unit->recipe_id != (uint16_t)RECIPE_LEGACY_MIGRATE) {
        return true;
    }

    cargo_legacy_inventory_candidate_t *candidate =
        &scan->candidates[scan->candidate_count++];
    memset(candidate, 0, sizeof(*candidate));
    memcpy(candidate->pub, unit->pub, sizeof(candidate->pub));
    candidate->holder = (uint8_t)holder;
    candidate->holder_index = holder_index;
    candidate->unit_index = unit_index;
    candidate->alias_id = alias_id;
    candidate->reason_flags =
        CARGO_LEGACY_REASON_FLAG_REIDENTIFICATION;
    if (!legacy_unit_is_structurally_valid(unit))
        candidate->reason_flags |=
            CARGO_LEGACY_REASON_FLAG_MALFORMED;
    if (sidecar_status == CARGO_LEGACY_SIDECAR_MISMATCH)
        candidate->reason_flags |=
            CARGO_LEGACY_REASON_FLAG_RECEIPT_MISMATCH;
    if (sidecar_status == CARGO_LEGACY_SIDECAR_ABSENT)
        candidate->reason_flags |=
            CARGO_LEGACY_REASON_FLAG_RECEIPT_ABSENT;
    if (!custodian_resolved)
        candidate->reason_flags |=
            CARGO_LEGACY_REASON_FLAG_CUSTODIAN;
    return true;
}

static bool scan_manifest(
    cargo_legacy_inventory_scan_t *scan,
    cargo_legacy_holder_t holder,
    uint32_t holder_index,
    const manifest_t *manifest,
    const ship_receipts_t *receipts,
    bool custodian_resolved) {
    if (!manifest || manifest->count > manifest->cap ||
        (manifest->count > 0u && !manifest->units)) {
        add_holder_error(scan);
        return true;
    }
    bool sidecar_shape_valid =
        manifest_sidecar_shape_valid(manifest, receipts);
    if (!sidecar_shape_valid) add_holder_error(scan);
    for (uint16_t i = 0; i < manifest->count; i++) {
        cargo_legacy_sidecar_status_t sidecar_status =
            CARGO_LEGACY_SIDECAR_MISMATCH;
        if (sidecar_shape_valid) {
            const cargo_receipt_chain_t *chain =
                &receipts->chains[i];
            /*
             * Alignment proves only that the sidecar slot exists. This
             * inventory API has no verified station-log evidence, so an
             * empty slot is explicitly absent; non-empty bytes must verify.
             */
            sidecar_status = sidecar_status_for_chain(
                scan->world, chain, &manifest->units[i]);
        }
        if (!scan_one_unit(
                scan, holder, holder_index, i,
                &manifest->units[i], sidecar_status,
                holder == CARGO_LEGACY_HOLDER_NPC_SHIP
                    ? npc_manifest_alias_id(
                          scan, holder_index, i)
                    : 0u,
                custodian_resolved)) {
            return false;
        }
    }
    return true;
}

static const ship_t *asset_serialized_ship(
    const world_t *world,
    const ship_asset_t *asset,
    bool *out_alias) {
    const ship_t *live = NULL;
    if (out_alias) *out_alias = false;
    if (!world || !asset || !asset->active)
        return NULL;
    if (!asset->destroyed &&
        asset->status == SHIP_ASSET_STATUS_ASSIGNED) {
        if (asset->operator_kind ==
                SHIP_ASSET_OPERATOR_PLAYER &&
            asset->operator_slot >= 0 &&
            asset->operator_slot < MAX_PLAYERS) {
            const server_player_t *player =
                &world->players[asset->operator_slot];
            if (player->connected &&
                player->ship_asset_id == asset->asset_id) {
                live = world_player_ship_for_const(
                    world, asset->operator_slot);
            }
        } else if (asset->operator_kind ==
                       SHIP_ASSET_OPERATOR_NPC &&
                   asset->operator_slot >= 0 &&
                   asset->operator_slot < MAX_NPC_SHIPS) {
            const npc_ship_t *npc =
                &world->npc_ships[asset->operator_slot];
            if (npc->active &&
                npc->ship_asset_id == asset->asset_id) {
                live = world_npc_ship_for_const(
                    world, asset->operator_slot);
            }
        }
    }
    if (live) {
        if (out_alias) *out_alias = true;
        return live;
    }
    return &asset->stored_ship;
}

static bool delivery_pod_alias_shape(
    const world_t *world,
    int pod_index,
    int *out_shipment_index,
    uint16_t *out_cargo_offset) {
    if (!world || pod_index < 0 || pod_index >= MAX_CARGO_PODS)
        return false;
    const cargo_pod_t *pod = &world->cargo_pods[pod_index];
    if (!pod->active || pod->kind != CARGO_POD_CARGO ||
        pod->shipment_id == 0u ||
        pod->manifest_count > CARGO_POD_MANIFEST_CAP) {
        return false;
    }

    int shipment_index = -1;
    int shipment_matches = 0;
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        const delivery_shipment_t *shipment =
            &world->delivery_shipments[i];
        if (!shipment->active ||
            shipment->shipment_id != pod->shipment_id) {
            continue;
        }
        shipment_index = i;
        shipment_matches++;
    }
    int pod_matches = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *other = &world->cargo_pods[i];
        if (other->active &&
            other->shipment_id == pod->shipment_id) {
            pod_matches++;
        }
    }
    if (shipment_matches != 1 || pod_matches != 1)
        return false;

    const delivery_shipment_t *shipment =
        &world->delivery_shipments[shipment_index];
    uint32_t consumed =
        (uint32_t)shipment->quantity_delivered +
        (uint32_t)shipment->quantity_black_market_sold;
    if (shipment->status != DELIVERY_SHIPMENT_PICKED_UP ||
        shipment->commodity >= COMMODITY_COUNT ||
        shipment->quantity_bound > MAX_DELIVERY_BOUND_CARGO ||
        shipment->quantity_total != shipment->quantity_bound ||
        consumed >= shipment->quantity_total) {
        return false;
    }
    uint16_t remaining =
        (uint16_t)((uint32_t)shipment->quantity_total - consumed);
    if (remaining == 0u ||
        remaining > CARGO_POD_MANIFEST_CAP ||
        consumed + remaining > shipment->quantity_bound ||
        pod->commodity != (commodity_t)shipment->commodity ||
        pod->quantity != remaining ||
        pod->manifest_count != remaining) {
        return false;
    }
    for (uint16_t i = 0; i < remaining; i++) {
        uint16_t shipment_unit = (uint16_t)(consumed + i);
        const cargo_unit_t *envelope =
            &shipment->cargo_units[shipment_unit];
        const cargo_unit_t *physical =
            &pod->manifest_units[i];
        if (envelope->commodity != shipment->commodity ||
            memcmp(shipment->cargo_pub[shipment_unit],
                   envelope->pub, sizeof(envelope->pub)) != 0 ||
            memcmp(physical, envelope, sizeof(*physical)) != 0) {
            return false;
        }
    }
    if (out_shipment_index) *out_shipment_index = shipment_index;
    if (out_cargo_offset) *out_cargo_offset = (uint16_t)consumed;
    return true;
}

static bool receipt_chain_bytes_equal(
    const cargo_receipt_chain_t *left,
    const cargo_receipt_chain_t *right) {
    if (!left || !right ||
        left->len > CARGO_RECEIPT_CHAIN_MAX_LEN ||
        right->len > CARGO_RECEIPT_CHAIN_MAX_LEN ||
        left->len != right->len) {
        return false;
    }
    return left->len == 0u ||
           memcmp(
               left->links, right->links,
               (size_t)left->len *
                   sizeof(left->links[0])) == 0;
}

static void build_delivery_npc_alias_map(
    cargo_legacy_inventory_scan_t *scan) {
    if (!scan || !scan->world || !scan->delivery_aliases)
        return;
    cargo_legacy_delivery_alias_map_t *aliases =
        scan->delivery_aliases;
    for (int shipment_index = 0;
         shipment_index < MAX_DELIVERY_SHIPMENTS;
         shipment_index++) {
        const delivery_shipment_t *shipment =
            &scan->world->delivery_shipments[shipment_index];
        uint32_t consumed =
            (uint32_t)shipment->quantity_delivered +
            (uint32_t)shipment->quantity_black_market_sold;
        if (!shipment->active || shipment->shipment_id == 0u ||
            shipment->status != DELIVERY_SHIPMENT_PICKED_UP ||
            shipment->commodity >= COMMODITY_COUNT ||
            shipment->quantity_bound > MAX_DELIVERY_BOUND_CARGO ||
            shipment->quantity_total != shipment->quantity_bound ||
            consumed >= shipment->quantity_total) {
            continue;
        }

        int id_matches = 0;
        for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
            const delivery_shipment_t *other =
                &scan->world->delivery_shipments[i];
            if (other->active &&
                other->shipment_id == shipment->shipment_id) {
                id_matches++;
            }
        }
        if (id_matches != 1) continue;

        int npc_index = -1;
        int owner_matches = 0;
        for (int i = 0; i < MAX_NPC_SHIPS; i++) {
            if (delivery_ownership_matches_npc(
                    shipment, scan->world, i)) {
                npc_index = i;
                owner_matches++;
            }
        }
        if (owner_matches != 1) continue;
        const ship_t *ship =
            world_npc_ship_for_const(scan->world, npc_index);
        const ship_receipts_t *receipts =
            ship ? ship_get_receipts_const(ship) : NULL;
        if (!ship ||
            !manifest_sidecar_shape_valid(
                &ship->manifest, receipts)) {
            continue;
        }

        uint16_t remaining =
            (uint16_t)((uint32_t)shipment->quantity_total -
                       consumed);
        if (remaining == 0u ||
            consumed + remaining >
                shipment->quantity_bound ||
            aliases->npc_manifest_alias_count + remaining >
                CARGO_LEGACY_NPC_ALIAS_CAP) {
            continue;
        }

        bool exact = true;
        uint16_t matched_rows[MAX_DELIVERY_BOUND_CARGO] = {0};
        for (uint16_t unit = 0;
             exact && unit < remaining; unit++) {
            uint16_t shipment_unit =
                (uint16_t)(consumed + unit);
            const cargo_unit_t *envelope =
                &shipment->cargo_units[shipment_unit];
            const cargo_receipt_chain_t *envelope_chain =
                &shipment->cargo_chains[shipment_unit];
            if (envelope->commodity != shipment->commodity ||
                memcmp(shipment->cargo_pub[shipment_unit],
                       envelope->pub,
                       sizeof(envelope->pub)) != 0 ||
                aliases->shipment_alias_id
                    [shipment_index][shipment_unit] != 0u) {
                exact = false;
                break;
            }

            int match_count = 0;
            uint16_t matched_row = 0u;
            for (uint16_t row = 0;
                 row < ship->manifest.count; row++) {
                if (memcmp(
                        &ship->manifest.units[row],
                        envelope, sizeof(*envelope)) != 0 ||
                    !receipt_chain_bytes_equal(
                        &receipts->chains[row],
                        envelope_chain)) {
                    continue;
                }
                match_count++;
                matched_row = row;
            }
            if (match_count != 1) {
                exact = false;
                break;
            }
            for (uint16_t earlier = 0;
                 earlier < unit; earlier++) {
                if (matched_rows[earlier] == matched_row) {
                    exact = false;
                    break;
                }
            }
            if (!exact ||
                npc_manifest_alias_id(
                    scan, (uint32_t)npc_index,
                    matched_row) != 0u) {
                exact = false;
                break;
            }
            matched_rows[unit] = matched_row;
        }
        if (!exact) continue;

        for (uint16_t unit = 0;
             unit < remaining; unit++) {
            uint16_t shipment_unit =
                (uint16_t)(consumed + unit);
            uint32_t alias_id =
                1u +
                (uint32_t)MAX_CARGO_PODS *
                    (uint32_t)CARGO_POD_MANIFEST_CAP +
                (uint32_t)shipment_index *
                    (uint32_t)MAX_DELIVERY_BOUND_CARGO +
                (uint32_t)shipment_unit;
            cargo_legacy_npc_manifest_alias_t *manifest_alias =
                &aliases->npc_manifest_aliases[
                    aliases->npc_manifest_alias_count++];
            manifest_alias->alias_id = alias_id;
            manifest_alias->npc_index = (uint8_t)npc_index;
            manifest_alias->manifest_index =
                matched_rows[unit];
            aliases->shipment_alias_id
                [shipment_index][shipment_unit] = alias_id;
            scan->report->delivery_npc_ship_aliases++;
        }
    }
}

static void build_delivery_alias_map(
    cargo_legacy_inventory_scan_t *scan) {
    if (!scan || !scan->world || !scan->delivery_aliases)
        return;
    for (int pod_index = 0;
         pod_index < MAX_CARGO_PODS; pod_index++) {
        int shipment_index = -1;
        uint16_t cargo_offset = 0u;
        if (!delivery_pod_alias_shape(
                scan->world, pod_index, &shipment_index,
                &cargo_offset)) {
            continue;
        }
        const cargo_pod_t *pod =
            &scan->world->cargo_pods[pod_index];
        const delivery_shipment_t *shipment =
            &scan->world->delivery_shipments[shipment_index];
        bool custodian_resolved =
            shipment->debtor_quarantine_record_id == 0u &&
            principal_is_resolved(
                &shipment->debtor_principal);
        for (uint16_t unit = 0;
             unit < pod->manifest_count; unit++) {
            uint16_t shipment_unit =
                (uint16_t)(cargo_offset + unit);
            uint32_t alias_id =
                1u +
                (uint32_t)pod_index *
                    (uint32_t)CARGO_POD_MANIFEST_CAP +
                (uint32_t)unit;
            scan->delivery_aliases
                ->pod_alias_id[pod_index][unit] = alias_id;
            scan->delivery_aliases
                ->pod_sidecar[pod_index][unit] =
                sidecar_status_for_chain(
                    scan->world,
                    &shipment->cargo_chains[shipment_unit],
                    &shipment->cargo_units[shipment_unit]);
            scan->delivery_aliases
                ->pod_custodian_resolved[pod_index][unit] =
                custodian_resolved;
            scan->delivery_aliases
                ->shipment_alias_id
                    [shipment_index][shipment_unit] = alias_id;
            scan->report->delivery_pod_aliases++;
        }
    }
    build_delivery_npc_alias_map(scan);
}

static int candidate_compare(const void *lhs, const void *rhs) {
    const cargo_legacy_inventory_candidate_t *left = lhs;
    const cargo_legacy_inventory_candidate_t *right = rhs;
    int pub_order = memcmp(left->pub, right->pub, sizeof(left->pub));
    if (pub_order != 0) return pub_order;
    if (left->alias_id != right->alias_id)
        return left->alias_id < right->alias_id ? -1 : 1;
    if (left->holder != right->holder)
        return left->holder < right->holder ? -1 : 1;
    if (left->holder_index != right->holder_index)
        return left->holder_index < right->holder_index ? -1 : 1;
    if (left->unit_index != right->unit_index)
        return left->unit_index < right->unit_index ? -1 : 1;
    return 0;
}

static void classify_duplicates_and_totals(
    cargo_legacy_inventory_scan_t *scan) {
    qsort(scan->candidates, scan->candidate_count,
          sizeof(scan->candidates[0]), candidate_compare);
    uint32_t cursor = 0;
    while (cursor < scan->candidate_count) {
        uint32_t end = cursor + 1u;
        while (end < scan->candidate_count &&
               memcmp(scan->candidates[cursor].pub,
                      scan->candidates[end].pub, 32) == 0) {
            end++;
        }
        bool nonzero = !bytes_are_zero(
            scan->candidates[cursor].pub, 32);
        uint32_t logical_occurrences = 0u;
        uint32_t previous_alias = UINT32_MAX;
        for (uint32_t i = cursor; i < end; i++) {
            uint32_t alias_id = scan->candidates[i].alias_id;
            if (alias_id == 0u || alias_id != previous_alias)
                logical_occurrences++;
            previous_alias = alias_id;
        }
        if (nonzero && logical_occurrences > 1u) {
            for (uint32_t i = cursor; i < end; i++) {
                scan->candidates[i].reason_flags |=
                    CARGO_LEGACY_REASON_FLAG_DUPLICATE;
            }
        }
        cursor = end;
    }

    scan->report->legacy_candidates = scan->candidate_count;
    for (uint32_t i = 0; i < scan->candidate_count; i++) {
        const cargo_legacy_inventory_candidate_t *candidate =
            &scan->candidates[i];
        if (candidate->holder < CARGO_LEGACY_HOLDER_COUNT) {
            scan->report->holder_candidate_count[
                candidate->holder]++;
        }
        for (uint32_t reason = 0;
             reason < CARGO_LEGACY_REASON_COUNT; reason++) {
            if ((candidate->reason_flags &
                 (UINT32_C(1) << reason)) != 0u) {
                scan->report->reason_count[reason]++;
            }
        }
    }
}

const char *cargo_legacy_holder_name(cargo_legacy_holder_t holder) {
    switch (holder) {
        case CARGO_LEGACY_HOLDER_STATION_MANIFEST:
            return "station_manifest";
        case CARGO_LEGACY_HOLDER_PLAYER_SHIP:
            return "player_ship";
        case CARGO_LEGACY_HOLDER_NPC_SHIP:
            return "npc_ship";
        case CARGO_LEGACY_HOLDER_SHIP_ASSET_STORED:
            return "ship_asset_stored";
        case CARGO_LEGACY_HOLDER_CARGO_POD_MANIFEST:
            return "cargo_pod_manifest";
        case CARGO_LEGACY_HOLDER_CARGO_POD_SHELL:
            return "cargo_pod_shell";
        case CARGO_LEGACY_HOLDER_DELIVERY_SHIPMENT:
            return "delivery_shipment";
        case CARGO_LEGACY_HOLDER_COUNT:
            break;
    }
    return "unknown";
}

const char *cargo_legacy_inventory_reason_name(
    cargo_legacy_inventory_reason_t reason) {
    switch (reason) {
        case CARGO_LEGACY_REASON_EXPLICIT_REIDENTIFICATION_REQUIRED:
            return "explicit_reidentification_required";
        case CARGO_LEGACY_REASON_MALFORMED_UNIT:
            return "malformed_unit";
        case CARGO_LEGACY_REASON_DUPLICATE_IDENTITY:
            return "duplicate_identity";
        case CARGO_LEGACY_REASON_RECEIPT_SIDECAR_MISMATCH:
            return "receipt_sidecar_mismatch";
        case CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT:
            return "receipt_sidecar_absent";
        case CARGO_LEGACY_REASON_UNRESOLVED_CUSTODIAN:
            return "unresolved_custodian";
        case CARGO_LEGACY_REASON_HOLDER_BOUNDS_INVALID:
            return "holder_bounds_invalid";
        case CARGO_LEGACY_REASON_SCAN_LIMIT_REACHED:
            return "scan_limit_reached";
        case CARGO_LEGACY_REASON_COUNT:
            break;
    }
    return "unknown";
}

bool cargo_legacy_inventory_scan_world(
    const world_t *world,
    cargo_legacy_inventory_report_t *report) {
    if (!world || !report) return false;
    memset(report, 0, sizeof(*report));
    report->schema_version = CARGO_LEGACY_INVENTORY_SCHEMA_V1;
    report->scan_limit = CARGO_LEGACY_INVENTORY_SCAN_LIMIT;
    report->external_player_saves_not_enumerated = true;

    cargo_legacy_inventory_candidate_t *candidates = calloc(
        CARGO_LEGACY_INVENTORY_SCAN_LIMIT, sizeof(*candidates));
    if (!candidates) return false;
    cargo_legacy_delivery_alias_map_t *delivery_aliases =
        calloc(1, sizeof(*delivery_aliases));
    if (!delivery_aliases) {
        free(candidates);
        return false;
    }
    cargo_legacy_inventory_scan_t scan = {
        .world = world,
        .report = report,
        .candidates = candidates,
        .delivery_aliases = delivery_aliases,
    };
    build_delivery_alias_map(&scan);
    bool keep_scanning = true;

    int station_count = world->station_count;
    if (station_count < 0 || station_count > MAX_STATIONS) {
        add_holder_error(&scan);
        if (station_count < 0) station_count = 0;
        if (station_count > MAX_STATIONS)
            station_count = MAX_STATIONS;
    }
    for (int station = 0;
         keep_scanning && station < station_count; station++) {
        const station_t *holder = &world->stations[station];
        keep_scanning = scan_manifest(
            &scan, CARGO_LEGACY_HOLDER_STATION_MANIFEST,
            (uint32_t)station, &holder->manifest,
            station_get_receipts_const(holder),
            station_custodian_resolved(world, station));
    }

    for (int player = 0;
         keep_scanning && player < MAX_PLAYERS; player++) {
        const ship_t *ship =
            world_player_ship_for_const(world, player);
        if (!ship) continue;
        keep_scanning = scan_manifest(
            &scan, CARGO_LEGACY_HOLDER_PLAYER_SHIP,
            (uint32_t)player, &ship->manifest,
            ship_get_receipts_const(ship),
            player_custodian_resolved(world, player));
    }

    for (int npc = 0;
         keep_scanning && npc < MAX_NPC_SHIPS; npc++) {
        const ship_t *ship =
            world_npc_ship_for_const(world, npc);
        if (!ship) continue;
        keep_scanning = scan_manifest(
            &scan, CARGO_LEGACY_HOLDER_NPC_SHIP,
            (uint32_t)npc, &ship->manifest,
            ship_get_receipts_const(ship),
            npc_custodian_resolved(world, npc));
    }

    for (int asset_index = 0;
         keep_scanning && asset_index < MAX_SHIP_ASSETS;
         asset_index++) {
        const ship_asset_t *asset =
            &world->ship_assets[asset_index];
        bool alias = false;
        const ship_t *ship =
            asset_serialized_ship(world, asset, &alias);
        if (!ship) continue;
        if (alias) {
            report->assigned_asset_aliases++;
            continue;
        }
        bool custodian_resolved =
            asset->owner_quarantine_record_id == 0u &&
            principal_is_resolved(&asset->owner_principal);
        keep_scanning = scan_manifest(
            &scan, CARGO_LEGACY_HOLDER_SHIP_ASSET_STORED,
            (uint32_t)asset_index, &ship->manifest,
            ship_get_receipts_const(ship),
            custodian_resolved);
    }

    for (int pod_index = 0;
         keep_scanning && pod_index < MAX_CARGO_PODS;
         pod_index++) {
        const cargo_pod_t *pod = &world->cargo_pods[pod_index];
        if (!pod->active) continue;
        bool manifest_bounds_valid =
            pod->manifest_count <= CARGO_POD_MANIFEST_CAP;
        if (pod->manifest_count > CARGO_POD_MANIFEST_CAP) {
            add_holder_error(&scan);
        }
        bool custodian_resolved =
            pod_custodian_resolved(world, pod);
        if (manifest_bounds_valid) {
            for (uint16_t unit = 0;
                 keep_scanning && unit < pod->manifest_count;
                 unit++) {
                uint32_t alias_id =
                    delivery_aliases
                        ->pod_alias_id[pod_index][unit];
                cargo_legacy_sidecar_status_t sidecar_status =
                    CARGO_LEGACY_SIDECAR_ABSENT;
                bool unit_custodian_resolved =
                    custodian_resolved;
                if (alias_id != 0u) {
                    sidecar_status =
                        delivery_aliases
                            ->pod_sidecar[pod_index][unit];
                    unit_custodian_resolved =
                        delivery_aliases
                            ->pod_custodian_resolved
                                [pod_index][unit];
                }
                keep_scanning = scan_one_unit(
                    &scan,
                    CARGO_LEGACY_HOLDER_CARGO_POD_MANIFEST,
                    (uint32_t)pod_index, unit,
                    &pod->manifest_units[unit],
                    sidecar_status, alias_id,
                    unit_custodian_resolved);
            }
        }
        if (keep_scanning && pod->has_shell_frame) {
            keep_scanning = scan_one_unit(
                &scan,
                CARGO_LEGACY_HOLDER_CARGO_POD_SHELL,
                (uint32_t)pod_index, 0u,
                &pod->shell_frame,
                CARGO_LEGACY_SIDECAR_ABSENT, 0u,
                custodian_resolved);
        }
    }

    for (int shipment_index = 0;
         keep_scanning &&
         shipment_index < MAX_DELIVERY_SHIPMENTS;
         shipment_index++) {
        const delivery_shipment_t *shipment =
            &world->delivery_shipments[shipment_index];
        if (!shipment->active) continue;
        if (shipment->quantity_bound >
            MAX_DELIVERY_BOUND_CARGO) {
            add_holder_error(&scan);
            continue;
        }
        bool custodian_resolved =
            shipment->debtor_quarantine_record_id == 0u &&
            principal_is_resolved(
                &shipment->debtor_principal);
        for (uint16_t unit = 0;
             keep_scanning &&
             unit < shipment->quantity_bound; unit++) {
            cargo_legacy_sidecar_status_t sidecar_status =
                sidecar_status_for_chain(
                    world,
                    &shipment->cargo_chains[unit],
                    &shipment->cargo_units[unit]);
            uint32_t alias_id =
                delivery_aliases
                    ->shipment_alias_id[shipment_index][unit];
            keep_scanning = scan_one_unit(
                &scan,
                CARGO_LEGACY_HOLDER_DELIVERY_SHIPMENT,
                (uint32_t)shipment_index, unit,
                &shipment->cargo_units[unit],
                sidecar_status, alias_id,
                custodian_resolved);
        }
    }

    if (!keep_scanning || report->truncated) {
        report->truncated = true;
        report->reason_count[
            CARGO_LEGACY_REASON_SCAN_LIMIT_REACHED]++;
    }
    classify_duplicates_and_totals(&scan);
    free(delivery_aliases);
    free(candidates);
    return true;
}

bool cargo_legacy_inventory_report_write(
    FILE *out,
    const cargo_legacy_inventory_report_t *report) {
    if (!out || !report ||
        report->schema_version !=
            CARGO_LEGACY_INVENTORY_SCHEMA_V1 ||
        report->scan_limit !=
            CARGO_LEGACY_INVENTORY_SCAN_LIMIT) {
        return false;
    }
    if (fprintf(
            out,
            "legacy_cargo_inventory schema=%u scan_limit=%u "
            "units_examined=%u candidates=%u truncated=%s "
            "assigned_asset_aliases=%u "
            "delivery_pod_aliases=%u "
            "delivery_npc_ship_aliases=%u "
            "external_player_saves=not_enumerated\n",
            report->schema_version,
            report->scan_limit,
            report->units_examined,
            report->legacy_candidates,
            report->truncated ? "true" : "false",
            report->assigned_asset_aliases,
            report->delivery_pod_aliases,
            report->delivery_npc_ship_aliases) < 0) {
        return false;
    }
    for (uint32_t holder = 0;
         holder < CARGO_LEGACY_HOLDER_COUNT; holder++) {
        if (fprintf(
                out,
                "legacy_cargo_holder code=%u name=%s candidates=%u\n",
                holder,
                cargo_legacy_holder_name(
                    (cargo_legacy_holder_t)holder),
                report->holder_candidate_count[holder]) < 0) {
            return false;
        }
    }
    for (uint32_t reason = 0;
         reason < CARGO_LEGACY_REASON_COUNT; reason++) {
        if (fprintf(
                out,
                "legacy_cargo_reason code=%u name=%s count=%u\n",
                reason,
                cargo_legacy_inventory_reason_name(
                    (cargo_legacy_inventory_reason_t)reason),
                report->reason_count[reason]) < 0) {
            return false;
        }
    }
    return true;
}
