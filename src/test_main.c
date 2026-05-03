#include "tests/test_harness.h"

/* Already-extracted subsystem registries */
void register_commodity_tests(void);
void register_math_tests(void);
void register_ship_tests(void);
void register_manifest_tests(void);

/* Subsystem registries from tests/test_*.c */
void register_economy_basic_tests(void);
void register_world_sim_basic_tests(void);
void register_bug_regression_batch1_tests(void);
void register_protocol_main_tests(void);
void register_bug_regression_batch2_tests(void);
void register_bug_regression_batch3_tests(void);
void register_bug_regression_batch4_tests(void);
void register_bug_regression_batch5_tests(void);
void register_world_sim_scenarios_tests(void);
void register_bug_regression_batch7_tests(void);
void register_world_sim_signal_tests(void);
void register_bug_regression_batch6_tests(void);
void register_construction_outposts_tests(void);
void register_bug_regression_b88_90_tests(void);
void register_economy_contracts_tests(void);
void register_save_persistence_tests(void);
void register_save_format_tests(void);
void register_construction_modules_tests(void);
void register_economy_contract3_tests(void);
void register_economy_pricing_tests(void);
void register_world_sim_belt_tests(void);
void register_world_sim_chunk_tests(void);
void register_anchor_tests(void);
void register_economy_mixed_cargo_tests(void);
void register_economy_service259_tests(void);
void register_economy_refinery_smelt_tests(void);
void register_economy_demand_tests(void);
void register_navigation_autopilot_mining_tests(void);
void register_construction_collision238_tests(void);
void register_construction_station_geom_tests(void);
void register_construction_scaffold_tests(void);
void register_construction_placed_scaffold_tests(void);
void register_construction_module_schema_tests(void);
void register_navigation_nav_tests(void);
void register_navigation_autopilot_stress_tests(void);
void register_econ_sim_sim_tests(void);
void register_econ_sim_bug312_tests(void);
void register_econ_sim_invariant_tests(void);
void register_asteroid_tests(void);
void register_signal_chain_tests(void);
void register_label_tests(void);
void register_motd_rarity_tests(void);
void register_pvp_rocks_tests(void);
void register_crypto_tests(void);
void register_identity_tests(void);
void register_registry_tests(void);
void register_signed_action_tests(void);
void register_save_keyed_by_pubkey_tests(void);
void register_station_authority_tests(void);
void register_chain_log_tests(void);
void register_signal_verify_tests(void);
void register_cross_station_settlement_tests(void);
void register_sovereign_ledger_tests(void);
void register_prefix_class_pricing_tests(void);
void register_furnace_color_tests(void);
void register_respawn_fee_tests(void);
void register_relationship_tests(void);
void register_tractor_tests(void);
void register_laser_tests(void);

int main(int argc, char **argv) {
    setbuf(stdout, NULL); /* unbuffered so crash location is visible */

    /* --shard=K/N splits the suite across N workers; worker K runs
     * every Nth test starting at index K. Unset = run everything.
     * --quiet suppresses banners + per-test "ok" lines + [WARN] noise;
     * a single FAIL line still prints with full file:line context.
     * --filter=<substr> runs only tests whose name contains <substr>;
     * composes with --shard (filter happens first, so filtered tests
     * don't burn shard slots). */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--shard=", 8) == 0) {
            int k = 0, n = 1;
            if (sscanf(argv[i] + 8, "%d/%d", &k, &n) == 2 && n > 0 && k >= 0 && k < n) {
                g_shard_index = k;
                g_shard_total = n;
                printf("[shard %d/%d] ", k, n);
            }
        } else if (strcmp(argv[i], "--quiet") == 0) {
            g_quiet = 1;
        } else if (strncmp(argv[i], "--filter=", 9) == 0) {
            g_filter = argv[i] + 9;
            if (g_filter[0] == '\0') g_filter = NULL;
        } else if (strcmp(argv[i], "--soak") == 0) {
            g_soak_enabled = 1;
        } else if (strcmp(argv[i], "--soak-only") == 0) {
            g_soak_enabled = 1;
            g_only_soak    = 1;
        } else if (strcmp(argv[i], "--no-soak") == 0) {
            g_soak_enabled = 0;
            g_only_soak    = 0;
        }
    }

    register_commodity_tests();
    register_math_tests();
    register_ship_tests();
    register_economy_basic_tests();
    register_manifest_tests();
    register_world_sim_basic_tests();
    register_bug_regression_batch1_tests();
    register_protocol_main_tests();
    register_bug_regression_batch2_tests();
    register_bug_regression_batch3_tests();
    register_bug_regression_batch4_tests();
    register_bug_regression_batch5_tests();
    register_world_sim_scenarios_tests();
    register_bug_regression_batch7_tests();
    register_world_sim_signal_tests();
    register_bug_regression_batch6_tests();
    register_construction_outposts_tests();
    register_bug_regression_b88_90_tests();
    register_economy_contracts_tests();
    register_save_persistence_tests();
    register_save_format_tests();
    register_construction_modules_tests();
    register_economy_contract3_tests();
    register_economy_pricing_tests();
    register_world_sim_belt_tests();
    register_world_sim_chunk_tests();
    register_anchor_tests();
    register_economy_mixed_cargo_tests();
    register_economy_service259_tests();
    register_economy_refinery_smelt_tests();
    register_economy_demand_tests();
    register_navigation_autopilot_mining_tests();
    register_construction_collision238_tests();
    register_construction_station_geom_tests();
    register_construction_scaffold_tests();
    register_construction_placed_scaffold_tests();
    register_construction_module_schema_tests();
    register_navigation_nav_tests();
    register_navigation_autopilot_stress_tests();
    register_econ_sim_sim_tests();
    register_econ_sim_bug312_tests();
    register_econ_sim_invariant_tests();
    register_asteroid_tests();
    register_signal_chain_tests();
    register_label_tests();
    register_motd_rarity_tests();
    register_pvp_rocks_tests();
    register_crypto_tests();
    register_identity_tests();
    register_registry_tests();
    register_signed_action_tests();
    register_save_keyed_by_pubkey_tests();
    register_station_authority_tests();
    register_chain_log_tests();
    register_signal_verify_tests();
    register_cross_station_settlement_tests();
    register_sovereign_ledger_tests();
    register_prefix_class_pricing_tests();
    register_furnace_color_tests();
    register_respawn_fee_tests();
    register_relationship_tests();
    register_tractor_tests();
    register_laser_tests();

    printf("\n%d tests run, %d passed, %d failed", tests_run, tests_passed, tests_failed);
    if (g_warnings > 0) printf(", %d warnings", g_warnings);
    printf("\n");
    return tests_failed > 0 ? 1 : 0;
}
