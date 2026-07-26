#include "test_harness.h"

#include "signal_contract_brain.h"
#include "signal_npc_worker_brain.h"

static signal_npc_worker_candidate_t worker_candidate(void) {
    return (signal_npc_worker_candidate_t){
        .option = SIGNAL_NPC_WORKER_OPTION_HAUL_CONTRACT,
        .role = NPC_ROLE_HAULER,
        .home_station = 0,
        .mining_level = 2,
        .hold_level = 4,
        .tractor_level = 3,
        .desired_upgrade = SHIP_UPGRADE_HOLD,
        .desired_commodity = COMMODITY_FRAME,
        .desired_units = 12,
        .home_balance = 750.0f,
        .home_refit_stock = 8.0f,
        .remote_refit_stock = 16.0f,
        .refit_cost = 400.0f,
        .best_contract_value = 240.0f,
        .best_contract_stock = 18.0f,
        .best_contract_dest = 2,
        .best_contract_commodity = COMMODITY_FERRITE_INGOT,
        .mine_pressure = true,
        .frontier_pressure = 0.7f,
        .route_success_memory = 0.8f,
        .route_danger_memory = 0.2f,
        .route_proof_memory = 0.6f,
        .hologram_resonance = 0.5f,
        .source_memory = 0.4f,
        .provenance_pressure = 0.3f,
        .trust_bias = -0.2f,
        .black_market_acceptance = 0.1f,
        .escort_bonus = 0.25f,
        .convoy_bonus = 0.15f,
        .persona_risk = 0.35f,
        .persona_growth = 0.75f,
        .persona_patience = 0.45f,
        .route_km = 12.0f,
        .home_has_dock = true,
        .home_has_shipyard = true,
        .home_has_furnace = true,
        .home_has_frame_press = true,
        .legal = true,
        .travel = true,
        .frontier_supply = true,
        .credit_delta = 300.0f,
        .refit_progress = 0.4f,
        .contract_value = 240.0f,
        .cargo_moved = 6.0f,
        .teacher_score = 0.9f,
    };
}

static signal_contract_candidate_t contract_candidate(void) {
    return (signal_contract_candidate_t){
        .action = SIGNAL_CONTRACT_ACTION_BUY_AND_DELIVER,
        .source_station = 0,
        .dest_station = 2,
        .commodity = COMMODITY_FRAME,
        .quantity_needed = 5.0f,
        .contract_price = 80.0f,
        .source_price = 30.0f,
        .source_stock = 12.0f,
        .dest_stock = 2.0f,
        .ledger_balance = 500.0f,
        .free_cargo = 10.0f,
        .distance = 1200.0f,
        .age = 24.0f,
        .hull_ratio = 0.8f,
        .hologram_resonance = 0.5f,
        .source_memory = 0.4f,
        .route_success_memory = 0.7f,
        .route_danger_memory = 0.2f,
        .route_proof_memory = 0.6f,
        .trust_bias = 0.1f,
        .source_memory_id = 42,
        .teacher_score = 15.0f,
    };
}

static void add_test_module(station_t *station, module_type_t type) {
    station->modules[station->module_count++] =
        (station_module_t){.type = type};
}

TEST(test_worker_features_are_slot_permutation_invariant) {
    signal_npc_worker_candidate_t original = worker_candidate();
    signal_npc_worker_candidate_t permuted = original;
    permuted.home_station = 7;
    permuted.best_contract_dest = 5;

    float a[SIGNAL_NPC_WORKER_FEATURE_COUNT];
    float b[SIGNAL_NPC_WORKER_FEATURE_COUNT];
    ASSERT(signal_npc_worker_build_features(&original, a));
    ASSERT(signal_npc_worker_build_features(&permuted, b));
    for (int i = 0; i < SIGNAL_NPC_WORKER_FEATURE_COUNT; i++)
        ASSERT_EQ_FLOAT(a[i], b[i], 0.0f);
}

TEST(test_worker_features_distinguish_every_option) {
    signal_npc_worker_candidate_t candidate = worker_candidate();
    float rows[SIGNAL_NPC_WORKER_OPTION_COUNT]
              [SIGNAL_NPC_WORKER_FEATURE_COUNT];

    for (int option = 0; option < SIGNAL_NPC_WORKER_OPTION_COUNT; option++) {
        candidate.option = (signal_npc_worker_option_t)option;
        ASSERT(signal_npc_worker_build_features(&candidate, rows[option]));
        for (int bit = 0; bit < SIGNAL_NPC_WORKER_OPTION_COUNT; bit++) {
            ASSERT_EQ_FLOAT(
                rows[option]
                    [SIGNAL_NPC_WORKER_FEATURE_OPTION_ONE_HOT_BEGIN + bit],
                option == bit ? 1.0f : 0.0f, 0.0f);
        }
        for (int feature = 0; feature < SIGNAL_NPC_WORKER_FEATURE_COUNT;
             feature++) {
            ASSERT(isfinite(rows[option][feature]));
            ASSERT(rows[option][feature] >= -1.0f);
            ASSERT(rows[option][feature] <= 2.0f);
        }
    }

    for (int left = 0; left < SIGNAL_NPC_WORKER_OPTION_COUNT; left++) {
        for (int right = left + 1; right < SIGNAL_NPC_WORKER_OPTION_COUNT;
             right++) {
            ASSERT(memcmp(rows[left], rows[right], sizeof(rows[left])) != 0);
        }
    }
}

TEST(test_worker_features_reject_invalid_options_and_nonfinite_values) {
    signal_npc_worker_candidate_t candidate = worker_candidate();
    float row[SIGNAL_NPC_WORKER_FEATURE_COUNT];
    candidate.option = SIGNAL_NPC_WORKER_OPTION_COUNT;
    ASSERT(!signal_npc_worker_build_features(&candidate, row));

    candidate = worker_candidate();
    candidate.home_balance = NAN;
    candidate.route_km = INFINITY;
    candidate.trust_bias = -INFINITY;
    ASSERT(signal_npc_worker_build_features(&candidate, row));
    for (int i = 0; i < SIGNAL_NPC_WORKER_FEATURE_COUNT; i++)
        ASSERT(isfinite(row[i]));
}

TEST(test_contract_features_are_slot_permutation_invariant) {
    WORLD_DECL;
    SERVER_PLAYER_DECL(player);
    w.time = 123.0f;
    w.tick = 456;

    station_t *source = &w.stations[0];
    source->signal_range = 100.0f;
    add_test_module(source, MODULE_DOCK);
    add_test_module(source, MODULE_FRAME_PRESS);

    station_t *dest = &w.stations[2];
    dest->signal_range = 100.0f;
    add_test_module(dest, MODULE_DOCK);
    add_test_module(dest, MODULE_SHIPYARD);

    signal_contract_candidate_t candidate = contract_candidate();
    float before[SIGNAL_CONTRACT_FEATURE_COUNT];
    float after[SIGNAL_CONTRACT_FEATURE_COUNT];
    ASSERT(signal_contract_build_features(&w, &player, &candidate, before));

    w.stations[7] = w.stations[0];
    w.stations[5] = w.stations[2];
    memset(&w.stations[0], 0, sizeof(w.stations[0]));
    memset(&w.stations[2], 0, sizeof(w.stations[2]));
    candidate.source_station = 7;
    candidate.dest_station = 5;
    ASSERT(signal_contract_build_features(&w, &player, &candidate, after));

    for (int i = 0; i < SIGNAL_CONTRACT_FEATURE_COUNT; i++) {
        ASSERT(isfinite(before[i]));
        ASSERT(before[i] >= -1.0f);
        ASSERT(before[i] <= 1.0f);
        ASSERT_EQ_FLOAT(before[i], after[i], 0.0f);
    }
}

TEST(test_contract_features_distinguish_every_action) {
    WORLD_DECL;
    SERVER_PLAYER_DECL(player);
    signal_contract_candidate_t candidate = contract_candidate();
    float row[SIGNAL_CONTRACT_FEATURE_COUNT];

    for (int action = 0; action < SIGNAL_CONTRACT_ACTION_COUNT; action++) {
        candidate.action = (signal_contract_action_t)action;
        ASSERT(signal_contract_build_features(&w, &player, &candidate, row));
        for (int bit = 0; bit < SIGNAL_CONTRACT_ACTION_COUNT; bit++) {
            ASSERT_EQ_FLOAT(
                row[SIGNAL_CONTRACT_FEATURE_ACTION_ONE_HOT_BEGIN + bit],
                action == bit ? 1.0f : 0.0f, 0.0f);
        }
    }

    candidate.action = SIGNAL_CONTRACT_ACTION_COUNT;
    ASSERT(!signal_contract_build_features(&w, &player, &candidate, row));
}

static bool checkpoint_write_u32(FILE *fp, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 24),
    };
    return fwrite(bytes, 1, sizeof(bytes), fp) == sizeof(bytes);
}

static bool checkpoint_write_u64(FILE *fp, uint64_t value) {
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++)
        bytes[i] = (uint8_t)(value >> (unsigned)(i * 8));
    return fwrite(bytes, 1, sizeof(bytes), fp) == sizeof(bytes);
}

static bool write_zeros(FILE *fp, size_t count) {
    static const uint8_t zeros[256] = {0};
    while (count > 0) {
        size_t chunk = count < sizeof(zeros) ? count : sizeof(zeros);
        if (fwrite(zeros, 1, chunk, fp) != chunk) return false;
        count -= chunk;
    }
    return true;
}

static bool write_checkpoint(const char *path,
                             uint64_t input_size,
                             uint32_t encoder_version,
                             const char *feature_set,
                             bool complete) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;

    const uint64_t sizes[4] = {input_size, 32, 16, 1};
    bool ok = fwrite("NNCKPT01", 1, 8, fp) == 8 &&
              checkpoint_write_u32(fp, 1) &&
              checkpoint_write_u32(fp, 1) &&
              checkpoint_write_u64(fp, 4);
    for (int i = 0; ok && i < 4; i++)
        ok = checkpoint_write_u64(fp, sizes[i]);
    if (!complete) {
        fclose(fp);
        return ok;
    }

    ok = ok &&
         checkpoint_write_u32(fp, 1) &&
         checkpoint_write_u32(fp, 3) &&
         checkpoint_write_u32(fp, 0);
    for (int layer = 1; ok && layer < 4; layer++) {
        uint64_t scalars = sizes[layer - 1] * sizes[layer] + sizes[layer];
        ok = write_zeros(fp, (size_t)scalars * sizeof(float));
    }
    ok = ok &&
         checkpoint_write_u64(fp, 487) &&
         checkpoint_write_u32(fp, encoder_version) &&
         write_zeros(fp, 65u * 3u + 256u);

    char encoded_set[32] = {0};
    if (feature_set) snprintf(encoded_set, sizeof(encoded_set), "%s", feature_set);
    ok = ok && fwrite(encoded_set, 1, sizeof(encoded_set), fp) ==
                   sizeof(encoded_set);
    fclose(fp);
    return ok;
}

TEST(test_legacy_ai_checkpoints_fail_closed_with_actionable_errors) {
    const char *worker_v1_path = "signal-test-worker-v1.nnckpt";
    const char *worker_old_v2_path = "signal-test-worker-old-v2.nnckpt";
    const char *contract_v1_path = "signal-test-contract-v1.nnckpt";
    char err[256];

    ASSERT(write_checkpoint(worker_v1_path, 56, 1,
                            "signal-npc-worker-v1", false));
    ASSERT(!signal_npc_worker_brain_load_checkpoint(
        worker_v1_path, err, sizeof(err)));
    ASSERT(strstr(err, "expected 78") != NULL);
    ASSERT(strstr(err, "retrained") != NULL);
    ASSERT_EQ_INT(remove(worker_v1_path), 0);

    ASSERT(write_checkpoint(worker_old_v2_path, 78, 1,
                            "signal-npc-worker-v2", true));
    ASSERT(!signal_npc_worker_brain_load_checkpoint(
        worker_old_v2_path, err, sizeof(err)));
    ASSERT(strstr(err, "encoder 1") != NULL);
    ASSERT(strstr(err, "encoder 2") != NULL);
    ASSERT_EQ_INT(remove(worker_old_v2_path), 0);

    ASSERT(write_checkpoint(contract_v1_path, 40, 1,
                            "signal-contract-live-v1", true));
    ASSERT(!signal_contract_brain_load_checkpoint(
        contract_v1_path, err, sizeof(err)));
    ASSERT(strstr(err, "signal-contract-live-v1") != NULL);
    ASSERT(strstr(err, "signal-contract-live-v2") != NULL);
    ASSERT_EQ_INT(remove(contract_v1_path), 0);
}

TEST(test_v2_checkpoints_score_every_declared_option) {
    const char *worker_path = "signal-test-worker-v2.nnckpt";
    const char *contract_path = "signal-test-contract-v2.nnckpt";
    char err[256];

    ASSERT(write_checkpoint(worker_path, SIGNAL_NPC_WORKER_FEATURE_COUNT,
                            SIGNAL_NPC_WORKER_FEATURE_ENCODER_VERSION,
                            SIGNAL_NPC_WORKER_FEATURE_SET, true));
    ASSERT(signal_npc_worker_brain_load_checkpoint(
        worker_path, err, sizeof(err)));

    signal_npc_worker_candidate_t
        workers[SIGNAL_NPC_WORKER_OPTION_COUNT];
    double worker_scores[SIGNAL_NPC_WORKER_OPTION_COUNT];
    for (int option = 0; option < SIGNAL_NPC_WORKER_OPTION_COUNT; option++) {
        workers[option] = worker_candidate();
        workers[option].option = (signal_npc_worker_option_t)option;
    }
    uint64_t worker_inferences =
        signal_npc_worker_brain_inference_count();
    ASSERT_EQ_INT(signal_npc_worker_brain_choose_with_scores(
                      workers, SIGNAL_NPC_WORKER_OPTION_COUNT,
                      worker_scores, SIGNAL_NPC_WORKER_OPTION_COUNT),
                  0);
    ASSERT(signal_npc_worker_brain_inference_count() - worker_inferences ==
           SIGNAL_NPC_WORKER_OPTION_COUNT);
    for (int option = 0; option < SIGNAL_NPC_WORKER_OPTION_COUNT; option++)
        ASSERT(isfinite(worker_scores[option]));
    ASSERT_EQ_INT(remove(worker_path), 0);

    ASSERT(write_checkpoint(contract_path, SIGNAL_CONTRACT_FEATURE_COUNT,
                            SIGNAL_CONTRACT_FEATURE_ENCODER_VERSION,
                            SIGNAL_CONTRACT_FEATURE_SET, true));
    ASSERT(signal_contract_brain_load_checkpoint(
        contract_path, err, sizeof(err)));

    WORLD_DECL;
    SERVER_PLAYER_DECL(player);
    signal_contract_candidate_t contracts[SIGNAL_CONTRACT_ACTION_COUNT];
    double contract_scores[SIGNAL_CONTRACT_ACTION_COUNT];
    for (int action = 0; action < SIGNAL_CONTRACT_ACTION_COUNT; action++) {
        contracts[action] = contract_candidate();
        contracts[action].action = (signal_contract_action_t)action;
    }
    uint64_t contract_inferences =
        signal_contract_brain_inference_count();
    ASSERT_EQ_INT(signal_contract_brain_choose_with_scores(
                      &w, &player, contracts, SIGNAL_CONTRACT_ACTION_COUNT,
                      contract_scores, SIGNAL_CONTRACT_ACTION_COUNT),
                  0);
    ASSERT(signal_contract_brain_inference_count() - contract_inferences ==
           SIGNAL_CONTRACT_ACTION_COUNT);
    for (int action = 0; action < SIGNAL_CONTRACT_ACTION_COUNT; action++)
        ASSERT(isfinite(contract_scores[action]));
    ASSERT_EQ_INT(remove(contract_path), 0);
}

void register_ai_feature_contract_tests(void) {
    TEST_SECTION("\n[ai_feature_contract]\n");
    RUN(test_worker_features_are_slot_permutation_invariant);
    RUN(test_worker_features_distinguish_every_option);
    RUN(test_worker_features_reject_invalid_options_and_nonfinite_values);
    RUN(test_contract_features_are_slot_permutation_invariant);
    RUN(test_contract_features_distinguish_every_action);
    RUN(test_legacy_ai_checkpoints_fail_closed_with_actionable_errors);
    RUN(test_v2_checkpoints_score_every_declared_option);
}
