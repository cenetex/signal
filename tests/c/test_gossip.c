#include "test_harness.h"
#include "chain_log.h"
#include "gossip.h"
#include "holographic_nn_backend.h"
#include "sim_ai.h"
#include "signal_intelligence.h"

#ifndef _WIN32
#include <pthread.h>
#include <stdatomic.h>
#endif

static bool view_has_contract(const knowledge_view_t *view,
                              uint8_t action,
                              uint8_t station,
                              uint8_t commodity,
                              contract_summary_t *out) {
    if (!view) return false;
    for (int i = 0; i < view->count && i < KNOWLEDGE_VIEW_MAX_CAP; i++) {
        contract_summary_t cs;
        if (!contract_summary_from_knowledge_item(&view->items[i], &cs))
            continue;
        if (cs.action == action &&
            cs.station_index == station &&
            cs.commodity == commodity) {
            if (out) *out = cs;
            return true;
        }
    }
    return false;
}

static bool view_has_market_memory(const knowledge_view_t *view,
                                   uint8_t memory_kind,
                                   uint8_t station_a,
                                   uint8_t station_b,
                                   uint8_t commodity,
                                   market_memory_t *out) {
    if (!view) return false;
    for (int i = 0; i < view->count && i < KNOWLEDGE_VIEW_MAX_CAP; i++) {
        market_memory_t memory;
        if (!market_memory_from_knowledge_item(&view->items[i], &memory))
            continue;
        if (memory.memory_kind == memory_kind &&
            memory.station_a == station_a &&
            memory.station_b == station_b &&
            memory.commodity == commodity) {
            if (out) *out = memory;
            return true;
        }
    }
    return false;
}

static bool read_route_history_payload(const station_t *st,
                                       chain_payload_route_history_t *out) {
    if (!st || !out) return false;
    char path[256];
    if (!chain_log_path_for(st->station_pubkey, path, sizeof(path)))
        return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    bool found = false;
    for (;;) {
        uint8_t header[CHAIN_EVENT_HEADER_SIZE];
        size_t got = fread(header, 1, sizeof(header), f);
        if (got == 0 && feof(f)) break;
        if (got != sizeof(header)) break;
        uint8_t len_bytes[2];
        if (fread(len_bytes, 1, sizeof(len_bytes), f) != sizeof(len_bytes))
            break;
        uint16_t payload_len = (uint16_t)len_bytes[0] |
                               (uint16_t)((uint16_t)len_bytes[1] << 8);
        if (header[16] == CHAIN_EVT_ROUTE_HISTORY &&
            payload_len == sizeof(chain_payload_route_history_t)) {
            if (fread(out, 1, sizeof(*out), f) != sizeof(*out))
                break;
            found = true;
        } else {
            if (fseek(f, payload_len, SEEK_CUR) != 0)
                break;
        }
    }
    fclose(f);
    return found;
}

TEST(test_knowledge_contract_summary_adapter_round_trips) {
    contract_summary_t in = {
        .active = true,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = (uint8_t)COMMODITY_CUPRITE_INGOT,
        .required_grade = (uint8_t)MINING_GRADE_RARE,
        .quantity_needed = 7.0f,
        .base_price = 42.0f,
        .age_at_copy = 19.5f,
    };

    knowledge_item_t item;
    ASSERT(knowledge_item_from_contract_summary(&in, &item));
    ASSERT_EQ_INT(item.kind, KNOW_CONTRACT);
    ASSERT_EQ_INT(item.payload_kind, KNOW_PAYLOAD_CONTRACT_SUMMARY);

    uint8_t zero[32] = {0};
    ASSERT(memcmp(item.subject_hash, zero, sizeof(zero)) != 0);

    contract_summary_t out = {0};
    ASSERT(contract_summary_from_knowledge_item(&item, &out));
    ASSERT_EQ_INT(out.active, true);
    ASSERT_EQ_INT(out.action, in.action);
    ASSERT_EQ_INT(out.station_index, in.station_index);
    ASSERT_EQ_INT(out.commodity, in.commodity);
    ASSERT_EQ_INT(out.required_grade, in.required_grade);
    ASSERT_EQ_FLOAT(out.quantity_needed, in.quantity_needed, 0.001f);
    ASSERT_EQ_FLOAT(out.base_price, in.base_price, 0.001f);
    ASSERT_EQ_FLOAT(out.age_at_copy, in.age_at_copy, 0.001f);
}

TEST(test_knowledge_view_contract_dedup_keeps_newer_snapshot) {
    knowledge_view_t view = {0};
    knowledge_view_configure(&view, 4);

    contract_summary_t cs = {
        .active = true,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .quantity_needed = 10.0f,
        .base_price = 12.0f,
        .age_at_copy = 10.0f,
    };
    knowledge_item_t item;
    ASSERT(knowledge_item_from_contract_summary(&cs, &item));
    knowledge_view_insert(&view, &item);
    ASSERT_EQ_INT(view.count, 1);

    cs.age_at_copy = 20.0f;
    cs.base_price = 18.0f;
    ASSERT(knowledge_item_from_contract_summary(&cs, &item));
    knowledge_view_insert(&view, &item);
    ASSERT_EQ_INT(view.count, 1);

    cs.age_at_copy = 5.0f;
    cs.base_price = 99.0f;
    ASSERT(knowledge_item_from_contract_summary(&cs, &item));
    knowledge_view_insert(&view, &item);
    ASSERT_EQ_INT(view.count, 1);

    contract_summary_t out = {0};
    ASSERT(contract_summary_from_knowledge_item(&view.items[0], &out));
    ASSERT_EQ_FLOAT(out.age_at_copy, 20.0f, 0.001f);
    ASSERT_EQ_FLOAT(out.base_price, 18.0f, 0.001f);
}

TEST(test_market_memory_adapter_round_trips) {
    market_memory_t in = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_DEMAND,
        .station_a = 2,
        .station_b = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_DELIVERY,
        .confidence = 210,
        .salience = 180,
        .quantity_hint = 12,
        .value_hint = 55,
        .observed_tick = 1234,
        .subject_nonce = 99,
    };

    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&in, &item));
    ASSERT_EQ_INT(item.kind, KNOW_MARKET);
    ASSERT_EQ_INT(item.payload_kind, KNOW_PAYLOAD_MARKET_MEMORY);
    ASSERT_EQ_INT(item.confidence, 210);
    ASSERT_EQ_INT(item.salience, 180);
    ASSERT_EQ_INT((int)item.observed_tick, 1234);

    uint8_t zero[32] = {0};
    ASSERT(memcmp(item.subject_hash, zero, sizeof(zero)) != 0);

    market_memory_t out = {0};
    ASSERT(market_memory_from_knowledge_item(&item, &out));
    ASSERT_EQ_INT(out.active, true);
    ASSERT_EQ_INT(out.memory_kind, in.memory_kind);
    ASSERT_EQ_INT(out.station_a, in.station_a);
    ASSERT_EQ_INT(out.station_b, in.station_b);
    ASSERT_EQ_INT(out.commodity, in.commodity);
    ASSERT_EQ_INT(out.action, in.action);
    ASSERT_EQ_INT(out.confidence, in.confidence);
    ASSERT_EQ_INT(out.salience, in.salience);
    ASSERT_EQ_INT(out.quantity_hint, in.quantity_hint);
    ASSERT_EQ_INT(out.value_hint, in.value_hint);
    ASSERT_EQ_INT((int)out.observed_tick, (int)in.observed_tick);
}

TEST(test_contract_summary_creates_market_demand_memory) {
    contract_summary_t cs = {
        .active = true,
        .action = (uint8_t)CONTRACT_DELIVERY,
        .station_index = 2,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .quantity_needed = 7.0f,
        .base_price = 42.0f,
        .age_at_copy = 19.0f,
    };

    market_memory_t memory = {0};
    ASSERT(market_memory_from_contract_summary(&cs, &memory));
    ASSERT_EQ_INT(memory.active, true);
    ASSERT_EQ_INT(memory.memory_kind, MARKET_MEMORY_DEMAND);
    ASSERT_EQ_INT(memory.station_a, 2);
    ASSERT_EQ_INT(memory.station_b, 0xff);
    ASSERT_EQ_INT(memory.commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(memory.action, CONTRACT_DELIVERY);
    ASSERT_EQ_INT(memory.quantity_hint, 7);
    ASSERT_EQ_INT(memory.value_hint, 42);
    ASSERT(memory.confidence > 0);
    ASSERT(memory.salience > 0);
}

TEST(test_stale_contract_summary_dampens_market_demand_memory) {
    contract_summary_t fresh = {
        .active = true,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .quantity_needed = 7.0f,
        .base_price = 42.0f,
        .age_at_copy = 20.0f,
    };
    contract_summary_t stale = fresh;
    stale.age_at_copy = 650.0f;

    market_memory_t fresh_memory = {0};
    market_memory_t stale_memory = {0};
    ASSERT(market_memory_from_contract_summary(&fresh, &fresh_memory));
    ASSERT(market_memory_from_contract_summary(&stale, &stale_memory));

    ASSERT(stale_memory.confidence < fresh_memory.confidence);
    ASSERT(stale_memory.salience < fresh_memory.salience);
    ASSERT_EQ_INT(stale_memory.quantity_hint, fresh_memory.quantity_hint);
    ASSERT_EQ_INT(stale_memory.value_hint, fresh_memory.value_hint);
}

TEST(test_delivery_receipt_memory_adapter_sets_route_and_value) {
    market_memory_t memory = {0};
    ASSERT(market_memory_from_delivery_receipt(0, 2,
                                               COMMODITY_FERRITE_INGOT,
                                               3,
                                               150.0f,
                                               4242,
                                               99,
                                               &memory));
    ASSERT_EQ_INT(memory.active, true);
    ASSERT_EQ_INT(memory.memory_kind, MARKET_MEMORY_DELIVERY_RECEIPT);
    ASSERT_EQ_INT(memory.station_a, 2);
    ASSERT_EQ_INT(memory.station_b, 0);
    ASSERT_EQ_INT(memory.commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(memory.action, CONTRACT_DELIVERY);
    ASSERT_EQ_INT(memory.quantity_hint, 3);
    ASSERT_EQ_INT(memory.value_hint, 150);
    ASSERT_EQ_INT((int)memory.observed_tick, 4242);
    ASSERT_EQ_INT((int)memory.subject_nonce, 99);
    ASSERT(memory.confidence > 0);
    ASSERT(memory.salience > 0);
}

TEST(test_hnn_market_memory_resonance_round_trips) {
    market_memory_t memory = {0};
    ASSERT(market_memory_from_contract_summary(
        &(contract_summary_t){
            .active = true,
            .action = (uint8_t)CONTRACT_TRACTOR,
            .station_index = 2,
            .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
            .quantity_needed = 3.0f,
            .base_price = 90.0f,
            .age_at_copy = 12.0f,
        },
        &memory));

    hnn_memory_t hnn;
    hnn_memory_init(&hnn);
    ASSERT_EQ_FLOAT(gossip_hnn_market_resonance(&hnn,
                                                &memory,
                                                GOSSIP_HNN_JOB_HAUL),
                    0.0f,
                    0.0001f);
    ASSERT(gossip_hnn_store_market_memory(&hnn, &memory));
    float haul_resonance = gossip_hnn_market_resonance(&hnn,
                                                       &memory,
                                                       GOSSIP_HNN_JOB_HAUL);
    float repair_resonance = gossip_hnn_market_resonance(&hnn,
                                                         &memory,
                                                         GOSSIP_HNN_JOB_REPAIR);
    ASSERT(haul_resonance > 0.05f);
    ASSERT(haul_resonance > repair_resonance);
}

TEST(test_signal_intelligence_contract_reason_records_memory_pressure) {
    WORLD_DECL;
    SERVER_PLAYER_DECL(sp);
    signal_contract_candidate_t candidates[2];
    memset(candidates, 0, sizeof(candidates));
    candidates[0].action = SIGNAL_CONTRACT_ACTION_WAIT_FOR_STOCK;
    candidates[0].source_station = 0;
    candidates[0].dest_station = 1;
    candidates[0].commodity = COMMODITY_FERRITE_INGOT;
    candidates[0].teacher_score = 0.25f;
    candidates[1].action = SIGNAL_CONTRACT_ACTION_BUY_AND_DELIVER;
    candidates[1].source_station = 0;
    candidates[1].dest_station = 2;
    candidates[1].commodity = COMMODITY_FERRITE_INGOT;
    candidates[1].teacher_score = 1.50f;
    candidates[1].hologram_resonance = 0.31f;
    candidates[1].source_memory = 0.72f;
    candidates[1].route_success_memory = 0.44f;
    candidates[1].route_danger_memory = 0.18f;
    candidates[1].route_proof_memory = 0.56f;
    candidates[1].trust_bias = 0.22f;

    signal_intelligence_decision_reason_t reason;
    int choice = signal_intelligence_choose_contract_with_reason(
        &w, &sp, candidates, 2, &reason);

    ASSERT_EQ_INT(choice, 1);
    ASSERT_EQ_INT(reason.task, SIGNAL_INTEL_TASK_CONTRACT_CHOICE);
    ASSERT_EQ_INT(reason.selected_index, 1);
    ASSERT_EQ_INT(reason.candidate_count, 2);
    ASSERT_EQ_FLOAT(reason.teacher_score, 1.50f, 0.0001f);
    ASSERT_EQ_FLOAT(reason.hologram_resonance, 0.31f, 0.0001f);
    ASSERT_EQ_FLOAT(reason.source_memory, 0.72f, 0.0001f);
    ASSERT_EQ_FLOAT(reason.proof_memory, 0.56f, 0.0001f);
    ASSERT_EQ_FLOAT(reason.route_success, 0.44f, 0.0001f);
    ASSERT_EQ_FLOAT(reason.route_risk, 0.18f, 0.0001f);
    ASSERT_EQ_FLOAT(reason.trust_bias, 0.22f, 0.0001f);
    ASSERT(reason.source_memory_id != 0ull);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_USED_TEACHER);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_ADVISORY_ONLY);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HARD_APPROVED);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HAS_HOLOGRAM);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HAS_SOURCE_MEMORY);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HAS_PROOF_MEMORY);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HAS_ROUTE_RISK);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HAS_TRUST_BIAS);
}

TEST(test_signal_intelligence_worker_reason_records_route_risk) {
    signal_npc_worker_candidate_t candidates[2];
    double scores[2] = {0.0};
    memset(candidates, 0, sizeof(candidates));
    candidates[0].option = SIGNAL_NPC_WORKER_OPTION_WAIT;
    candidates[0].role = NPC_ROLE_HAULER;
    candidates[0].home_station = 0;
    candidates[0].best_contract_dest = -1;
    candidates[0].best_contract_commodity = COMMODITY_COUNT;
    candidates[0].legal = true;
    candidates[0].teacher_score = 0.0f;

    candidates[1].option = SIGNAL_NPC_WORKER_OPTION_ESCORT_CONVOY;
    candidates[1].role = NPC_ROLE_HAULER;
    candidates[1].home_station = 0;
    candidates[1].best_contract_dest = 2;
    candidates[1].best_contract_commodity = COMMODITY_FERRITE_INGOT;
    candidates[1].legal = true;
    candidates[1].travel = true;
    candidates[1].escort = true;
    candidates[1].teacher_score = 0.20f;
    candidates[1].persona_risk = 0.50f;
    candidates[1].route_success_memory = 0.12f;
    candidates[1].route_danger_memory = 0.70f;
    candidates[1].route_proof_memory = 0.24f;
    candidates[1].hologram_resonance = 0.33f;
    candidates[1].source_memory = 0.64f;
    candidates[1].trust_bias = 0.19f;
    candidates[1].escort_bonus = 0.08f;

    signal_intelligence_decision_reason_t reason;
    int choice = signal_intelligence_choose_npc_worker_with_scores_and_reason(
        candidates, 2, scores, 2, &reason);

    ASSERT_EQ_INT(choice, 1);
    ASSERT(scores[1] > scores[0]);
    ASSERT_EQ_INT(reason.task, SIGNAL_INTEL_TASK_NPC_WORKER_ASSIGNMENT);
    ASSERT_EQ_INT(reason.selected_index, 1);
    ASSERT_EQ_FLOAT(reason.teacher_score, 0.20f, 0.0001f);
    ASSERT_EQ_FLOAT(reason.hologram_resonance, 0.33f, 0.0001f);
    ASSERT_EQ_FLOAT(reason.source_memory, 0.64f, 0.0001f);
    ASSERT_EQ_FLOAT(reason.proof_memory, 0.24f, 0.0001f);
    ASSERT_EQ_FLOAT(reason.route_success, 0.12f, 0.0001f);
    ASSERT_EQ_FLOAT(reason.route_risk, 0.70f, 0.0001f);
    ASSERT_EQ_FLOAT(reason.trust_bias, 0.19f, 0.0001f);
    ASSERT(reason.source_memory_id != 0ull);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_USED_TEACHER);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_ADVISORY_ONLY);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HARD_APPROVED);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HAS_HOLOGRAM);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HAS_SOURCE_MEMORY);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HAS_PROOF_MEMORY);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HAS_ROUTE_RISK);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HAS_TRUST_BIAS);
}

TEST(test_hnn_core_key_cache_preserves_bind_round_trip) {
    float key_a[HNN_DIM];
    float key_b[HNN_DIM];
    float value[HNN_DIM];
    float pair[HNN_DIM];
    float recovered[HNN_DIM];

    hnn_key_vector(0x12345678abcdef00ull, key_a);
    hnn_key_vector(0x12345678abcdef00ull, key_b);
    for (int i = 0; i < HNN_DIM; i++)
        ASSERT_EQ_FLOAT(key_a[i], key_b[i], 0.0f);

    hnn_key_vector(0xf00dfeed00000011ull, value);
    hnn_bind(key_a, value, pair);
    hnn_unbind(pair, key_a, recovered);
    ASSERT(hnn_similarity(recovered, value) > 0.99f);
}

TEST(test_hnn_state_encoding_is_deterministic_and_nonzero) {
    hnn_pilot_features_t features = {
        .target_dist = 0.45f,
        .heading_error = -0.25f,
        .heading_cos = 0.96f,
        .heading_sin = -0.24f,
        .speed = 0.5f,
        .forward_speed = 0.4f,
        .lateral_speed = 0.1f,
        .brake_distance = 0.2f,
        .fwd_clear = 0.8f,
        .left_clear = 0.7f,
        .right_clear = 0.9f,
        .signal_quality = 0.6f,
        .hull_ratio = 1.0f,
        .goal_close = 0.55f,
        .action_delta_turn = 1.0f,
        .action_delta_thrust = 1.0f,
        .composite_dot = -0.24f,
    };
    float a[HNN_DIM];
    float b[HNN_DIM];

    hnn_encode_state(&features, a);
    hnn_encode_state(&features, b);
    ASSERT(hnn_similarity(a, a) > 0.99f);
    for (int i = 0; i < HNN_DIM; i++)
        ASSERT_EQ_FLOAT(a[i], b[i], 0.0f);
}

TEST(test_hnn_state_encoding_preserves_continuous_magnitude) {
    hnn_pilot_features_t low = {.target_dist = 0.1f};
    hnn_pilot_features_t mid = {.target_dist = 0.5f};
    hnn_pilot_features_t high = {.target_dist = 1.0f};
    float low_vec[HNN_DIM];
    float mid_vec[HNN_DIM];
    float high_vec[HNN_DIM];

    hnn_encode_state(&low, low_vec);
    hnn_encode_state(&mid, mid_vec);
    hnn_encode_state(&high, high_vec);

    float low_mid = hnn_similarity(low_vec, mid_vec);
    float mid_high = hnn_similarity(mid_vec, high_vec);
    float low_high = hnn_similarity(low_vec, high_vec);
    ASSERT(low_mid < 0.9999f);
    ASSERT(mid_high < 0.9999f);
    ASSERT(low_high < low_mid);
    ASSERT(low_high < mid_high);
}

TEST(test_hnn_state_encoding_defines_zero_sign_and_clipping) {
    hnn_pilot_features_t negative = {.heading_error = -0.5f};
    hnn_pilot_features_t zero = {0};
    hnn_pilot_features_t positive = {.heading_error = 0.5f};
    hnn_pilot_features_t positive_max = {.heading_error = 1.0f};
    hnn_pilot_features_t positive_over = {.heading_error = 7.0f};
    hnn_pilot_features_t negative_max = {.heading_error = -1.0f};
    hnn_pilot_features_t negative_over = {.heading_error = -7.0f};
    hnn_pilot_features_t not_finite_nan = {.heading_error = NAN};
    hnn_pilot_features_t not_finite_pos = {.heading_error = INFINITY};
    hnn_pilot_features_t not_finite_neg = {.heading_error = -INFINITY};
    float negative_vec[HNN_DIM];
    float zero_vec[HNN_DIM];
    float positive_vec[HNN_DIM];
    float positive_max_vec[HNN_DIM];
    float positive_over_vec[HNN_DIM];
    float negative_max_vec[HNN_DIM];
    float negative_over_vec[HNN_DIM];
    float not_finite_nan_vec[HNN_DIM];
    float not_finite_pos_vec[HNN_DIM];
    float not_finite_neg_vec[HNN_DIM];

    hnn_encode_state(&negative, negative_vec);
    hnn_encode_state(&zero, zero_vec);
    hnn_encode_state(&positive, positive_vec);
    hnn_encode_state(&positive_max, positive_max_vec);
    hnn_encode_state(&positive_over, positive_over_vec);
    hnn_encode_state(&negative_max, negative_max_vec);
    hnn_encode_state(&negative_over, negative_over_vec);
    hnn_encode_state(&not_finite_nan, not_finite_nan_vec);
    hnn_encode_state(&not_finite_pos, not_finite_pos_vec);
    hnn_encode_state(&not_finite_neg, not_finite_neg_vec);

    ASSERT(hnn_similarity(negative_vec, positive_vec) < 0.9999f);
    ASSERT(hnn_similarity(negative_vec, zero_vec) < 0.9999f);
    ASSERT(hnn_similarity(positive_vec, zero_vec) < 0.9999f);
    for (int i = 0; i < HNN_DIM; i++) {
        ASSERT_EQ_FLOAT(positive_max_vec[i], positive_over_vec[i], 0.0f);
        ASSERT_EQ_FLOAT(negative_max_vec[i], negative_over_vec[i], 0.0f);
        ASSERT_EQ_FLOAT(zero_vec[i], not_finite_nan_vec[i], 0.0f);
        ASSERT_EQ_FLOAT(zero_vec[i], not_finite_pos_vec[i], 0.0f);
        ASSERT_EQ_FLOAT(zero_vec[i], not_finite_neg_vec[i], 0.0f);
    }
}

TEST(test_hnn_magnitude_sensitive_action_retrieval) {
    hnn_action_table_t actions;
    hnn_memory_t distance_memory;
    hnn_memory_t signal_memory;
    hnn_memory_t feature_memory;
    hnn_pilot_features_t near = {.target_dist = 0.1f};
    hnn_pilot_features_t far = {.target_dist = 0.9f};
    hnn_pilot_features_t weak = {.signal_quality = 0.1f};
    hnn_pilot_features_t strong = {.signal_quality = 0.9f};
    float state_vec[HNN_DIM];

    hnn_action_table_init(&actions);
    hnn_memory_init(&distance_memory);
    hnn_encode_state(&near, state_vec);
    hnn_memory_store(&distance_memory, state_vec, actions.vecs[4]);
    hnn_encode_state(&far, state_vec);
    hnn_memory_store(&distance_memory, state_vec, actions.vecs[1]);
    ASSERT_EQ_INT(hnn_select_action(&distance_memory, &actions, &near, NULL), 4);
    ASSERT_EQ_INT(hnn_select_action(&distance_memory, &actions, &far, NULL), 1);

    hnn_memory_init(&signal_memory);
    hnn_encode_state(&weak, state_vec);
    hnn_memory_store(&signal_memory, state_vec, actions.vecs[0]);
    hnn_encode_state(&strong, state_vec);
    hnn_memory_store(&signal_memory, state_vec, actions.vecs[3]);
    ASSERT_EQ_INT(hnn_select_action(&signal_memory, &actions, &weak, NULL), 0);
    ASSERT_EQ_INT(hnn_select_action(&signal_memory, &actions, &strong, NULL), 3);

    hnn_memory_init(&feature_memory);
    hnn_encode_state(&far, state_vec);
    hnn_memory_store(&feature_memory, state_vec, actions.vecs[5]);
    hnn_encode_state(&strong, state_vec);
    hnn_memory_store(&feature_memory, state_vec, actions.vecs[7]);
    ASSERT_EQ_INT(hnn_select_action(&feature_memory, &actions, &far, NULL), 5);
    ASSERT_EQ_INT(hnn_select_action(&feature_memory, &actions, &strong, NULL), 7);
}

static hnn_pilot_features_t hnn_capacity_probe_state(int i) {
    hnn_pilot_features_t f = {0};
    f.target_dist = (float)((i * 37) % 101) / 100.0f;
    f.heading_error = (float)(((i * 53) % 201) - 100) / 100.0f;
    f.heading_cos = (float)(((i * 71) % 201) - 100) / 100.0f;
    f.heading_sin = (float)(((i * 89) % 201) - 100) / 100.0f;
    f.speed = (float)((i * 29) % 101) / 100.0f;
    f.forward_speed = (float)(((i * 43) % 201) - 100) / 100.0f;
    f.lateral_speed = (float)(((i * 61) % 201) - 100) / 100.0f;
    f.brake_distance = (float)((i * 17) % 101) / 100.0f;
    f.fwd_clear = (float)((i * 23) % 101) / 100.0f;
    f.left_clear = (float)((i * 31) % 101) / 100.0f;
    f.right_clear = (float)((i * 47) % 101) / 100.0f;
    f.signal_quality = (float)((i * 59) % 101) / 100.0f;
    f.hull_ratio = (float)((i * 67) % 101) / 100.0f;
    f.goal_close = 1.0f - f.target_dist;
    return f;
}

TEST(test_hnn_capacity_preserves_action_signal_through_limit) {
    static const int checkpoints[] = {1, 16, 32, 64, 128};
    /*
     * Encoder-v2 deterministic baseline with three cleanup steps is
     * 1/1, 4/16, 5/32, 11/64, and 16/128 correct. Keep conservative
     * floors here so the configured capacity cannot silently fall to
     * chance while still allowing future fidelity improvements.
     */
    static const int minimum_correct[] = {1, 3, 5, 9, 15};
    hnn_action_table_t actions;
    hnn_memory_t memory;
    int checkpoint = 0;

    hnn_action_table_init(&actions);
    hnn_memory_init(&memory);
    for (int count = 1; count <= (int)HNN_TRACE_CAPACITY; count++) {
        hnn_pilot_features_t state = hnn_capacity_probe_state(count - 1);
        float state_vec[HNN_DIM];
        hnn_encode_state(&state, state_vec);
        hnn_memory_store(&memory, state_vec,
                         actions.vecs[(count - 1) % HNN_ACTION_COUNT]);

        if (count != checkpoints[checkpoint]) continue;

        int correct = 0;
        float margin_sum = 0.0f;
        float fidelity_sum = 0.0f;
        for (int query = 0; query < count; query++) {
            hnn_pilot_features_t probe = hnn_capacity_probe_state(query);
            float margin = 0.0f;
            float fidelity = 0.0f;
            int selected = hnn_score_actions(
                &memory, &actions, &probe, NULL, &margin, &fidelity, 3);
            if (selected == query % HNN_ACTION_COUNT) correct++;
            margin_sum += margin;
            fidelity_sum += fidelity;
        }

        ASSERT(correct >= minimum_correct[checkpoint]);
        ASSERT(correct * HNN_ACTION_COUNT > count);
        ASSERT(margin_sum > 0.0f);
        ASSERT(fidelity_sum > 0.0f);
        ASSERT_EQ_FLOAT(hnn_memory_capacity_load(&memory),
                        (float)count / (float)HNN_TRACE_CAPACITY,
                        0.000001f);
        checkpoint++;
    }
    ASSERT_EQ_INT(checkpoint,
                  (int)(sizeof(checkpoints) / sizeof(checkpoints[0])));
}

TEST(test_hnn_memory_contract_reports_trace_diagnostics) {
    hnn_memory_t hnn;
    hnn_memory_init(&hnn);

    hnn_memory_contract_t empty = hnn_memory_contract(&hnn);
    ASSERT_EQ_INT(empty.dim, HNN_DIM);
    ASSERT_EQ_INT((int)empty.keygen_version, (int)HNN_KEYGEN_VERSION);
    ASSERT_EQ_INT((int)empty.encoder_version, (int)HNN_PILOT_ENCODER_VERSION);
    ASSERT_EQ_INT((int)empty.trace_format_version, (int)HNN_TRACE_FORMAT_VERSION);
    ASSERT_EQ_INT(empty.stored_count, 0);
    ASSERT_EQ_FLOAT(empty.capacity_load, 0.0f, 0.0f);
    ASSERT_EQ_FLOAT(empty.fidelity_estimate, 0.0f, 0.0f);
    ASSERT(empty.seed == HNN_CONTRACT_SEED);
    ASSERT(empty.action_vocabulary_hash == hnn_action_vocabulary_hash());

    hnn_action_table_t actions;
    hnn_action_table_init(&actions);
    hnn_pilot_features_t features = {
        .target_dist = 0.25f,
        .heading_error = 0.1f,
        .heading_cos = 0.995f,
        .heading_sin = 0.1f,
        .speed = 0.35f,
        .forward_speed = 0.2f,
        .lateral_speed = 0.05f,
        .brake_distance = 0.1f,
        .fwd_clear = 0.9f,
        .left_clear = 0.8f,
        .right_clear = 0.85f,
        .signal_quality = 0.95f,
        .hull_ratio = 1.0f,
        .goal_close = 0.75f,
        .action_delta_turn = 0.0f,
        .action_delta_thrust = 1.0f,
    };
    float state_vec[HNN_DIM];
    hnn_encode_state(&features, state_vec);
    hnn_memory_store(&hnn, state_vec, actions.vecs[1]);

    float scores[HNN_ACTION_COUNT];
    float margin = 0.0f;
    float fidelity = 0.0f;
    int best = hnn_score_actions(&hnn,
                                 &actions,
                                 &features,
                                 scores,
                                 &margin,
                                 &fidelity,
                                 0);
    ASSERT(best >= 0);
    ASSERT(isfinite(scores[best]));
    ASSERT(fidelity > 0.0f);
    hnn.last_retrieval_similarity = scores[best];
    hnn.last_margin = margin;

    hnn_memory_contract_t filled = hnn_memory_contract(&hnn);
    ASSERT_EQ_INT(filled.stored_count, 1);
    ASSERT(filled.capacity_load > 0.0f);
    ASSERT(filled.fidelity_estimate > 0.0f);
    ASSERT_EQ_FLOAT(filled.last_margin, margin, 0.0001f);
}

TEST(test_hnn_backend_metadata_records_builtin_and_liblecore_pin) {
    hnn_backend_metadata_t metadata = hnn_backend_metadata();

    ASSERT_EQ_INT(metadata.dimension, HNN_DIM);
    ASSERT_EQ_INT((int)metadata.active_abi_version,
                  (int)HNN_CONTRACT_VERSION);
    ASSERT(strcmp(metadata.active_library, "signal") == 0);
    ASSERT(strcmp(metadata.active_backend, "builtin-radix2") == 0);
    ASSERT(metadata.active_source_revision[0] != '\0');
    ASSERT(metadata.scratch_bytes ==
           (size_t)HNN_DIM * 4u * sizeof(float));
    ASSERT(strcmp(metadata.liblecore_version, "0.1.0") == 0);
    ASSERT_EQ_INT((int)metadata.liblecore_abi_version, 0);
    ASSERT(strncmp(metadata.liblecore_source_revision,
                   "sha256:", 7) == 0);
    ASSERT(strncmp(metadata.liblecore_source_checksum,
                   "sha256:", 7) == 0);
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
    ASSERT(metadata.liblecore_compiled);
#else
    ASSERT(!metadata.liblecore_compiled);
#endif
}

TEST(test_hnn_holonet_single_cell_matches_flat_trace) {
    hnn_action_table_t actions;
    hnn_action_table_init(&actions);
    hnn_memory_t flat;
    hnn_holonet_t net;
    hnn_memory_init(&flat);
    hnn_holonet_init(&net);

    hnn_pilot_features_t state = {
        .target_dist = 0.25f,
        .heading_error = 0.1f,
        .heading_cos = 0.995f,
        .heading_sin = 0.1f,
        .speed = 0.35f,
        .forward_speed = 0.2f,
        .lateral_speed = 0.05f,
        .brake_distance = 0.1f,
        .fwd_clear = 0.9f,
        .left_clear = 0.8f,
        .right_clear = 0.85f,
        .signal_quality = 0.95f,
        .hull_ratio = 1.0f,
        .goal_close = 0.75f,
    };
    hnn_pilot_features_t full = state;
    full.action_delta_thrust = 1.0f;

    float route_vec[HNN_DIM];
    float assoc_vec[HNN_DIM];
    hnn_encode_state(&state, route_vec);
    hnn_encode_state(&full, assoc_vec);
    hnn_memory_store(&flat, assoc_vec, actions.vecs[1]);
    hnn_holonet_store(&net, route_vec, assoc_vec, actions.vecs[1]);

    float flat_scores[HNN_ACTION_COUNT];
    float net_scores[HNN_ACTION_COUNT];
    float flat_margin = 0.0f;
    float net_margin = 0.0f;
    float net_fidelity = 0.0f;
    int flat_best = hnn_score_actions(&flat, &actions, &state,
                                      flat_scores, &flat_margin, NULL, 0);
    int net_best = hnn_holonet_score_actions(&net, &actions, &state,
                                             net_scores, &net_margin,
                                             &net_fidelity, 0);

    ASSERT_EQ_INT(flat_best, net_best);
    for (int i = 0; i < HNN_ACTION_COUNT; i++)
        ASSERT_EQ_FLOAT(flat_scores[i], net_scores[i], 0.00001f);
    ASSERT_EQ_FLOAT(flat_margin, net_margin, 0.00001f);
    ASSERT(net_fidelity > 0.0f);
    ASSERT_EQ_INT(hnn_holonet_active_count(&net), 1);
    ASSERT_EQ_INT(net.last_route, 0);
    ASSERT_EQ_INT(net.last_scored_count, 1);

    hnn_memory_contract_t contract = hnn_holonet_contract(&net);
    ASSERT_EQ_INT(contract.stored_count, 1);
    ASSERT(contract.capacity_load > 0.0f);
    ASSERT(contract.fidelity_estimate > 0.0f);
}

TEST(test_hnn_holonet_routes_novel_states_to_distinct_cells) {
    hnn_action_table_t actions;
    hnn_action_table_init(&actions);
    hnn_holonet_t net;
    hnn_holonet_init(&net);

    float route_a[HNN_DIM];
    float route_b[HNN_DIM];
    float assoc_a[HNN_DIM];
    float assoc_b[HNN_DIM];
    hnn_key_vector(0xabcdu, route_a);
    for (int i = 0; i < HNN_DIM; i++) route_b[i] = -route_a[i];
    memcpy(assoc_a, route_a, sizeof(assoc_a));
    memcpy(assoc_b, route_b, sizeof(assoc_b));

    hnn_holonet_store(&net, route_a, assoc_a, actions.vecs[1]);
    ASSERT_EQ_INT(hnn_holonet_active_count(&net), 1);
    ASSERT_EQ_INT(net.last_route, 0);

    hnn_holonet_store(&net, route_b, assoc_b, actions.vecs[4]);
    ASSERT_EQ_INT(hnn_holonet_active_count(&net), 2);
    ASSERT_EQ_INT(net.stored_count, 2);
    ASSERT(net.last_route >= 0 && net.last_route < (int)HNN_HOLONET_TRACE_COUNT);

    hnn_memory_contract_t contract = hnn_holonet_contract(&net);
    ASSERT_EQ_INT(contract.stored_count, 2);
    ASSERT(contract.capacity_load > 0.0f);
}

TEST(test_hnn_state_encoding_sanitizes_nonfinite_features) {
    hnn_pilot_features_t features = {
        .target_dist = NAN,
        .heading_error = INFINITY,
        .heading_cos = 1.0f,
        .heading_sin = -INFINITY,
        .speed = 0.3f,
        .forward_speed = 0.2f,
        .lateral_speed = NAN,
        .brake_distance = 0.1f,
        .fwd_clear = 0.9f,
        .left_clear = 0.8f,
        .right_clear = 0.7f,
        .signal_quality = 1.0f,
        .hull_ratio = 1.0f,
        .goal_close = 0.5f,
        .action_delta_turn = 1.0f,
        .action_delta_thrust = 1.0f,
        .composite_dot = NAN,
    };
    hnn_action_table_t actions;
    hnn_memory_t hnn;
    float state_vec[HNN_DIM];
    float scores[HNN_ACTION_COUNT];
    float margin = 0.0f;
    float fidelity = 0.0f;

    hnn_encode_state(&features, state_vec);
    for (int i = 0; i < HNN_DIM; i++) ASSERT(isfinite(state_vec[i]));

    hnn_action_table_init(&actions);
    hnn_memory_init(&hnn);
    hnn_memory_store(&hnn, state_vec, actions.vecs[1]);
    ASSERT(hnn_score_actions(&hnn,
                             &actions,
                             &features,
                             scores,
                             &margin,
                             &fidelity,
                             3) >= 0);
    for (int i = 0; i < HNN_ACTION_COUNT; i++) ASSERT(isfinite(scores[i]));
    ASSERT(isfinite(margin));
    ASSERT(isfinite(fidelity));
}

#ifndef _WIN32
enum {
    HNN_REENTRANT_THREAD_COUNT = 4,
    HNN_REENTRANT_SIM_STEPS = 32,
};

typedef struct {
    atomic_int ready;
    atomic_bool go;
} hnn_reentrant_start_t;

typedef struct {
    hnn_reentrant_start_t *start;
    world_t *world;
    float key[HNN_DIM];
    float state[HNN_DIM];
    vec2 final_pos;
} hnn_reentrant_worker_t;

static void *hnn_reentrant_worker_main(void *opaque) {
    hnn_reentrant_worker_t *ctx = opaque;
    hnn_pilot_features_t features = {
        .target_dist = 0.75f,
        .heading_error = -0.25f,
        .heading_cos = 0.96875f,
        .heading_sin = -0.25f,
        .speed = 0.40f,
        .forward_speed = 0.35f,
        .lateral_speed = 0.05f,
        .brake_distance = 0.10f,
        .fwd_clear = 0.80f,
        .left_clear = 0.60f,
        .right_clear = 0.70f,
        .signal_quality = 0.90f,
        .hull_ratio = 1.0f,
        .path_count = 0.25f,
        .path_current = 0.125f,
    };

    atomic_fetch_add_explicit(&ctx->start->ready, 1,
                              memory_order_release);
    while (!atomic_load_explicit(&ctx->start->go,
                                 memory_order_acquire)) {
    }

    /*
     * Each worker enters deterministic key generation, its cache, and
     * feature/value-key initialization from a fresh thread. The independent
     * worlds exercise the player-only simulation path at the same time.
     */
    hnn_key_vector(0x6245afe123456789ull, ctx->key);
    hnn_encode_state(&features, ctx->state);
    for (int i = 0; i < HNN_REENTRANT_SIM_STEPS; i++)
        world_sim_step_player_only(ctx->world, 0, SIM_DT);
    ctx->final_pos = ctx->world->players[0].ship->pos;
    return NULL;
}
#endif

TEST(test_hnn_reentrant_key_state_and_simulation) {
#ifdef _WIN32
    /* The bounded ThreadSanitizer lane is Linux-only. */
    ASSERT(true);
#else
    hnn_reentrant_start_t start;
    atomic_init(&start.ready, 0);
    atomic_init(&start.go, false);

    pthread_t threads[HNN_REENTRANT_THREAD_COUNT];
    hnn_reentrant_worker_t workers[HNN_REENTRANT_THREAD_COUNT] = {0};
    int created = 0;
    int thread_error = 0;
    bool results_match = true;

    for (int i = 0; i < HNN_REENTRANT_THREAD_COUNT; i++) {
        workers[i].start = &start;
        workers[i].world = calloc(1, sizeof(*workers[i].world));
        if (!workers[i].world) {
            thread_error = -1;
            break;
        }
        world_reset(workers[i].world);
        server_player_t *sp = &workers[i].world->players[0];
        player_init_ship(sp, workers[i].world);
        sp->connected = true;
        sp->session_ready = true;
        sp->docked = false;
        sp->current_station = -1;
        sp->ship->pos = v2(120.0f, -80.0f);
        sp->ship->vel = v2(12.0f, -3.0f);
        sp->input.thrust = 0.5f;
        sp->input.turn = -0.25f;
    }

    if (thread_error == 0) {
        for (int i = 0; i < HNN_REENTRANT_THREAD_COUNT; i++) {
            thread_error = pthread_create(
                &threads[i], NULL, hnn_reentrant_worker_main,
                &workers[i]);
            if (thread_error != 0) break;
            created++;
        }
    }

    while (thread_error == 0 &&
           atomic_load_explicit(&start.ready, memory_order_acquire) !=
               HNN_REENTRANT_THREAD_COUNT) {
    }
    atomic_store_explicit(&start.go, true, memory_order_release);

    for (int i = 0; i < created; i++) {
        if (pthread_join(threads[i], NULL) != 0 &&
            thread_error == 0) {
            thread_error = -1;
        }
    }

    if (thread_error == 0) {
        for (int i = 1; i < HNN_REENTRANT_THREAD_COUNT; i++) {
            results_match =
                results_match &&
                memcmp(workers[0].key, workers[i].key,
                       sizeof(workers[0].key)) == 0 &&
                memcmp(workers[0].state, workers[i].state,
                       sizeof(workers[0].state)) == 0 &&
                fabsf(workers[0].final_pos.x -
                      workers[i].final_pos.x) <= 0.000001f &&
                fabsf(workers[0].final_pos.y -
                      workers[i].final_pos.y) <= 0.000001f;
        }
    }

    for (int i = 0; i < HNN_REENTRANT_THREAD_COUNT; i++) {
        if (!workers[i].world) continue;
        world_cleanup(workers[i].world);
        free(workers[i].world);
    }

    ASSERT_EQ_INT(thread_error, 0);
    ASSERT(results_match);
#endif
}

TEST(test_hnn_market_memory_maps_specialized_jobs) {
    market_memory_t delivery = {0};
    ASSERT(market_memory_from_contract_summary(
        &(contract_summary_t){
            .active = true,
            .action = (uint8_t)CONTRACT_DELIVERY,
            .station_index = 2,
            .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
            .quantity_needed = 1.0f,
            .base_price = 120.0f,
            .age_at_copy = 5.0f,
        },
        &delivery));
    hnn_memory_t hnn;
    hnn_memory_init(&hnn);
    ASSERT(gossip_hnn_store_market_memory(&hnn, &delivery));
    float proof_resonance = gossip_hnn_market_resonance(
        &hnn, &delivery, GOSSIP_HNN_JOB_DELIVER_PROOF);
    float haul_resonance = gossip_hnn_market_resonance(
        &hnn, &delivery, GOSSIP_HNN_JOB_HAUL);
    ASSERT(proof_resonance > 0.05f);
    ASSERT(proof_resonance > haul_resonance);

    market_memory_t fracture = {0};
    ASSERT(market_memory_from_contract_summary(
        &(contract_summary_t){
            .active = true,
            .action = (uint8_t)CONTRACT_FRACTURE,
            .station_index = 1,
            .commodity = (uint8_t)COMMODITY_FERRITE_ORE,
            .quantity_needed = 1.0f,
            .base_price = 80.0f,
            .age_at_copy = 2.0f,
        },
        &fracture));
    hnn_memory_init(&hnn);
    ASSERT(gossip_hnn_store_market_memory(&hnn, &fracture));
    float scout_resonance = gossip_hnn_market_resonance(
        &hnn, &fracture, GOSSIP_HNN_JOB_SCOUT);
    haul_resonance = gossip_hnn_market_resonance(
        &hnn, &fracture, GOSSIP_HNN_JOB_HAUL);
    ASSERT(scout_resonance > 0.05f);
    ASSERT(scout_resonance > haul_resonance);

    market_memory_t repair_supply = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_SUPPLY,
        .station_a = 0,
        .station_b = 0xff,
        .commodity = (uint8_t)COMMODITY_REPAIR_KIT,
        .quantity_hint = 4,
        .confidence = 235,
        .salience = 190,
    };
    hnn_memory_init(&hnn);
    ASSERT(gossip_hnn_store_market_memory(&hnn, &repair_supply));
    float repair_resonance = gossip_hnn_market_resonance(
        &hnn, &repair_supply, GOSSIP_HNN_JOB_REPAIR);
    haul_resonance = gossip_hnn_market_resonance(
        &hnn, &repair_supply, GOSSIP_HNN_JOB_HAUL);
    ASSERT(repair_resonance > 0.05f);
    ASSERT(repair_resonance > haul_resonance);

    market_memory_t ore_pressure = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_ORE_PRESSURE,
        .station_a = 0,
        .station_b = 0xff,
        .commodity = (uint8_t)COMMODITY_FERRITE_ORE,
        .action = 0xff,
        .confidence = 220,
        .salience = 210,
    };
    hnn_memory_init(&hnn);
    ASSERT(gossip_hnn_store_market_memory(&hnn, &ore_pressure));
    float mine_resonance = gossip_hnn_market_resonance(
        &hnn, &ore_pressure, GOSSIP_HNN_JOB_MINE);
    haul_resonance = gossip_hnn_market_resonance(
        &hnn, &ore_pressure, GOSSIP_HNN_JOB_HAUL);
    ASSERT(mine_resonance > 0.05f);
    ASSERT(mine_resonance > haul_resonance);

    market_memory_t scaffold_pressure = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_SCAFFOLD_PRESSURE,
        .station_a = 3,
        .station_b = 1,
        .commodity = (uint8_t)COMMODITY_COUNT,
        .action = 0xff,
        .confidence = 220,
        .salience = 210,
        .quantity_hint = MODULE_SIGNAL_RELAY,
    };
    hnn_memory_init(&hnn);
    ASSERT(gossip_hnn_store_market_memory(&hnn, &scaffold_pressure));
    float tow_resonance = gossip_hnn_market_resonance(
        &hnn, &scaffold_pressure, GOSSIP_HNN_JOB_TOW);
    haul_resonance = gossip_hnn_market_resonance(
        &hnn, &scaffold_pressure, GOSSIP_HNN_JOB_HAUL);
    ASSERT(tow_resonance > 0.05f);
    ASSERT(tow_resonance > haul_resonance);

    market_memory_t receipt = {0};
    ASSERT(market_memory_from_delivery_receipt(1, 3,
                                               COMMODITY_FERRITE_INGOT,
                                               2,
                                               90.0f,
                                               77,
                                               9,
                                               &receipt));
    hnn_memory_init(&hnn);
    ASSERT(gossip_hnn_store_market_memory(&hnn, &receipt));
    proof_resonance = gossip_hnn_market_resonance(
        &hnn, &receipt, GOSSIP_HNN_JOB_DELIVER_PROOF);
    haul_resonance = gossip_hnn_market_resonance(
        &hnn, &receipt, GOSSIP_HNN_JOB_HAUL);
    ASSERT(proof_resonance > 0.05f);
    ASSERT(haul_resonance > 0.05f);
}

TEST(test_route_reputation_memory_reinforces_repeated_evidence) {
    market_memory_t memory = {0};
    ASSERT(market_memory_from_route_reputation(0, 2,
                                               COMMODITY_FERRITE_INGOT,
                                               2,
                                               100.0f,
                                               42,
                                               false,
                                               &memory));
    ASSERT_EQ_INT(memory.active, true);
    ASSERT_EQ_INT(memory.memory_kind, MARKET_MEMORY_ROUTE_REPUTATION);
    ASSERT_EQ_INT(memory.station_a, 2);
    ASSERT_EQ_INT(memory.station_b, 0);
    ASSERT_EQ_INT(memory.commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(memory.action, CONTRACT_TRACTOR);
    ASSERT_EQ_INT(memory.quantity_hint, 2);
    ASSERT_EQ_INT(memory.value_hint, 100);

    knowledge_view_t view = {0};
    knowledge_view_configure(&view, 4);
    knowledge_view_reinforce_route_reputation(&view, &memory);
    ASSERT_EQ_INT(view.count, 1);

    uint8_t first_conf = view.items[0].confidence;
    ASSERT(market_memory_from_route_reputation(0, 2,
                                               COMMODITY_FERRITE_INGOT,
                                               3,
                                               120.0f,
                                               43,
                                               false,
                                               &memory));
    knowledge_view_reinforce_route_reputation(&view, &memory);
    ASSERT_EQ_INT(view.count, 1);
    market_memory_t out = {0};
    ASSERT(market_memory_from_knowledge_item(&view.items[0], &out));
    ASSERT_EQ_INT(out.memory_kind, MARKET_MEMORY_ROUTE_REPUTATION);
    ASSERT_EQ_INT(out.quantity_hint, 5);
    ASSERT_EQ_INT(out.value_hint, 220);
    ASSERT(out.confidence > first_conf);
}

TEST(test_station_trust_memory_reinforces_repeated_evidence) {
    market_memory_t memory = {0};
    ASSERT(market_memory_from_station_trust(2,
                                            (uint8_t)CONTRACT_TRACTOR,
                                            COMMODITY_FERRITE_INGOT,
                                            2,
                                            75.0f,
                                            20,
                                            &memory));
    ASSERT_EQ_INT(memory.active, true);
    ASSERT_EQ_INT(memory.memory_kind, MARKET_MEMORY_STATION_TRUST);
    ASSERT_EQ_INT(memory.station_a, 2);
    ASSERT_EQ_INT(memory.station_b, 0xff);
    ASSERT_EQ_INT(memory.commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(memory.action, CONTRACT_TRACTOR);
    ASSERT_EQ_INT(memory.quantity_hint, 2);

    knowledge_view_t view = {0};
    knowledge_view_configure(&view, 4);
    knowledge_view_reinforce_station_trust(&view, &memory);
    ASSERT_EQ_INT(view.count, 1);

    uint8_t first_conf = view.items[0].confidence;
    ASSERT(market_memory_from_station_trust(2,
                                            (uint8_t)CONTRACT_TRACTOR,
                                            COMMODITY_FERRITE_INGOT,
                                            3,
                                            125.0f,
                                            21,
                                            &memory));
    knowledge_view_reinforce_station_trust(&view, &memory);
    ASSERT_EQ_INT(view.count, 1);
    market_memory_t out = {0};
    ASSERT(market_memory_from_knowledge_item(&view.items[0], &out));
    ASSERT_EQ_INT(out.memory_kind, MARKET_MEMORY_STATION_TRUST);
    ASSERT_EQ_INT(out.quantity_hint, 5);
    ASSERT_EQ_INT(out.value_hint, 200);
    ASSERT(out.confidence > first_conf);
}

TEST(test_station_risk_memory_reinforces_repeated_evidence) {
    market_memory_t memory = {0};
    ASSERT(market_memory_from_station_risk(1,
                                           (uint8_t)CONTRACT_TRACTOR,
                                           COMMODITY_FERRITE_INGOT,
                                           1,
                                           10.0f,
                                           31,
                                           &memory));
    ASSERT_EQ_INT(memory.active, true);
    ASSERT_EQ_INT(memory.memory_kind, MARKET_MEMORY_STATION_RISK);
    ASSERT_EQ_INT(memory.station_a, 1);
    ASSERT_EQ_INT(memory.station_b, 0xff);
    ASSERT_EQ_INT(memory.commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(memory.action, CONTRACT_TRACTOR);
    ASSERT_EQ_INT(memory.quantity_hint, 1);

    knowledge_view_t view = {0};
    knowledge_view_configure(&view, 4);
    knowledge_view_reinforce_station_trust(&view, &memory);
    ASSERT_EQ_INT(view.count, 1);

    uint8_t first_conf = view.items[0].confidence;
    ASSERT(market_memory_from_station_risk(1,
                                           (uint8_t)CONTRACT_TRACTOR,
                                           COMMODITY_FERRITE_INGOT,
                                           2,
                                           15.0f,
                                           32,
                                           &memory));
    knowledge_view_reinforce_station_trust(&view, &memory);
    ASSERT_EQ_INT(view.count, 1);
    market_memory_t out = {0};
    ASSERT(market_memory_from_knowledge_item(&view.items[0], &out));
    ASSERT_EQ_INT(out.memory_kind, MARKET_MEMORY_STATION_RISK);
    ASSERT_EQ_INT(out.quantity_hint, 3);
    ASSERT_EQ_INT(out.value_hint, 25);
    ASSERT(out.confidence > first_conf);
}

TEST(test_knowledge_view_market_dedup_keeps_fresher_observation) {
    knowledge_view_t view = {0};
    knowledge_view_configure(&view, 4);

    market_memory_t memory = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_DEMAND,
        .station_a = 2,
        .station_b = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_DELIVERY,
        .confidence = 120,
        .salience = 140,
        .quantity_hint = 4,
        .value_hint = 10,
        .observed_tick = 10,
    };

    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&memory, &item));
    knowledge_view_insert(&view, &item);
    ASSERT_EQ_INT(view.count, 1);

    memory.quantity_hint = 9;
    memory.value_hint = 25;
    memory.confidence = 220;
    memory.observed_tick = 20;
    ASSERT(knowledge_item_from_market_memory(&memory, &item));
    knowledge_view_insert(&view, &item);
    ASSERT_EQ_INT(view.count, 1);

    memory.quantity_hint = 1;
    memory.value_hint = 99;
    memory.confidence = 10;
    memory.observed_tick = 5;
    ASSERT(knowledge_item_from_market_memory(&memory, &item));
    knowledge_view_insert(&view, &item);
    ASSERT_EQ_INT(view.count, 1);

    market_memory_t out = {0};
    ASSERT(market_memory_from_knowledge_item(&view.items[0], &out));
    ASSERT_EQ_INT(out.quantity_hint, 9);
    ASSERT_EQ_INT(out.value_hint, 25);
    ASSERT_EQ_INT(out.confidence, 220);
    ASSERT_EQ_INT((int)out.observed_tick, 20);
}

TEST(test_knowledge_exchange_copies_bounded_items_bidirectionally) {
    knowledge_view_t station = {0};
    knowledge_view_t ship = {0};
    knowledge_view_configure(&station, 4);
    knowledge_view_configure(&ship, 4);

    contract_summary_t station_contract = {
        .active = true,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .base_price = 10.0f,
        .age_at_copy = 4.0f,
    };
    knowledge_item_t item;
    ASSERT(knowledge_item_from_contract_summary(&station_contract, &item));
    knowledge_view_insert(&station, &item);

    contract_summary_t ship_contract = {
        .active = true,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = (uint8_t)COMMODITY_CUPRITE_INGOT,
        .base_price = 15.0f,
        .age_at_copy = 8.0f,
    };
    ASSERT(knowledge_item_from_contract_summary(&ship_contract, &item));
    knowledge_view_insert(&ship, &item);

    knowledge_view_exchange(&station, &ship);

    ASSERT(view_has_contract(&ship, (uint8_t)CONTRACT_TRACTOR, 1,
                             (uint8_t)COMMODITY_FERRITE_INGOT, NULL));
    ASSERT(view_has_contract(&station, (uint8_t)CONTRACT_TRACTOR, 2,
                             (uint8_t)COMMODITY_CUPRITE_INGOT, NULL));
}

TEST(test_knowledge_exchange_copies_market_memories_bidirectionally) {
    knowledge_view_t station = {0};
    knowledge_view_t ship = {0};
    knowledge_view_configure(&station, 4);
    knowledge_view_configure(&ship, 4);

    market_memory_t station_memory = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_DEMAND,
        .station_a = 2,
        .station_b = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_DELIVERY,
        .confidence = 200,
        .salience = 190,
        .quantity_hint = 6,
        .observed_tick = 44,
    };
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&station_memory, &item));
    knowledge_view_insert(&station, &item);

    market_memory_t ship_memory = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_DANGER,
        .station_a = 1,
        .station_b = 2,
        .commodity = (uint8_t)COMMODITY_COUNT,
        .action = 0xff,
        .confidence = 160,
        .salience = 150,
        .observed_tick = 45,
    };
    ASSERT(knowledge_item_from_market_memory(&ship_memory, &item));
    knowledge_view_insert(&ship, &item);

    knowledge_view_exchange(&station, &ship);

    ASSERT(view_has_market_memory(&ship,
                                  (uint8_t)MARKET_MEMORY_DEMAND,
                                  2, 0,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  NULL));
    ASSERT(view_has_market_memory(&station,
                                  (uint8_t)MARKET_MEMORY_ROUTE_DANGER,
                                  1, 2,
                                  (uint8_t)COMMODITY_COUNT,
                                  NULL));
}

TEST(test_receipt_gossip_promotes_distinct_receipts_to_route_history) {
    knowledge_view_t source = {0};
    knowledge_view_t heard = {0};
    knowledge_view_configure(&source, 8);
    knowledge_view_configure(&heard, 8);

    market_memory_t receipt = {0};
    ASSERT(market_memory_from_delivery_receipt(1, 3,
                                               COMMODITY_FERRITE_INGOT,
                                               2,
                                               90.0f,
                                               77,
                                               1001,
                                               &receipt));
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&receipt, &item));
    knowledge_view_insert(&source, &item);

    knowledge_view_exchange(&source, &heard);

    market_memory_t reputation = {0};
    ASSERT(view_has_market_memory(&heard,
                                  (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
                                  3, 1,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  &reputation));
    ASSERT_EQ_INT(reputation.action, CONTRACT_TRACTOR);
    ASSERT_EQ_INT(reputation.quantity_hint, 2);
    ASSERT_EQ_INT(reputation.value_hint, 90);

    market_memory_t trust = {0};
    ASSERT(view_has_market_memory(&heard,
                                  (uint8_t)MARKET_MEMORY_STATION_TRUST,
                                  3, 0xff,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  &trust));
    ASSERT_EQ_INT(trust.action, CONTRACT_DELIVERY);
    ASSERT_EQ_INT(trust.quantity_hint, 2);

    knowledge_view_exchange(&source, &heard);
    ASSERT(view_has_market_memory(&heard,
                                  (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
                                  3, 1,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  &reputation));
    ASSERT_EQ_INT(reputation.quantity_hint, 2);
    ASSERT_EQ_INT(reputation.value_hint, 90);

    ASSERT(market_memory_from_delivery_receipt(1, 3,
                                               COMMODITY_FERRITE_INGOT,
                                               3,
                                               120.0f,
                                               82,
                                               1002,
                                               &receipt));
    ASSERT(knowledge_item_from_market_memory(&receipt, &item));
    knowledge_view_insert(&source, &item);

    knowledge_view_exchange(&source, &heard);

    ASSERT(view_has_market_memory(&heard,
                                  (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
                                  3, 1,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  &reputation));
    ASSERT_EQ_INT(reputation.quantity_hint, 5);
    ASSERT_EQ_INT(reputation.value_hint, 210);
    ASSERT(view_has_market_memory(&heard,
                                  (uint8_t)MARKET_MEMORY_STATION_TRUST,
                                  3, 0xff,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  &trust));
    ASSERT_EQ_INT(trust.quantity_hint, 5);
    ASSERT_EQ_INT(trust.value_hint, 210);
}

TEST(test_dock_receipt_gossip_emits_route_history_chain_event) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s_gossip_route_history", TMP("clog"));
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(dir);

    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[3];
    chain_log_reset(st);
    st->chain_event_count = 0;
    memset(st->chain_last_hash, 0, sizeof(st->chain_last_hash));
    memset(&st->knowledge, 0, sizeof(st->knowledge));

    SHIP_DECL(ship);
    knowledge_view_configure(&ship.knowledge, SHIP_KNOWN_ITEM_CAP);

    market_memory_t receipt = {0};
    knowledge_item_t item;
    ASSERT(market_memory_from_delivery_receipt(1, 3,
                                               COMMODITY_FERRITE_INGOT,
                                               2,
                                               90.0f,
                                               77,
                                               2001,
                                               &receipt));
    ASSERT(knowledge_item_from_market_memory(&receipt, &item));
    knowledge_view_insert(&ship.knowledge, &item);

    gossip_dock_handshake(&w, 3, &ship.knowledge);
    ASSERT_EQ_INT((int)st->chain_event_count, 0);

    ASSERT(market_memory_from_delivery_receipt(1, 3,
                                               COMMODITY_FERRITE_INGOT,
                                               3,
                                               120.0f,
                                               82,
                                               2002,
                                               &receipt));
    ASSERT(knowledge_item_from_market_memory(&receipt, &item));
    knowledge_view_insert(&ship.knowledge, &item);

    gossip_dock_handshake(&w, 3, &ship.knowledge);

    ASSERT_EQ_INT((int)st->chain_event_count, 1);
    uint64_t walked = 0;
    ASSERT(chain_log_verify(st, &walked, NULL));
    ASSERT_EQ_INT((int)walked, 1);

    chain_payload_route_history_t payload = {0};
    ASSERT(read_route_history_payload(st, &payload));
    ASSERT_EQ_INT(payload.memory_kind, MARKET_MEMORY_ROUTE_REPUTATION);
    ASSERT_EQ_INT(payload.origin_station, 1);
    ASSERT_EQ_INT(payload.destination_station, 3);
    ASSERT_EQ_INT(payload.commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(payload.action, CONTRACT_TRACTOR);
    ASSERT_EQ_INT(payload.evidence_count, 5);
    ASSERT_EQ_INT(payload.value_hint, 210);

    gossip_dock_handshake(&w, 3, &ship.knowledge);
    ASSERT_EQ_INT((int)st->chain_event_count, 1);

    chain_log_set_dir(NULL);
}

TEST(test_knowledge_view_decay_fades_and_compacts_items) {
    knowledge_view_t view = {0};
    knowledge_view_configure(&view, 4);

    market_memory_t keep = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_SUPPLY,
        .station_a = 0,
        .station_b = 0xff,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = 0xff,
        .confidence = 50,
        .salience = 40,
        .observed_tick = 1,
    };
    market_memory_t drop = keep;
    drop.memory_kind = (uint8_t)MARKET_MEMORY_DEMAND;
    drop.station_a = 2;
    drop.confidence = 5;
    drop.salience = 30;

    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&keep, &item));
    knowledge_view_insert(&view, &item);
    ASSERT(knowledge_item_from_market_memory(&drop, &item));
    knowledge_view_insert(&view, &item);
    ASSERT_EQ_INT(view.count, 2);

    knowledge_view_decay(&view, 10, 15);

    ASSERT_EQ_INT(view.count, 1);
    market_memory_t out = {0};
    ASSERT(market_memory_from_knowledge_item(&view.items[0], &out));
    ASSERT_EQ_INT(out.memory_kind, MARKET_MEMORY_SUPPLY);
    ASSERT_EQ_INT(out.confidence, 40);
    ASSERT_EQ_INT(out.salience, 25);
    ASSERT_EQ_INT(view.items[0].confidence, 40);
    ASSERT_EQ_INT(view.items[0].salience, 25);
}

TEST(test_knowledge_view_forget_contract_drops_market_demand_too) {
    knowledge_view_t view = {0};
    knowledge_view_configure(&view, 8);

    contract_summary_t cs = {
        .active = true,
        .action = (uint8_t)CONTRACT_DELIVERY,
        .station_index = 2,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .quantity_needed = 8.0f,
        .base_price = 32.0f,
    };
    knowledge_item_t item;
    ASSERT(knowledge_item_from_contract_summary(&cs, &item));
    knowledge_view_insert(&view, &item);

    market_memory_t memory;
    ASSERT(market_memory_from_contract_summary(&cs, &memory));
    ASSERT(knowledge_item_from_market_memory(&memory, &item));
    knowledge_view_insert(&view, &item);
    ASSERT_EQ_INT(view.count, 2);

    knowledge_view_forget_contract(&view, (uint8_t)CONTRACT_DELIVERY,
                                   2, COMMODITY_FERRITE_INGOT);

    ASSERT_EQ_INT(view.count, 0);
}

TEST(test_dock_gossip_dual_writes_contract_knowledge) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    memset(&w.stations[0].knowledge, 0, sizeof(w.stations[0].knowledge));

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .required_grade = (uint8_t)MINING_GRADE_COMMON,
        .quantity_needed = 5.0f,
        .base_price = 11.0f,
        .age = 3.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    SHIP_DECL(ship);
    gossip_dock_handshake(&w, 0, &ship.knowledge);

    ASSERT_EQ_INT(knowledge_view_contract_count(&ship.knowledge), 1);
    ASSERT_EQ_INT(knowledge_view_contract_count(&w.stations[0].knowledge), 1);
    ASSERT(view_has_contract(&ship.knowledge, (uint8_t)CONTRACT_TRACTOR, 0,
                             (uint8_t)COMMODITY_FERRITE_INGOT, NULL));
    ASSERT(view_has_contract(&w.stations[0].knowledge,
                             (uint8_t)CONTRACT_TRACTOR, 0,
                             (uint8_t)COMMODITY_FERRITE_INGOT, NULL));
    ASSERT(view_has_market_memory(&ship.knowledge,
                                  (uint8_t)MARKET_MEMORY_DEMAND,
                                  0, 0xff,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  NULL));
    ASSERT(view_has_market_memory(&w.stations[0].knowledge,
                                  (uint8_t)MARKET_MEMORY_DEMAND,
                                  0, 0xff,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  NULL));
}

TEST(test_dock_gossip_emits_stale_contract_risk_memory) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    memset(&w.stations[2].knowledge, 0, sizeof(w.stations[2].knowledge));

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = COMMODITY_FERRITE_INGOT,
        .required_grade = (uint8_t)MINING_GRADE_COMMON,
        .quantity_needed = 5.0f,
        .base_price = 110.0f,
        .age = 650.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    SHIP_DECL(ship);
    gossip_dock_handshake(&w, 2, &ship.knowledge);

    ASSERT_EQ_INT(knowledge_view_contract_count(&ship.knowledge), 1);
    ASSERT(view_has_contract(&ship.knowledge, (uint8_t)CONTRACT_TRACTOR, 2,
                             (uint8_t)COMMODITY_FERRITE_INGOT, NULL));
    market_memory_t demand = {0};
    ASSERT(view_has_market_memory(&ship.knowledge,
                                  (uint8_t)MARKET_MEMORY_DEMAND,
                                  2, 0xff,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  &demand));
    ASSERT(demand.confidence < 235);
    ASSERT(demand.salience < 180);
    market_memory_t risk = {0};
    ASSERT(view_has_market_memory(&w.stations[2].knowledge,
                                  (uint8_t)MARKET_MEMORY_STATION_RISK,
                                  2, 0xff,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  &risk));
    ASSERT_EQ_INT(risk.action, CONTRACT_TRACTOR);
    ASSERT(risk.quantity_hint > 0);
}

TEST(test_dock_gossip_decays_carried_market_memory) {
    WORLD_DECL;
    world_reset(&w);
    signal_field_init(&w.signal_field);
    memset(w.contracts, 0, sizeof(w.contracts));
    memset(&w.stations[0].knowledge, 0, sizeof(w.stations[0].knowledge));

    SHIP_DECL(ship);
    knowledge_view_configure(&ship.knowledge, SHIP_KNOWN_ITEM_CAP);
    market_memory_t memory = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_DANGER,
        .station_a = 0,
        .station_b = 1,
        .commodity = (uint8_t)COMMODITY_COUNT,
        .action = 0xff,
        .confidence = 20,
        .salience = 20,
        .observed_tick = 77,
    };
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&memory, &item));
    knowledge_view_insert(&ship.knowledge, &item);

    gossip_dock_handshake(&w, 0, &ship.knowledge);

    market_memory_t out = {0};
    ASSERT(view_has_market_memory(&ship.knowledge,
                                  (uint8_t)MARKET_MEMORY_ROUTE_DANGER,
                                  0, 1,
                                  (uint8_t)COMMODITY_COUNT,
                                  &out));
    ASSERT_EQ_INT(out.confidence, 19);
    ASSERT_EQ_INT(out.salience, 18);
    ASSERT(view_has_market_memory(&w.stations[0].knowledge,
                                  (uint8_t)MARKET_MEMORY_ROUTE_DANGER,
                                  0, 1,
                                  (uint8_t)COMMODITY_COUNT,
                                  NULL));
    ASSERT(signal_field_query(&w.signal_field, w.stations[0].pos,
                              SIGNAL_FIELD_KIND_RISK, 0) > 0.0f);
}

TEST(test_dock_gossip_marks_signal_field_at_physical_station) {
    WORLD_DECL;
    world_reset(&w);
    signal_field_init(&w.signal_field);
    memset(w.contracts, 0, sizeof(w.contracts));
    memset(&w.stations[0].knowledge, 0, sizeof(w.stations[0].knowledge));

    SHIP_DECL(ship);
    knowledge_view_configure(&ship.knowledge, SHIP_KNOWN_ITEM_CAP);
    market_memory_t receipt = {0};
    knowledge_item_t item;
    ASSERT(market_memory_from_delivery_receipt(1, 2,
                                               COMMODITY_FERRITE_INGOT,
                                               3, 90.0f, 77, 9001,
                                               &receipt));
    ASSERT(knowledge_item_from_market_memory(&receipt, &item));
    knowledge_view_insert(&ship.knowledge, &item);

    gossip_dock_handshake(&w, 0, &ship.knowledge);

    ASSERT(signal_field_query(&w.signal_field, w.stations[0].pos,
                              SIGNAL_FIELD_KIND_PROOF, 0) > 0.0f);
    ASSERT_EQ_FLOAT(signal_field_query(&w.signal_field, w.stations[2].pos,
                                       SIGNAL_FIELD_KIND_PROOF, 0),
                    0.0f, 0.001f);
}

TEST(test_dock_gossip_dual_writes_station_supply_memory) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    memset(&w.stations[0].knowledge, 0, sizeof(w.stations[0].knowledge));

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT, 5));

    SHIP_DECL(ship);
    gossip_dock_handshake(&w, 0, &ship.knowledge);

    market_memory_t supply = {0};
    ASSERT(view_has_market_memory(&w.stations[0].knowledge,
                                  (uint8_t)MARKET_MEMORY_SUPPLY,
                                  0, 0xff,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  &supply));
    ASSERT_EQ_INT(supply.quantity_hint, 5);
    ASSERT(view_has_market_memory(&ship.knowledge,
                                  (uint8_t)MARKET_MEMORY_SUPPLY,
                                  0, 0xff,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  NULL));
}

TEST(test_ship_contact_gossip_exchanges_memory_and_holograms) {
    WORLD_DECL;
    world_reset(&w);
    signal_field_init(&w.signal_field);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    int a_slot = spawn_npc(&w, 0, NPC_ROLE_MINER);
    int b_slot = spawn_npc(&w, 0, NPC_ROLE_MINER);
    ASSERT(a_slot >= 0);
    ASSERT(b_slot >= 0);
    npc_ship_t *a = &w.npc_ships[a_slot];
    npc_ship_t *b = &w.npc_ships[b_slot];

    a->state = NPC_STATE_IDLE;
    b->state = NPC_STATE_IDLE;
    a->ship->pos = v2(100.0f, 100.0f);
    b->ship->pos = v2(220.0f, 100.0f);
    memset(&a->ship->knowledge, 0, sizeof(a->ship->knowledge));
    memset(&b->ship->knowledge, 0, sizeof(b->ship->knowledge));
    knowledge_view_configure(&a->ship->knowledge, SHIP_KNOWN_ITEM_CAP);
    knowledge_view_configure(&b->ship->knowledge, SHIP_KNOWN_ITEM_CAP);
    hnn_memory_init(&a->hnn_market_mem);
    hnn_memory_init(&b->hnn_market_mem);

    contract_summary_t contract = {
        .active = true,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .base_price = 80.0f,
        .age_at_copy = 5.0f,
    };
    ASSERT(test_add_known_contract(&a->ship->knowledge, &contract));

    market_memory_t memory = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_DEMAND,
        .station_a = 2,
        .station_b = 0xffu,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .confidence = 220,
        .salience = 190,
        .quantity_hint = 3,
        .value_hint = 80,
        .observed_tick = 10,
    };
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&memory, &item));
    knowledge_view_insert(&a->ship->knowledge, &item);
    ASSERT(gossip_hnn_store_market_memory(&a->hnn_market_mem, &memory));

    ASSERT_EQ_INT(gossip_ship_contact_exchange(&w), 1);

    ASSERT_EQ_INT(knowledge_view_contract_count(&b->ship->knowledge), 1);
    ASSERT(view_has_market_memory(&b->ship->knowledge,
                                  (uint8_t)MARKET_MEMORY_DEMAND,
                                  2, 0xff,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  NULL));
    ASSERT(b->hnn_market_mem.experience_count > 0);
    ASSERT(gossip_hnn_market_resonance(&b->hnn_market_mem,
                                       &memory,
                                       GOSSIP_HNN_JOB_HAUL) > 0.05f);
    vec2 contact_pos = v2(160.0f, 100.0f);
    ASSERT(signal_field_query(&w.signal_field, contact_pos,
                              SIGNAL_FIELD_KIND_DEMAND, 0) > 0.0f);
    ASSERT(signal_field_query(&w.signal_field, contact_pos,
                              SIGNAL_FIELD_KIND_HOLOGRAM, 0) > 0.0f);
}

static npc_ship_t *test_reset_holographic_pilot(world_t *w, int npc_slot) {
    if (!test_world_npc_slot_reset(w, npc_slot)) return NULL;
    npc_ship_t *npc = &w->npc_ships[npc_slot];
    npc->active = true;
    npc->brain_mode = SERVER_BRAIN_MODE_HOLOGRAPHIC;
    npc->home_station = 0;
    npc->hnn_experience_station = 0xffu;
    npc->hnn_experience_uploaded_station = 0xffu;
    npc->hnn_experience_uploaded_source_station = 0xffu;
    hnn_memory_init(&npc->hnn_mem);
    return npc;
}

static void test_reset_station_hnn_experience(station_t *st) {
    hnn_memory_init(&st->hnn_experience);
    st->hnn_experience_version = 0;
    st->hnn_experience_upload_count = 0;
    st->hnn_experience_download_count = 0;
    st->hnn_experience_last_source_station = 0xffu;
}

static void test_store_hnn_trace(hnn_memory_t *mem,
                                 uint64_t key_seed,
                                 uint64_t value_seed) {
    float key[HNN_DIM];
    float value[HNN_DIM];
    hnn_key_vector(key_seed, key);
    hnn_key_vector(value_seed, value);
    hnn_memory_store(mem, key, value);
}

TEST(test_holographic_pilot_uploads_experience_once) {
    WORLD_DECL;
    world_reset(&w);

    npc_ship_t *npc = test_reset_holographic_pilot(&w, 0);
    ASSERT(npc != NULL);
    test_store_hnn_trace(&npc->hnn_mem, 0x1234u, 0x5678u);
    npc->hnn_experience_local_version++;
    ASSERT_EQ_INT(npc->hnn_mem.experience_count, 1);

    test_reset_station_hnn_experience(&w.stations[0]);
    npc->hnn_experience_version = 0;

    gossip_hnn_exchange(&w, 0, npc);

    ASSERT_EQ_INT(w.stations[0].hnn_experience.experience_count, 1);
    ASSERT_EQ_INT((int)w.stations[0].hnn_experience_version, 1);
    ASSERT_EQ_INT((int)w.stations[0].hnn_experience_upload_count, 1);
    ASSERT_EQ_INT((int)w.stations[0].hnn_experience_download_count, 1);
    ASSERT_EQ_INT((int)w.stations[0].hnn_experience_last_source_station, 0);
    ASSERT_EQ_INT((int)npc->hnn_experience_version, 1);
    ASSERT_EQ_INT((int)npc->hnn_experience_station, 0);
    ASSERT_EQ_INT(npc->hnn_mem.experience_count, 1);

    gossip_hnn_exchange(&w, 0, npc);

    ASSERT_EQ_INT(w.stations[0].hnn_experience.experience_count, 1);
    ASSERT_EQ_INT((int)w.stations[0].hnn_experience_version, 1);
    ASSERT_EQ_INT((int)w.stations[0].hnn_experience_upload_count, 1);
}

TEST(test_holographic_pilot_transports_station_cell) {
    WORLD_DECL;
    world_reset(&w);

    npc_ship_t *npc = test_reset_holographic_pilot(&w, 0);
    ASSERT(npc != NULL);
    test_store_hnn_trace(&npc->hnn_mem, 0x2234u, 0x6678u);
    npc->hnn_experience_local_version++;

    for (int s = 0; s < 2; s++) {
        test_reset_station_hnn_experience(&w.stations[s]);
    }

    gossip_hnn_exchange(&w, 0, npc);
    ASSERT_EQ_INT(w.stations[0].hnn_experience.experience_count, 1);
    ASSERT_EQ_INT((int)npc->hnn_experience_station, 0);
    ASSERT_EQ_INT((int)npc->hnn_experience_version, 1);

    gossip_hnn_exchange(&w, 1, npc);

    ASSERT(w.stations[1].hnn_experience.experience_count > 0);
    ASSERT_EQ_INT((int)w.stations[1].hnn_experience_version, 1);
    ASSERT_EQ_INT((int)w.stations[1].hnn_experience_upload_count, 1);
    ASSERT_EQ_INT((int)w.stations[1].hnn_experience_last_source_station, 0);
    ASSERT_EQ_INT((int)npc->hnn_experience_station, 1);
    ASSERT_EQ_INT((int)npc->hnn_experience_version, 1);
}

TEST(test_holographic_pilot_rejects_unproven_trace_cargo) {
    WORLD_DECL;
    world_reset(&w);

    npc_ship_t *npc = test_reset_holographic_pilot(&w, 0);
    ASSERT(npc != NULL);
    test_store_hnn_trace(&npc->hnn_mem, 0x3234u, 0x7678u);
    npc->hnn_experience_station = 0;
    npc->hnn_experience_version = 2;

    for (int s = 0; s < 2; s++)
        test_reset_station_hnn_experience(&w.stations[s]);
    test_store_hnn_trace(&w.stations[0].hnn_experience, 0x3234u, 0x7678u);
    w.stations[0].hnn_experience_version = 1;

    gossip_hnn_exchange(&w, 1, npc);

    ASSERT_EQ_INT(w.stations[1].hnn_experience.experience_count, 0);
    ASSERT_EQ_INT((int)w.stations[1].hnn_experience_version, 0);
    ASSERT_EQ_INT((int)w.stations[1].hnn_experience_upload_count, 0);
    ASSERT_EQ_INT((int)w.stations[1].hnn_experience_last_source_station, 0xff);
    ASSERT_EQ_INT((int)npc->hnn_experience_station, 0);
    ASSERT_EQ_INT((int)npc->hnn_experience_version, 2);
}

TEST(test_holographic_pilot_rejects_low_fidelity_trace_cargo) {
    WORLD_DECL;
    world_reset(&w);

    npc_ship_t *npc = test_reset_holographic_pilot(&w, 0);
    ASSERT(npc != NULL);
    test_store_hnn_trace(&npc->hnn_mem, 0x4234u, 0x8678u);
    npc->hnn_mem.last_retrieval_similarity = -1.0f;
    npc->hnn_mem.last_margin = 0.0f;
    npc->hnn_experience_station = 0;
    npc->hnn_experience_version = 1;

    for (int s = 0; s < 2; s++)
        test_reset_station_hnn_experience(&w.stations[s]);
    test_store_hnn_trace(&w.stations[0].hnn_experience, 0x4234u, 0x8678u);
    w.stations[0].hnn_experience_version = 1;

    gossip_hnn_exchange(&w, 1, npc);

    ASSERT_EQ_INT(w.stations[1].hnn_experience.experience_count, 0);
    ASSERT_EQ_INT((int)w.stations[1].hnn_experience_version, 0);
    ASSERT_EQ_INT((int)w.stations[1].hnn_experience_upload_count, 0);
    ASSERT_EQ_INT((int)w.stations[1].hnn_experience_last_source_station, 0xff);
    ASSERT_EQ_INT((int)npc->hnn_experience_station, 0);
    ASSERT_EQ_INT((int)npc->hnn_experience_version, 1);
}

TEST(test_bootstrap_seeds_station_local_contracts_only) {
    WORLD_DECL;
    world_reset(&w);
    signal_field_init(&w.signal_field);

    memset(w.contracts, 0, sizeof(w.contracts));
    for (int i = 0; i < MAX_STATIONS; i++) {
        memset(&w.stations[i].knowledge, 0, sizeof(w.stations[i].knowledge));
    }

    w.contracts[0].active = true;
    w.contracts[0].action = CONTRACT_TRACTOR;
    w.contracts[0].station_index = 2;
    w.contracts[0].commodity = COMMODITY_FERRITE_INGOT;
    w.contracts[0].quantity_needed = 4.0f;
    w.contracts[0].base_price = 80.0f;

    gossip_bootstrap_world_stations(&w);

    contract_summary_t station_contracts[STATION_KNOWN_ITEM_CAP];
    ASSERT_EQ_INT(test_known_contracts(&w.stations[2].knowledge,
                                       station_contracts,
                                       STATION_KNOWN_ITEM_CAP), 1);
    ASSERT_EQ_INT(station_contracts[0].station_index, 2);
    ASSERT_EQ_INT(station_contracts[0].commodity, COMMODITY_FERRITE_INGOT);
    ASSERT(view_has_contract(&w.stations[2].knowledge,
                             (uint8_t)CONTRACT_TRACTOR,
                             2,
                             (uint8_t)COMMODITY_FERRITE_INGOT,
                             NULL));
    ASSERT(signal_field_query(&w.signal_field, w.stations[2].pos,
                              SIGNAL_FIELD_KIND_DEMAND, 0) > 0.0f);

    ASSERT_EQ_INT(knowledge_view_contract_count(&w.stations[0].knowledge), 0);
    ASSERT(!view_has_contract(&w.stations[0].knowledge,
                              (uint8_t)CONTRACT_TRACTOR,
                              2,
                              (uint8_t)COMMODITY_FERRITE_INGOT,
                              NULL));
}

void register_gossip_tests(void);
void register_gossip_tests(void) {
    TEST_SECTION("\nGossip knowledge:\n");
    RUN(test_knowledge_contract_summary_adapter_round_trips);
    RUN(test_knowledge_view_contract_dedup_keeps_newer_snapshot);
    RUN(test_market_memory_adapter_round_trips);
    RUN(test_contract_summary_creates_market_demand_memory);
    RUN(test_stale_contract_summary_dampens_market_demand_memory);
    RUN(test_delivery_receipt_memory_adapter_sets_route_and_value);
    RUN(test_hnn_market_memory_resonance_round_trips);
    RUN(test_signal_intelligence_contract_reason_records_memory_pressure);
    RUN(test_signal_intelligence_worker_reason_records_route_risk);
    RUN(test_hnn_core_key_cache_preserves_bind_round_trip);
    RUN(test_hnn_state_encoding_is_deterministic_and_nonzero);
    RUN(test_hnn_state_encoding_preserves_continuous_magnitude);
    RUN(test_hnn_state_encoding_defines_zero_sign_and_clipping);
    RUN(test_hnn_magnitude_sensitive_action_retrieval);
    RUN(test_hnn_capacity_preserves_action_signal_through_limit);
    RUN(test_hnn_memory_contract_reports_trace_diagnostics);
    RUN(test_hnn_backend_metadata_records_builtin_and_liblecore_pin);
    RUN(test_hnn_holonet_single_cell_matches_flat_trace);
    RUN(test_hnn_holonet_routes_novel_states_to_distinct_cells);
    RUN(test_hnn_state_encoding_sanitizes_nonfinite_features);
    RUN(test_hnn_reentrant_key_state_and_simulation);
    RUN(test_hnn_market_memory_maps_specialized_jobs);
    RUN(test_route_reputation_memory_reinforces_repeated_evidence);
    RUN(test_station_trust_memory_reinforces_repeated_evidence);
    RUN(test_station_risk_memory_reinforces_repeated_evidence);
    RUN(test_knowledge_view_market_dedup_keeps_fresher_observation);
    RUN(test_knowledge_exchange_copies_bounded_items_bidirectionally);
    RUN(test_knowledge_exchange_copies_market_memories_bidirectionally);
    RUN(test_receipt_gossip_promotes_distinct_receipts_to_route_history);
    RUN(test_dock_receipt_gossip_emits_route_history_chain_event);
    RUN(test_knowledge_view_decay_fades_and_compacts_items);
    RUN(test_knowledge_view_forget_contract_drops_market_demand_too);
    RUN(test_dock_gossip_dual_writes_contract_knowledge);
    RUN(test_dock_gossip_emits_stale_contract_risk_memory);
    RUN(test_dock_gossip_decays_carried_market_memory);
    RUN(test_dock_gossip_marks_signal_field_at_physical_station);
    RUN(test_dock_gossip_dual_writes_station_supply_memory);
    RUN(test_ship_contact_gossip_exchanges_memory_and_holograms);
    RUN(test_holographic_pilot_uploads_experience_once);
    RUN(test_holographic_pilot_transports_station_cell);
    RUN(test_holographic_pilot_rejects_unproven_trace_cargo);
    RUN(test_holographic_pilot_rejects_low_fidelity_trace_cargo);
    RUN(test_bootstrap_seeds_station_local_contracts_only);
}
