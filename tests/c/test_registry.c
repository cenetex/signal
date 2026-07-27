/*
 * test_registry.c — Layer A.2 of #479: pubkey registry on the server.
 *
 * Verifies that NET_MSG_REGISTER_PUBKEY-style registration binds a
 * pubkey to the current session_token, that re-registration with a
 * rotated token is treated as a reconnect and rebinds, and that the
 * registry survives a world save / load roundtrip.
 *
 * Identity at the wire level is still the 8-byte session_token; this
 * layer is registration only, not authentication. Signing inputs is
 * Layer A.3.
 */
#include "test_harness.h"

#include <string.h>

#include "actor_principal_resolver.h"
#include "protocol.h"
#include "pubkey_proof.h"
#include "signal_crypto.h"

static void fill_pubkey(uint8_t pk[32], uint8_t seed) {
    for (int i = 0; i < 32; i++) pk[i] = (uint8_t)(seed + i);
}

static void fill_token(uint8_t tok[8], uint8_t seed) {
    for (int i = 0; i < 8; i++) tok[i] = (uint8_t)(seed * 13 + i);
}

typedef struct {
    uint8_t fill;
    size_t bytes_to_write;
    bool succeed;
} registry_entropy_fixture_t;

static bool registry_entropy_provider(uint8_t *buf, size_t len, void *user) {
    registry_entropy_fixture_t *fixture =
        (registry_entropy_fixture_t *)user;
    size_t write_len = fixture && fixture->bytes_to_write < len
        ? fixture->bytes_to_write : len;
    if (buf && write_len > 0)
        memset(buf, fixture ? fixture->fill : 0xA5, write_len);
    return fixture && fixture->succeed;
}

/* Helper: simulate a completed registration/proof sequence, then bind the
 * verified pubkey through the registry API. */
static void setup_registered_player(world_t *w, int slot,
                                    const uint8_t pubkey[32],
                                    const uint8_t token[8]) {
    server_player_t *sp = &w->players[slot];
    sp->connected = true;
    sp->id = (uint8_t)slot;
    memcpy(sp->session_token, token, 8);
    sp->session_ready = true;
    memcpy(sp->pubkey, pubkey, 32);
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    sp->pubkey_challenge_consumed = true;
    ASSERT(server_finalize_pubkey_identity(w, slot));
}

TEST(test_registry_fresh_registration) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32];  fill_pubkey(pk, 1);
    uint8_t tok[8];  fill_token(tok, 7);

    /* Empty registry: lookup misses. */
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), -1);

    setup_registered_player(w, 3, pk, tok);

    /* Lookup returns the player_idx we registered. */
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), 3);
}

TEST(test_registry_idempotent_reregistration) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32];  fill_pubkey(pk, 2);
    uint8_t tok[8];  fill_token(tok, 11);

    setup_registered_player(w, 1, pk, tok);
    /* Same (pubkey, token) again — must return true and produce no
     * duplicate registry entry. */
    ASSERT(registry_register_pubkey(w, pk, tok));

    int seen = 0;
    for (int r = 0; r < MAX_PLAYERS; r++) {
        if (!w->pubkey_registry[r].in_use) continue;
        if (memcmp(w->pubkey_registry[r].pubkey, pk, 32) == 0) seen++;
    }
    ASSERT_EQ_INT(seen, 1);
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), 1);
}

TEST(test_registry_reconnect_with_new_token) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32];  fill_pubkey(pk, 3);
    uint8_t tok1[8]; fill_token(tok1, 5);
    uint8_t tok2[8]; fill_token(tok2, 99);

    /* First connection: pubkey P @ slot 0 with token T1, plus a state
     * marker we'll check survives the rebinding, plus a station ledger
     * entry keyed by T1 to verify ledger migration. */
    setup_registered_player(w, 0, pk, tok1);
    w->players[0].ship->stat_credits_earned = 4242.0f;
    station_t *st = &w->stations[0];
    memcpy(st->ledger[st->ledger_count].player_pubkey, tok1, 8);
    st->ledger[st->ledger_count].balance = 1234.0f;
    st->ledger[st->ledger_count].lifetime_supply = 0.0f;
    st->ledger_count++;

    /* Server-side reconnect logic: the new connection arrives on a
     * fresh slot with a NEW session_token T2. The handler ship-copies
     * the old slot's persistent state, migrates ledger entries from
     * T1 → T2 across stations, frees the old slot, and rebinds the
     * registry to T2. */
    server_player_t *new_slot = &w->players[5];
    new_slot->connected = true;
    new_slot->id = 5;
    memcpy(new_slot->session_token, tok2, 8);
    new_slot->session_ready = true;
    /* Carry the persistent state. */
    new_slot->ship->stat_credits_earned = w->players[0].ship->stat_credits_earned;
    /* Heal any historic token-pseudokey balance into the verified key.
     * Token rotation must never manufacture a second pseudo identity. */
    ASSERT(world_migrate_legacy_ledger_to_pubkey(w, tok1, pk));
    /* Tear down old slot, as the handler does. */
    w->players[0].connected = false;
    w->players[0].session_ready = false;
    memset(w->players[0].session_token, 0, 8);
    /* Now rebind via REGISTER_PUBKEY. */
    memcpy(new_slot->pubkey, pk, 32);
    new_slot->pubkey_set = true;
    new_slot->pubkey_proof_ok = true;
    new_slot->pubkey_challenge_consumed = true;
    ASSERT(server_finalize_pubkey_identity(w, 5));

    /* Lookup now resolves to the new slot. */
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), 5);
    /* State marker survives. */
    ASSERT_EQ_FLOAT(w->players[5].ship->stat_credits_earned, 4242.0f, 0.01f);

    /* Old token T1 is no longer claimed by any live, ready slot. */
    bool old_alive = false;
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (!w->players[p].session_ready) continue;
        if (memcmp(w->players[p].session_token, tok1, 8) == 0) {
            old_alive = true; break;
        }
    }
    ASSERT(!old_alive);

    /* Ledger entry migrated: balance remains on the durable pubkey. */
    ASSERT(ledger_balance_by_pubkey(st, pk) > 1000.0f);
    ASSERT_EQ_FLOAT(ledger_balance(st, tok2), 0.0f, 0.001f);
}

TEST(test_registry_lookup_rejects_token_only_impostor) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32];  fill_pubkey(pk, 4);
    uint8_t tok[8];  fill_token(tok, 41);

    setup_registered_player(w, 0, pk, tok);
    w->players[0].connected = false;
    w->players[0].session_ready = true;

    server_player_t *new_slot = &w->players[5];
    new_slot->connected = true;
    new_slot->id = 5;
    memcpy(new_slot->session_token, tok, 8);
    new_slot->session_ready = true;

    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), -1);
}

TEST(test_registry_same_token_takeover_moves_live_ship) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32];  fill_pubkey(pk, 44);
    uint8_t tok[8];  fill_token(tok, 45);

    server_player_t *old = &w->players[0];
    player_init_ship(old, w);
    setup_registered_player(w, 0, pk, tok);
    old->docked = false;
    old->ship->pos = v2(1234.0f, 5678.0f);
    old->ship->vel = v2(11.0f, -3.0f);
    old->ship->angle = 0.75f;
    old->ship->stat_credits_earned = 91.0f;
    uint32_t old_asset_id = old->ship_asset_id;
    ASSERT(old_asset_id != SHIP_ASSET_ID_NONE);

    server_player_t *new_slot = &w->players[5];
    player_init_ship(new_slot, w);
    new_slot->connected = true;
    new_slot->id = 5;
    memcpy(new_slot->session_token, tok, 8);
    new_slot->session_ready = true;
    memcpy(new_slot->pubkey, pk, 32);
    new_slot->pubkey_set = true;
    new_slot->pubkey_proof_ok = true;
    new_slot->pubkey_challenge_consumed = true;

    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), 0);
    actor_principal_t stable_owner = actor_principal_none();
    ASSERT(actor_principal_from_verified_player(old, &stable_owner));
    ship_asset_t *owned_asset =
        world_ship_asset_by_id(w, old_asset_id);
    ASSERT(owned_asset != NULL);
    owned_asset->owner_principal = stable_owner;
    owned_asset->loaner = false;
    pending_ship_build_t *pending =
        &w->stations[0].pending_ship_builds[0];
    w->stations[0].pending_ship_build_count = 1;
    pending->hull_class = HULL_CLASS_MINER;
    pending->owner_principal = stable_owner;
    pending->mode = (uint8_t)PENDING_SHIP_BUILD_MODE_MATERIAL;
    ASSERT(world_player_transfer_ship_state(w, 5, 0));
    old->connected = false;
    old->grace_period = false;
    server_player_clear_live_session_identity(old);
    server_player_clear_transient_input(old);
    ASSERT(server_finalize_pubkey_identity(w, 5));

    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), 5);
    ASSERT_EQ_INT(old->ship_asset_id, SHIP_ASSET_ID_NONE);
    ASSERT_EQ_INT(new_slot->ship_asset_id, old_asset_id);
    ASSERT(!new_slot->docked);
    ASSERT_EQ_FLOAT(new_slot->ship->pos.x, 1234.0f, 0.001f);
    ASSERT_EQ_FLOAT(new_slot->ship->pos.y, 5678.0f, 0.001f);
    ASSERT_EQ_FLOAT(new_slot->ship->vel.x, 11.0f, 0.001f);
    ASSERT_EQ_FLOAT(new_slot->ship->vel.y, -3.0f, 0.001f);
    ASSERT_EQ_FLOAT(new_slot->ship->angle, 0.75f, 0.001f);
    ASSERT_EQ_FLOAT(new_slot->ship->stat_credits_earned, 91.0f, 0.001f);
    actor_principal_t reconnected_owner = actor_principal_none();
    ASSERT(actor_principal_from_verified_player(
        new_slot, &reconnected_owner));
    ASSERT(actor_principal_equal(&stable_owner, &reconnected_owner));
    owned_asset = world_ship_asset_by_id(w, old_asset_id);
    ASSERT(owned_asset != NULL);
    ASSERT(actor_principal_equal(
        &owned_asset->owner_principal, &stable_owner));
    ASSERT(actor_principal_equal(
        &pending->owner_principal, &stable_owner));
}

TEST(test_player_transfer_rebinds_slot_obligations_and_character) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    server_player_t *old = &w->players[0];
    old->connected = true;
    old->id = 0;
    uint8_t token[8];
    fill_token(token, 53);
    memcpy(old->session_token, token, sizeof(token));
    old->session_ready = true;
    player_init_ship(old, w);
    old->last_signed_nonce = 777;
    old->autopilot_mode = 1;
    old->autopilot_target = 123;
    old->autopilot_station_target = 2;
    old->autopilot_cargo = COMMODITY_FERRITE_INGOT;
    old->autopilot_state = 7;
    old->autopilot_timer = 4.5f;
    old->autopilot_last_pos = v2(91.0f, -37.0f);
    old->autopilot_stuck_timer = 2.25f;
    old->autopilot_teacher_tick = 55;
    old->autopilot_teacher_features[0] = 0.75f;
    old->autopilot_decision_flags = 0x1234u;
    old->autopilot_decision_score = 8.5f;
    old->hail_decision_source_id = UINT64_C(0x12345678);
    old->server_brain_mode = SERVER_BRAIN_MODE_NEURAL_FLIGHT;
    fill_token(old->last_damage_killer_token, 54);
    old->last_damage_cause = DEATH_CAUSE_THROWN_ROCK;
    old->input.turn = 0.5f;
    old->input.thrust = 1.0f;
    old->input.mine = true;
    old->beam_active = true;
    old->scan_active = true;
    old->ship->tractor_active = true;
    ASSERT(w->characters[0].active);

    server_player_t *incoming = &w->players[5];
    incoming->connected = true;
    incoming->id = 5;
    memcpy(incoming->session_token, token, sizeof(token));
    incoming->session_ready = true;

    w->contracts[0].active = true;
    w->contracts[0].claimed_by = 0;
    w->delivery_shipments[0].active = true;
    w->delivery_shipments[0].debtor_player = 0;
    w->delivery_shipments[1].active = true;
    w->delivery_shipments[1].debtor_player =
        (uint8_t)MAX_PLAYERS; /* NPC-coded ID must not move. */

    station_t *st = &w->stations[0];
    st->planned_owner = 0;
    st->pending_scaffold_count = 2;
    st->pending_scaffolds[0].owner = 0;
    st->pending_scaffolds[1].owner = -1;
    st->placement_plan_count = 1;
    st->placement_plans[0].owner = 0;
    st->pending_ship_build_count = 2;
    uint8_t durable_player_id[32];
    fill_pubkey(durable_player_id, 61);
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, durable_player_id,
        &st->pending_ship_builds[0].owner_principal));
    ASSERT(actor_principal_from_station(
        w, 0, &st->pending_ship_builds[1].owner_principal));
    actor_principal_t player_build_owner =
        st->pending_ship_builds[0].owner_principal;
    actor_principal_t station_build_owner =
        st->pending_ship_builds[1].owner_principal;
    w->scaffolds[0].active = true;
    w->scaffolds[0].owner = 0;

    ASSERT(world_player_transfer_ship_state(w, 5, 0));
    ASSERT_EQ_INT(w->contracts[0].claimed_by, 5);
    ASSERT_EQ_INT(w->delivery_shipments[0].debtor_player, 5);
    ASSERT_EQ_INT(w->delivery_shipments[1].debtor_player, MAX_PLAYERS);
    ASSERT_EQ_INT(st->planned_owner, 5);
    ASSERT_EQ_INT(st->pending_scaffolds[0].owner, 5);
    ASSERT_EQ_INT(st->pending_scaffolds[1].owner, -1);
    ASSERT_EQ_INT(st->placement_plans[0].owner, 5);
    ASSERT(actor_principal_equal(
        &st->pending_ship_builds[0].owner_principal,
        &player_build_owner));
    ASSERT(actor_principal_equal(
        &st->pending_ship_builds[1].owner_principal,
        &station_build_owner));
    ASSERT_EQ_INT(w->scaffolds[0].owner, 5);
    ASSERT_EQ_INT((int)incoming->last_signed_nonce, 777);
    ASSERT_EQ_INT(incoming->autopilot_mode, 1);
    ASSERT_EQ_INT(incoming->autopilot_target, 123);
    ASSERT_EQ_INT(incoming->autopilot_station_target, 2);
    ASSERT_EQ_INT(incoming->autopilot_cargo,
                  COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(incoming->autopilot_state, 7);
    ASSERT_EQ_FLOAT(incoming->autopilot_timer, 4.5f, 0.001f);
    ASSERT_EQ_FLOAT(incoming->autopilot_last_pos.x, 91.0f, 0.001f);
    ASSERT_EQ_FLOAT(incoming->autopilot_last_pos.y, -37.0f, 0.001f);
    ASSERT_EQ_FLOAT(incoming->autopilot_stuck_timer, 2.25f, 0.001f);
    ASSERT_EQ_INT((int)incoming->autopilot_teacher_tick, 55);
    ASSERT_EQ_FLOAT(incoming->autopilot_teacher_features[0],
                    0.75f, 0.001f);
    ASSERT_EQ_INT((int)incoming->autopilot_decision_flags, 0x1234);
    ASSERT_EQ_FLOAT(incoming->autopilot_decision_score,
                    8.5f, 0.001f);
    ASSERT(incoming->hail_decision_source_id ==
           UINT64_C(0x12345678));
    ASSERT_EQ_INT(incoming->server_brain_mode,
                  SERVER_BRAIN_MODE_NEURAL_FLIGHT);
    uint8_t damage_token[8];
    fill_token(damage_token, 54);
    ASSERT_EQ_INT(memcmp(incoming->last_damage_killer_token,
                         damage_token, sizeof(damage_token)), 0);
    ASSERT_EQ_INT(incoming->last_damage_cause,
                  DEATH_CAUSE_THROWN_ROCK);
    ASSERT_EQ_FLOAT(incoming->input.turn, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(incoming->input.thrust, 0.0f, 0.001f);
    ASSERT(!incoming->input.mine);
    ASSERT(!incoming->beam_active);
    ASSERT(!incoming->scan_active);
    ASSERT(!incoming->ship->tractor_active);
    ASSERT(!w->characters[0].active);
    ASSERT(w->characters[5].active);
    ASSERT_EQ_INT(w->characters[5].actor_slot, 5);
    ASSERT(entity_ref_equal(
        w->characters[5].ship_ref, incoming->ship_ref));
}

TEST(test_player_transfer_rejects_invalid_destination_asset) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    server_player_t *old = &w->players[0];
    old->connected = true;
    old->id = 0;
    player_init_ship(old, w);
    uint32_t old_asset_id = old->ship_asset_id;
    ASSERT(old_asset_id != SHIP_ASSET_ID_NONE);
    old->ship->hull = 37.0f;

    server_player_t *incoming = &w->players[5];
    incoming->connected = true;
    incoming->id = 5;
    uint32_t invalid_asset_id = UINT32_C(0xFFFFFFFE);
    ASSERT(world_ship_asset_by_id(w, invalid_asset_id) == NULL);
    incoming->ship_asset_id = invalid_asset_id;
    ASSERT(!w->characters[5].active);

    w->contracts[0].active = true;
    w->contracts[0].claimed_by = 0;
    ASSERT(!world_player_transfer_ship_state(w, 5, 0));

    ASSERT_EQ_INT(old->ship_asset_id, old_asset_id);
    ASSERT_EQ_FLOAT(old->ship->hull, 37.0f, 0.001f);
    ASSERT_EQ_INT(incoming->ship_asset_id, invalid_asset_id);
    ASSERT_EQ_INT(w->contracts[0].claimed_by, 0);
    ASSERT(!w->characters[5].active);
    const ship_asset_t *asset =
        world_ship_asset_by_id_const(w, old_asset_id);
    ASSERT(asset != NULL);
    ASSERT_EQ_INT(asset->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(asset->operator_kind,
                  SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT_EQ_INT(asset->operator_slot, 0);
}

TEST(test_registry_same_token_reattach_finds_active_duplicate) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t tok[8]; fill_token(tok, 57);
    uint8_t other_tok[8]; fill_token(other_tok, 58);

    server_player_t *old = &w->players[0];
    old->connected = true;
    old->id = 0;
    memcpy(old->session_token, tok, 8);
    old->session_ready = true;
    old->grace_period = false;

    server_player_t *fresh = &w->players[5];
    fresh->connected = true;
    fresh->id = 5;
    memcpy(fresh->session_token, tok, 8);
    fresh->session_ready = true;

    bool conflict = false;
    ASSERT_EQ_INT(server_find_session_reattach_slot(w, 5, &conflict), 0);
    ASSERT(!conflict);
    memcpy(fresh->session_token, other_tok, 8);
    ASSERT_EQ_INT(server_find_session_reattach_slot(w, 5, &conflict), -1);
    ASSERT(!conflict);

    memcpy(fresh->session_token, tok, 8);
    old->grace_period = true;
    ASSERT_EQ_INT(server_find_session_reattach_slot(w, 5, &conflict), 0);
    ASSERT(!conflict);

    old->connected = false;
    ASSERT_EQ_INT(server_find_session_reattach_slot(w, 5, &conflict), -1);
    ASSERT(!conflict);
}

TEST(test_session_reattach_rejects_cross_identity_token_takeover) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t key_a[32], key_b[32], token[8];
    fill_pubkey(key_a, 61);
    fill_pubkey(key_b, 131);
    fill_token(token, 67);
    setup_registered_player(w, 0, key_a, token);

    server_player_t *incoming = &w->players[1];
    incoming->connected = true;
    incoming->id = 1;
    memcpy(incoming->session_token, token, sizeof(token));
    incoming->session_ready = true;

    bool conflict = false;
    ASSERT_EQ_INT(server_find_session_reattach_slot(
                      w, 1, &conflict), -1);
    ASSERT(conflict); /* anonymous/T cannot take verified A/T */

    memcpy(incoming->pubkey, key_b, sizeof(key_b));
    incoming->pubkey_set = true;
    incoming->pubkey_proof_ok = true;
    incoming->pubkey_challenge_consumed = true;
    ASSERT_EQ_INT(server_find_session_reattach_slot(
                      w, 1, &conflict), -1);
    ASSERT(conflict); /* verified B/T cannot take verified A/T */

    memcpy(incoming->pubkey, key_a, sizeof(key_a));
    ASSERT_EQ_INT(server_find_session_reattach_slot(
                      w, 1, &conflict), 0);
    ASSERT(!conflict); /* proved exact A/T is compatible */

    server_player_clear_live_session_identity(&w->players[0]);
    memcpy(w->players[0].session_token, token, sizeof(token));
    w->players[0].session_ready = true;

    ASSERT_EQ_INT(server_find_session_reattach_slot(
                      w, 1, &conflict), -1);
    ASSERT(conflict); /* verified A/T cannot take anonymous/T */

    ASSERT(server_player_abandon_pending_pubkey_identity(incoming));
    ASSERT_EQ_INT(server_find_session_reattach_slot(
                      w, 1, &conflict), 0);
    ASSERT(!conflict); /* anonymous/T may reattach anonymous/T */
}

TEST(test_session_reattach_conflict_wins_over_grace_match) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t key_b[32], token[8];
    fill_pubkey(key_b, 139);
    fill_token(token, 71);

    server_player_t *anonymous = &w->players[0];
    anonymous->connected = true;
    anonymous->id = 0;
    memcpy(anonymous->session_token, token, sizeof(token));
    anonymous->session_ready = true;
    anonymous->grace_period = true;

    setup_registered_player(w, 2, key_b, token);

    server_player_t *incoming = &w->players[1];
    incoming->connected = true;
    incoming->id = 1;
    memcpy(incoming->session_token, token, sizeof(token));
    incoming->session_ready = true;

    bool conflict = false;
    ASSERT_EQ_INT(server_find_session_reattach_slot(
                      w, 1, &conflict), -1);
    ASSERT(conflict);
}

TEST(test_registry_lookup_requires_exact_finalized_identity) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t key_a[32], key_b[32], token[8];
    fill_pubkey(key_a, 73);
    fill_pubkey(key_b, 173);
    fill_token(token, 79);
    setup_registered_player(w, 0, key_a, token);
    w->players[0].connected = false;

    server_player_t *candidate = &w->players[1];
    candidate->connected = true;
    candidate->id = 1;
    memcpy(candidate->session_token, token, sizeof(token));
    candidate->session_ready = true;
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, key_a), -1);

    memcpy(candidate->pubkey, key_b, sizeof(key_b));
    candidate->pubkey_set = true;
    candidate->pubkey_proof_ok = true;
    candidate->pubkey_challenge_consumed = true;
    candidate->pubkey_identity_finalized = true;
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, key_a), -1);

    memcpy(candidate->pubkey, key_a, sizeof(key_a));
    candidate->pubkey_identity_finalized = false;
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, key_a), -1);

    candidate->pubkey_identity_finalized = true;
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, key_a), 1);
}

TEST(test_registry_duplicate_stale_row_cannot_hide_live_binding) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t key[32], stale_token[8], live_token[8], next_token[8];
    fill_pubkey(key, 181);
    fill_token(stale_token, 107);
    fill_token(live_token, 109);
    fill_token(next_token, 111);
    w->pubkey_registry[0].in_use = true;
    memcpy(w->pubkey_registry[0].pubkey, key, sizeof(key));
    memcpy(w->pubkey_registry[0].session_token,
           stale_token, sizeof(stale_token));
    w->pubkey_registry[1].in_use = true;
    memcpy(w->pubkey_registry[1].pubkey, key, sizeof(key));
    memcpy(w->pubkey_registry[1].session_token,
           live_token, sizeof(live_token));

    server_player_t *sp = &w->players[2];
    sp->connected = true;
    sp->id = 2;
    memcpy(sp->session_token, live_token, sizeof(live_token));
    sp->session_ready = true;
    memcpy(sp->pubkey, key, sizeof(key));
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    sp->pubkey_challenge_consumed = true;
    sp->pubkey_identity_finalized = true;
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, key), 2);

    ASSERT(registry_register_pubkey(w, key, next_token));
    int matching_rows = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (w->pubkey_registry[i].in_use &&
            memcmp(w->pubkey_registry[i].pubkey,
                   key, sizeof(key)) == 0) {
            matching_rows++;
            ASSERT_EQ_INT(memcmp(
                w->pubkey_registry[i].session_token,
                next_token, sizeof(next_token)), 0);
        }
    }
    ASSERT_EQ_INT(matching_rows, 1);
}

TEST(test_registry_capacity_reclaims_only_stale_bindings) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        uint8_t key[32], token[8];
        fill_pubkey(key, (uint8_t)(i + 1));
        fill_token(token, (uint8_t)(i + 1));
        ASSERT(registry_register_pubkey(w, key, token));
    }
    uint8_t replacement_key[32], replacement_token[8];
    fill_pubkey(replacement_key, 201);
    fill_token(replacement_token, 211);
    ASSERT(registry_register_pubkey(
        w, replacement_key, replacement_token));

    bool found_replacement = false;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (w->pubkey_registry[i].in_use &&
            memcmp(w->pubkey_registry[i].pubkey,
                   replacement_key, sizeof(replacement_key)) == 0) {
            found_replacement = true;
            break;
        }
    }
    ASSERT(found_replacement);

    world_reset(w);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        uint8_t key[32], token[8];
        fill_pubkey(key, (uint8_t)(i + 33));
        fill_token(token, (uint8_t)(i + 33));
        setup_registered_player(w, i, key, token);
    }
    fill_pubkey(replacement_key, 241);
    fill_token(replacement_token, 251);
    ASSERT(!registry_register_pubkey(
        w, replacement_key, replacement_token));

    /* Duplicate stale rows for the same pubkey must be judged by the
     * exact (pubkey, token) binding. A live canonical binding protects
     * itself, but must not make every stale duplicate unreclaimable. */
    world_reset(w);
    uint8_t live_key[32], live_token[8];
    fill_pubkey(live_key, 171);
    fill_token(live_token, 173);
    setup_registered_player(w, 0, live_key, live_token);
    for (int i = 1; i < MAX_PLAYERS; i++) {
        uint8_t stale_token[8];
        memset(stale_token, i + 1, sizeof(stale_token));
        w->pubkey_registry[i].in_use = true;
        memcpy(w->pubkey_registry[i].pubkey,
               live_key, sizeof(live_key));
        memcpy(w->pubkey_registry[i].session_token,
               stale_token, sizeof(stale_token));
    }
    fill_pubkey(replacement_key, 241);
    fill_token(replacement_token, 251);
    ASSERT(registry_register_pubkey(
        w, replacement_key, replacement_token));
    ASSERT_EQ_INT(memcmp(w->pubkey_registry[0].pubkey,
                         live_key, sizeof(live_key)), 0);
    ASSERT_EQ_INT(memcmp(w->pubkey_registry[0].session_token,
                         live_token, sizeof(live_token)), 0);
    bool replacement_found = false;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (w->pubkey_registry[i].in_use &&
            memcmp(w->pubkey_registry[i].pubkey,
                   replacement_key, sizeof(replacement_key)) == 0) {
            replacement_found = true;
            break;
        }
    }
    ASSERT(replacement_found);
}

TEST(test_pending_pubkey_abandon_restores_published_token_identity) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    server_player_t *sp = &w->players[0];
    sp->connected = true;
    sp->id = 0;
    uint8_t token[8], key[32];
    fill_token(token, 83);
    fill_pubkey(key, 89);
    memcpy(sp->session_token, token, sizeof(token));
    sp->session_ready = true;
    sp->ship->stat_credits_earned = 987.0f;
    sp->last_signed_nonce = 456;
    memcpy(sp->pubkey, key, sizeof(key));
    sp->pubkey_set = true;
    sp->pubkey_challenge_issued = true;
    memset(sp->pubkey_challenge, 0xA9,
           sizeof(sp->pubkey_challenge));
    sp->preserve_live_state_on_pubkey_finalize = true;

    ASSERT(!server_player_has_live_session(sp));
    ASSERT(server_player_abandon_pending_pubkey_identity(sp));
    ASSERT(server_player_has_live_session(sp));
    ASSERT_EQ_INT(memcmp(sp->session_token, token, sizeof(token)), 0);
    ASSERT_EQ_FLOAT(sp->ship->stat_credits_earned, 987.0f, 0.001f);
    ASSERT_EQ_INT((int)sp->last_signed_nonce, 456);
    ASSERT(!sp->pubkey_set);
    ASSERT(!sp->pubkey_proof_ok);
    ASSERT(!sp->pubkey_challenge_issued);
    ASSERT(!sp->pubkey_challenge_consumed);
    ASSERT(!sp->preserve_live_state_on_pubkey_finalize);
}

TEST(test_session_first_reattach_pubkey_upgrade_preserves_ship) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t token[8], key[32], secret[SIGNAL_CRYPTO_SECRET_BYTES];
    fill_token(token, 97);
    ASSERT(signal_crypto_keypair(key, secret));

    server_player_t *old = &w->players[0];
    old->connected = true;
    old->id = 0;
    memcpy(old->session_token, token, sizeof(token));
    old->session_ready = true;
    player_init_ship(old, w);
    old->ship->stat_credits_earned = 6543.0f;
    uint32_t asset_id = old->ship_asset_id;
    ship_asset_t *asset = world_ship_asset_by_id(w, asset_id);
    ASSERT(asset != NULL);
    actor_principal_t loaner_owner = actor_principal_none();
    ASSERT(actor_principal_from_station(w, 0, &loaner_owner));
    ASSERT(actor_principal_equal(
        &asset->owner_principal, &loaner_owner));
    ASSERT(asset->loaner);

    station_t *station = &w->stations[0];
    ledger_earn(station, token, 100.0f);
    ledger_earn_by_pubkey(station, key, 25.0f);
    int legacy_ledger = -1;
    int keyed_ledger = -1;
    uint8_t pseudo[32] = {0};
    memcpy(pseudo, token, sizeof(token));
    for (int i = 0; i < station->ledger_count; i++) {
        if (memcmp(station->ledger[i].player_pubkey,
                   pseudo, sizeof(pseudo)) == 0) {
            legacy_ledger = i;
        }
        if (memcmp(station->ledger[i].player_pubkey,
                   key, sizeof(key)) == 0) {
            keyed_ledger = i;
        }
    }
    ASSERT(legacy_ledger >= 0);
    ASSERT(keyed_ledger >= 0);
    station->ledger[legacy_ledger].first_dock_tick = 0;
    station->ledger[legacy_ledger].last_dock_tick = 8;
    station->ledger[legacy_ledger].total_docks = 1;
    station->ledger[keyed_ledger].first_dock_tick = 10;
    station->ledger[keyed_ledger].last_dock_tick = 10;
    station->ledger[keyed_ledger].total_docks = 1;

    station->pending_ship_build_count = 1;
    pending_ship_build_t *build =
        &station->pending_ship_builds[0];
    build->owner_principal = actor_principal_none();
    build->mode = (uint8_t)PENDING_SHIP_BUILD_MODE_UNKNOWN;

    server_player_t *incoming = &w->players[1];
    incoming->connected = true;
    incoming->id = 1;
    memcpy(incoming->session_token, token, sizeof(token));
    incoming->session_ready = true;
    bool conflict = false;
    ASSERT_EQ_INT(server_find_session_reattach_slot(
                      w, 1, &conflict), 0);
    ASSERT(!conflict);
    ASSERT(world_player_transfer_ship_state(w, 1, 0));
    incoming->preserve_live_state_on_pubkey_finalize = true;
    old->connected = false;
    server_player_clear_live_session_identity(old);

    uint8_t register_msg[REGISTER_PUBKEY_MSG_SIZE] =
        { NET_MSG_REGISTER_PUBKEY };
    memcpy(&register_msg[1], key, sizeof(key));
    ASSERT(server_dispatch_register_pubkey_message(
        w, 1, register_msg, sizeof(register_msg), NULL));
    ASSERT(incoming->preserve_live_state_on_pubkey_finalize);

    registry_entropy_fixture_t entropy = {
        .fill = 0xB7,
        .bytes_to_write = PUBKEY_PROOF_CHALLENGE_SIZE,
        .succeed = true,
    };
    uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    signal_crypto_test_set_entropy_provider(
        registry_entropy_provider, &entropy);
    bool issued = server_issue_pubkey_challenge(w, 1, challenge);
    signal_crypto_test_reset_entropy_provider();
    ASSERT(issued);

    uint8_t proof_msg[PROVE_PUBKEY_MSG_SIZE] =
        { NET_MSG_PROVE_PUBKEY };
    memcpy(&proof_msg[PROVE_PUBKEY_PUBKEY_OFFSET], key, sizeof(key));
    memcpy(&proof_msg[PROVE_PUBKEY_TOKEN_OFFSET], token, sizeof(token));
    ASSERT(pubkey_proof_sign(
        &proof_msg[PROVE_PUBKEY_SIG_OFFSET],
        key, secret, token, challenge));
    server_pubkey_proof_result_t proof;
    ASSERT(server_dispatch_pubkey_proof_message(
        w, 1, proof_msg, sizeof(proof_msg), &proof));
    ASSERT(proof.verified);
    ASSERT(server_finalize_pubkey_identity(w, 1));
    ASSERT_EQ_FLOAT(incoming->ship->stat_credits_earned,
                    6543.0f, 0.001f);
    ASSERT(incoming->preserve_live_state_on_pubkey_finalize);
    ASSERT_EQ_FLOAT(ledger_balance_by_pubkey(station, key),
                    125.0f, 0.001f);
    ASSERT_EQ_FLOAT(ledger_balance(station, token), 0.0f, 0.001f);
    keyed_ledger = -1;
    for (int i = 0; i < station->ledger_count; i++) {
        if (memcmp(station->ledger[i].player_pubkey,
                   key, sizeof(key)) == 0) {
            keyed_ledger = i;
            break;
        }
    }
    ASSERT(keyed_ledger >= 0);
    ASSERT_EQ_INT((int)station->ledger[keyed_ledger].first_dock_tick, 0);
    ASSERT_EQ_INT((int)station->ledger[keyed_ledger].last_dock_tick, 10);
    ASSERT_EQ_INT((int)station->ledger[keyed_ledger].total_docks, 2);

    asset = world_ship_asset_by_id(w, asset_id);
    ASSERT(asset != NULL);
    actor_principal_t verified_owner = actor_principal_none();
    ASSERT(actor_principal_from_verified_player(
        incoming, &verified_owner));
    ASSERT(actor_principal_equal(
        &asset->owner_principal, &loaner_owner));
    ASSERT(!actor_principal_equal(
        &asset->owner_principal, &verified_owner));
    ASSERT(asset->loaner);
    actor_principal_t no_owner = actor_principal_none();
    ASSERT(actor_principal_equal(
        &build->owner_principal, &no_owner));
    ASSERT_EQ_INT(build->mode, PENDING_SHIP_BUILD_MODE_UNKNOWN);
}

TEST(test_live_session_requires_nonzero_token) {
    SERVER_PLAYER_DECL(sp);
    sp.connected = true;
    sp.session_ready = true;
    ASSERT(!server_player_has_live_session(&sp));
    fill_token(sp.session_token, 101);
    ASSERT(server_player_has_live_session(&sp));
}

TEST(test_player_clear_live_session_identity) {
    SERVER_PLAYER_DECL(sp);
    memset(sp.session_token, 0xAB, sizeof(sp.session_token));
    memset(sp.pubkey, 0xCD, sizeof(sp.pubkey));
    memset(sp.pubkey_challenge, 0xEF, sizeof(sp.pubkey_challenge));
    sp.session_ready = true;
    sp.pubkey_set = true;
    sp.pubkey_proof_ok = true;
    sp.pubkey_identity_finalized = true;
    sp.pubkey_challenge_issued = true;
    sp.pubkey_challenge_consumed = true;
    sp.preserve_live_state_on_pubkey_finalize = true;
    sp.last_signed_nonce = 99;

    server_player_clear_live_session_identity(&sp);

    uint8_t zero_token[8] = {0};
    uint8_t zero_pubkey[32] = {0};
    uint8_t zero_challenge[PUBKEY_PROOF_CHALLENGE_SIZE] = {0};
    ASSERT_EQ_INT(memcmp(sp.session_token, zero_token, sizeof(zero_token)), 0);
    ASSERT_EQ_INT(memcmp(sp.pubkey, zero_pubkey, sizeof(zero_pubkey)), 0);
    ASSERT_EQ_INT(memcmp(sp.pubkey_challenge, zero_challenge,
                         sizeof(zero_challenge)), 0);
    ASSERT(!sp.session_ready);
    ASSERT(!sp.pubkey_set);
    ASSERT(!sp.pubkey_proof_ok);
    ASSERT(!sp.pubkey_identity_finalized);
    ASSERT(!sp.pubkey_challenge_issued);
    ASSERT(!sp.pubkey_challenge_consumed);
    ASSERT(!sp.preserve_live_state_on_pubkey_finalize);
    ASSERT_EQ_INT((int)sp.last_signed_nonce, 0);
}

TEST(test_registry_two_pubkeys_one_machine) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t p1[32]; fill_pubkey(p1, 10);
    uint8_t p2[32]; fill_pubkey(p2, 200);
    uint8_t t1[8];  fill_token(t1, 1);
    uint8_t t2[8];  fill_token(t2, 2);

    setup_registered_player(w, 0, p1, t1);
    setup_registered_player(w, 1, p2, t2);

    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, p1), 0);
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, p2), 1);
}

TEST(test_registry_save_load_roundtrip) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32]; fill_pubkey(pk, 42);
    uint8_t tok[8]; fill_token(tok, 17);

    setup_registered_player(w, 2, pk, tok);
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), 2);

    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS, TMP("test_regcat")));
    ASSERT(world_save(w, TMP("test_registry.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    station_catalog_load_all(loaded->stations, MAX_STATIONS, TMP("test_regcat"));
    ASSERT(world_load(loaded, TMP("test_registry.sav")));

    /* Players are cleared on load (they reconnect), so the lookup
     * traversal won't find a live slot — but the persisted registry
     * binding must still be present. After re-establishing a player
     * slot with the persisted token, lookup must resolve again. */
    bool found_persisted = false;
    for (int r = 0; r < MAX_PLAYERS; r++) {
        if (!loaded->pubkey_registry[r].in_use) continue;
        if (memcmp(loaded->pubkey_registry[r].pubkey, pk, 32) == 0 &&
            memcmp(loaded->pubkey_registry[r].session_token, tok, 8) == 0) {
            found_persisted = true; break;
        }
    }
    ASSERT(found_persisted);

    /* Re-attach a player slot to the persisted token; lookup resolves. */
    server_player_t *sp = &loaded->players[4];
    sp->connected = true;
    sp->id = 4;
    memcpy(sp->session_token, tok, 8);
    sp->session_ready = true;
    memcpy(sp->pubkey, pk, 32);
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    sp->pubkey_challenge_consumed = true;
    sp->pubkey_identity_finalized = true;
    ASSERT_EQ_INT(registry_lookup_by_pubkey(loaded, pk), 4);

    remove(TMP("test_registry.sav"));
}

TEST(test_identity_dispatch_session_register_and_proof) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    server_player_t *sp = &w->players[0];
    sp->connected = true;
    sp->id = 0;

    uint8_t tok[8];
    fill_token(tok, 31);
    uint8_t session_msg[16] = { NET_MSG_SESSION };
    memcpy(&session_msg[1], tok, 8);
    memcpy(&session_msg[9], "PILOT01", 7);

    server_session_message_t session;
    ASSERT(server_parse_session_message(session_msg, sizeof(session_msg),
                                        &session));
    ASSERT(server_apply_session_message(w, 0, &session));
    ASSERT(sp->session_ready);
    ASSERT_EQ_INT(memcmp(sp->session_token, tok, 8), 0);
    ASSERT(strcmp(sp->callsign, "PILOT01") == 0);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(pk, sk));
    uint8_t register_msg[REGISTER_PUBKEY_MSG_SIZE] = { NET_MSG_REGISTER_PUBKEY };
    memcpy(&register_msg[1], pk, 32);

    server_pubkey_register_result_t reg;
    ASSERT(server_dispatch_register_pubkey_message(
        w, 0, register_msg, sizeof(register_msg), &reg));
    ASSERT(reg.accepted);
    ASSERT(!reg.same_pubkey);
    ASSERT(sp->pubkey_set);
    ASSERT(!sp->pubkey_proof_ok);

    uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    ASSERT(server_issue_pubkey_challenge(w, 0, challenge));
    uint8_t proof_msg[PROVE_PUBKEY_MSG_SIZE] = { NET_MSG_PROVE_PUBKEY };
    memcpy(&proof_msg[PROVE_PUBKEY_PUBKEY_OFFSET], pk, 32);
    memcpy(&proof_msg[PROVE_PUBKEY_TOKEN_OFFSET], tok, 8);
    ASSERT(pubkey_proof_sign(
        &proof_msg[PROVE_PUBKEY_SIG_OFFSET],
        pk, sk, tok, challenge));

    server_pubkey_proof_result_t proof;
    ASSERT(server_dispatch_pubkey_proof_message(
        w, 0, proof_msg, sizeof(proof_msg), &proof));
    ASSERT(proof.verified);
    ASSERT_EQ_INT(proof.status, SERVER_PUBKEY_PROOF_OK);
    ASSERT(sp->pubkey_proof_ok);
    ASSERT(sp->pubkey_challenge_consumed);
    ASSERT(server_player_has_live_session(sp));
    ASSERT(server_finalize_pubkey_identity(w, 0));
    ASSERT(sp->pubkey_identity_finalized);
    ASSERT_EQ_INT(registry_lookup_by_pubkey(w, pk), 0);
    sp->preserve_live_state_on_pubkey_finalize = true;

    server_pubkey_register_result_t same;
    ASSERT(server_dispatch_register_pubkey_message(
        w, 0, register_msg, sizeof(register_msg), &same));
    ASSERT(same.accepted);
    ASSERT(same.same_pubkey);
    ASSERT(sp->pubkey_proof_ok);
    ASSERT(sp->pubkey_challenge_issued);
    ASSERT(sp->pubkey_challenge_consumed);
    ASSERT(sp->pubkey_identity_finalized);
    ASSERT(sp->preserve_live_state_on_pubkey_finalize);

    uint8_t other_pk[32], other_sk[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(other_pk, other_sk));
    uint8_t other_register_msg[REGISTER_PUBKEY_MSG_SIZE] =
        { NET_MSG_REGISTER_PUBKEY };
    memcpy(&other_register_msg[1], other_pk, 32);
    server_pubkey_register_result_t other;
    ASSERT(!server_dispatch_register_pubkey_message(
        w, 0, other_register_msg, sizeof(other_register_msg), &other));
    ASSERT(!other.accepted);
    ASSERT(!other.same_pubkey);
    ASSERT(other.conflicting_pubkey);
    ASSERT_EQ_INT(memcmp(sp->pubkey, pk, sizeof(pk)), 0);
    ASSERT(sp->pubkey_proof_ok);
    ASSERT(sp->pubkey_challenge_issued);
    ASSERT(sp->pubkey_challenge_consumed);
    ASSERT(sp->pubkey_identity_finalized);
    ASSERT(sp->preserve_live_state_on_pubkey_finalize);
}

TEST(test_initial_pubkey_registration_resets_stale_challenge_state) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    server_player_t *sp = &w->players[0];
    sp->connected = true;
    memset(sp->pubkey_challenge, 0xA7, sizeof(sp->pubkey_challenge));
    sp->pubkey_challenge_issued = true;
    sp->pubkey_challenge_consumed = true;
    sp->pubkey_proof_ok = true;
    sp->pubkey_identity_finalized = true;
    sp->preserve_live_state_on_pubkey_finalize = true;

    uint8_t pk[32];
    fill_pubkey(pk, 42);
    uint8_t register_msg[REGISTER_PUBKEY_MSG_SIZE] =
        { NET_MSG_REGISTER_PUBKEY };
    memcpy(&register_msg[1], pk, sizeof(pk));
    server_pubkey_register_result_t result;
    ASSERT(server_dispatch_register_pubkey_message(
        w, 0, register_msg, sizeof(register_msg), &result));
    uint8_t zero_challenge[PUBKEY_PROOF_CHALLENGE_SIZE] = {0};
    ASSERT(result.accepted);
    ASSERT(!result.same_pubkey);
    ASSERT(!result.conflicting_pubkey);
    ASSERT_EQ_INT(memcmp(sp->pubkey_challenge, zero_challenge,
                         sizeof(zero_challenge)), 0);
    ASSERT(!sp->pubkey_challenge_issued);
    ASSERT(!sp->pubkey_challenge_consumed);
    ASSERT(!sp->pubkey_proof_ok);
    ASSERT(!sp->pubkey_identity_finalized);
    ASSERT(sp->preserve_live_state_on_pubkey_finalize);
}

TEST(test_identity_dispatch_rejects_wrong_session_proof) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    server_player_t *sp = &w->players[0];
    sp->connected = true;
    sp->id = 0;
    uint8_t tok[8], other_tok[8];
    fill_token(tok, 33);
    fill_token(other_tok, 34);
    server_session_message_t session = {0};
    memcpy(session.token, tok, 8);
    ASSERT(server_apply_session_message(w, 0, &session));

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(pk, sk));
    uint8_t register_msg[REGISTER_PUBKEY_MSG_SIZE] = { NET_MSG_REGISTER_PUBKEY };
    memcpy(&register_msg[1], pk, 32);
    ASSERT(server_dispatch_register_pubkey_message(
        w, 0, register_msg, sizeof(register_msg), NULL));

    uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    ASSERT(server_issue_pubkey_challenge(w, 0, challenge));
    uint8_t proof_msg[PROVE_PUBKEY_MSG_SIZE] = { NET_MSG_PROVE_PUBKEY };
    memcpy(&proof_msg[PROVE_PUBKEY_PUBKEY_OFFSET], pk, 32);
    memcpy(&proof_msg[PROVE_PUBKEY_TOKEN_OFFSET], other_tok, 8);
    ASSERT(pubkey_proof_sign(
        &proof_msg[PROVE_PUBKEY_SIG_OFFSET],
        pk, sk, other_tok, challenge));

    server_pubkey_proof_result_t proof;
    ASSERT(server_dispatch_pubkey_proof_message(
        w, 0, proof_msg, sizeof(proof_msg), &proof));
    ASSERT(!proof.verified);
    ASSERT_EQ_INT(proof.status, SERVER_PUBKEY_PROOF_SESSION_MISMATCH);
    ASSERT(!sp->pubkey_proof_ok);
    ASSERT(!server_finalize_pubkey_identity(w, 0));
}

TEST(test_register_pubkey_rejects_zero_and_oversized_assertions) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    server_player_t *sp = &w->players[0];
    sp->connected = true;
    sp->id = 0;
    uint8_t zero_msg[REGISTER_PUBKEY_MSG_SIZE] =
        { NET_MSG_REGISTER_PUBKEY };
    server_pubkey_register_result_t result;
    memset(&result, 0xA5, sizeof(result));
    ASSERT(!server_dispatch_register_pubkey_message(
        w, 0, zero_msg, sizeof(zero_msg), &result));
    ASSERT(!result.accepted);
    ASSERT(!sp->pubkey_set);

    uint8_t oversized[REGISTER_PUBKEY_MSG_SIZE + 1] =
        { NET_MSG_REGISTER_PUBKEY };
    fill_pubkey(&oversized[1], 71);
    memset(&result, 0xB6, sizeof(result));
    ASSERT(!server_dispatch_register_pubkey_message(
        w, 0, oversized, sizeof(oversized), &result));
    ASSERT(!result.accepted);
    ASSERT(!sp->pubkey_set);
}

TEST(test_challenge_requires_connected_registered_session) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    server_player_t *sp = &w->players[0];
    sp->id = 0;
    uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    uint8_t zero_challenge[PUBKEY_PROOF_CHALLENGE_SIZE] = {0};

    memset(challenge, 0xA1, sizeof(challenge));
    ASSERT(!server_issue_pubkey_challenge(w, 0, challenge));
    ASSERT_EQ_INT(memcmp(challenge, zero_challenge,
                         sizeof(challenge)), 0);

    sp->connected = true;
    memset(challenge, 0xB2, sizeof(challenge));
    ASSERT(!server_issue_pubkey_challenge(w, 0, challenge));
    ASSERT_EQ_INT(memcmp(challenge, zero_challenge,
                         sizeof(challenge)), 0);

    uint8_t pk[32];
    fill_pubkey(pk, 73);
    uint8_t register_msg[REGISTER_PUBKEY_MSG_SIZE] =
        { NET_MSG_REGISTER_PUBKEY };
    memcpy(&register_msg[1], pk, sizeof(pk));
    ASSERT(server_dispatch_register_pubkey_message(
        w, 0, register_msg, sizeof(register_msg), NULL));
    memset(challenge, 0xC3, sizeof(challenge));
    ASSERT(!server_issue_pubkey_challenge(w, 0, challenge));
    ASSERT_EQ_INT(memcmp(challenge, zero_challenge,
                         sizeof(challenge)), 0);

    server_session_message_t session = {0};
    fill_token(session.token, 75);
    ASSERT(server_apply_session_message(w, 0, &session));
    registry_entropy_fixture_t deterministic = {
        .fill = 0x4D,
        .bytes_to_write = PUBKEY_PROOF_CHALLENGE_SIZE,
        .succeed = true,
    };
    signal_crypto_test_set_entropy_provider(
        registry_entropy_provider, &deterministic);
    bool issued = server_issue_pubkey_challenge(w, 0, challenge);
    signal_crypto_test_reset_entropy_provider();
    ASSERT(issued);
    ASSERT(sp->pubkey_challenge_issued);
}

TEST(test_network_auth_bootstrap_waits_for_both_message_orders) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    int fake_connections[2] = {0};

    for (int session_first = 0; session_first < 2; session_first++) {
        int slot = session_first;
        server_player_t *sp = &w->players[slot];
        sp->connected = true;
        sp->id = (uint8_t)slot;
        sp->connection->conn = &fake_connections[slot];

        uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
        ASSERT(signal_crypto_keypair(pk, sk));
        uint8_t register_msg[REGISTER_PUBKEY_MSG_SIZE] =
            { NET_MSG_REGISTER_PUBKEY };
        memcpy(&register_msg[1], pk, sizeof(pk));
        server_session_message_t session = {0};
        fill_token(session.token, (uint8_t)(83 + slot));

        if (session_first) {
            ASSERT(server_apply_session_message(w, slot, &session));
            /* SESSION alone is the legacy token-only compatibility path. */
            ASSERT(server_player_has_live_session(sp));
            ASSERT(server_player_is_gameplay_ready(sp));
            ASSERT(server_dispatch_register_pubkey_message(
                w, slot, register_msg, sizeof(register_msg), NULL));
        } else {
            ASSERT(server_dispatch_register_pubkey_message(
                w, slot, register_msg, sizeof(register_msg), NULL));
            ASSERT(!server_player_has_live_session(sp));
            ASSERT(!server_player_is_gameplay_ready(sp));
            ASSERT(server_apply_session_message(w, slot, &session));
        }

        /* Having both client messages is still unpublished until the
         * dedicated transport has generated its one-time challenge. */
        ASSERT(!server_player_has_live_session(sp));
        ASSERT(!server_player_is_gameplay_ready(sp));
        registry_entropy_fixture_t deterministic = {
            .fill = (uint8_t)(0x51 + slot),
            .bytes_to_write = PUBKEY_PROOF_CHALLENGE_SIZE,
            .succeed = true,
        };
        uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
        signal_crypto_test_set_entropy_provider(
            registry_entropy_provider, &deterministic);
        bool issued = server_issue_pubkey_challenge(w, slot, challenge);
        signal_crypto_test_reset_entropy_provider();
        ASSERT(issued);
        ASSERT(!server_player_has_live_session(sp));
        ASSERT(!server_player_is_gameplay_ready(sp));

        uint8_t duplicate[PUBKEY_PROOF_CHALLENGE_SIZE];
        memset(duplicate, 0xE1, sizeof(duplicate));
        ASSERT(!server_issue_pubkey_challenge(w, slot, duplicate));
        uint8_t zeros[PUBKEY_PROOF_CHALLENGE_SIZE] = {0};
        ASSERT_EQ_INT(memcmp(duplicate, zeros, sizeof(duplicate)), 0);

        uint8_t proof_msg[PROVE_PUBKEY_MSG_SIZE] =
            { NET_MSG_PROVE_PUBKEY };
        memcpy(&proof_msg[PROVE_PUBKEY_PUBKEY_OFFSET], pk, sizeof(pk));
        memcpy(&proof_msg[PROVE_PUBKEY_TOKEN_OFFSET],
               session.token, sizeof(session.token));
        ASSERT(pubkey_proof_sign(
            &proof_msg[PROVE_PUBKEY_SIG_OFFSET],
            pk, sk, session.token, challenge));
        server_pubkey_proof_result_t proof;
        ASSERT(server_dispatch_pubkey_proof_message(
            w, slot, proof_msg, sizeof(proof_msg), &proof));
        ASSERT(proof.verified);
        ASSERT(server_player_has_live_session(sp));
        ASSERT(server_player_is_gameplay_ready(sp));
    }
}

TEST(test_socket_token_only_session_remains_gameplay_compatible) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    int fake_connection = 0;
    server_player_t *sp = &w->players[0];
    sp->connected = true;
    sp->id = 0;
    sp->connection->conn = &fake_connection;
    server_session_message_t session = {0};
    fill_token(session.token, 91);

    ASSERT(server_apply_session_message(w, 0, &session));
    ASSERT(sp->session_ready);
    ASSERT(!sp->pubkey_set);
    ASSERT(!sp->pubkey_challenge_issued);
    ASSERT(server_player_has_live_session(sp));
    ASSERT(server_player_is_gameplay_ready(sp));
    float deadline_before = sp->grace_timer;
    ASSERT(!server_player_tick_auth_timeout(
        sp, SERVER_PUBKEY_PROOF_TIMEOUT_SECONDS * 2.0f));
    ASSERT_EQ_FLOAT(sp->grace_timer, deadline_before, 0.0001f);
}

TEST(test_pubkey_proof_pending_timeout_is_bounded) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    server_player_t *sp = &w->players[0];
    sp->connected = true;

    server_session_message_t session = {0};
    fill_token(session.token, 93);
    ASSERT(server_apply_session_message(w, 0, &session));
    uint8_t pk[32];
    fill_pubkey(pk, 95);
    uint8_t register_msg[REGISTER_PUBKEY_MSG_SIZE] =
        { NET_MSG_REGISTER_PUBKEY };
    memcpy(&register_msg[1], pk, sizeof(pk));
    ASSERT(server_dispatch_register_pubkey_message(
        w, 0, register_msg, sizeof(register_msg), NULL));

    registry_entropy_fixture_t deterministic = {
        .fill = 0x7B,
        .bytes_to_write = PUBKEY_PROOF_CHALLENGE_SIZE,
        .succeed = true,
    };
    uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    signal_crypto_test_set_entropy_provider(
        registry_entropy_provider, &deterministic);
    bool issued = server_issue_pubkey_challenge(w, 0, challenge);
    signal_crypto_test_reset_entropy_provider();
    ASSERT(issued);
    ASSERT_EQ_FLOAT(sp->grace_timer,
                    SERVER_PUBKEY_PROOF_TIMEOUT_SECONDS, 0.0001f);
    ASSERT(!server_player_tick_auth_timeout(
        sp, SERVER_PUBKEY_PROOF_TIMEOUT_SECONDS * 0.5f));
    ASSERT(server_player_tick_auth_timeout(
        sp, SERVER_PUBKEY_PROOF_TIMEOUT_SECONDS * 0.5f));
}

TEST(test_unproved_asserted_pubkey_is_anonymous_for_death_provenance) {
    SERVER_PLAYER_DECL(sp);
    sp.session_ready = true;
    fill_token(sp.session_token, 103);
    sp.pubkey_set = true;
    fill_pubkey(sp.pubkey, 97);
    uint8_t attributed[32];
    memset(attributed, 0xD4, sizeof(attributed));
    ASSERT(!server_player_copy_verified_pubkey(&sp, attributed));
    uint8_t zero[32] = {0};
    ASSERT_EQ_INT(memcmp(attributed, zero, sizeof(zero)), 0);

    sp.pubkey_proof_ok = true;
    ASSERT(!server_player_copy_verified_pubkey(&sp, attributed));
    ASSERT_EQ_INT(memcmp(attributed, zero, sizeof(zero)), 0);
    sp.pubkey_challenge_consumed = true;
    ASSERT(server_player_copy_verified_pubkey(&sp, attributed));
    ASSERT_EQ_INT(memcmp(attributed, sp.pubkey, sizeof(attributed)), 0);
}

TEST(test_generated_session_entropy_failure_refuses_connection) {
    SERVER_PLAYER_DECL(sp);
    memset(sp.session_token, 0xA4, sizeof(sp.session_token));
    memset(sp.pubkey, 0xB5, sizeof(sp.pubkey));
    memset(sp.pubkey_challenge, 0xC6, sizeof(sp.pubkey_challenge));
    sp.connected = true;
    sp.session_ready = true;
    sp.pubkey_set = true;
    sp.pubkey_proof_ok = true;
    sp.pubkey_challenge_issued = true;
    sp.pubkey_challenge_consumed = true;

    registry_entropy_fixture_t fault = {
        .fill = 0x7E,
        .bytes_to_write = 3,
        .succeed = false,
    };
    signal_crypto_test_set_entropy_provider(
        registry_entropy_provider, &fault);
    bool ok = server_player_start_generated_session(&sp);
    signal_crypto_test_reset_entropy_provider();

    uint8_t zero_token[sizeof(sp.session_token)] = {0};
    uint8_t zero_pubkey[sizeof(sp.pubkey)] = {0};
    uint8_t zero_challenge[sizeof(sp.pubkey_challenge)] = {0};
    ASSERT(!ok);
    ASSERT(!sp.connected);
    ASSERT(!sp.session_ready);
    ASSERT(!sp.pubkey_set);
    ASSERT(!sp.pubkey_proof_ok);
    ASSERT(!sp.pubkey_challenge_issued);
    ASSERT(!sp.pubkey_challenge_consumed);
    ASSERT_EQ_INT(memcmp(sp.session_token, zero_token,
                         sizeof(zero_token)), 0);
    ASSERT_EQ_INT(memcmp(sp.pubkey, zero_pubkey,
                         sizeof(zero_pubkey)), 0);
    ASSERT_EQ_INT(memcmp(sp.pubkey_challenge, zero_challenge,
                         sizeof(zero_challenge)), 0);
}

TEST(test_challenge_entropy_failure_clears_and_cannot_complete) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    server_player_t *sp = &w->players[0];
    sp->connected = true;
    sp->id = 0;
    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(pk, sk));
    uint8_t tok[8];
    fill_token(tok, 61);

    uint8_t register_msg[REGISTER_PUBKEY_MSG_SIZE] =
        { NET_MSG_REGISTER_PUBKEY };
    memcpy(&register_msg[1], pk, sizeof(pk));
    ASSERT(server_dispatch_register_pubkey_message(
        w, 0, register_msg, sizeof(register_msg), NULL));
    server_session_message_t session = {0};
    memcpy(session.token, tok, sizeof(tok));
    ASSERT(server_apply_session_message(w, 0, &session));

    memset(sp->pubkey_challenge, 0xD7, sizeof(sp->pubkey_challenge));
    sp->pubkey_challenge_issued = false;
    sp->pubkey_challenge_consumed = true;
    sp->pubkey_proof_ok = true;
    uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    memset(challenge, 0xE8, sizeof(challenge));
    registry_entropy_fixture_t fault = {
        .fill = 0x9F,
        .bytes_to_write = 13,
        .succeed = false,
    };

    signal_crypto_test_set_entropy_provider(
        registry_entropy_provider, &fault);
    bool issued = server_issue_pubkey_challenge(w, 0, challenge);
    signal_crypto_test_reset_entropy_provider();

    uint8_t zero_challenge[PUBKEY_PROOF_CHALLENGE_SIZE] = {0};
    ASSERT(!issued);
    ASSERT(!sp->pubkey_challenge_issued);
    ASSERT(!sp->pubkey_challenge_consumed);
    ASSERT(!sp->pubkey_proof_ok);
    ASSERT_EQ_INT(memcmp(challenge, zero_challenge,
                         sizeof(challenge)), 0);
    ASSERT_EQ_INT(memcmp(sp->pubkey_challenge, zero_challenge,
                         sizeof(zero_challenge)), 0);

    uint8_t proof_msg[PROVE_PUBKEY_MSG_SIZE] =
        { NET_MSG_PROVE_PUBKEY };
    memcpy(&proof_msg[PROVE_PUBKEY_PUBKEY_OFFSET], pk, sizeof(pk));
    memcpy(&proof_msg[PROVE_PUBKEY_TOKEN_OFFSET], tok, sizeof(tok));
    ASSERT(pubkey_proof_sign(
        &proof_msg[PROVE_PUBKEY_SIG_OFFSET],
        pk, sk, tok, zero_challenge));
    server_pubkey_proof_result_t proof;
    ASSERT(server_dispatch_pubkey_proof_message(
        w, 0, proof_msg, sizeof(proof_msg), &proof));
    ASSERT(!proof.verified);
    ASSERT_EQ_INT(proof.status, SERVER_PUBKEY_PROOF_NO_CHALLENGE);
    ASSERT(!sp->pubkey_proof_ok);
}

TEST(test_protocol_v3_classifies_legacy_pubkey_proof_without_accepting_it) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    server_player_t *sp = &w->players[0];
    sp->connected = true;
    sp->id = 0;
    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(pk, sk));
    uint8_t tok[8];
    fill_token(tok, 62);

    uint8_t register_msg[REGISTER_PUBKEY_MSG_SIZE] =
        { NET_MSG_REGISTER_PUBKEY };
    memcpy(&register_msg[1], pk, sizeof(pk));
    ASSERT(server_dispatch_register_pubkey_message(
        w, 0, register_msg, sizeof(register_msg), NULL));
    server_session_message_t session = {0};
    memcpy(session.token, tok, sizeof(tok));
    ASSERT(server_apply_session_message(w, 0, &session));

    uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    ASSERT(server_issue_pubkey_challenge(w, 0, challenge));
    uint8_t saved_challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    memcpy(saved_challenge, challenge, sizeof(saved_challenge));

    uint8_t proof_msg[PROVE_PUBKEY_MSG_SIZE] =
        { NET_MSG_PROVE_PUBKEY };
    memcpy(&proof_msg[PROVE_PUBKEY_PUBKEY_OFFSET], pk, sizeof(pk));
    memcpy(&proof_msg[PROVE_PUBKEY_TOKEN_OFFSET], tok, sizeof(tok));
    ASSERT(pubkey_proof_v1_sign(
        &proof_msg[PROVE_PUBKEY_SIG_OFFSET], pk, sk, tok));

    server_pubkey_proof_result_t proof;
    ASSERT(server_dispatch_pubkey_proof_message(
        w, 0, proof_msg, sizeof(proof_msg), &proof));
    ASSERT_EQ_INT(SIGNAL_PROTOCOL_VERSION, 3);
    ASSERT_EQ_INT(
        proof.status, SERVER_PUBKEY_PROOF_LEGACY_VERSION);
    ASSERT(strcmp(server_pubkey_proof_status_name(proof.status),
                  "legacy-version") == 0);
    ASSERT(server_pubkey_proof_status_requires_disconnect(proof.status));
    ASSERT(!server_pubkey_proof_status_requires_disconnect(
        SERVER_PUBKEY_PROOF_BAD_SIGNATURE));
    ASSERT(!proof.verified);
    ASSERT(!sp->pubkey_proof_ok);
    ASSERT(sp->pubkey_challenge_issued);
    ASSERT(!sp->pubkey_challenge_consumed);
    ASSERT_EQ_INT(
        memcmp(sp->pubkey_challenge, saved_challenge,
               sizeof(saved_challenge)),
        0);

    /* Classification is rejection, not accidental challenge consumption.
     * The valid protocol-v3 proof can still complete in this unit boundary;
     * the WebSocket layer closes immediately on LEGACY_VERSION. */
    ASSERT(pubkey_proof_sign(
        &proof_msg[PROVE_PUBKEY_SIG_OFFSET],
        pk, sk, tok, challenge));
    ASSERT(server_dispatch_pubkey_proof_message(
        w, 0, proof_msg, sizeof(proof_msg), &proof));
    ASSERT_EQ_INT(proof.status, SERVER_PUBKEY_PROOF_OK);
    ASSERT(proof.verified);
    ASSERT(sp->pubkey_proof_ok);
    ASSERT(sp->pubkey_challenge_consumed);
}

TEST(test_loopback_register_session_challenge_proof_is_one_time) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    server_player_t *sp = &w->players[0];
    sp->connected = true;
    sp->id = 0;
    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(pk, sk));
    uint8_t tok[8];
    fill_token(tok, 63);

    /* This is the synchronous loopback wire order: REGISTER, SESSION,
     * server challenge, then client proof. */
    uint8_t register_msg[REGISTER_PUBKEY_MSG_SIZE] =
        { NET_MSG_REGISTER_PUBKEY };
    memcpy(&register_msg[1], pk, sizeof(pk));
    ASSERT(server_dispatch_register_pubkey_message(
        w, 0, register_msg, sizeof(register_msg), NULL));
    server_session_message_t session = {0};
    memcpy(session.token, tok, sizeof(tok));
    ASSERT(server_apply_session_message(w, 0, &session));

    uint8_t proof_msg[PROVE_PUBKEY_MSG_SIZE] =
        { NET_MSG_PROVE_PUBKEY };
    memcpy(&proof_msg[PROVE_PUBKEY_PUBKEY_OFFSET], pk, sizeof(pk));
    memcpy(&proof_msg[PROVE_PUBKEY_TOKEN_OFFSET], tok, sizeof(tok));
    uint8_t zero_challenge[PUBKEY_PROOF_CHALLENGE_SIZE] = {0};
    ASSERT(pubkey_proof_sign(
        &proof_msg[PROVE_PUBKEY_SIG_OFFSET],
        pk, sk, tok, zero_challenge));
    server_pubkey_proof_result_t proof;
    ASSERT(server_dispatch_pubkey_proof_message(
        w, 0, proof_msg, sizeof(proof_msg), &proof));
    ASSERT_EQ_INT(proof.status, SERVER_PUBKEY_PROOF_NO_CHALLENGE);
    ASSERT(!proof.verified);

    registry_entropy_fixture_t deterministic = {
        .fill = 0x6D,
        .bytes_to_write = PUBKEY_PROOF_CHALLENGE_SIZE,
        .succeed = true,
    };
    uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    signal_crypto_test_set_entropy_provider(
        registry_entropy_provider, &deterministic);
    bool issued = server_issue_pubkey_challenge(w, 0, challenge);
    signal_crypto_test_reset_entropy_provider();
    ASSERT(issued);
    ASSERT(sp->pubkey_challenge_issued);
    ASSERT(!sp->pubkey_challenge_consumed);
    for (size_t i = 0; i < sizeof(challenge); i++)
        ASSERT_EQ_INT(challenge[i], 0x6D);

    ASSERT(pubkey_proof_sign(
        &proof_msg[PROVE_PUBKEY_SIG_OFFSET],
        pk, sk, tok, challenge));
    uint8_t valid_sig[SIGNAL_CRYPTO_SIG_BYTES];
    memcpy(valid_sig, &proof_msg[PROVE_PUBKEY_SIG_OFFSET],
           sizeof(valid_sig));
    proof_msg[PROVE_PUBKEY_SIG_OFFSET] ^= 0x80u;
    ASSERT(server_dispatch_pubkey_proof_message(
        w, 0, proof_msg, sizeof(proof_msg), &proof));
    ASSERT_EQ_INT(proof.status, SERVER_PUBKEY_PROOF_BAD_SIGNATURE);
    ASSERT(!proof.verified);
    ASSERT(!sp->pubkey_challenge_consumed);
    ASSERT_EQ_INT(memcmp(sp->pubkey_challenge, challenge,
                         sizeof(challenge)), 0);

    memcpy(&proof_msg[PROVE_PUBKEY_SIG_OFFSET], valid_sig,
           sizeof(valid_sig));
    ASSERT(server_dispatch_pubkey_proof_message(
        w, 0, proof_msg, sizeof(proof_msg), &proof));
    ASSERT_EQ_INT(proof.status, SERVER_PUBKEY_PROOF_OK);
    ASSERT(proof.verified);
    ASSERT(sp->pubkey_proof_ok);
    ASSERT(sp->pubkey_challenge_consumed);
    ASSERT_EQ_INT(memcmp(sp->pubkey_challenge, zero_challenge,
                         sizeof(zero_challenge)), 0);

    ASSERT(server_dispatch_pubkey_proof_message(
        w, 0, proof_msg, sizeof(proof_msg), &proof));
    ASSERT_EQ_INT(proof.status, SERVER_PUBKEY_PROOF_CHALLENGE_REPLAY);
    ASSERT(!proof.verified);
    ASSERT(sp->pubkey_proof_ok);
}

void register_registry_tests(void) {
    TEST_SECTION("\nPubkey registry (#479 A.2):\n");
    RUN(test_registry_fresh_registration);
    RUN(test_registry_idempotent_reregistration);
    RUN(test_registry_reconnect_with_new_token);
    RUN(test_registry_lookup_rejects_token_only_impostor);
    RUN(test_registry_same_token_takeover_moves_live_ship);
    RUN(test_player_transfer_rebinds_slot_obligations_and_character);
    RUN(test_player_transfer_rejects_invalid_destination_asset);
    RUN(test_registry_same_token_reattach_finds_active_duplicate);
    RUN(test_session_reattach_rejects_cross_identity_token_takeover);
    RUN(test_session_reattach_conflict_wins_over_grace_match);
    RUN(test_registry_lookup_requires_exact_finalized_identity);
    RUN(test_registry_duplicate_stale_row_cannot_hide_live_binding);
    RUN(test_registry_capacity_reclaims_only_stale_bindings);
    RUN(test_pending_pubkey_abandon_restores_published_token_identity);
    RUN(test_session_first_reattach_pubkey_upgrade_preserves_ship);
    RUN(test_live_session_requires_nonzero_token);
    RUN(test_player_clear_live_session_identity);
    RUN(test_registry_two_pubkeys_one_machine);
    RUN(test_registry_save_load_roundtrip);
    RUN(test_identity_dispatch_session_register_and_proof);
    RUN(test_initial_pubkey_registration_resets_stale_challenge_state);
    RUN(test_identity_dispatch_rejects_wrong_session_proof);
    RUN(test_register_pubkey_rejects_zero_and_oversized_assertions);
    RUN(test_challenge_requires_connected_registered_session);
    RUN(test_network_auth_bootstrap_waits_for_both_message_orders);
    RUN(test_socket_token_only_session_remains_gameplay_compatible);
    RUN(test_pubkey_proof_pending_timeout_is_bounded);
    RUN(test_unproved_asserted_pubkey_is_anonymous_for_death_provenance);
    RUN(test_generated_session_entropy_failure_refuses_connection);
    RUN(test_challenge_entropy_failure_clears_and_cannot_complete);
    RUN(test_protocol_v3_classifies_legacy_pubkey_proof_without_accepting_it);
    RUN(test_loopback_register_session_challenge_proof_is_one_time);
}
