#include "test_harness.h"

#include <errno.h>
#include <limits.h>

/* Already-extracted subsystem registries */
void register_commodity_tests(void);
void register_math_tests(void);
void register_ship_tests(void);
void register_client_log_tests(void);
void register_episode_lifecycle_tests(void);
void register_story_loop_tests(void);
void register_local_authority_tests(void);
void register_legacy_recovery_ui_tests(void);
void register_cell_geometry_tests(void);
void register_cargo_package_tests(void);
void register_cell_stress_tests(void);
void register_manifest_tests(void);

/* Subsystem registries from tests/test_*.c */
void register_economy_basic_tests(void);
void register_world_sim_basic_tests(void);
void register_bug_regression_batch1_tests(void);
void register_protocol_main_tests(void);
void register_actor_principal_tests(void);
void register_public_actor_id_tests(void);
void register_public_actor_resolver_tests(void);
void register_public_actor_presentation_tests(void);
void register_contract_ownership_tests(void);
void register_station_catalog_tests(void);
void register_ownership_quarantine_tests(void);
void register_player_identity_registry_tests(void);
void register_cargo_legacy_classify_tests(void);
void register_cargo_legacy_inventory_tests(void);
void register_cargo_craft_provenance_tests(void);
void register_cargo_smelt_provenance_tests(void);
void register_ship_birth_reservation_tests(void);
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
void register_persistence_generation_tests(void);
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
void register_cargo_lineage_tests(void);
void register_trade_paging_tests(void);
void register_pvp_rocks_tests(void);
void register_crypto_tests(void);
void register_identity_tests(void);
void register_registry_tests(void);
void register_signed_action_tests(void);
void register_save_keyed_by_pubkey_tests(void);
void register_station_authority_tests(void);
void register_rx_envelope_tests(void);
void register_chain_log_tests(void);
void register_highscore_replay_tests(void);
void register_signal_verify_tests(void);
void register_cross_station_settlement_tests(void);
void register_sovereign_ledger_tests(void);
void register_prefix_class_pricing_tests(void);
void register_furnace_color_tests(void);
void register_respawn_fee_tests(void);
void register_relationship_tests(void);
void register_tractor_tests(void);
void register_laser_tests(void);
void register_inspect_anim_tests(void);
void register_inspect_label_tests(void);
void register_route_history_label_tests(void);
void register_rock_usefulness_tests(void);
void register_settlement_engine_tests(void);
void register_signal_field_tests(void);
void register_state_digest_tests(void);
void register_reconciliation_diagnostics_tests(void);
void register_gameplay_observability_tests(void);
void register_asteroid_presentation_tests(void);
void register_tow_presentation_diagnostics_tests(void);
void register_hnn_backend_tests(void);
void register_hnn_confidence_tests(void);
void register_gossip_tests(void);
void register_npc_radio_tests(void);
void register_ai_feature_contract_tests(void);
void register_hud_attention_tests(void);
void register_ws_outbox_tests(void);

static int parse_shard_arg(const char *arg, int *out_index, int *out_total) {
    char *slash = NULL;
    char *tail = NULL;
    errno = 0;
    long k = strtol(arg, &slash, 10);
    if (errno != 0 || slash == arg || *slash != '/') return 0;
    errno = 0;
    long n = strtol(slash + 1, &tail, 10);
    if (errno != 0 || tail == slash + 1 || *tail != '\0') return 0;
    if (n <= 0 || k < 0 || k >= n) return 0;
    if (k > INT_MAX || n > INT_MAX) return 0;
    *out_index = (int)k;
    *out_total = (int)n;
    return 1;
}

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
            if (parse_shard_arg(argv[i] + 8, &k, &n)) {
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
    register_client_log_tests();
    register_episode_lifecycle_tests();
    register_story_loop_tests();
    register_local_authority_tests();
    register_legacy_recovery_ui_tests();
    register_cell_geometry_tests();
    register_cargo_package_tests();
    register_cell_stress_tests();
    register_economy_basic_tests();
    register_manifest_tests();
    register_world_sim_basic_tests();
    register_bug_regression_batch1_tests();
    register_protocol_main_tests();
    register_actor_principal_tests();
    register_public_actor_id_tests();
    register_public_actor_resolver_tests();
    register_public_actor_presentation_tests();
    register_contract_ownership_tests();
    register_station_catalog_tests();
    register_ownership_quarantine_tests();
    register_player_identity_registry_tests();
    register_cargo_legacy_classify_tests();
    register_cargo_legacy_inventory_tests();
    register_cargo_craft_provenance_tests();
    register_cargo_smelt_provenance_tests();
    register_ship_birth_reservation_tests();
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
    register_persistence_generation_tests();
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
    register_cargo_lineage_tests();
    register_trade_paging_tests();
    register_pvp_rocks_tests();
    register_crypto_tests();
    register_identity_tests();
    register_registry_tests();
    register_signed_action_tests();
    register_save_keyed_by_pubkey_tests();
    register_station_authority_tests();
    register_rx_envelope_tests();
    register_chain_log_tests();
    register_highscore_replay_tests();
    register_signal_verify_tests();
    register_cross_station_settlement_tests();
    register_sovereign_ledger_tests();
    register_prefix_class_pricing_tests();
    register_furnace_color_tests();
    register_respawn_fee_tests();
    register_relationship_tests();
    register_tractor_tests();
    register_laser_tests();
    register_inspect_anim_tests();
    register_inspect_label_tests();
    register_route_history_label_tests();
    register_rock_usefulness_tests();
    register_settlement_engine_tests();
    register_signal_field_tests();
    register_state_digest_tests();
    register_reconciliation_diagnostics_tests();
    register_gameplay_observability_tests();
    register_asteroid_presentation_tests();
    register_tow_presentation_diagnostics_tests();
    register_hnn_backend_tests();
    register_hnn_confidence_tests();
    register_gossip_tests();
    register_npc_radio_tests();
    register_ai_feature_contract_tests();
    register_hud_attention_tests();
    register_ws_outbox_tests();

    printf("\n%d tests run, %d passed, %d failed", tests_run, tests_passed, tests_failed);
    if (g_warnings > 0) printf(", %d warnings", g_warnings);
    printf("\n");
    return tests_failed > 0 ? 1 : 0;
}
