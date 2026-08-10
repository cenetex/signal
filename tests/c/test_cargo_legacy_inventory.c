#include "test_harness.h"

#include "actor_principal_resolver.h"
#include "cargo_legacy_inventory.h"
#include "cargo_receipt_issue.h"
#include "contract_ownership.h"
#include "manifest.h"

#include <stdlib.h>
#include <string.h>

static bool make_legacy_unit(
    cargo_unit_t *unit,
    uint8_t seed,
    commodity_t commodity) {
    uint8_t origin[8] = {
        'I', 'N', 'V', 'E', 'N', 'T', seed, 1u
    };
    return hash_legacy_migrate_unit(
        origin, commodity, seed, unit);
}

static bool read_report_text(
    const cargo_legacy_inventory_report_t *report,
    char *out,
    size_t out_cap) {
    if (!report || !out || out_cap == 0u) return false;
    FILE *file = tmpfile();
    if (!file) return false;
    bool ok = cargo_legacy_inventory_report_write(file, report) &&
              fflush(file) == 0 &&
              fseek(file, 0, SEEK_SET) == 0;
    size_t got = 0;
    if (ok) {
        got = fread(out, 1, out_cap - 1u, file);
        out[got] = '\0';
        ok = !ferror(file);
    }
    if (fclose(file) != 0) ok = false;
    return ok && got > 0u;
}

static int find_inactive_pod(const world_t *world) {
    if (!world) return -1;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (!world->cargo_pods[i].active) return i;
    }
    return -1;
}

static int find_inactive_shipment(const world_t *world) {
    if (!world) return -1;
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        if (!world->delivery_shipments[i].active) return i;
    }
    return -1;
}

static int find_inactive_npc(const world_t *world) {
    if (!world) return -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!world->npc_ships[i].active) return i;
    }
    return -1;
}

static uint16_t unused_shipment_id(const world_t *world) {
    if (!world) return 0u;
    for (uint32_t candidate = 1u;
         candidate <= UINT16_MAX; candidate++) {
        bool used = false;
        for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
            if (world->delivery_shipments[i].active &&
                world->delivery_shipments[i].shipment_id ==
                    (uint16_t)candidate) {
                used = true;
                break;
            }
        }
        if (!used) return (uint16_t)candidate;
    }
    return 0u;
}

TEST(test_legacy_inventory_codes_and_names_are_stable) {
    ASSERT_EQ_INT(CARGO_LEGACY_HOLDER_STATION_MANIFEST, 0);
    ASSERT_EQ_INT(CARGO_LEGACY_HOLDER_PLAYER_SHIP, 1);
    ASSERT_EQ_INT(CARGO_LEGACY_HOLDER_NPC_SHIP, 2);
    ASSERT_EQ_INT(CARGO_LEGACY_HOLDER_SHIP_ASSET_STORED, 3);
    ASSERT_EQ_INT(CARGO_LEGACY_HOLDER_CARGO_POD_MANIFEST, 4);
    ASSERT_EQ_INT(CARGO_LEGACY_HOLDER_CARGO_POD_SHELL, 5);
    ASSERT_EQ_INT(CARGO_LEGACY_HOLDER_DELIVERY_SHIPMENT, 6);
    ASSERT_EQ_INT(CARGO_LEGACY_HOLDER_COUNT, 7);
    ASSERT_STR_EQ(cargo_legacy_holder_name(
                      CARGO_LEGACY_HOLDER_STATION_MANIFEST),
                  "station_manifest");
    ASSERT_STR_EQ(cargo_legacy_holder_name(
                      CARGO_LEGACY_HOLDER_DELIVERY_SHIPMENT),
                  "delivery_shipment");

    ASSERT_EQ_INT(
        CARGO_LEGACY_REASON_EXPLICIT_REIDENTIFICATION_REQUIRED, 0);
    ASSERT_EQ_INT(CARGO_LEGACY_REASON_MALFORMED_UNIT, 1);
    ASSERT_EQ_INT(CARGO_LEGACY_REASON_DUPLICATE_IDENTITY, 2);
    ASSERT_EQ_INT(
        CARGO_LEGACY_REASON_RECEIPT_SIDECAR_MISMATCH, 3);
    ASSERT_EQ_INT(
        CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT, 4);
    ASSERT_EQ_INT(CARGO_LEGACY_REASON_UNRESOLVED_CUSTODIAN, 5);
    ASSERT_EQ_INT(CARGO_LEGACY_REASON_HOLDER_BOUNDS_INVALID, 6);
    ASSERT_EQ_INT(CARGO_LEGACY_REASON_SCAN_LIMIT_REACHED, 7);
    ASSERT_EQ_INT(CARGO_LEGACY_REASON_COUNT, 8);
    ASSERT_STR_EQ(cargo_legacy_inventory_reason_name(
                      CARGO_LEGACY_REASON_DUPLICATE_IDENTITY),
                  "duplicate_identity");
    ASSERT_STR_EQ(cargo_legacy_inventory_reason_name(
                      CARGO_LEGACY_REASON_UNRESOLVED_CUSTODIAN),
                  "unresolved_custodian");
    ASSERT_STR_EQ(cargo_legacy_inventory_reason_name(
                      CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT),
                  "receipt_sidecar_absent");
}

TEST(test_legacy_inventory_covers_serialized_holders_deterministically) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world->rng = 67601u;
    world_reset(world);
    cargo_legacy_inventory_report_t baseline = {0};
    ASSERT(cargo_legacy_inventory_scan_world(
        world, &baseline));

    cargo_unit_t station_unit = {0};
    cargo_unit_t player_unit = {0};
    cargo_unit_t npc_unit = {0};
    cargo_unit_t asset_unit = {0};
    cargo_unit_t shell_unit = {0};
    cargo_unit_t malformed = {0};
    ASSERT(make_legacy_unit(
        &station_unit, 1u, COMMODITY_FRAME));
    ASSERT(make_legacy_unit(
        &player_unit, 2u, COMMODITY_LASER_MODULE));
    ASSERT(make_legacy_unit(
        &npc_unit, 3u, COMMODITY_TRACTOR_MODULE));
    ASSERT(make_legacy_unit(
        &asset_unit, 4u, COMMODITY_REPAIR_KIT));
    ASSERT(make_legacy_unit(
        &shell_unit, 5u, COMMODITY_FRAME));
    ASSERT(make_legacy_unit(
        &malformed, 6u, COMMODITY_FRAME));
    malformed.quantity = 0u;

    ASSERT(station_manifest_push_with_chain(
        &world->stations[0], &station_unit, NULL));

    ASSERT(world_player_ship_slot_activate(world, 0));
    ASSERT(ship_manifest_push_with_chain(
        world->players[0].ship, &player_unit, NULL));
    memcpy(world->players[0].session_token,
           "PLYSECRET", 8);

    ASSERT(world_npc_ship_slot_activate(world, 0));
    world->npc_ships[0].active = true;
    memcpy(world->npc_ships[0].session_token,
           "NPCSECRT", 8);
    ASSERT(ship_manifest_push_with_chain(
        world->npc_ships[0].ship, &npc_unit, NULL));

    int asset_index = -1;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        if (!world->ship_assets[i].active) {
            asset_index = i;
            break;
        }
    }
    ASSERT(asset_index >= 0);
    ship_asset_t *asset = &world->ship_assets[asset_index];
    memset(asset, 0, sizeof(*asset));
    asset->active = true;
    asset->asset_id = UINT32_C(676);
    asset->status = SHIP_ASSET_STATUS_STORED;
    asset->operator_kind = SHIP_ASSET_OPERATOR_NONE;
    asset->operator_slot = -1;
    asset->custody_station = 0;
    ASSERT(actor_principal_from_station(
        world, 0, &asset->owner_principal));
    ASSERT(ship_manifest_bootstrap(&asset->stored_ship));
    ASSERT(ship_manifest_push_with_chain(
        &asset->stored_ship, &asset_unit, NULL));

    int pod_index = find_inactive_pod(world);
    ASSERT(pod_index >= 0);
    cargo_pod_t *pod = &world->cargo_pods[pod_index];
    memset(pod, 0, sizeof(*pod));
    pod->active = true;
    pod->kind = CARGO_POD_CARGO;
    pod->commodity = COMMODITY_FRAME;
    pod->quantity = 2u;
    pod->manifest_count = 1u;
    pod->manifest_units[0] = station_unit;
    pod->has_shell_frame = true;
    pod->shell_frame = shell_unit;
    cargo_pod_set_station_custody(pod, 0);

    int shipment_index = find_inactive_shipment(world);
    ASSERT(shipment_index >= 0);
    delivery_shipment_t *shipment =
        &world->delivery_shipments[shipment_index];
    memset(shipment, 0, sizeof(*shipment));
    shipment->active = true;
    shipment->quantity_bound = 1u;
    shipment->cargo_units[0] = malformed;
    ASSERT(actor_principal_from_station(
        world, 0, &shipment->debtor_principal));

    memset(world->stations[0].station_secret,
           0x5au, sizeof(world->stations[0].station_secret));

    cargo_legacy_inventory_report_t first = {0};
    cargo_legacy_inventory_report_t second = {0};
    ASSERT(cargo_legacy_inventory_scan_world(world, &first));
    ASSERT(cargo_legacy_inventory_scan_world(world, &second));
    ASSERT(memcmp(&first, &second, sizeof(first)) == 0);
    ASSERT_EQ_INT(first.schema_version,
                  CARGO_LEGACY_INVENTORY_SCHEMA_V1);
    ASSERT_EQ_INT(
        first.legacy_candidates - baseline.legacy_candidates,
        7);
    ASSERT(!first.truncated);
    for (int holder = 0;
         holder < CARGO_LEGACY_HOLDER_COUNT; holder++) {
        ASSERT_EQ_INT(
            first.holder_candidate_count[holder] -
                baseline.holder_candidate_count[holder],
            1);
    }
    ASSERT_EQ_INT(
        first.reason_count[
            CARGO_LEGACY_REASON_EXPLICIT_REIDENTIFICATION_REQUIRED] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_EXPLICIT_REIDENTIFICATION_REQUIRED],
        7);
    ASSERT_EQ_INT(
        first.reason_count[
            CARGO_LEGACY_REASON_DUPLICATE_IDENTITY] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_DUPLICATE_IDENTITY],
        2);
    ASSERT_EQ_INT(
        first.reason_count[CARGO_LEGACY_REASON_MALFORMED_UNIT] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_MALFORMED_UNIT],
        1);
    ASSERT_EQ_INT(
        first.reason_count[
            CARGO_LEGACY_REASON_UNRESOLVED_CUSTODIAN] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_UNRESOLVED_CUSTODIAN],
        1);
    ASSERT_EQ_INT(
        first.reason_count[
            CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT],
        7);
    ASSERT(first.external_player_saves_not_enumerated);

    char output[4096] = {0};
    ASSERT(read_report_text(&first, output, sizeof(output)));
    ASSERT(strstr(output,
                  "external_player_saves=not_enumerated") != NULL);
    ASSERT(strstr(output, "delivery_pod_aliases=") != NULL);
    ASSERT(strstr(output, "delivery_npc_ship_aliases=") != NULL);
    ASSERT(strstr(output, "name=duplicate_identity count=") != NULL);
    ASSERT(strstr(output, "PLYSECRET") == NULL);
    ASSERT(strstr(output, "NPCSECRT") == NULL);
    ASSERT(strstr(output, "5a5a5a5a") == NULL);
}

TEST(test_legacy_inventory_validates_orphan_sidecars_on_empty_holders) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world->rng = 67602u;
    world_reset(world);

    cargo_legacy_inventory_report_t baseline = {0};
    ASSERT(cargo_legacy_inventory_scan_world(
        world, &baseline));
    ASSERT(world_player_ship_slot_activate(world, 0));
    ship_t *ship = world->players[0].ship;
    ASSERT(ship != NULL);
    ASSERT_EQ_INT(ship->manifest.count, 0);
    ship_receipts_t *receipts = ship_get_receipts(ship);
    ASSERT(receipts != NULL);
    ASSERT_EQ_INT(receipts->count, 0);
    ASSERT(ship_receipts_push_empty(receipts));

    cargo_legacy_inventory_report_t report = {0};
    ASSERT(cargo_legacy_inventory_scan_world(world, &report));
    ASSERT_EQ_INT(
        report.reason_count[
            CARGO_LEGACY_REASON_HOLDER_BOUNDS_INVALID] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_HOLDER_BOUNDS_INVALID],
        1);
    ASSERT_EQ_INT(
        report.legacy_candidates - baseline.legacy_candidates,
        0);

}

TEST(test_legacy_inventory_rejects_signed_but_unanchored_sidecar) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world->rng = 67607u;
    world_reset(world);

    cargo_legacy_inventory_report_t baseline = {0};
    ASSERT(cargo_legacy_inventory_scan_world(world, &baseline));

    cargo_unit_t unit = {0};
    ASSERT(make_legacy_unit(&unit, 71u, COMMODITY_FRAME));
    cargo_receipt_chain_t chain = {
        .len = 1u,
    };
    uint8_t unanchored_origin[32];
    memset(unanchored_origin, 0x3c, sizeof(unanchored_origin));
    ASSERT(cargo_receipt_issue(
        &world->stations[0], 10u,
        world->stations[0].chain_event_count + 1u,
        unit.pub, world->stations[0].station_pubkey,
        unanchored_origin, &chain.links[0]));
    ASSERT_EQ_INT(cargo_receipt_chain_verify(
                      chain.links, chain.len, unit.pub),
                  CARGO_RECEIPT_OK);
    ASSERT(station_manifest_push_with_chain(
        &world->stations[0], &unit, &chain));

    cargo_legacy_inventory_report_t report = {0};
    ASSERT(cargo_legacy_inventory_scan_world(world, &report));
    ASSERT_EQ_INT(
        report.legacy_candidates - baseline.legacy_candidates, 1);
    ASSERT_EQ_INT(
        report.reason_count[
            CARGO_LEGACY_REASON_RECEIPT_SIDECAR_MISMATCH] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_RECEIPT_SIDECAR_MISMATCH],
        1);
    ASSERT_EQ_INT(
        report.reason_count[
            CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT],
        0);
}

TEST(test_legacy_inventory_scans_shell_when_pod_manifest_is_malformed) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world->rng = 67603u;
    world_reset(world);

    cargo_legacy_inventory_report_t baseline = {0};
    ASSERT(cargo_legacy_inventory_scan_world(
        world, &baseline));
    int pod_index = find_inactive_pod(world);
    ASSERT(pod_index >= 0);
    cargo_pod_t *pod = &world->cargo_pods[pod_index];
    memset(pod, 0, sizeof(*pod));
    pod->active = true;
    pod->kind = CARGO_POD_CARGO;
    pod->manifest_count =
        (uint16_t)(CARGO_POD_MANIFEST_CAP + 1u);
    pod->has_shell_frame = true;
    ASSERT(make_legacy_unit(
        &pod->shell_frame, 31u, COMMODITY_FRAME));
    cargo_pod_set_station_custody(pod, 0);

    cargo_legacy_inventory_report_t report = {0};
    ASSERT(cargo_legacy_inventory_scan_world(world, &report));
    ASSERT_EQ_INT(
        report.legacy_candidates - baseline.legacy_candidates,
        1);
    ASSERT_EQ_INT(
        report.holder_candidate_count[
            CARGO_LEGACY_HOLDER_CARGO_POD_MANIFEST] -
            baseline.holder_candidate_count[
                CARGO_LEGACY_HOLDER_CARGO_POD_MANIFEST],
        0);
    ASSERT_EQ_INT(
        report.holder_candidate_count[
            CARGO_LEGACY_HOLDER_CARGO_POD_SHELL] -
            baseline.holder_candidate_count[
                CARGO_LEGACY_HOLDER_CARGO_POD_SHELL],
        1);
    ASSERT_EQ_INT(
        report.reason_count[
            CARGO_LEGACY_REASON_HOLDER_BOUNDS_INVALID] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_HOLDER_BOUNDS_INVALID],
        1);
    ASSERT_EQ_INT(
        report.reason_count[
            CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT],
        1);

}

TEST(test_legacy_inventory_collapses_only_exact_delivery_pod_alias) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world->rng = 67604u;
    world_reset(world);

    cargo_legacy_inventory_report_t baseline = {0};
    ASSERT(cargo_legacy_inventory_scan_world(
        world, &baseline));
    int pod_index = find_inactive_pod(world);
    int shipment_index = find_inactive_shipment(world);
    uint16_t shipment_id = unused_shipment_id(world);
    ASSERT(pod_index >= 0);
    ASSERT(shipment_index >= 0);
    ASSERT(shipment_id != 0u);

    cargo_unit_t unit = {0};
    ASSERT(make_legacy_unit(
        &unit, 41u, COMMODITY_FRAME));
    delivery_shipment_t *shipment =
        &world->delivery_shipments[shipment_index];
    memset(shipment, 0, sizeof(*shipment));
    shipment->active = true;
    shipment->shipment_id = shipment_id;
    shipment->commodity = COMMODITY_FRAME;
    shipment->quantity_total = 1u;
    shipment->quantity_bound = 1u;
    shipment->status = DELIVERY_SHIPMENT_PICKED_UP;
    shipment->cargo_units[0] = unit;
    memcpy(shipment->cargo_pub[0], unit.pub, sizeof(unit.pub));
    ASSERT(actor_principal_from_station(
        world, 0, &shipment->debtor_principal));

    cargo_pod_t *pod = &world->cargo_pods[pod_index];
    memset(pod, 0, sizeof(*pod));
    pod->active = true;
    pod->kind = CARGO_POD_CARGO;
    pod->commodity = COMMODITY_FRAME;
    pod->quantity = 1u;
    pod->manifest_count = 1u;
    pod->manifest_units[0] = unit;
    pod->shipment_id = shipment_id;

    cargo_legacy_inventory_report_t exact = {0};
    ASSERT(cargo_legacy_inventory_scan_world(world, &exact));
    ASSERT_EQ_INT(
        exact.legacy_candidates - baseline.legacy_candidates,
        2);
    ASSERT_EQ_INT(
        exact.delivery_pod_aliases -
            baseline.delivery_pod_aliases,
        1);
    ASSERT_EQ_INT(
        exact.reason_count[
            CARGO_LEGACY_REASON_DUPLICATE_IDENTITY] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_DUPLICATE_IDENTITY],
        0);
    ASSERT_EQ_INT(
        exact.reason_count[
            CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT],
        2);
    ASSERT_EQ_INT(
        exact.reason_count[
            CARGO_LEGACY_REASON_UNRESOLVED_CUSTODIAN] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_UNRESOLVED_CUSTODIAN],
        0);

    pod->quantity = 2u;
    cargo_legacy_inventory_report_t conflicting = {0};
    ASSERT(cargo_legacy_inventory_scan_world(
        world, &conflicting));
    ASSERT_EQ_INT(
        conflicting.delivery_pod_aliases -
            baseline.delivery_pod_aliases,
        0);
    ASSERT_EQ_INT(
        conflicting.reason_count[
            CARGO_LEGACY_REASON_DUPLICATE_IDENTITY] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_DUPLICATE_IDENTITY],
        2);
    pod->quantity = 1u;
    ASSERT(station_manifest_push_with_chain(
        &world->stations[0], &unit, NULL));
    cargo_legacy_inventory_report_t extra = {0};
    ASSERT(cargo_legacy_inventory_scan_world(world, &extra));
    ASSERT_EQ_INT(
        extra.delivery_pod_aliases -
            baseline.delivery_pod_aliases,
        1);
    ASSERT_EQ_INT(
        extra.reason_count[
            CARGO_LEGACY_REASON_DUPLICATE_IDENTITY] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_DUPLICATE_IDENTITY],
        3);

}

TEST(test_legacy_inventory_collapses_only_exact_delivery_npc_alias) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world->rng = 67605u;
    world_reset(world);

    cargo_legacy_inventory_report_t baseline = {0};
    ASSERT(cargo_legacy_inventory_scan_world(
        world, &baseline));
    int npc_index = find_inactive_npc(world);
    int shipment_index = find_inactive_shipment(world);
    uint16_t shipment_id = unused_shipment_id(world);
    ASSERT(npc_index >= 0);
    ASSERT(shipment_index >= 0);
    ASSERT(shipment_id != 0u);
    ASSERT(world_npc_ship_slot_activate(world, npc_index));
    npc_ship_t *npc = &world->npc_ships[npc_index];
    npc->active = true;
    memset(npc->session_token, 0xa5,
           sizeof(npc->session_token));
    npc->session_token[0] = (uint8_t)npc_index;
    ship_t *ship = world_npc_ship_for(world, npc_index);
    ASSERT(ship != NULL);

    cargo_unit_t unit = {0};
    ASSERT(make_legacy_unit(
        &unit, 51u, COMMODITY_REPAIR_KIT));
    ASSERT(ship_manifest_push_with_chain(
        ship, &unit, NULL));

    delivery_shipment_t *shipment =
        &world->delivery_shipments[shipment_index];
    memset(shipment, 0, sizeof(*shipment));
    shipment->active = true;
    shipment->shipment_id = shipment_id;
    shipment->commodity = COMMODITY_REPAIR_KIT;
    shipment->quantity_total = 1u;
    shipment->quantity_bound = 1u;
    shipment->status = DELIVERY_SHIPMENT_PICKED_UP;
    shipment->cargo_units[0] = unit;
    memcpy(shipment->cargo_pub[0], unit.pub, sizeof(unit.pub));
    ASSERT(delivery_ownership_assign_npc(
        shipment, world, npc_index));

    cargo_legacy_inventory_report_t exact = {0};
    ASSERT(cargo_legacy_inventory_scan_world(world, &exact));
    ASSERT_EQ_INT(
        exact.legacy_candidates - baseline.legacy_candidates,
        2);
    ASSERT_EQ_INT(
        exact.delivery_npc_ship_aliases -
            baseline.delivery_npc_ship_aliases,
        1);
    ASSERT_EQ_INT(
        exact.reason_count[
            CARGO_LEGACY_REASON_DUPLICATE_IDENTITY] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_DUPLICATE_IDENTITY],
        0);
    ASSERT_EQ_INT(
        exact.reason_count[
            CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT],
        2);

    shipment->cargo_chains[0].len = 1u;
    cargo_legacy_inventory_report_t conflicting = {0};
    ASSERT(cargo_legacy_inventory_scan_world(
        world, &conflicting));
    ASSERT_EQ_INT(
        conflicting.delivery_npc_ship_aliases -
            baseline.delivery_npc_ship_aliases,
        0);
    ASSERT_EQ_INT(
        conflicting.reason_count[
            CARGO_LEGACY_REASON_DUPLICATE_IDENTITY] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_DUPLICATE_IDENTITY],
        2);
    ASSERT_EQ_INT(
        conflicting.reason_count[
            CARGO_LEGACY_REASON_RECEIPT_SIDECAR_MISMATCH] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_RECEIPT_SIDECAR_MISMATCH],
        1);

    memset(&shipment->cargo_chains[0], 0,
           sizeof(shipment->cargo_chains[0]));
    ASSERT(station_manifest_push_with_chain(
        &world->stations[0], &unit, NULL));
    cargo_legacy_inventory_report_t extra = {0};
    ASSERT(cargo_legacy_inventory_scan_world(world, &extra));
    ASSERT_EQ_INT(
        extra.delivery_npc_ship_aliases -
            baseline.delivery_npc_ship_aliases,
        1);
    ASSERT_EQ_INT(
        extra.reason_count[
            CARGO_LEGACY_REASON_DUPLICATE_IDENTITY] -
            baseline.reason_count[
                CARGO_LEGACY_REASON_DUPLICATE_IDENTITY],
        3);

}

void register_cargo_legacy_inventory_tests(void);
void register_cargo_legacy_inventory_tests(void) {
    TEST_SECTION("\n--- Legacy cargo inventory (#676 bounded slice) ---\n");
    RUN(test_legacy_inventory_codes_and_names_are_stable);
    RUN(test_legacy_inventory_covers_serialized_holders_deterministically);
    RUN(test_legacy_inventory_validates_orphan_sidecars_on_empty_holders);
    RUN(test_legacy_inventory_rejects_signed_but_unanchored_sidecar);
    RUN(test_legacy_inventory_scans_shell_when_pod_manifest_is_malformed);
    RUN(test_legacy_inventory_collapses_only_exact_delivery_pod_alias);
    RUN(test_legacy_inventory_collapses_only_exact_delivery_npc_alias);
}
