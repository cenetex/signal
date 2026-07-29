#include "test_harness.h"

#include "actor_principal_resolver.h"
#include "contract_ownership.h"

static void ownership_test_verified_player(
    server_player_t *player,
    uint8_t id,
    uint8_t key_tag) {
    ASSERT(player != NULL);
    player->id = id;
    player->connected = true;
    player->session_ready = true;
    player->pubkey_set = true;
    player->pubkey_proof_ok = true;
    player->pubkey_challenge_consumed = true;
    player->pubkey_identity_finalized = true;
    for (size_t i = 0; i < sizeof(player->session_token); i++)
        player->session_token[i] = (uint8_t)(key_tag + i + 1u);
    for (size_t i = 0; i < sizeof(player->pubkey); i++)
        player->pubkey[i] = (uint8_t)(key_tag + i + 17u);
}

static void ownership_test_npc(
    npc_ship_t *npc,
    uint8_t token_tag) {
    ASSERT(npc != NULL);
    npc->active = true;
    for (size_t i = 0; i < sizeof(npc->session_token); i++)
        npc->session_token[i] = (uint8_t)(token_tag + i + 1u);
}

static bool ownership_test_files_equal(
    const char *left_path,
    const char *right_path) {
    FILE *left = fopen(left_path, "rb");
    FILE *right = fopen(right_path, "rb");
    if (!left || !right) {
        if (left) fclose(left);
        if (right) fclose(right);
        return false;
    }
    bool equal = true;
    for (;;) {
        uint8_t left_buf[4096];
        uint8_t right_buf[4096];
        size_t left_len = fread(
            left_buf, 1, sizeof(left_buf), left);
        size_t right_len = fread(
            right_buf, 1, sizeof(right_buf), right);
        if (left_len != right_len ||
            memcmp(left_buf, right_buf, left_len) != 0) {
            equal = false;
            break;
        }
        if (left_len < sizeof(left_buf)) {
            if (ferror(left) || ferror(right))
                equal = false;
            break;
        }
    }
    if (fclose(left) != 0) equal = false;
    if (fclose(right) != 0) equal = false;
    return equal;
}

TEST(test_contract_claim_requires_proven_identity_and_is_byte_inert) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);
    contract_t contract = {
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 0,
        .commodity = COMMODITY_FERRITE_ORE,
        .target_index = -1,
        .claimed_by = -1,
    };
    server_player_t *pending = &world->players[4];
    pending->id = 4;
    pending->connected = true;
    pending->session_ready = true;
    pending->pubkey_set = true;
    pending->pubkey[0] = 0x41;

    contract_t before = contract;
    ASSERT(!contract_ownership_try_claim_player(
        &contract, world, 4));
    ASSERT(memcmp(&contract, &before, sizeof(contract)) == 0);

    before = contract;
    ASSERT(!contract_ownership_try_claim_player(
        &contract, world, -1));
    ASSERT(memcmp(&contract, &before, sizeof(contract)) == 0);
    ASSERT(!contract_ownership_try_claim_player(
        &contract, world, MAX_PLAYERS));
    ASSERT(memcmp(&contract, &before, sizeof(contract)) == 0);
    delivery_shipment_t staged_shipment = {0};
    delivery_shipment_t shipment_before = staged_shipment;
    ASSERT(!delivery_ownership_assign_player(
        &staged_shipment, world, -1));
    ASSERT(memcmp(
        &staged_shipment, &shipment_before,
        sizeof(staged_shipment)) == 0);
    ASSERT(!delivery_ownership_assign_player(
        &staged_shipment, world, MAX_PLAYERS));
    ASSERT(memcmp(
        &staged_shipment, &shipment_before,
        sizeof(staged_shipment)) == 0);

    ownership_test_verified_player(
        &world->players[5], 4, 0x62);
    ASSERT(!contract_ownership_try_claim_player(
        &contract, world, 5));
    ASSERT(memcmp(&contract, &before, sizeof(contract)) == 0);

    ownership_test_verified_player(pending, 4, 0x41);
    ASSERT(contract_ownership_try_claim_player(
        &contract, world, 4));
    ASSERT_EQ_INT(
        contract.claimed_by_principal.kind,
        ACTOR_PRINCIPAL_PLAYER);
    actor_principal_t stable_owner =
        contract.claimed_by_principal;

    server_player_clear_live_session_identity(pending);
    server_player_t *reconnected =
        &world->players[19];
    ownership_test_verified_player(
        reconnected, 19, 0x41);
    ASSERT(contract_ownership_matches_player(
        &contract, world, 19));
    ASSERT(contract_ownership_try_claim_player(
        &contract, world, 19));
    ASSERT(actor_principal_equal(
        &contract.claimed_by_principal,
        &stable_owner));
    ASSERT_EQ_INT(contract.claimed_by, 19);

    server_player_t *reused_slot =
        &world->players[4];
    ownership_test_verified_player(
        reused_slot, 4, 0x73);
    before = contract;
    ASSERT(!contract_ownership_matches_player(
        &contract, world, 4));
    ASSERT(!contract_ownership_try_claim_player(
        &contract, world, 4));
    ASSERT(memcmp(&contract, &before, sizeof(contract)) == 0);
}

TEST(test_player_resolution_tracks_grace_and_key_not_slot) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);

    server_player_t *original = &world->players[2];
    ownership_test_verified_player(original, 2, 0x22);
    original->grace_period = true;
    actor_principal_t principal = actor_principal_none();
    ASSERT(actor_principal_from_verified_player(
        original, &principal));

    actor_resolution_result_t result =
        world_resolve_player_principal(world, &principal);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_GRACE);
    ASSERT_EQ_INT(result.slot, 2);

    ownership_test_verified_player(original, 2, 0x93);
    original->grace_period = false;
    server_player_t *reconnected = &world->players[11];
    ownership_test_verified_player(reconnected, 11, 0x22);
    result = world_resolve_player_principal(world, &principal);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_ONLINE);
    ASSERT_EQ_INT(result.slot, 11);

    reconnected->connected = false;
    result = world_resolve_player_principal(world, &principal);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_OFFLINE);
    ASSERT_EQ_INT(result.slot, -1);
}

TEST(test_npc_delivery_principal_survives_slot_move_and_offline) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);
    delivery_shipment_t shipment = {
        .active = true,
        .debtor_player = UINT8_MAX,
    };
    npc_ship_t *original = &world->npc_ships[3];
    ownership_test_npc(original, 0x35);
    delivery_shipment_t before = shipment;
    ASSERT(!delivery_ownership_assign_npc(
        &shipment, world, -1));
    ASSERT(memcmp(&shipment, &before, sizeof(shipment)) == 0);
    ASSERT(!delivery_ownership_assign_npc(
        &shipment, world, MAX_NPC_SHIPS));
    ASSERT(memcmp(&shipment, &before, sizeof(shipment)) == 0);
    ASSERT(delivery_ownership_assign_npc(
        &shipment, world, 3));
    actor_principal_t stable_debtor =
        shipment.debtor_principal;

    ASSERT(delivery_ownership_matches_npc(
        &shipment, world, 3));
    ASSERT(actor_principal_equal(
        &shipment.debtor_principal,
        &stable_debtor));

    uint8_t original_token[8];
    memcpy(original_token, original->session_token,
           sizeof(original_token));
    ownership_test_npc(&world->npc_ships[3], 0x77);
    ASSERT(!delivery_ownership_matches_npc(
        &shipment, world, 3));
    memcpy(world->npc_ships[3].session_token,
           original_token, sizeof(original_token));

    actor_resolution_result_t result =
        world_resolve_npc_principal(
            world, &stable_debtor);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_ONLINE);
    ASSERT_EQ_INT(result.slot, 3);

    world->npc_ships[3].active = false;
    result = world_resolve_npc_principal(
        world, &stable_debtor);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_OFFLINE);
    ASSERT_EQ_INT(result.slot, -1);

    world->npc_ships[17].active = true;
    memcpy(world->npc_ships[17].session_token,
           original_token, sizeof(original_token));
    result = world_resolve_npc_principal(
        world, &stable_debtor);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_ONLINE);
    ASSERT_EQ_INT(result.slot, 17);
}

TEST(test_slot_rebind_repairs_stale_projections_from_principals) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);

    ownership_test_verified_player(
        &world->players[3], 3, 0x44);
    ownership_test_npc(&world->npc_ships[5], 0x71);

    world->contracts[0] = (contract_t){
        .active = true,
        .claimed_by = -1,
    };
    ASSERT(contract_ownership_try_claim_npc(
        &world->contracts[0], world, 5));
    world->contracts[0].claimed_by = 3; /* stale/corrupt coincidence */

    world->delivery_shipments[0] =
        (delivery_shipment_t){.active = true};
    ASSERT(delivery_ownership_assign_npc(
        &world->delivery_shipments[0], world, 5));
    world->delivery_shipments[0].debtor_player = 3;

    world->contracts[1] = (contract_t){
        .active = true,
        .claimed_by = -1,
    };
    ASSERT(contract_ownership_try_claim_player(
        &world->contracts[1], world, 3));
    world->delivery_shipments[1] =
        (delivery_shipment_t){.active = true};
    ASSERT(delivery_ownership_assign_player(
        &world->delivery_shipments[1], world, 3));

    ASSERT(world_rebind_player_slot_refs(world, 9, 3));
    ASSERT_EQ_INT(
        world->contracts[0].claimed_by,
        MAX_PLAYERS + 5);
    ASSERT_EQ_INT(
        world->delivery_shipments[0].debtor_player,
        MAX_PLAYERS + 5);
    ASSERT_EQ_INT(world->contracts[1].claimed_by, 9);
    ASSERT_EQ_INT(
        world->delivery_shipments[1].debtor_player, 9);
}

TEST(test_duplicate_actor_identities_fail_closed_without_mutation) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);

    ownership_test_verified_player(
        &world->players[4], 4, 0x58);
    ownership_test_verified_player(
        &world->players[5], 5, 0x58);
    contract_t player_contract = {
        .active = true,
        .claimed_by = -1,
    };
    contract_t contract_before = player_contract;
    ASSERT(!contract_ownership_try_claim_player(
        &player_contract, world, 4));
    ASSERT(memcmp(
        &player_contract, &contract_before,
        sizeof(player_contract)) == 0);
    delivery_shipment_t player_shipment = {.active = true};
    delivery_shipment_t shipment_before = player_shipment;
    ASSERT(!delivery_ownership_assign_player(
        &player_shipment, world, 4));
    ASSERT(memcmp(
        &player_shipment, &shipment_before,
        sizeof(player_shipment)) == 0);

    world->players[5].pubkey[0] ^= 0x80u;
    ASSERT(contract_ownership_try_claim_player(
        &player_contract, world, 4));
    ASSERT(delivery_ownership_assign_player(
        &player_shipment, world, 4));
    memcpy(world->players[5].pubkey,
           world->players[4].pubkey,
           sizeof(world->players[5].pubkey));
    ASSERT(!contract_ownership_matches_player(
        &player_contract, world, 4));
    ASSERT(!contract_ownership_matches_player(
        &player_contract, world, 5));
    ASSERT(!delivery_ownership_matches_player(
        &player_shipment, world, 4));

    ownership_test_npc(&world->npc_ships[6], 0x69);
    ownership_test_npc(&world->npc_ships[7], 0x69);
    contract_t npc_contract = {
        .active = true,
        .claimed_by = -1,
    };
    contract_before = npc_contract;
    ASSERT(!contract_ownership_try_claim_npc(
        &npc_contract, world, 6));
    ASSERT(memcmp(
        &npc_contract, &contract_before,
        sizeof(npc_contract)) == 0);
    delivery_shipment_t npc_shipment = {.active = true};
    shipment_before = npc_shipment;
    ASSERT(!delivery_ownership_assign_npc(
        &npc_shipment, world, 6));
    ASSERT(memcmp(
        &npc_shipment, &shipment_before,
        sizeof(npc_shipment)) == 0);

    world->npc_ships[7].active = false;
    ASSERT(contract_ownership_try_claim_npc(
        &npc_contract, world, 6));
    ASSERT(delivery_ownership_assign_npc(
        &npc_shipment, world, 6));
    world->npc_ships[7].active = true;
    ASSERT(!contract_ownership_matches_npc(
        &npc_contract, world, 6));
    ASSERT(!delivery_ownership_matches_npc(
        &npc_shipment, world, 7));
}

TEST(test_principal_ownership_roundtrip_is_deterministic) {
    const char *baseline_path =
        TMP("contract_owner_baseline.sav");
    const char *first_path =
        TMP("contract_owner_current_a.sav");
    const char *second_path =
        TMP("contract_owner_current_b.sav");
    WORLD_HEAP baseline = calloc(1, sizeof(*baseline));
    ASSERT(baseline != NULL);
    world_reset(baseline);

    /*
     * world_reset() includes genesis ship assets whose compatibility
     * projections are normalized by the first load.  Start from that
     * canonical loaded state so this byte-for-byte assertion isolates
     * ownership persistence instead of testing unrelated asset
     * projection normalization.
     */
    ASSERT(world_save(baseline, baseline_path));
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);
    ASSERT(world_load(world, baseline_path));
    remove(baseline_path);

    server_player_t *player = &world->players[7];
    ownership_test_verified_player(player, 7, 0x52);
    world->contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 1,
        .commodity = COMMODITY_FRAME,
        .target_index = 0,
        .claimed_by = -1,
    };
    ASSERT(contract_ownership_try_claim_player(
        &world->contracts[0], world, 7));

    world->delivery_shipments[0] =
        (delivery_shipment_t){
            .active = true,
            .shipment_id = 91,
            .origin_station = 0,
            .destination_station = 1,
            .contract_index = 0,
            .debtor_player = UINT8_MAX,
            .commodity = COMMODITY_FRAME,
            .status = DELIVERY_SHIPMENT_DEFAULTED,
        };
    ASSERT(delivery_ownership_assign_player(
        &world->delivery_shipments[0], world, 7));

    actor_principal_t claimant =
        world->contracts[0].claimed_by_principal;
    actor_principal_t debtor =
        world->delivery_shipments[0].debtor_principal;
    /*
     * Ownership must remain deterministic while the actor is offline.
     * Clear the synthetic live slot before saving so this fixture does
     * not accidentally exercise an uninitialized player/ship-asset bind.
     */
    server_player_clear_live_session_identity(player);
    player->connected = false;
    player->grace_period = false;
    ASSERT(world_save(world, first_path));

    WORLD_HEAP loaded = calloc(1, sizeof(*loaded));
    ASSERT(loaded != NULL);
    world_reset(loaded);
    ASSERT(world_load(loaded, first_path));
    ASSERT(actor_principal_equal(
        &loaded->contracts[0].claimed_by_principal,
        &claimant));
    ASSERT(actor_principal_equal(
        &loaded->delivery_shipments[0].debtor_principal,
        &debtor));
    ASSERT_EQ_INT(loaded->contracts[0].claimed_by, -1);
    ASSERT_EQ_INT(
        loaded->delivery_shipments[0].debtor_player,
        UINT8_MAX);
    ASSERT(world_save(loaded, second_path));
    ASSERT(ownership_test_files_equal(
        first_path, second_path));

    remove(first_path);
    remove(second_path);
}

#if defined(SIGNAL_SAVE_TESTING)
TEST(test_v80_player_slots_migrate_to_bound_quarantine) {
    const char *path =
        TMP("contract_owner_legacy_v80.sav");
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);
    server_player_t writer_identity = {0};
    ownership_test_verified_player(
        &writer_identity, 3, 0x31);
    actor_principal_t writer_principal =
        actor_principal_none();
    ASSERT(actor_principal_from_verified_player(
        &writer_identity, &writer_principal));

    world->contracts[4] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 0,
        .commodity = COMMODITY_FERRITE_ORE,
        .target_index = -1,
        .claimed_by = 3,
    };
    world->delivery_shipments[6] =
        (delivery_shipment_t){
            .active = true,
            .shipment_id = 17,
            .origin_station = 0,
            .destination_station = 1,
            .contract_index = 4,
            .debtor_player = 3,
            .debtor_principal = writer_principal,
            .commodity = COMMODITY_FERRITE_ORE,
            .status = DELIVERY_SHIPMENT_PICKED_UP,
        };
    ASSERT(world_save_legacy_v80_for_test(
        world, path));

    WORLD_HEAP loaded = calloc(1, sizeof(*loaded));
    ASSERT(loaded != NULL);
    world_reset(loaded);
    ASSERT(world_load(loaded, path));
    const contract_t *contract = &loaded->contracts[4];
    const delivery_shipment_t *shipment =
        &loaded->delivery_shipments[6];
    ASSERT_EQ_INT(
        contract->claimed_by_principal.kind,
        ACTOR_PRINCIPAL_NONE);
    ASSERT(contract->claimed_by_quarantine_record_id != 0);
    ASSERT(!contract_ownership_is_open(contract));
    ASSERT_EQ_INT(
        shipment->debtor_principal.kind,
        ACTOR_PRINCIPAL_NONE);
    ASSERT(shipment->debtor_quarantine_record_id != 0);
    ASSERT_EQ_INT(
        loaded->ownership_quarantine.count, 2);
    ASSERT_EQ_INT(
        loaded->ownership_quarantine.entries[0].source_kind,
        OWNERSHIP_QUARANTINE_SOURCE_CONTRACT);
    ASSERT_EQ_INT(
        loaded->ownership_quarantine.entries[0].legacy_actor_code,
        3);
    ASSERT_EQ_INT(
        loaded->ownership_quarantine.entries[1].source_kind,
        OWNERSHIP_QUARANTINE_SOURCE_DELIVERY_SHIPMENT);

    server_player_t *future_slot =
        &loaded->players[3];
    ownership_test_verified_player(
        future_slot, 3, 0x91);
    contract_t before = *contract;
    ASSERT(!contract_ownership_try_claim_player(
        &loaded->contracts[4], loaded, 3));
    ASSERT(memcmp(
        &loaded->contracts[4], &before,
        sizeof(before)) == 0);
    ASSERT(!delivery_ownership_matches_player(
        shipment, loaded, 3));

    remove(path);
}

TEST(test_v80_npc_codes_migrate_without_slot_authority) {
    const char *path =
        TMP("contract_owner_legacy_npc_v80.sav");
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);
    int npc_slot = 4;
    ownership_test_npc(
        &world->npc_ships[npc_slot], 0x63);
    int legacy_code = MAX_PLAYERS + npc_slot;
    actor_principal_t writer_principal =
        actor_principal_none();
    ASSERT(actor_principal_from_npc(
        &world->npc_ships[npc_slot],
        &writer_principal));
    world->contracts[2] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 1,
        .commodity = COMMODITY_FRAME,
        .target_index = 0,
        .claimed_by = (int8_t)legacy_code,
    };
    world->delivery_shipments[2] =
        (delivery_shipment_t){
            .active = true,
            .shipment_id = 22,
            .origin_station = 0,
            .destination_station = 1,
            .contract_index = 2,
            .debtor_player = (uint8_t)legacy_code,
            .debtor_principal = writer_principal,
            .commodity = COMMODITY_FRAME,
            .status = DELIVERY_SHIPMENT_DELIVERED,
        };
    ASSERT(world_save_legacy_v80_for_test(
        world, path));

    WORLD_HEAP loaded = calloc(1, sizeof(*loaded));
    ASSERT(loaded != NULL);
    world_reset(loaded);
    ASSERT(world_load(loaded, path));
    ASSERT_EQ_INT(
        loaded->contracts[2].claimed_by_principal.kind,
        ACTOR_PRINCIPAL_NPC);
    ASSERT_EQ_INT(
        loaded->delivery_shipments[2].debtor_principal.kind,
        ACTOR_PRINCIPAL_NPC);
    ASSERT_EQ_INT(
        loaded->contracts[2].claimed_by_quarantine_record_id,
        0);
    ASSERT_EQ_INT(
        loaded->delivery_shipments[2].debtor_quarantine_record_id,
        0);
    ASSERT(delivery_ownership_matches_npc(
        &loaded->delivery_shipments[2],
        loaded, npc_slot));

    remove(path);
}

TEST(test_v80_duplicate_npc_tokens_quarantine_contract_and_delivery) {
    const char *path =
        TMP("contract_owner_legacy_duplicate_npc_v80.sav");
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);
    int npc_slot = 4;
    int duplicate_slot = 5;
    ownership_test_npc(
        &world->npc_ships[npc_slot], 0x74);
    ownership_test_npc(
        &world->npc_ships[duplicate_slot], 0x74);
    int legacy_code = MAX_PLAYERS + npc_slot;
    actor_principal_t writer_principal =
        actor_principal_none();
    ASSERT(actor_principal_from_npc(
        &world->npc_ships[npc_slot],
        &writer_principal));

    world->contracts[3] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 1,
        .commodity = COMMODITY_FRAME,
        .target_index = 0,
        .claimed_by = (int8_t)legacy_code,
    };
    world->delivery_shipments[3] =
        (delivery_shipment_t){
            .active = true,
            .shipment_id = 23,
            .origin_station = 0,
            .destination_station = 1,
            .contract_index = 3,
            .debtor_player = (uint8_t)legacy_code,
            .debtor_principal = writer_principal,
            .commodity = COMMODITY_FRAME,
            .status = DELIVERY_SHIPMENT_DELIVERED,
        };
    ASSERT(world_save_legacy_v80_for_test(
        world, path));

    WORLD_HEAP loaded = calloc(1, sizeof(*loaded));
    ASSERT(loaded != NULL);
    world_reset(loaded);
    ASSERT(world_load(loaded, path));

    contract_t *contract = &loaded->contracts[3];
    delivery_shipment_t *shipment =
        &loaded->delivery_shipments[3];
    ASSERT_EQ_INT(
        contract->claimed_by_principal.kind,
        ACTOR_PRINCIPAL_NONE);
    ASSERT_EQ_INT(
        shipment->debtor_principal.kind,
        ACTOR_PRINCIPAL_NONE);
    ASSERT(contract->claimed_by_quarantine_record_id != 0);
    ASSERT(shipment->debtor_quarantine_record_id != 0);
    ASSERT(
        contract->claimed_by_quarantine_record_id !=
        shipment->debtor_quarantine_record_id);
    ASSERT_EQ_INT(contract->claimed_by, -1);
    ASSERT_EQ_INT(shipment->debtor_player, UINT8_MAX);

    ASSERT_EQ_INT(loaded->ownership_quarantine.count, 2);
    ASSERT_EQ_INT(
        loaded->ownership_quarantine.entries[0].source_kind,
        OWNERSHIP_QUARANTINE_SOURCE_CONTRACT);
    ASSERT_EQ_INT(
        loaded->ownership_quarantine.entries[0].reason,
        OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL);
    ASSERT_EQ_INT(
        loaded->ownership_quarantine.entries[1].source_kind,
        OWNERSHIP_QUARANTINE_SOURCE_DELIVERY_SHIPMENT);
    ASSERT_EQ_INT(
        loaded->ownership_quarantine.entries[1].reason,
        OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL);

    ASSERT(!contract_ownership_matches_npc(
        contract, loaded, npc_slot));
    ASSERT(!contract_ownership_matches_npc(
        contract, loaded, duplicate_slot));
    ASSERT(!delivery_ownership_matches_npc(
        shipment, loaded, npc_slot));
    ASSERT(!delivery_ownership_matches_npc(
        shipment, loaded, duplicate_slot));

    contract_t contract_before = *contract;
    delivery_shipment_t shipment_before = *shipment;
    ASSERT(!contract_ownership_try_claim_npc(
        contract, loaded, npc_slot));
    ASSERT(!contract_ownership_try_claim_npc(
        contract, loaded, duplicate_slot));
    ASSERT(!delivery_ownership_assign_npc(
        shipment, loaded, npc_slot));
    ASSERT(!delivery_ownership_assign_npc(
        shipment, loaded, duplicate_slot));
    ASSERT(memcmp(
        contract, &contract_before,
        sizeof(*contract)) == 0);
    ASSERT(memcmp(
        shipment, &shipment_before,
        sizeof(*shipment)) == 0);

    remove(path);
}
#endif

TEST(test_station_and_unattributed_ownership_fail_closed) {
    contract_t contract = {
        .claimed_by = -1,
        .claimed_by_principal =
            {.kind = ACTOR_PRINCIPAL_UNATTRIBUTED},
    };
    ASSERT(!contract_ownership_is_valid(&contract));
    ASSERT(!contract_ownership_is_open(&contract));

    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);
    actor_principal_t station = actor_principal_none();
    ASSERT(actor_principal_from_station(
        world, 0, &station));
    contract.claimed_by_principal = station;
    ASSERT(!contract_ownership_is_valid(&contract));
    world->contracts[0] = contract;
    ASSERT(!world_save(
        world, TMP("malformed_station_contract_owner.sav")));
    remove(TMP("malformed_station_contract_owner.sav"));

    delivery_shipment_t shipment = {
        .active = true,
        .debtor_principal =
            {.kind = ACTOR_PRINCIPAL_UNATTRIBUTED},
    };
    ASSERT(!delivery_ownership_is_valid(&shipment));
    world->contracts[0] = (contract_t){
        .claimed_by = -1,
    };
    world->delivery_shipments[0] = shipment;
    ASSERT(!world_save(
        world, TMP("malformed_unattributed_debtor.sav")));
    remove(TMP("malformed_unattributed_debtor.sav"));

    actor_resolution_result_t result =
        world_resolve_actor_principal(world, &station);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_ONLINE);
    ASSERT_EQ_INT(result.slot, 0);
}

void register_contract_ownership_tests(void) {
    TEST_SECTION("\nDurable contract/delivery ownership:\n");
    RUN(test_contract_claim_requires_proven_identity_and_is_byte_inert);
    RUN(test_player_resolution_tracks_grace_and_key_not_slot);
    RUN(test_npc_delivery_principal_survives_slot_move_and_offline);
    RUN(test_slot_rebind_repairs_stale_projections_from_principals);
    RUN(test_duplicate_actor_identities_fail_closed_without_mutation);
    RUN(test_principal_ownership_roundtrip_is_deterministic);
#if defined(SIGNAL_SAVE_TESTING)
    RUN(test_v80_player_slots_migrate_to_bound_quarantine);
    RUN(test_v80_npc_codes_migrate_without_slot_authority);
    RUN(test_v80_duplicate_npc_tokens_quarantine_contract_and_delivery);
#endif
    RUN(test_station_and_unattributed_ownership_fail_closed);
}
