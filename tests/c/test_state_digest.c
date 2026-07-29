#include "test_harness.h"
#include "actor_principal_resolver.h"
#include "cargo_receipt_issue.h"
#include "state_digest.h"
#include "station_authority.h"

static void state_root(const world_t *world,
                       uint8_t out[SIGNAL_AUTH_STATE_DIGEST_SIZE])
{
    signal_authoritative_state_digest(world, out);
}

static bool roots_equal(
    const uint8_t a[SIGNAL_AUTH_STATE_DIGEST_SIZE],
    const uint8_t b[SIGNAL_AUTH_STATE_DIGEST_SIZE])
{
    return memcmp(a, b, SIGNAL_AUTH_STATE_DIGEST_SIZE) == 0;
}

TEST(test_state_digest_reports_versioned_schema_and_is_repeatable)
{
    WORLD_DECL;
    uint8_t first[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t second[SIGNAL_AUTH_STATE_DIGEST_SIZE];

    world_reset(&w);
    ASSERT_STR_EQ(signal_authoritative_state_digest_schema(),
                  "signal.authoritative_state.v3");
    ASSERT_EQ_INT((int)signal_authoritative_state_digest_version(), 3);

    state_root(&w, first);
    state_root(&w, second);
    ASSERT(roots_equal(first, second));
}

TEST(test_state_digest_commits_each_authoritative_domain)
{
    WORLD_DECL;
    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];

    world_reset(&w);
    ASSERT(world_player_ship_slot_activate(&w, 0));

    state_root(&w, before);
    w.rng++;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    memcpy(before, after, sizeof(before));
    w.stations[0].base_price[COMMODITY_FERRITE_ORE] += 1.0f;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    memcpy(before, after, sizeof(before));
    w.ships[WORLD_PLAYER_SHIP_BASE].component.mining_level++;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.asteroids[0].active = true;
    w.asteroids[0].max_hp = 10.0f;
    state_root(&w, before);
    w.asteroids[0].max_hp = 11.0f;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.tow_links[0].active = true;
    w.tow_links[0].state = TOW_LINK_CAPTURE;
    state_root(&w, before);
    w.tow_links[0].state = TOW_LINK_HELD;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.cargo_pods[0].active = true;
    w.cargo_pods[0].radius = 20.0f;
    state_root(&w, before);
    w.cargo_pods[0].radius = 21.0f;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.delivery_shipments[0].active = true;
    w.delivery_shipments[0].debt_principal = 100.0f;
    state_root(&w, before);
    w.delivery_shipments[0].debt_principal = 101.0f;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.stations[0].hnn_market_memory.store[0] = 0.25f;
    state_root(&w, before);
    w.stations[0].hnn_market_memory.store[0] = 0.50f;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.signal_channel.count = 1;
    w.signal_channel.head = 1;
    w.signal_channel.next_id = 2;
    w.signal_channel.msgs[0].id = 1;
    w.signal_channel.msgs[0].text_len = 1;
    w.signal_channel.msgs[0].text[0] = 'a';
    state_root(&w, before);
    w.signal_channel.msgs[0].text[0] = 'b';
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.players[0].movement_queue_count = 1;
    w.players[0].movement_queue[0].apply_tick = w.tick + 1;
    w.players[0].movement_queue[0].input_seq = 1;
    state_root(&w, before);
    w.players[0].movement_queue[0].intent.thrust = 1.0f;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.pubkey_registry[0].in_use = true;
    w.pubkey_registry[0].pubkey[0] = 1;
    state_root(&w, before);
    w.pubkey_registry[0].pubkey[0] = 2;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));
}

TEST(test_state_digest_tow_links_are_pool_permutation_invariant)
{
    WORLD_DECL;
    uint8_t first[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t permuted[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t transient_changed[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t relation_changed[SIGNAL_AUTH_STATE_DIGEST_SIZE];

    world_reset(&w);
    memset(w.tow_links, 0, sizeof(w.tow_links));
    tow_link_t first_link = {
        .active = true,
        .source = {
            .kind = ENTITY_KIND_SHIP,
            .index = 2,
            .part = -1,
            .generation = 4,
        },
        .target = {
            .kind = ENTITY_KIND_CARGO_POD,
            .index = 8,
            .part = -1,
            .generation = 7,
        },
        .profile = TOW_PROFILE_SHIP_POD,
        .slot = 0,
        .state = TOW_LINK_HELD,
        .attached_tick = 100,
        .revision = 9,
    };
    tow_link_t second_link = first_link;
    second_link.target.index = 9;
    second_link.target.generation = 3;
    second_link.slot = 1;
    second_link.attached_tick = 101;
    second_link.revision = 10;
    w.tow_links[7] = first_link;
    w.tow_links[401] = second_link;
    state_root(&w, first);

    memset(w.tow_links, 0, sizeof(w.tow_links));
    w.tow_links[3] = second_link;
    w.tow_links[510] = first_link;
    state_root(&w, permuted);
    ASSERT(roots_equal(first, permuted));

    w.tow_links[510].attached_tick++;
    w.tow_links[510].revision++;
    state_root(&w, transient_changed);
    ASSERT(roots_equal(permuted, transient_changed));

    w.tow_links[510].target.generation++;
    state_root(&w, relation_changed);
    ASSERT(!roots_equal(transient_changed, relation_changed));
}

TEST(test_state_digest_commits_current_trust_ownership_and_charge_state)
{
    WORLD_DECL;
    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t principal_id[ACTOR_PRINCIPAL_ID_SIZE] = {0};
    uint8_t denied_authority[32] = {0};

    world_reset(&w);
    ASSERT(world_player_ship_slot_activate(&w, 0));

    state_root(&w, before);
    w.stations[0].station_actor_id[0] ^= 0x01u;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    memcpy(before, after, sizeof(before));
    w.stations[0].station_actor_catalog_attested =
        !w.stations[0].station_actor_catalog_attested;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    denied_authority[0] = 0x5au;
    memcpy(before, after, sizeof(before));
    ASSERT(station_authority_registry_set_trust(
        &w.stations[0], denied_authority,
        CARGO_RECEIPT_AUTHORITY_UNTRUSTED));
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    memcpy(before, after, sizeof(before));
    w.stations[0].chain_append_blocked =
        !w.stations[0].chain_append_blocked;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    principal_id[0] = 0x31u;
    pending_ship_build_t *build = &w.stations[0].pending_ship_builds[0];
    memset(build, 0, sizeof(*build));
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, principal_id, &build->owner_principal));
    build->hull_class = HULL_CLASS_MINER;
    build->mode = PENDING_SHIP_BUILD_MODE_MATERIAL;
    build->build_progress = 0.25f;
    w.stations[0].pending_ship_build_count = 1;
    state_root(&w, before);
    build->owner_principal.id[0] ^= 0x01u;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    memcpy(before, after, sizeof(before));
    build->mode = PENDING_SHIP_BUILD_MODE_BIRTH_ASSEMBLY;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    build->owner_principal = actor_principal_none();
    build->owner_quarantine_record_id = 7;
    build->mode_quarantine_record_id = 8;
    build->mode = PENDING_SHIP_BUILD_MODE_UNKNOWN;
    state_root(&w, before);
    build->owner_quarantine_record_id++;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    int asset_index = -1;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        if (!w.ship_assets[i].active) {
            asset_index = i;
            break;
        }
    }
    ASSERT(asset_index >= 0);
    ship_asset_t *asset = &w.ship_assets[asset_index];
    memset(asset, 0, sizeof(*asset));
    asset->active = true;
    if (w.next_ship_asset_id == SHIP_ASSET_ID_NONE)
        w.next_ship_asset_id = 1;
    asset->asset_id = w.next_ship_asset_id++;
    asset->hull_class = HULL_CLASS_MINER;
    asset->status = SHIP_ASSET_STATUS_STORED;
    principal_id[0] = 0x42u;
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, principal_id, &asset->owner_principal));
    state_root(&w, before);
    asset->owner_principal.id[0] ^= 0x01u;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    asset->owner_principal = actor_principal_none();
    asset->owner_quarantine_record_id = 11;
    state_root(&w, before);
    asset->owner_quarantine_record_id++;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    memcpy(before, after, sizeof(before));
    asset->birth_proof_version = SHIP_BIRTH_PROOF_VERSION_V1;
    asset->birth_fragment_grades[0] = MINING_GRADE_RARE;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    int pod_index = -1;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (!w.cargo_pods[i].active) {
            pod_index = i;
            break;
        }
    }
    ASSERT(pod_index >= 0);
    cargo_pod_t *pod = &w.cargo_pods[pod_index];
    memset(pod, 0, sizeof(*pod));
    pod->active = true;
    pod->kind = CARGO_POD_CARGO;
    pod->commodity = COMMODITY_FRAME;
    pod->quantity = 1;
    pod->manifest_count = 1;
    uint8_t origin[8] = {1};
    ASSERT(hash_legacy_migrate_unit(
        origin, COMMODITY_FRAME, 0, &pod->manifest_units[0]));
    cargo_pod_set_station_custody(pod, 0);
    pod->custody_charge_total = 5;
    pod->custody_charge_unit_count = 1;
    pod->custody_charge_units_processed = 0;
    ASSERT(cargo_pod_ordered_manifest_digest(
        pod, pod->custody_charge_manifest_digest));
    ASSERT(cargo_pod_custody_charge_anchor_valid(pod));
    state_root(&w, before);
    pod->custody_charge_total++;
    ASSERT(cargo_pod_custody_charge_anchor_valid(pod));
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    memcpy(before, after, sizeof(before));
    principal_id[0] = 0x73u;
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, principal_id,
        &pod->tow_owner_principal));
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    pod->tow_owner_principal = actor_principal_none();
    pod->tow_owner_quarantine_record_id = 17;
    state_root(&w, before);
    pod->tow_owner_quarantine_record_id++;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    cargo_unit_t receipt_cargo;
    cargo_receipt_chain_t receipt_chain = {0};
    uint8_t recipient[32];
    uint8_t origin_pin[32];
    for (int i = 0; i < 32; i++) {
        recipient[i] = (uint8_t)(0x30 + i);
        origin_pin[i] = (uint8_t)(0x90 + i);
    }
    ASSERT(hash_legacy_migrate_unit(
        origin, COMMODITY_FRAME, 1, &receipt_cargo));
    ASSERT(cargo_receipt_issue(
        &w.stations[0], 1, 101, receipt_cargo.pub,
        recipient, origin_pin, &receipt_chain.links[0]));
    receipt_chain.len = 1;
    uint16_t receipt_index = w.stations[0].manifest.count;
    ASSERT(station_manifest_push_with_chain(
        &w.stations[0], &receipt_cargo, &receipt_chain));
    ship_receipts_t *station_receipts =
        station_get_receipts(&w.stations[0]);
    ASSERT(station_receipts != NULL);
    ASSERT(receipt_index < station_receipts->count);
    cargo_receipt_chain_t *anchored_chain =
        &station_receipts->chains[receipt_index];
    state_root(&w, before);
    anchored_chain->links[0].event_id++;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    ownership_quarantine_entry_t diagnostic = {
        .record_id = 1,
        .source_kind = OWNERSHIP_QUARANTINE_SOURCE_CONTRACT,
        .reason = OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN,
        .station_index = OWNERSHIP_QUARANTINE_NA,
        .row_index = 0,
        .legacy_actor_code = 0,
    };
    memcpy(before, after, sizeof(before));
    ASSERT(ownership_quarantine_add(
        &w.ownership_quarantine, &diagnostic));
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));
}

TEST(test_state_digest_excludes_transport_secrets_and_derived_views)
{
    WORLD_DECL;
    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];

    world_reset(&w);
    ASSERT(world_player_ship_slot_activate(&w, 0));
    state_root(&w, before);

    w.connections[0].analytics_metrics_seq++;
    w.replications[0].world_time_sent = true;
    w.pending_resolves[0].active = true;
    w.pending_resolves[0].tx_count = 2;
    w.stations[0].station_secret[0] ^= 0x5au;
    w.stations[0].modules[0].flow_diag = STATION_FLOW_DIAG_NO_INPUT;
    w.stations[0].hnn_market_memory.last_margin = 0.75f;
    w.signal_field.cells[0].strength[0] = 0.5f;
    w.signal_field_decay_tick++;
    w.players[0].pubkey_challenge[0] = 0x11u;
    w.players[0].pubkey_challenge_issued = true;
    w.players[0].pubkey_challenge_consumed = true;
    w.players[0].input.present_pod = true;
    w.players[0].input.present_pod_index = 3;
    w.players[0].input.present_pod_token[0] = 0x22u;
    ship_receipts_t *receipts =
        station_get_receipts(&w.stations[0]);
    ASSERT(receipts != NULL);
    receipts->semantic_generation++;

    state_root(&w, after);
    ASSERT(roots_equal(before, after));

    w.cargo_pods[0].active = true;
    state_root(&w, before);
    w.cargo_pods[0].summary_flags = 1;
    w.cargo_pods[0].summary_grade = MINING_GRADE_RATI;
    w.cargo_pods[0].selection_token[0] = 0x7bu;
    w.cargo_pods[0].tractor.source_index = 7;
    w.cargo_pods[0].tractor.source_part = 3;
    w.cargo_pods[0].tractor.source_generation = 9;
    state_root(&w, after);
    ASSERT(roots_equal(before, after));

    w.asteroids[0].active = true;
    w.asteroid_origin[0].from_chunk = false;
    state_root(&w, before);
    w.asteroid_origin[0].chunk_x = 44;
    w.asteroid_origin[0].chunk_y = -12;
    state_root(&w, after);
    ASSERT(roots_equal(before, after));

    w.station_count = 4;
    station_reset(&w.stations[3]);
    state_root(&w, before);
    w.stations[3].id = 0xfeedu;
    memcpy(w.stations[3].name, "stale gap", sizeof("stale gap"));
    w.stations[3].policy_generation = 9;
    state_root(&w, after);
    ASSERT(roots_equal(before, after));
}

TEST(test_state_digest_excludes_bearer_attribution_material)
{
    WORLD_DECL;
    uint8_t baseline[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    const uint8_t token_a[8] =
        {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    const uint8_t token_b[8] =
        {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x17, 0x28};

    world_reset(&w);
    ASSERT(world_player_ship_slot_activate(&w, 0));
    server_player_t *player = &w.players[0];
    memcpy(player->session_token, token_a, sizeof(token_a));
    memcpy(player->last_damage_killer_token, token_a, sizeof(token_a));
    player->last_damage_cause = DEATH_CAUSE_THROWN_ROCK;

    npc_ship_t *npc = &w.npc_ships[0];
    npc->active = true;
    memcpy(npc->session_token, token_a, sizeof(token_a));

    contract_t *contract = &w.contracts[0];
    memset(contract, 0, sizeof(*contract));
    contract->active = true;
    ASSERT(actor_principal_from_npc(
        npc, &contract->claimed_by_principal));

    asteroid_t *asteroid = &w.asteroids[0];
    asteroid->active = true;
    memcpy(asteroid->last_towed_token, token_a, sizeof(token_a));
    memcpy(asteroid->thrown_by_token, token_a, sizeof(token_a));
    memcpy(asteroid->last_fractured_token, token_a, sizeof(token_a));
    asteroid->thrown_timer_q = 7;

    fracture_claim_state_t *claim = &w.fracture_claims[0];
    claim->active = true;
    claim->fracture_id = 1;
    claim->seen_claimant_count = 1;
    sha256_bytes(token_a, sizeof(token_a), claim->best_player_pub);
    memcpy(claim->seen_claimant_tokens[0], token_a, sizeof(token_a));

    w.pubkey_registry[0].in_use = true;
    w.pubkey_registry[0].pubkey[0] = 0x51;
    memcpy(w.pubkey_registry[0].session_token,
           token_a, sizeof(token_a));

    station_t *station = &w.stations[0];
    station->ledger_count = 1;
    memset(&station->ledger[0], 0, sizeof(station->ledger[0]));
    ledger_pubkey_from_token(
        token_a, station->ledger[0].player_pubkey);
    station->ledger[0].balance = 12.0f;

    state_root(&w, baseline);

    memcpy(player->session_token, token_b, sizeof(token_b));
    state_root(&w, after);
    ASSERT(roots_equal(baseline, after));

    memcpy(player->last_damage_killer_token, token_b, sizeof(token_b));
    state_root(&w, after);
    ASSERT(roots_equal(baseline, after));

    memcpy(npc->session_token, token_b, sizeof(token_b));
    ASSERT(actor_principal_from_npc(
        npc, &contract->claimed_by_principal));
    state_root(&w, after);
    ASSERT(roots_equal(baseline, after));

    memcpy(asteroid->last_towed_token, token_b, sizeof(token_b));
    memcpy(asteroid->thrown_by_token, token_b, sizeof(token_b));
    memcpy(asteroid->last_fractured_token, token_b, sizeof(token_b));
    state_root(&w, after);
    ASSERT(roots_equal(baseline, after));

    sha256_bytes(token_b, sizeof(token_b), claim->best_player_pub);
    memcpy(claim->seen_claimant_tokens[0], token_b, sizeof(token_b));
    state_root(&w, after);
    ASSERT(roots_equal(baseline, after));

    memcpy(w.pubkey_registry[0].session_token,
           token_b, sizeof(token_b));
    state_root(&w, after);
    ASSERT(roots_equal(baseline, after));

    ledger_pubkey_from_token(
        token_b, station->ledger[0].player_pubkey);
    state_root(&w, after);
    ASSERT(roots_equal(baseline, after));
}

TEST(test_state_digest_keeps_public_identity_state_authoritative)
{
    WORLD_DECL;
    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t principal_id[ACTOR_PRINCIPAL_ID_SIZE] = {0};
    const uint8_t token[8] =
        {0x91, 0x82, 0x73, 0x64, 0x55, 0x46, 0x37, 0x28};

    world_reset(&w);
    ASSERT(world_player_ship_slot_activate(&w, 0));
    w.players[0].pubkey_set = true;
    w.players[0].pubkey_proof_ok = true;
    w.players[0].pubkey_identity_finalized = true;
    w.players[0].pubkey[0] = 0x31;
    state_root(&w, before);
    w.players[0].pubkey[0] = 0x32;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    station_t *station = &w.stations[0];
    station->ledger_count = 1;
    memset(&station->ledger[0], 0, sizeof(station->ledger[0]));
    ledger_pubkey_from_token(
        token, station->ledger[0].player_pubkey);
    state_root(&w, before);
    memset(station->ledger[0].player_pubkey, 0x5a,
           sizeof(station->ledger[0].player_pubkey));
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    contract_t *contract = &w.contracts[0];
    memset(contract, 0, sizeof(*contract));
    contract->active = true;
    principal_id[0] = 0x61;
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, principal_id,
        &contract->claimed_by_principal));
    state_root(&w, before);
    contract->claimed_by_principal.id[0] = 0x62;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));
}

TEST(test_state_digest_bounds_corrupt_lengths_without_hiding_them)
{
    WORLD_DECL;
    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];

    world_reset(&w);
    station_t *station = &w.stations[0];
    ship_receipts_t *receipts = station_get_receipts(station);
    ASSERT(receipts != NULL);
    ASSERT(station->manifest.cap < UINT16_MAX);
    ASSERT(receipts->cap < UINT16_MAX);
    uint16_t manifest_count = station->manifest.count;
    uint16_t receipt_count = receipts->count;

    state_root(&w, before);
    station->manifest.count = (uint16_t)(station->manifest.cap + 1u);
    receipts->count = (uint16_t)(receipts->cap + 1u);
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    station->manifest.count = manifest_count;
    receipts->count = receipt_count;

    signal_channel_msg_t *message = &w.signal_channel.msgs[0];
    memset(message, 0, sizeof(*message));
    message->id = 1;
    message->audio_len = UINT8_MAX;
    memset(message->audio_url, 'a', sizeof(message->audio_url));
    w.signal_channel.count = 1;
    w.signal_channel.head = 1;
    state_root(&w, before);
    message->audio_url[UINT8_MAX - 1u] = 'b';
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));
}

void register_state_digest_tests(void)
{
    RUN(test_state_digest_reports_versioned_schema_and_is_repeatable);
    RUN(test_state_digest_commits_each_authoritative_domain);
    RUN(test_state_digest_tow_links_are_pool_permutation_invariant);
    RUN(test_state_digest_commits_current_trust_ownership_and_charge_state);
    RUN(test_state_digest_excludes_transport_secrets_and_derived_views);
    RUN(test_state_digest_excludes_bearer_attribution_material);
    RUN(test_state_digest_keeps_public_identity_state_authoritative);
    RUN(test_state_digest_bounds_corrupt_lengths_without_hiding_them);
}
