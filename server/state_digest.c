#include "state_digest.h"

#include <stddef.h>
#include <string.h>

#include "manifest.h"
#include "sha256.h"

typedef sha256_ctx_t state_digest_ctx_t;

static void digest_bytes(state_digest_ctx_t *ctx, const void *data, size_t len)
{
    sha256_update(ctx, data, len);
}

static void digest_u8(state_digest_ctx_t *ctx, uint8_t value)
{
    digest_bytes(ctx, &value, sizeof(value));
}

static void digest_bool(state_digest_ctx_t *ctx, bool value)
{
    digest_u8(ctx, value ? 1u : 0u);
}

static void digest_u16(state_digest_ctx_t *ctx, uint16_t value)
{
    uint8_t packed[2] = {
        (uint8_t)value,
        (uint8_t)(value >> 8),
    };
    digest_bytes(ctx, packed, sizeof(packed));
}

static void digest_i16(state_digest_ctx_t *ctx, int16_t value)
{
    digest_u16(ctx, (uint16_t)value);
}

static void digest_u32(state_digest_ctx_t *ctx, uint32_t value)
{
    uint8_t packed[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 24),
    };
    digest_bytes(ctx, packed, sizeof(packed));
}

static void digest_i32(state_digest_ctx_t *ctx, int32_t value)
{
    digest_u32(ctx, (uint32_t)value);
}

static void digest_u64(state_digest_ctx_t *ctx, uint64_t value)
{
    uint8_t packed[8];
    for (int i = 0; i < 8; i++)
        packed[i] = (uint8_t)(value >> (i * 8));
    digest_bytes(ctx, packed, sizeof(packed));
}

static void digest_i64(state_digest_ctx_t *ctx, int64_t value)
{
    digest_u64(ctx, (uint64_t)value);
}

static void digest_float(state_digest_ctx_t *ctx, float value)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    digest_u32(ctx, bits);
}

static void digest_text(state_digest_ctx_t *ctx, const char *text, size_t cap)
{
    size_t len = 0;
    if (text) {
        while (len < cap && text[len] != '\0')
            len++;
    }
    digest_u32(ctx, (uint32_t)len);
    if (len > 0)
        digest_bytes(ctx, text, len);
}

static void digest_vec2(state_digest_ctx_t *ctx, vec2 value)
{
    digest_float(ctx, value.x);
    digest_float(ctx, value.y);
}

static void digest_entity_ref(state_digest_ctx_t *ctx, entity_ref_t ref)
{
    if (entity_ref_is_none(ref))
        ref = entity_ref_none();
    digest_u8(ctx, ref.kind);
    digest_i16(ctx, ref.index);
    digest_i16(ctx, ref.part);
    digest_u16(ctx, ref.generation);
}

static void digest_actor_principal(state_digest_ctx_t *ctx,
                                   const actor_principal_t *principal)
{
    static const uint8_t redacted_id[ACTOR_PRINCIPAL_ID_SIZE] = {0};
    actor_principal_t none = actor_principal_none();
    if (!principal)
        principal = &none;
    digest_u8(ctx, principal->kind);
    /*
     * NPC principals currently hash npc_ship_t.session_token. Committing that
     * low-entropy compatibility identity would make the public state root an
     * offline verifier for the token. Keep the namespace visible, but redact
     * the identifier until NPCs have a collision-resistant public birth ID.
     */
    if (principal->kind == ACTOR_PRINCIPAL_NPC) {
        digest_bytes(ctx, redacted_id, sizeof(redacted_id));
        return;
    }
    digest_bytes(ctx, principal->id, sizeof(principal->id));
}

static bool ledger_key_is_legacy_session_token(
    const uint8_t player_pubkey[32])
{
    /*
     * Token-based ledger callers encode their 8-byte session token directly
     * followed by 24 zero bytes. This is an explicit compatibility format,
     * not a public key, and must never enter a public digest preimage.
     */
    for (size_t i = 8; i < 32; i++) {
        if (player_pubkey[i] != 0)
            return false;
    }
    return true;
}

static void digest_ledger_identity(
    state_digest_ctx_t *ctx,
    const uint8_t player_pubkey[32])
{
    bool is_public_key =
        !ledger_key_is_legacy_session_token(player_pubkey);
    digest_bool(ctx, is_public_key);
    if (is_public_key)
        digest_bytes(ctx, player_pubkey, 32);
}

static void digest_tractor_binding(state_digest_ctx_t *ctx,
                                   const tractor_binding_t *binding)
{
    if (!binding) {
        digest_u8(ctx, (uint8_t)TRACTOR_SOURCE_NONE);
        digest_i16(ctx, -1);
        digest_i16(ctx, -1);
        digest_u16(ctx, 0);
        return;
    }
    if (binding->kind == TRACTOR_SOURCE_NONE) {
        digest_u8(ctx, (uint8_t)TRACTOR_SOURCE_NONE);
        digest_i16(ctx, -1);
        digest_i16(ctx, -1);
        digest_u16(ctx, 0);
        return;
    }
    digest_u8(ctx, (uint8_t)binding->kind);
    digest_i16(ctx, binding->source_index);
    digest_i16(ctx, binding->source_part);
    digest_u16(ctx, binding->source_generation);
}

static void digest_cargo_unit(state_digest_ctx_t *ctx,
                              const cargo_unit_t *unit)
{
    uint8_t packed[CARGO_UNIT_WIRE_SIZE];
    cargo_unit_t zero = {0};
    cargo_unit_wire_pack(unit ? unit : &zero, packed);
    digest_bytes(ctx, packed, sizeof(packed));
}

static void digest_receipt_chain(state_digest_ctx_t *ctx,
                                 const cargo_receipt_chain_t *chain)
{
    uint8_t len = chain ? chain->len : 0;
    if (len > CARGO_RECEIPT_CHAIN_MAX_LEN)
        len = CARGO_RECEIPT_CHAIN_MAX_LEN;
    digest_u8(ctx, len);
    for (uint8_t i = 0; i < len; i++) {
        uint8_t packed[CARGO_RECEIPT_SIZE];
        cargo_receipt_pack(&chain->links[i], packed);
        digest_bytes(ctx, packed, sizeof(packed));
    }
}

static void digest_cargo_store(state_digest_ctx_t *ctx,
                               const cargo_store_t *store)
{
    const manifest_t *manifest = store ? &store->manifest : NULL;
    const ship_receipts_t *receipts =
        store ? cargo_store_receipts_const(store) : NULL;
    uint16_t manifest_count = manifest ? manifest->count : 0;
    uint16_t receipt_count = receipts ? receipts->count : 0;
    uint16_t safe_manifest_count =
        manifest && manifest->units ? manifest_count : 0;
    uint16_t safe_receipt_count =
        receipts && receipts->chains ? receipt_count : 0;
    if (manifest && safe_manifest_count > manifest->cap)
        safe_manifest_count = manifest->cap;
    if (receipts && safe_receipt_count > receipts->cap)
        safe_receipt_count = receipts->cap;

    /*
     * Counts are semantic and intentionally hashed raw. The validity bits
     * keep corrupt count/capacity combinations distinct without making a
     * valid heap allocation capacity part of the canonical state.
     */
    digest_u16(ctx, manifest_count);
    digest_u16(ctx, receipt_count);
    digest_bool(ctx, safe_manifest_count == manifest_count);
    digest_bool(ctx, safe_receipt_count == receipt_count);
    digest_u16(ctx, safe_manifest_count);
    digest_u16(ctx, safe_receipt_count);
    for (uint16_t i = 0; i < safe_manifest_count; i++) {
        const cargo_unit_t *unit = &manifest->units[i];
        const cargo_receipt_chain_t *chain =
            i < safe_receipt_count
                ? &receipts->chains[i]
                : NULL;
        digest_cargo_unit(ctx, unit);
        digest_receipt_chain(ctx, chain);
    }
}

static void digest_knowledge_view(state_digest_ctx_t *ctx,
                                  const knowledge_view_t *view)
{
    uint8_t count = view ? view->count : 0;
    uint8_t capacity = view ? view->capacity : 0;
    if (count > KNOWLEDGE_VIEW_MAX_CAP)
        count = KNOWLEDGE_VIEW_MAX_CAP;
    if (capacity > KNOWLEDGE_VIEW_MAX_CAP)
        capacity = KNOWLEDGE_VIEW_MAX_CAP;

    digest_u8(ctx, count);
    digest_u8(ctx, capacity);
    for (uint8_t i = 0; i < count; i++) {
        const knowledge_item_t *item = &view->items[i];
        digest_u8(ctx, item->kind);
        digest_u8(ctx, item->hops);
        digest_u8(ctx, item->confidence);
        digest_u8(ctx, item->salience);
        digest_u8(ctx, item->payload_kind);
        digest_bytes(ctx, item->subject_hash, sizeof(item->subject_hash));
        digest_bytes(ctx, item->chain_anchor, sizeof(item->chain_anchor));
        digest_bytes(ctx, item->source_hash, sizeof(item->source_hash));
        digest_bytes(ctx, item->witness_hash, sizeof(item->witness_hash));
        digest_u64(ctx, item->observed_tick);
        digest_u64(ctx, item->learned_tick);
        digest_bytes(ctx, item->payload, sizeof(item->payload));
    }
}

static void digest_hnn_memory(state_digest_ctx_t *ctx,
                              const hnn_memory_t *memory)
{
    digest_i32(ctx, memory ? memory->experience_count : 0);
    if (!memory) {
        for (int i = 0; i < HNN_DIM; i++)
            digest_float(ctx, 0.0f);
        return;
    }
    for (int i = 0; i < HNN_DIM; i++)
        digest_float(ctx, memory->store[i]);
}

static void digest_input_intent(state_digest_ctx_t *ctx,
                                const input_intent_t *input)
{
    input_intent_t zero = {0};
    if (!input)
        input = &zero;

    digest_float(ctx, input->turn);
    digest_float(ctx, input->thrust);
    digest_bool(ctx, input->mine);
    digest_bool(ctx, input->dock);
    digest_bool(ctx, input->launch);
    digest_bool(ctx, input->interact);
    digest_bool(ctx, input->service_sell);
    digest_i32(ctx, (int32_t)input->service_sell_only);
    digest_i32(ctx, (int32_t)input->service_sell_grade);
    digest_bool(ctx, input->service_sell_one);
    digest_bool(ctx, input->service_repair);
    digest_bool(ctx, input->upgrade_mining);
    digest_bool(ctx, input->upgrade_hold);
    digest_bool(ctx, input->upgrade_tractor);
    digest_bool(ctx, input->place_outpost);
    digest_u8(ctx, (uint8_t)input->place_target_station);
    digest_u8(ctx, (uint8_t)input->place_target_ring);
    digest_u8(ctx, (uint8_t)input->place_target_slot);
    digest_bool(ctx, input->add_plan);
    digest_u8(ctx, (uint8_t)input->plan_station);
    digest_u8(ctx, (uint8_t)input->plan_ring);
    digest_u8(ctx, (uint8_t)input->plan_slot);
    digest_i32(ctx, (int32_t)input->plan_type);
    digest_bool(ctx, input->create_planned_outpost);
    digest_vec2(ctx, input->planned_outpost_pos);
    digest_bool(ctx, input->cancel_planned_outpost);
    digest_u8(ctx, (uint8_t)input->cancel_planned_station);
    digest_bool(ctx, input->cancel_plan_slot);
    digest_u8(ctx, (uint8_t)input->cancel_plan_st);
    digest_u8(ctx, (uint8_t)input->cancel_plan_ring);
    digest_u8(ctx, (uint8_t)input->cancel_plan_sl);
    digest_bool(ctx, input->buy_scaffold_kit);
    digest_i32(ctx, (int32_t)input->scaffold_kit_module);
    digest_bool(ctx, input->commission_ship);
    digest_i32(ctx, (int32_t)input->commission_hull_class);
    digest_bool(ctx, input->buy_product);
    digest_i32(ctx, (int32_t)input->buy_commodity);
    digest_bool(ctx, input->buy_station_pod);
    digest_u16(ctx, input->buy_station_pod_index);
    digest_i32(ctx, (int32_t)input->buy_grade);
    digest_i32(ctx, input->mining_target_hint);
    digest_bool(ctx, input->hail);
    digest_bool(ctx, input->tractor_hold);
    digest_bool(ctx, input->release_tow);
    digest_bool(ctx, input->reset);
    digest_bool(ctx, input->toggle_autopilot);
    digest_bool(ctx, input->boost);
    digest_bool(ctx, input->reverse_thrust);
}

static void digest_ship(state_digest_ctx_t *ctx, const ship_t *ship)
{
    ship_t zero = {0};
    if (!ship)
        ship = &zero;

    digest_vec2(ctx, ship->pos);
    digest_vec2(ctx, ship->vel);
    digest_float(ctx, ship->angle);
    digest_float(ctx, ship->hull);
    for (int c = 0; c < COMMODITY_COUNT; c++)
        digest_float(ctx, ship->cargo[c]);
    digest_i32(ctx, (int32_t)ship->hull_class);
    digest_i32(ctx, ship->mining_level);
    digest_i32(ctx, ship->hold_level);
    digest_i32(ctx, ship->tractor_level);
    for (size_t i = 0;
         i < sizeof(ship->towed_fragments) / sizeof(ship->towed_fragments[0]);
         i++) {
        digest_i16(ctx, ship->towed_fragments[i]);
    }
    digest_u8(ctx, ship->towed_count);
    for (size_t i = 0;
         i < sizeof(ship->towed_pods) / sizeof(ship->towed_pods[0]);
         i++) {
        digest_i16(ctx, ship->towed_pods[i]);
    }
    digest_u8(ctx, ship->towed_pod_count);
    digest_i16(ctx, ship->towed_scaffold);
    digest_bool(ctx, ship->tractor_active);
    digest_float(ctx, ship->comm_range);
    digest_u32(ctx, ship->unlocked_modules);
    digest_float(ctx, ship->stat_ore_mined);
    digest_float(ctx, ship->stat_credits_earned);
    digest_float(ctx, ship->stat_credits_spent);
    digest_i32(ctx, ship->stat_asteroids_fractured);
    digest_cargo_store(ctx, &ship->cargo_store);
    digest_knowledge_view(ctx, &ship->knowledge);
}

static void digest_station_module(state_digest_ctx_t *ctx,
                                  const station_module_t *module)
{
    digest_i32(ctx, (int32_t)module->type);
    digest_u8(ctx, module->ring);
    digest_u8(ctx, module->slot);
    digest_bool(ctx, module->scaffold);
    digest_u8(ctx, module->last_smelt_commodity);
    digest_u8(ctx, module->commodity);
    digest_float(ctx, module->build_progress);
    digest_float(ctx, module->input_buffer);
    digest_float(ctx, module->output_buffer);
    digest_float(ctx, module->active_pulse);
    digest_float(ctx, module->craft_progress);
}

static void digest_station(state_digest_ctx_t *ctx, const station_t *station)
{
    int module_count = station->module_count;
    int pending_scaffold_count = station->pending_scaffold_count;
    int pending_ship_count = station->pending_ship_build_count;
    int placement_plan_count = station->placement_plan_count;
    int ledger_count = station->ledger_count;

    if (module_count < 0) module_count = 0;
    if (module_count > MAX_MODULES_PER_STATION)
        module_count = MAX_MODULES_PER_STATION;
    if (pending_scaffold_count < 0) pending_scaffold_count = 0;
    if (pending_scaffold_count > 4) pending_scaffold_count = 4;
    if (pending_ship_count < 0) pending_ship_count = 0;
    if (pending_ship_count > 4) pending_ship_count = 4;
    if (placement_plan_count < 0) placement_plan_count = 0;
    if (placement_plan_count > 8) placement_plan_count = 8;
    if (ledger_count < 0) ledger_count = 0;
    if (ledger_count > STATION_LEDGER_MAX)
        ledger_count = STATION_LEDGER_MAX;

    digest_u32(ctx, station->id);
    digest_text(ctx, station->name, sizeof(station->name));
    digest_vec2(ctx, station->pos);
    digest_vec2(ctx, station->jostle_vel);
    digest_float(ctx, station->radius);
    digest_float(ctx, station->dock_radius);
    digest_float(ctx, station->signal_range);
    digest_bool(ctx, station->signal_connected);
    digest_bool(ctx, station->scaffold);
    digest_bool(ctx, station->planned);
    digest_u8(ctx, (uint8_t)station->planned_owner);
    digest_float(ctx, station->scaffold_progress);
    for (int c = 0; c < COMMODITY_COUNT; c++) {
        digest_float(ctx, station->base_price[c]);
        digest_float(ctx, station->_inventory_cache[c]);
        digest_float(ctx, station->_finished_residue[c]);
    }
    digest_u32(ctx, station->services);

    digest_i32(ctx, module_count);
    for (int i = 0; i < module_count; i++)
        digest_station_module(ctx, &station->modules[i]);

    digest_i32(ctx, station->arm_count);
    for (int i = 0; i < MAX_ARMS; i++) {
        digest_float(ctx, station->arm_rotation[i]);
        digest_float(ctx, station->arm_speed[i]);
        digest_float(ctx, station->arm_omega[i]);
        digest_float(ctx, station->ring_offset[i]);
    }

    digest_text(ctx, station->hail_message, sizeof(station->hail_message));
    for (size_t i = 0;
         i < sizeof(station->miner_chatter) / sizeof(station->miner_chatter[0]);
         i++) {
        digest_text(ctx, station->miner_chatter[i],
                    sizeof(station->miner_chatter[i]));
    }
    for (size_t i = 0;
         i < sizeof(station->hauler_chatter) /
                 sizeof(station->hauler_chatter[0]);
         i++) {
        digest_text(ctx, station->hauler_chatter[i],
                    sizeof(station->hauler_chatter[i]));
    }
    digest_text(ctx, station->rati_hail_message,
                sizeof(station->rati_hail_message));
    digest_text(ctx, station->station_slug, sizeof(station->station_slug));
    digest_text(ctx, station->currency_name, sizeof(station->currency_name));
    digest_u8(ctx, station->faction_id);
    digest_u8(ctx, station->faction_allegiance);
    digest_u8(ctx, station->faction_ideology);
    for (int i = 0; i < STATION_FACTION_COUNT; i++)
        digest_u8(ctx, (uint8_t)station->faction_relations[i]);

    digest_i32(ctx, ledger_count);
    for (int i = 0; i < ledger_count; i++) {
        digest_ledger_identity(
            ctx, station->ledger[i].player_pubkey);
        digest_float(ctx, station->ledger[i].balance);
        digest_float(ctx, station->ledger[i].lifetime_supply);
        digest_u64(ctx, station->ledger[i].first_dock_tick);
        digest_u64(ctx, station->ledger[i].last_dock_tick);
        digest_u32(ctx, station->ledger[i].total_docks);
        digest_u32(ctx, station->ledger[i].lifetime_ore_units);
        digest_u32(ctx, station->ledger[i].lifetime_credits_in);
        digest_u32(ctx, station->ledger[i].lifetime_credits_out);
        digest_u8(ctx, station->ledger[i].top_commodity);
    }

    digest_i32(ctx, pending_scaffold_count);
    for (int i = 0; i < pending_scaffold_count; i++) {
        digest_i32(ctx, (int32_t)station->pending_scaffolds[i].type);
        digest_u8(ctx, (uint8_t)station->pending_scaffolds[i].owner);
    }
    digest_i32(ctx, pending_ship_count);
    for (int i = 0; i < pending_ship_count; i++) {
        const pending_ship_build_t *build = &station->pending_ship_builds[i];
        digest_i32(ctx, (int32_t)build->hull_class);
        digest_actor_principal(ctx, &build->owner_principal);
        digest_u64(ctx, build->owner_quarantine_record_id);
        digest_u64(ctx, build->mode_quarantine_record_id);
        digest_float(ctx, build->build_progress);
        digest_u8(ctx, build->mode);
    }
    digest_i32(ctx, placement_plan_count);
    for (int i = 0; i < placement_plan_count; i++) {
        digest_i32(ctx, (int32_t)station->placement_plans[i].type);
        digest_u8(ctx, station->placement_plans[i].ring);
        digest_u8(ctx, station->placement_plans[i].slot);
        digest_u8(ctx, (uint8_t)station->placement_plans[i].owner);
    }

    digest_u64(ctx, station->policy_tick);
    digest_u32(ctx, station->policy_generation);
    digest_u8(ctx, station->policy_budget_trade);
    digest_u8(ctx, station->policy_budget_construction);
    digest_u8(ctx, station->policy_budget_finance);
    digest_u8(ctx, station->policy_card_count);
    for (int i = 0; i < 8; i++) {
        digest_u8(ctx, station->policy_card_ids[i]);
        digest_u8(ctx, station->policy_card_domains[i]);
        digest_u8(ctx, station->policy_card_costs[i]);
        digest_float(ctx, station->policy_card_scores[i]);
    }
    digest_u8(ctx, station->policy_top_demand_commodity);
    digest_float(ctx, station->policy_top_demand_severity);
    digest_float(ctx, station->policy_top_demand_price_mult);

    digest_cargo_store(ctx, &station->cargo_store);
    digest_float(ctx, station->repair_kit_fab_timer);
    digest_bytes(ctx, station->station_pubkey,
                 sizeof(station->station_pubkey));
    digest_bytes(ctx, station->outpost_founder_pubkey,
                 sizeof(station->outpost_founder_pubkey));
    digest_u64(ctx, station->outpost_planted_tick);
    digest_bytes(ctx, station->station_actor_id,
                 sizeof(station->station_actor_id));
    digest_bool(ctx, station->station_actor_catalog_attested);
    uint8_t authority_count = station->authority_registry_count;
    if (authority_count > STATION_AUTHORITY_REGISTRY_CAP)
        authority_count = STATION_AUTHORITY_REGISTRY_CAP;
    digest_u8(ctx, station->authority_registry_version);
    digest_u8(ctx, authority_count);
    for (uint8_t i = 0; i < authority_count; i++) {
        const station_authority_record_t *record =
            &station->authority_registry[i];
        digest_bytes(ctx, record->pubkey, sizeof(record->pubkey));
        digest_u8(ctx, record->lifecycle);
        digest_u8(ctx, record->trust);
    }
    digest_knowledge_view(ctx, &station->knowledge);
    digest_bytes(ctx, station->chain_last_hash,
                 sizeof(station->chain_last_hash));
    digest_u64(ctx, station->chain_event_count);
    digest_bool(ctx, station->chain_append_blocked);
    digest_hnn_memory(ctx, &station->hnn_market_memory);
    digest_u32(ctx, station->hnn_market_version);
    digest_u32(ctx, station->hnn_market_decay_tick);
    digest_hnn_memory(ctx, &station->hnn_experience);
    digest_u32(ctx, station->hnn_experience_version);
    digest_u32(ctx, station->hnn_experience_upload_count);
    digest_u32(ctx, station->hnn_experience_download_count);
    digest_u8(ctx, station->hnn_experience_last_source_station);
}

static void digest_player(state_digest_ctx_t *ctx,
                          const server_player_t *player)
{
    uint8_t movement_count = player->movement_queue_count;
    if (movement_count > PLAYER_MOVEMENT_QUEUE_CAP)
        movement_count = PLAYER_MOVEMENT_QUEUE_CAP;

    digest_bool(ctx, player->connected);
    digest_u8(ctx, player->id);
    digest_bool(ctx, player->session_ready);
    digest_bool(ctx, player->grace_period);
    digest_float(ctx, player->grace_timer);
    digest_u32(ctx, player->ship_asset_id);
    digest_entity_ref(ctx, player->ship_ref);
    digest_input_intent(ctx, &player->input);
    digest_float(ctx, player->boost_hold_timer);
    digest_i32(ctx, player->current_station);
    digest_i32(ctx, player->nearby_station);
    digest_bool(ctx, player->docked);
    digest_bool(ctx, player->in_dock_range);
    digest_bool(ctx, player->docking_approach);
    digest_i32(ctx, player->dock_berth);
    digest_bool(ctx, player->beam_active);
    digest_bool(ctx, player->beam_hit);
    digest_bool(ctx, player->beam_ineffective);
    digest_bool(ctx, player->scan_active);
    digest_i32(ctx, player->scan_target_type);
    digest_i32(ctx, player->scan_target_index);
    digest_i32(ctx, player->scan_module_index);
    digest_i32(ctx, player->hover_asteroid);
    digest_vec2(ctx, player->beam_start);
    digest_vec2(ctx, player->beam_end);
    digest_float(ctx, player->cargo_sale_value);
    digest_i32(ctx, player->nearby_fragments);
    digest_i32(ctx, player->tractor_fragments);
    digest_bool(ctx, player->was_in_signal);
    digest_bytes(ctx, player->callsign, sizeof(player->callsign));
    digest_bool(ctx, player->actual_thrusting);

    digest_u8(ctx, player->autopilot_mode);
    digest_i32(ctx, player->autopilot_target);
    digest_i32(ctx, player->autopilot_station_target);
    digest_i32(ctx, (int32_t)player->autopilot_cargo);
    digest_i32(ctx, player->autopilot_state);
    digest_float(ctx, player->autopilot_timer);
    digest_vec2(ctx, player->autopilot_last_pos);
    digest_float(ctx, player->autopilot_stuck_timer);
    digest_u8(ctx, player->autopilot_teacher_valid);
    digest_u8(ctx, player->autopilot_teacher_forward_blocked);
    digest_u16(ctx, player->autopilot_teacher_allowed_mask);
    digest_u32(ctx, player->autopilot_teacher_tick);
    digest_u8(ctx, (uint8_t)player->autopilot_teacher_action);
    digest_u8(ctx, (uint8_t)player->autopilot_teacher_turn);
    digest_u8(ctx, (uint8_t)player->autopilot_teacher_thrust);
    for (size_t i = 0;
         i < sizeof(player->autopilot_teacher_features) /
                 sizeof(player->autopilot_teacher_features[0]);
         i++) {
        digest_float(ctx, player->autopilot_teacher_features[i]);
    }
    digest_u8(ctx, player->autopilot_decision_valid);
    digest_u8(ctx, player->autopilot_decision_action);
    digest_u8(ctx, player->autopilot_decision_candidate_count);
    digest_u32(ctx, player->autopilot_decision_flags);
    digest_float(ctx, player->autopilot_decision_score);
    digest_float(ctx, player->autopilot_decision_neural_score);
    digest_float(ctx, player->autopilot_decision_route_risk);
    digest_float(ctx, player->autopilot_decision_signal_quality);
    digest_u8(ctx, player->hail_decision_valid);
    digest_u8(ctx, (uint8_t)player->hail_decision_station);
    digest_u8(ctx, player->hail_decision_candidate_count);
    digest_u32(ctx, player->hail_decision_flags);
    digest_float(ctx, player->hail_decision_score);
    digest_float(ctx, player->hail_decision_signal_quality);
    digest_u64(ctx, player->hail_decision_source_id);
    digest_u8(ctx, player->server_brain_mode);

    digest_u8(ctx, movement_count);
    for (uint8_t i = 0; i < movement_count; i++) {
        const movement_input_cmd_t *command = &player->movement_queue[i];
        digest_u32(ctx, command->apply_tick);
        digest_u16(ctx, command->input_seq);
        digest_input_intent(ctx, &command->intent);
    }
    digest_u16(ctx, player->last_input_seq);
    digest_u32(ctx, player->last_input_tick);
    digest_u16(ctx, player->last_input_action_id);
    digest_bool(ctx, player->last_input_action_id_valid);
    digest_u8(ctx, player->last_damage_cause);
    digest_bytes(ctx, player->pubkey, sizeof(player->pubkey));
    digest_bool(ctx, player->pubkey_set);
    digest_bool(ctx, player->pubkey_proof_ok);
    digest_bool(ctx, player->pubkey_identity_finalized);
    digest_bool(ctx, player->preserve_live_state_on_pubkey_finalize);
    digest_u64(ctx, player->last_signed_nonce);
}

static void digest_npc(state_digest_ctx_t *ctx, const npc_ship_t *npc)
{
    digest_i32(ctx, (int32_t)npc->role);
    digest_i32(ctx, (int32_t)npc->state);
    digest_u32(ctx, npc->ship_asset_id);
    digest_entity_ref(ctx, npc->ship_ref);
    digest_input_intent(ctx, &npc->input);
    digest_i32(ctx, npc->target_asteroid);
    digest_i32(ctx, npc->home_station);
    digest_i32(ctx, npc->dest_station);
    digest_i32(ctx, npc->pickup_station);
    digest_i32(ctx, (int32_t)npc->pickup_commodity);
    digest_u8(ctx, npc->pickup_action);
    digest_float(ctx, npc->state_timer);
    digest_bool(ctx, npc->thrusting);
    digest_u8(ctx, npc->brain_mode);
    digest_hnn_memory(ctx, &npc->hnn_market_mem);
    digest_u32(ctx, npc->hnn_market_version);
    digest_u8(ctx, npc->hnn_market_station);
    digest_u32(ctx, npc->hnn_market_decay_tick);
    digest_hnn_memory(ctx, &npc->hnn_mem);
    digest_u32(ctx, npc->hnn_experience_version);
    digest_u32(ctx, npc->hnn_experience_local_version);
    digest_u32(ctx, npc->hnn_experience_uploaded_local_version);
    digest_u32(ctx, npc->hnn_experience_uploaded_source_version);
    digest_u8(ctx, npc->hnn_experience_station);
    digest_u8(ctx, npc->hnn_experience_uploaded_station);
    digest_u8(ctx, npc->hnn_experience_uploaded_source_station);
}

static void digest_ship_asset(state_digest_ctx_t *ctx,
                              const ship_asset_t *asset)
{
    digest_u32(ctx, asset->asset_id);
    digest_i32(ctx, (int32_t)asset->hull_class);
    digest_entity_ref(ctx, asset->live_ship_ref);
    digest_actor_principal(ctx, &asset->owner_principal);
    digest_u64(ctx, asset->owner_quarantine_record_id);
    digest_u8(ctx, asset->status);
    digest_u8(ctx, asset->operator_kind);
    digest_u8(ctx, asset->provenance);
    digest_i16(ctx, asset->custody_station);
    digest_i16(ctx, asset->operator_slot);
    digest_i16(ctx, asset->build_station);
    digest_bool(ctx, asset->loaner);
    digest_bool(ctx, asset->destroyed);
    digest_u8(ctx, asset->birth_proof_version);
    digest_bytes(ctx, asset->birth_fragment_grades,
                 sizeof(asset->birth_fragment_grades));
    digest_bytes(ctx, asset->birth_soul_pub, sizeof(asset->birth_soul_pub));
    digest_bytes(ctx, asset->birth_material_root,
                 sizeof(asset->birth_material_root));
    digest_bytes(ctx, asset->birth_fragment_pubs,
                 sizeof(asset->birth_fragment_pubs));
    if (asset->status == SHIP_ASSET_STATUS_STORED)
        digest_ship(ctx, &asset->stored_ship);
}

static void digest_birth_assembly(state_digest_ctx_t *ctx,
                                  const ship_birth_assembly_t *assembly)
{
    for (int i = 0; i < 3; i++)
        digest_i16(ctx, assembly->fragment_slots[i]);
    digest_float(ctx, assembly->age);
    for (int i = 0; i < 3; i++)
        digest_float(ctx, assembly->start_dist[i]);
    digest_vec2(ctx, assembly->target);
    digest_bytes(ctx, assembly->fragment_pubs,
                 sizeof(assembly->fragment_pubs));
}

static void digest_character(state_digest_ctx_t *ctx,
                             const character_t *character)
{
    digest_i32(ctx, (int32_t)character->kind);
    digest_i32(ctx, character->actor_slot);
    digest_entity_ref(ctx, character->ship_ref);
}

static void digest_scaffold(state_digest_ctx_t *ctx,
                            const scaffold_t *scaffold)
{
    digest_i32(ctx, (int32_t)scaffold->module_type);
    digest_i32(ctx, (int32_t)scaffold->state);
    digest_i32(ctx, scaffold->owner);
    digest_vec2(ctx, scaffold->pos);
    digest_vec2(ctx, scaffold->vel);
    digest_float(ctx, scaffold->radius);
    digest_float(ctx, scaffold->rotation);
    digest_float(ctx, scaffold->spin);
    digest_float(ctx, scaffold->age);
    digest_i32(ctx, scaffold->placed_station);
    digest_i32(ctx, scaffold->placed_ring);
    digest_i32(ctx, scaffold->placed_slot);
    digest_tractor_binding(ctx, &scaffold->tractor);
    digest_i32(ctx, scaffold->built_at_station);
    digest_float(ctx, scaffold->build_amount);
}

static void digest_cargo_pod(state_digest_ctx_t *ctx,
                             const cargo_pod_t *pod)
{
    uint16_t manifest_count = pod->manifest_count;
    if (manifest_count > CARGO_POD_MANIFEST_CAP)
        manifest_count = CARGO_POD_MANIFEST_CAP;

    digest_i32(ctx, (int32_t)pod->kind);
    digest_i32(ctx, (int32_t)pod->commodity);
    digest_u16(ctx, pod->quantity);
    digest_u16(ctx, manifest_count);
    for (uint16_t i = 0; i < manifest_count; i++)
        digest_cargo_unit(ctx, &pod->manifest_units[i]);
    digest_bool(ctx, pod->has_shell_frame);
    if (pod->has_shell_frame)
        digest_cargo_unit(ctx, &pod->shell_frame);
    digest_u16(ctx, pod->shipment_id);
    digest_vec2(ctx, pod->pos);
    digest_vec2(ctx, pod->vel);
    digest_float(ctx, pod->radius);
    digest_float(ctx, pod->rotation);
    digest_float(ctx, pod->spin);
    digest_float(ctx, pod->age);
    digest_actor_principal(
        ctx, &pod->tow_owner_principal);
    digest_u64(
        ctx, pod->tow_owner_quarantine_record_id);
    digest_tractor_binding(ctx, &pod->tractor);
    digest_u8(ctx, pod->tow_hardpoint_tag);
    digest_u8(ctx, pod->custody_station);
    digest_i64(ctx, pod->custody_charge_total);
    digest_u16(ctx, pod->custody_charge_unit_count);
    digest_u16(ctx, pod->custody_charge_units_processed);
    digest_bytes(ctx, pod->custody_charge_manifest_digest,
                 sizeof(pod->custody_charge_manifest_digest));
}

static void digest_asteroid(state_digest_ctx_t *ctx,
                            const asteroid_t *asteroid)
{
    digest_bool(ctx, asteroid->fracture_child);
    digest_i32(ctx, (int32_t)asteroid->tier);
    digest_vec2(ctx, asteroid->pos);
    digest_vec2(ctx, asteroid->vel);
    digest_float(ctx, asteroid->radius);
    digest_float(ctx, asteroid->hp);
    digest_float(ctx, asteroid->max_hp);
    digest_float(ctx, asteroid->ore);
    digest_float(ctx, asteroid->max_ore);
    digest_i32(ctx, (int32_t)asteroid->commodity);
    digest_float(ctx, asteroid->rotation);
    digest_float(ctx, asteroid->spin);
    digest_float(ctx, asteroid->seed);
    digest_float(ctx, asteroid->age);
    digest_tractor_binding(ctx, &asteroid->tractor);
    digest_u8(ctx, (uint8_t)asteroid->last_towed_by);
    digest_u8(ctx, (uint8_t)asteroid->last_fractured_by);
    digest_float(ctx, asteroid->smelt_progress);
    digest_u8(ctx, asteroid->crystal_stage);
    digest_u8(ctx, asteroid->crystal_stage_station);
    digest_u8(ctx, asteroid->crystal_stage_module);
    digest_u8(ctx, asteroid->phase);
    digest_float(ctx, asteroid->gas_emit_timer);
    digest_u8(ctx, asteroid->thrown_timer_q);
    digest_bytes(ctx, asteroid->fracture_seed,
                 sizeof(asteroid->fracture_seed));
    digest_bytes(ctx, asteroid->fragment_pub,
                 sizeof(asteroid->fragment_pub));
    digest_u8(ctx, asteroid->grade);
    digest_bytes(ctx, asteroid->rock_pub, sizeof(asteroid->rock_pub));
}

static void digest_tow_link(state_digest_ctx_t *ctx,
                            const tow_link_t *link)
{
    digest_entity_ref(ctx, link->source);
    digest_entity_ref(ctx, link->target);
    digest_u8(ctx, link->profile);
    digest_u8(ctx, link->slot);
    digest_u8(ctx, link->state);
}

static void digest_fracture_claim(state_digest_ctx_t *ctx,
                                  const fracture_claim_state_t *claim)
{
    digest_bool(ctx, claim->active);
    digest_bool(ctx, claim->resolved);
    digest_u32(ctx, claim->fracture_id);
    digest_u32(ctx, claim->deadline_ms);
    digest_u16(ctx, claim->burst_cap);
    digest_u32(ctx, claim->best_nonce);
    digest_u8(ctx, claim->best_grade);
    digest_u8(ctx, claim->seen_claimant_count);
}

static void digest_contract(state_digest_ctx_t *ctx,
                            const contract_t *contract)
{
    digest_i32(ctx, (int32_t)contract->action);
    digest_u8(ctx, contract->station_index);
    digest_i32(ctx, (int32_t)contract->commodity);
    digest_u8(ctx, contract->required_grade);
    digest_u8(ctx, contract->proof_flags);
    digest_u8(ctx, contract->required_prefix_class);
    digest_u16(ctx, contract->required_recipe_id);
    digest_bytes(ctx, contract->required_parent,
                 sizeof(contract->required_parent));
    digest_bytes(ctx, contract->target_pub, sizeof(contract->target_pub));
    digest_u64(ctx, contract->forbidden_origin_mask);
    digest_float(ctx, contract->quantity_needed);
    digest_float(ctx, contract->base_price);
    digest_float(ctx, contract->age);
    digest_vec2(ctx, contract->target_pos);
    digest_i32(ctx, contract->target_index);
    digest_actor_principal(
        ctx, &contract->claimed_by_principal);
    digest_u64(
        ctx, contract->claimed_by_quarantine_record_id);
}

static void digest_delivery_shipment(state_digest_ctx_t *ctx,
                                     const delivery_shipment_t *shipment)
{
    uint16_t payload_count = shipment->quantity_bound;
    if (payload_count > MAX_DELIVERY_BOUND_CARGO)
        payload_count = MAX_DELIVERY_BOUND_CARGO;

    digest_u16(ctx, shipment->shipment_id);
    digest_u8(ctx, shipment->origin_station);
    digest_u8(ctx, shipment->destination_station);
    digest_u8(ctx, shipment->contract_index);
    digest_actor_principal(ctx, &shipment->debtor_principal);
    digest_u64(
        ctx, shipment->debtor_quarantine_record_id);
    digest_u8(ctx, shipment->commodity);
    digest_u16(ctx, shipment->quantity_total);
    digest_u16(ctx, shipment->quantity_bound);
    digest_u16(ctx, shipment->quantity_delivered);
    digest_u16(ctx, shipment->quantity_black_market_sold);
    digest_float(ctx, shipment->debt_principal);
    digest_float(ctx, shipment->destination_payout);
    digest_float(ctx, shipment->origin_completion_credit);
    digest_u32(ctx, shipment->due_tick);
    digest_u8(ctx, shipment->status);
    digest_u16(ctx, payload_count);
    for (uint16_t i = 0; i < payload_count; i++) {
        digest_bytes(ctx, shipment->cargo_pub[i],
                     sizeof(shipment->cargo_pub[i]));
        digest_cargo_unit(ctx, &shipment->cargo_units[i]);
        digest_receipt_chain(ctx, &shipment->cargo_chains[i]);
    }
}

static void digest_ownership_quarantine(
    state_digest_ctx_t *ctx,
    const ownership_quarantine_t *quarantine)
{
    uint16_t count = quarantine ? quarantine->count : 0;
    uint16_t safe_count = count;
    if (safe_count > OWNERSHIP_QUARANTINE_CAP)
        safe_count = OWNERSHIP_QUARANTINE_CAP;

    digest_u64(ctx, quarantine ? quarantine->record_id_high_water : 0);
    digest_u16(ctx, count);
    digest_u16(ctx, safe_count);
    for (uint16_t i = 0; i < safe_count; i++) {
        const ownership_quarantine_entry_t *entry =
            &quarantine->entries[i];
        digest_u64(ctx, entry->record_id);
        digest_u8(ctx, entry->source_kind);
        digest_u8(ctx, entry->reason);
        digest_u16(ctx, entry->station_index);
        digest_u16(ctx, entry->row_index);
        digest_u16(ctx, entry->legacy_actor_code);
    }
}

static void digest_signal_message(state_digest_ctx_t *ctx,
                                  const signal_channel_msg_t *message)
{
    uint8_t text_len = message->text_len;
    size_t audio_len = message->audio_len;
    if (text_len >= SIGNAL_CHANNEL_TEXT_MAX)
        text_len = SIGNAL_CHANNEL_TEXT_MAX - 1;
    if (audio_len > sizeof(message->audio_url))
        audio_len = sizeof(message->audio_url);
    digest_u64(ctx, message->id);
    digest_u32(ctx, message->timestamp_ms);
    digest_i16(ctx, message->sender_station);
    digest_u8(ctx, text_len);
    digest_bytes(ctx, message->text, text_len);
    digest_u8(ctx, (uint8_t)audio_len);
    digest_bytes(ctx, message->audio_url, audio_len);
    digest_bytes(ctx, message->entry_hash, sizeof(message->entry_hash));
}

static void digest_signal_channel(state_digest_ctx_t *ctx,
                                  const signal_channel_t *channel)
{
    int count = channel->count;
    int head = channel->head;
    if (count < 0) count = 0;
    if (count > SIGNAL_CHANNEL_CAPACITY)
        count = SIGNAL_CHANNEL_CAPACITY;
    if (head < 0 || head >= SIGNAL_CHANNEL_CAPACITY)
        head = 0;

    digest_i32(ctx, count);
    digest_u64(ctx, channel->next_id);
    digest_bytes(ctx, channel->last_hash, sizeof(channel->last_hash));
    int oldest = head - count;
    while (oldest < 0)
        oldest += SIGNAL_CHANNEL_CAPACITY;
    for (int i = 0; i < count; i++) {
        int slot = (oldest + i) % SIGNAL_CHANNEL_CAPACITY;
        digest_signal_message(ctx, &channel->msgs[slot]);
    }
}

const char *signal_authoritative_state_digest_schema(void)
{
    return SIGNAL_AUTH_STATE_DIGEST_SCHEMA;
}

uint32_t signal_authoritative_state_digest_version(void)
{
    return SIGNAL_AUTH_STATE_DIGEST_VERSION;
}

void signal_authoritative_state_digest(
    const world_t *world,
    uint8_t out[SIGNAL_AUTH_STATE_DIGEST_SIZE])
{
    state_digest_ctx_t ctx;
    sha256_init(&ctx);
    digest_text(&ctx, SIGNAL_AUTH_STATE_DIGEST_SCHEMA,
                sizeof(SIGNAL_AUTH_STATE_DIGEST_SCHEMA) - 1u);
    digest_u32(&ctx, SIGNAL_AUTH_STATE_DIGEST_VERSION);

    if (!world) {
        digest_bool(&ctx, false);
        sha256_final(&ctx, out);
        return;
    }
    digest_bool(&ctx, true);

    digest_u32(&ctx, world->rng);
    digest_u32(&ctx, world->belt_seed);
    digest_u32(&ctx, world->world_seq);
    digest_float(&ctx, world->time);
    digest_u32(&ctx, world->tick);
    digest_float(&ctx, world->field_spawn_timer);
    digest_float(&ctx, world->gravity_accumulator);
    digest_float(&ctx, world->npc_respawn_timer);
    digest_bool(&ctx, world->player_only_mode);
    digest_i32(&ctx, world->frontier_virtual_pilots);
    digest_float(&ctx, world->frontier_plan_timer);
    digest_u32(&ctx, world->frontier_plans_created);
    digest_u32(&ctx, world->frontier_scaffold_orders);
    digest_u32(&ctx, world->frontier_module_plans_created);
    digest_u32(&ctx, world->frontier_module_scaffold_orders);
    digest_u32(&ctx, world->frontier_virtual_scaffolds_manufactured);
    digest_u32(&ctx, world->frontier_virtual_scaffold_deliveries);
    digest_u32(&ctx, world->frontier_virtual_supply_deliveries);
    digest_u8(&ctx, world->frontier_decision_valid);
    digest_u8(&ctx, world->frontier_decision_action);
    digest_u16(&ctx, world->frontier_decision_plan_limit);
    digest_u32(&ctx, world->frontier_decision_flags);
    digest_float(&ctx, world->frontier_decision_score);
    digest_float(&ctx, world->frontier_decision_pressure);
    digest_u64(&ctx, world->frontier_decision_source_id);
    digest_u16(&ctx, world->next_npc_token);

    int station_count = world->station_count;
    if (station_count < 0) station_count = 0;
    if (station_count > MAX_STATIONS) station_count = MAX_STATIONS;
    digest_i32(&ctx, station_count);
    digest_u32(&ctx, world->next_station_id);
    for (int i = 0; i < station_count; i++) {
        bool exists = station_exists(&world->stations[i]);
        digest_i32(&ctx, i);
        digest_bool(&ctx, exists);
        if (exists)
            digest_station(&ctx, &world->stations[i]);
    }

    for (int i = 0; i < WORLD_SHIP_CAP; i++) {
        const ship_slot_t *slot = &world->ships[i];
        digest_bool(&ctx, slot->active);
        digest_u16(&ctx, slot->generation);
        if (slot->active)
            digest_ship(&ctx, &slot->component);
    }

    int active_players = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const server_player_t *player = &world->players[i];
        if (player->connected || player->session_ready ||
            player->grace_period || !entity_ref_is_none(player->ship_ref)) {
            active_players++;
        }
    }
    digest_i32(&ctx, active_players);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const server_player_t *player = &world->players[i];
        if (!player->connected && !player->session_ready &&
            !player->grace_period && entity_ref_is_none(player->ship_ref)) {
            continue;
        }
        digest_i32(&ctx, i);
        digest_player(&ctx, player);
    }

    int active_npcs = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (world->npc_ships[i].active)
            active_npcs++;
    }
    digest_i32(&ctx, active_npcs);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!world->npc_ships[i].active)
            continue;
        digest_i32(&ctx, i);
        digest_npc(&ctx, &world->npc_ships[i]);
    }

    digest_u32(&ctx, world->next_ship_asset_id);
    int active_assets = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        if (world->ship_assets[i].active)
            active_assets++;
    }
    digest_i32(&ctx, active_assets);
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        if (!world->ship_assets[i].active)
            continue;
        digest_i32(&ctx, i);
        digest_ship_asset(&ctx, &world->ship_assets[i]);
    }

    int active_assemblies = 0;
    for (int station = 0; station < MAX_STATIONS; station++) {
        for (int slot = 0; slot < 4; slot++) {
            if (world->ship_birth_assemblies[station][slot].active)
                active_assemblies++;
        }
    }
    digest_i32(&ctx, active_assemblies);
    for (int station = 0; station < MAX_STATIONS; station++) {
        for (int slot = 0; slot < 4; slot++) {
            const ship_birth_assembly_t *assembly =
                &world->ship_birth_assemblies[station][slot];
            if (!assembly->active)
                continue;
            digest_i32(&ctx, station);
            digest_i32(&ctx, slot);
            digest_birth_assembly(&ctx, assembly);
        }
    }

    int active_characters = 0;
    for (int i = 0; i < WORLD_SHIP_CAP; i++) {
        if (world->characters[i].active)
            active_characters++;
    }
    digest_i32(&ctx, active_characters);
    for (int i = 0; i < WORLD_SHIP_CAP; i++) {
        if (!world->characters[i].active)
            continue;
        digest_i32(&ctx, i);
        digest_character(&ctx, &world->characters[i]);
    }

    int active_asteroids = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (world->asteroids[i].active)
            active_asteroids++;
    }
    digest_i32(&ctx, active_asteroids);
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!world->asteroids[i].active)
            continue;
        digest_i32(&ctx, i);
        digest_asteroid(&ctx, &world->asteroids[i]);
        digest_bool(&ctx, world->asteroid_origin[i].from_chunk);
        if (world->asteroid_origin[i].from_chunk) {
            digest_i32(&ctx, world->asteroid_origin[i].chunk_x);
            digest_i32(&ctx, world->asteroid_origin[i].chunk_y);
        }
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const fracture_claim_state_t *claim = &world->fracture_claims[i];
        bool present = claim->active || claim->resolved;
        digest_bool(&ctx, present);
        if (present)
            digest_fracture_claim(&ctx, claim);
    }
    digest_u32(&ctx, world->next_fracture_id);

    uint16_t destroyed_count = world->destroyed_rock_count;
    if (destroyed_count > MAX_DESTROYED_ROCKS)
        destroyed_count = MAX_DESTROYED_ROCKS;
    digest_u16(&ctx, destroyed_count);
    for (uint16_t i = 0; i < destroyed_count; i++) {
        digest_bytes(&ctx, world->destroyed_rocks[i].rock_pub,
                     sizeof(world->destroyed_rocks[i].rock_pub));
        digest_u64(&ctx, world->destroyed_rocks[i].destroyed_at_ms);
    }

    int active_scaffolds = 0;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        if (world->scaffolds[i].active)
            active_scaffolds++;
    }
    digest_i32(&ctx, active_scaffolds);
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        if (!world->scaffolds[i].active)
            continue;
        digest_i32(&ctx, i);
        digest_scaffold(&ctx, &world->scaffolds[i]);
    }

    int active_pods = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (world->cargo_pods[i].active)
            active_pods++;
    }
    digest_i32(&ctx, active_pods);
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (!world->cargo_pods[i].active)
            continue;
        digest_i32(&ctx, i);
        digest_cargo_pod(&ctx, &world->cargo_pods[i]);
    }

    int active_tow_links = 0;
    for (int i = 0; i < MAX_TOW_LINKS; i++) {
        if (world->tow_links[i].active)
            active_tow_links++;
    }
    digest_i32(&ctx, active_tow_links);
    const tow_link_t *ordered_tow_links[MAX_TOW_LINKS];
    int ordered_tow_count = world_tow_collect_links_canonical(
        world, ordered_tow_links, MAX_TOW_LINKS);
    for (int i = 0; i < ordered_tow_count; i++) {
        digest_tow_link(&ctx, ordered_tow_links[i]);
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        digest_u16(&ctx, world->asteroid_generation[i]);
        digest_bool(&ctx, world->asteroid_generation_live[i]);
    }
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        digest_u16(&ctx, world->cargo_pod_generation[i]);
        digest_bool(&ctx, world->cargo_pod_generation_live[i]);
    }
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        digest_u16(&ctx, world->scaffold_generation[i]);
        digest_bool(&ctx, world->scaffold_generation_live[i]);
    }
    for (int station = 0; station < MAX_STATIONS; station++) {
        for (int module = 0; module < MAX_MODULES_PER_STATION; module++) {
            digest_u16(&ctx,
                       world->station_module_generation[station][module]);
            digest_bool(
                &ctx,
                world->station_module_generation_live[station][module]);
        }
    }

    int active_contracts = 0;
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (world->contracts[i].active)
            active_contracts++;
    }
    digest_i32(&ctx, active_contracts);
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!world->contracts[i].active)
            continue;
        digest_i32(&ctx, i);
        digest_contract(&ctx, &world->contracts[i]);
    }

    digest_u16(&ctx, world->next_delivery_shipment_id);
    int active_shipments = 0;
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        if (world->delivery_shipments[i].active)
            active_shipments++;
    }
    digest_i32(&ctx, active_shipments);
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        if (!world->delivery_shipments[i].active)
            continue;
        digest_i32(&ctx, i);
        digest_delivery_shipment(&ctx, &world->delivery_shipments[i]);
    }

    digest_ownership_quarantine(&ctx, &world->ownership_quarantine);
    digest_signal_channel(&ctx, &world->signal_channel);

    int registry_count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (world->pubkey_registry[i].in_use)
            registry_count++;
    }
    digest_i32(&ctx, registry_count);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!world->pubkey_registry[i].in_use)
            continue;
        digest_i32(&ctx, i);
        digest_bytes(&ctx, world->pubkey_registry[i].pubkey,
                     sizeof(world->pubkey_registry[i].pubkey));
    }

    uint16_t handoff_count = world->handoff_consumed_ticket_count;
    if (handoff_count > 128)
        handoff_count = 128;
    digest_u16(&ctx, handoff_count);
    digest_u16(&ctx, world->handoff_consumed_ticket_next);
    for (uint16_t i = 0; i < handoff_count; i++) {
        digest_bytes(&ctx, world->handoff_consumed_ticket_hashes[i],
                     sizeof(world->handoff_consumed_ticket_hashes[i]));
    }

    sha256_final(&ctx, out);
}
