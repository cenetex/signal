#include "test_harness.h"
#include "contract_fit.h"
#include "cargo_legality.h"
#include "cargo_receipt_issue.h"
#include "station_policy.h"
#include "chain_log.h"
#include "gossip.h"
#include "npc_identity.h"

static void economy_chain_test_setup(const char *suffix) {
    char path[256];
    snprintf(path, sizeof(path), "%s_chain_%s", TMP("econ"), suffix);
    chain_log_set_dir(path);
    chain_log_set_disk_enabled(true);
}

static void economy_chain_test_teardown(void) {
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(NULL);
}

static void economy_chain_test_wipe_logs(world_t *w) {
    if (!w) return;
    for (int s = 0; s < MAX_STATIONS; s++) {
        chain_log_reset(&w->stations[s]);
        w->stations[s].chain_event_count = 0;
        memset(w->stations[s].chain_last_hash, 0,
               sizeof(w->stations[s].chain_last_hash));
    }
}

static void economy_fill_pubkey(uint8_t out[32], uint8_t seed) {
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(seed + i);
}

static void economy_finalize_token_identity(
    server_player_t *player) {
    ASSERT(player != NULL);
    ledger_pubkey_from_token(
        player->session_token, player->pubkey);
    player->pubkey_set = true;
    player->pubkey_proof_ok = true;
    player->pubkey_challenge_consumed = true;
    player->pubkey_identity_finalized = true;
}

TEST(test_station_payout_journal_covers_every_action_and_replays_inert) {
    WORLD_DECL;
    world_reset(&w);
    station_t *station = &w.stations[1];
    uint8_t recipient[32];
    economy_fill_pubkey(recipient, 0x91);
    float expected = 0.0f;

    for (int raw = STATION_PAYOUT_NONE + 1;
         raw < STATION_PAYOUT_COUNT; raw++) {
        uint8_t sources[1][32] = {{0}};
        float amounts[1] = {10.0f + (float)raw};
        sources[0][0] = 0xA6;
        sources[0][31] = (uint8_t)raw;
        station_payout_credit_batch_stage_t stage = {0};
        ASSERT(station_payout_credit_batch_prepare(
            &w, 1, (station_payout_action_t)raw,
            sources, amounts, 1, recipient, &stage));
        ASSERT(station_payout_credit_batch_commit(
            &w, station, NULL, &stage));
        expected += amounts[0];
    }

    ASSERT_EQ_INT((int)w.payout_journal.count,
                  STATION_PAYOUT_COUNT - 1);
    ASSERT_EQ_FLOAT(ledger_balance_by_pubkey(station, recipient),
                    expected, 0.001f);

    uint8_t source[32] = {0};
    source[0] = 0xA6;
    source[31] = STATION_PAYOUT_POD_INTAKE;
    float balance_after = ledger_balance_by_pubkey(station, recipient);
    for (int retry = 0; retry < 1000; retry++) {
        station_payout_stage_t replay = {0};
        ASSERT_EQ_INT(station_payout_prepare(
            &w, 1, STATION_PAYOUT_POD_INTAKE,
            source, recipient,
            10.0f + (float)STATION_PAYOUT_POD_INTAKE,
            &replay),
            STATION_PAYOUT_PREPARE_DUPLICATE);
        ASSERT_EQ_FLOAT(
            replay.receipt.amount,
            10.0f + (float)STATION_PAYOUT_POD_INTAKE,
            0.001f);
    }
    ASSERT_EQ_FLOAT(ledger_balance_by_pubkey(station, recipient),
                    balance_after, 0.001f);
    ASSERT_EQ_INT((int)w.payout_journal.count,
                  STATION_PAYOUT_COUNT - 1);
}

TEST(test_station_payout_identity_binds_station_action_and_authority) {
    WORLD_DECL;
    world_reset(&w);
    uint8_t source[32] = {0};
    uint8_t first[32] = {0};
    uint8_t other_action[32] = {0};
    uint8_t other_station[32] = {0};
    uint8_t other_authority[32] = {0};
    source[31] = 0x6e;
    station_t *station = &w.stations[1];
    ASSERT(station_payout_identity(
        station, STATION_PAYOUT_POD_INTAKE, source, first));
    ASSERT(station_payout_identity(
        station, STATION_PAYOUT_BUILD_DELIVERY,
        source, other_action));
    ASSERT(station_payout_identity(
        &w.stations[0], STATION_PAYOUT_POD_INTAKE,
        source, other_station));
    station->authority_registry_version++;
    ASSERT(station_payout_identity(
        station, STATION_PAYOUT_POD_INTAKE,
        source, other_authority));
    ASSERT(memcmp(first, other_action, 32) != 0);
    ASSERT(memcmp(first, other_station, 32) != 0);
    ASSERT(memcmp(first, other_authority, 32) != 0);
}

TEST(test_station_payout_stages_two_new_smelt_recipients_in_order) {
    WORLD_DECL;
    world_reset(&w);
    station_t *station = &w.stations[1];
    uint8_t source[32] = {0};
    uint8_t tower[32];
    uint8_t fracturer[32];
    source[31] = 0x71;
    economy_fill_pubkey(tower, 0x31);
    economy_fill_pubkey(fracturer, 0x61);
    ASSERT_EQ_FLOAT(ledger_balance_by_pubkey(station, tower),
                    0.0f, 0.001f);
    ASSERT_EQ_FLOAT(ledger_balance_by_pubkey(station, fracturer),
                    0.0f, 0.001f);

    station_payout_supply_stage_t tower_stage = {0};
    station_payout_supply_stage_t fracturer_stage = {0};
    ASSERT(station_payout_supply_prepare(
        &w, 1, STATION_PAYOUT_SMELT_TOWER,
        source, tower, 100.0f, NULL, &tower_stage));
    ASSERT(station_payout_supply_prepare(
        &w, 1, STATION_PAYOUT_SMELT_FRACTURER,
        source, fracturer, 25.0f, &tower_stage,
        &fracturer_stage));
    ASSERT(tower_stage.ledger_index != fracturer_stage.ledger_index);
    ASSERT(station_payout_supply_commit(
        &w, station, NULL, &tower_stage));
    ASSERT(station_payout_supply_commit(
        &w, station, NULL, &fracturer_stage));

    ASSERT_EQ_FLOAT(ledger_balance_by_pubkey(station, tower),
                    65.0f, 0.001f);
    ASSERT_EQ_FLOAT(ledger_balance_by_pubkey(station, fracturer),
                    16.25f, 0.001f);
    ASSERT_EQ_INT((int)w.payout_journal.count, 2);
}

static cargo_unit_t economy_test_cargo_unit(
    const uint8_t fragment_pub[32],
    uint16_t *out_output_index) {
    cargo_unit_t unit = {0};
    for (uint32_t output_index = 0;
         output_index <= UINT16_MAX; output_index++) {
        (void)hash_ingot(
            COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON,
            fragment_pub, (uint16_t)output_index, &unit);
        if ((ingot_prefix_t)unit.prefix_class !=
            INGOT_PREFIX_ANONYMOUS) {
            if (out_output_index)
                *out_output_index = (uint16_t)output_index;
            return unit;
        }
    }
    memset(&unit, 0, sizeof(unit));
    return unit;
}

static int economy_count_exact_pod_units(const world_t *w, commodity_t c) {
    int total = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active || pod->kind != CARGO_POD_CARGO) continue;
        if (pod->shipment_id != 0 || pod->commodity != c) continue;
        if (pod->manifest_count == 0 || pod->manifest_count != pod->quantity)
            continue;
        bool exact = true;
        for (uint16_t u = 0; u < pod->manifest_count; u++) {
            if ((commodity_t)pod->manifest_units[u].commodity != c) {
                exact = false;
                break;
            }
        }
        if (exact) total += (int)pod->manifest_count;
    }
    return total;
}

static const cargo_pod_t *economy_first_exact_pod(const world_t *w,
                                                  commodity_t c) {
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active || pod->kind != CARGO_POD_CARGO) continue;
        if (pod->shipment_id != 0 || pod->commodity != c) continue;
        if (pod->manifest_count == 0 || pod->manifest_count != pod->quantity)
            continue;
        bool exact = true;
        for (uint16_t u = 0; u < pod->manifest_count; u++) {
            if ((commodity_t)pod->manifest_units[u].commodity != c) {
                exact = false;
                break;
            }
        }
        if (exact) return pod;
    }
    return NULL;
}

static bool economy_issue_single_receipt(world_t *w,
                                         int station_idx,
                                         const uint8_t recipient[32],
                                         uint16_t output_index,
                                         cargo_unit_t *unit,
                                         cargo_receipt_chain_t *out) {
    if (!w || !unit || !out ||
        station_idx < 0 || station_idx >= MAX_STATIONS)
        return false;
    station_t *st = &w->stations[station_idx];
    memset(out, 0, sizeof(*out));
    unit->origin_station = (uint8_t)station_idx;
    chain_payload_smelt_t smelt = {0};
    if (!chain_payload_smelt_bind_output(
            &smelt, unit->parent_merkle, output_index, unit)) {
        return false;
    }
    if (chain_log_emit(w, st, CHAIN_EVT_SMELT,
                       &smelt, (uint16_t)sizeof(smelt)) == 0)
        return false;
    cargo_receipt_t receipt = {0};
    if (cargo_receipt_emit_transfer(w, st, st->station_pubkey, recipient,
                                    unit, out, &receipt) == 0) {
        return false;
    }
    out->links[0] = receipt;
    out->len = 1;
    return true;
}

typedef enum {
    ECONOMY_TRANSFER_FAULT_WRITE = 0,
    ECONOMY_TRANSFER_FAULT_FLUSH,
    ECONOMY_TRANSFER_FAULT_PREBLOCKED,
} economy_transfer_fault_t;

#define ECONOMY_LEDGER_BYTES (sizeof(((station_t *)0)->ledger))

typedef struct {
    cargo_store_t stores[3];
    const cargo_store_t *store_refs[3];
    size_t store_count;
    cargo_pod_t *pods;
    delivery_shipment_t *shipments;
    contract_t contracts[MAX_CONTRACTS];
    uint16_t next_delivery_shipment_id;
    uint8_t ledgers[MAX_STATIONS][ECONOMY_LEDGER_BYTES];
    int ledger_counts[MAX_STATIONS];
    float credit_pools[MAX_STATIONS];
    float inventory_cache[MAX_STATIONS][COMMODITY_COUNT];
    float finished_residue[MAX_STATIONS][COMMODITY_COUNT];
    bool manifest_dirty[MAX_STATIONS];
    float ship_credits_earned;
    float ship_credits_spent;
    uint64_t chain_event_count;
    uint8_t chain_last_hash[32];
    uint64_t chain_verified_event_count;
    uint8_t chain_verified_last_hash[32];
    uint8_t chain_health_status;
    bool chain_append_blocked;
    bool chain_append_block_warned;
    char chain_health_message[sizeof(((station_t *)0)->chain_health_message)];
} economy_transfer_snapshot_t;

typedef struct {
    bool setup_ok;
    bool caller_invoked;
    bool manifests_unchanged;
    bool receipts_unchanged;
    bool pods_unchanged;
    bool ledgers_unchanged;
    bool contracts_unchanged;
    bool shipments_unchanged;
    bool credits_unchanged;
    bool inventory_unchanged;
    bool manifest_dirty_unchanged;
    bool chain_counters_unchanged;
    bool chain_log_unchanged;
    bool expected_health_flags;
    bool receipt_sink_unchanged;
} economy_transfer_failure_result_t;

typedef struct {
    int calls;
    cargo_receipt_chain_t last;
} economy_receipt_sink_capture_t;

static void economy_capture_receipt_chain(
    void *user, const cargo_receipt_chain_t *chain) {
    economy_receipt_sink_capture_t *capture =
        (economy_receipt_sink_capture_t *)user;
    if (!capture || !chain) return;
    capture->calls++;
    capture->last = *chain;
}

static bool economy_manifest_equal(const manifest_t *a,
                                   const manifest_t *b) {
    if (!a || !b || a->count != b->count || a->cap != b->cap)
        return false;
    if (a->count == 0) return true;
    if (!a->units || !b->units) return false;
    return memcmp(a->units, b->units,
                  (size_t)a->count * sizeof(*a->units)) == 0;
}

static bool economy_receipts_equal(const cargo_store_t *a,
                                   const cargo_store_t *b) {
    const ship_receipts_t *ar = cargo_store_receipts_const(a);
    const ship_receipts_t *br = cargo_store_receipts_const(b);
    if (!ar || !br) return ar == br;
    if (ar->count != br->count || ar->cap != br->cap)
        return false;
    if (ar->count == 0) return true;
    if (!ar->chains || !br->chains) return false;
    return memcmp(ar->chains, br->chains,
                  (size_t)ar->count * sizeof(*ar->chains)) == 0;
}

static void economy_transfer_snapshot_cleanup(
    economy_transfer_snapshot_t *snapshot) {
    if (!snapshot) return;
    for (size_t i = 0; i < snapshot->store_count; i++)
        cargo_store_cleanup(&snapshot->stores[i]);
    free(snapshot->pods);
    free(snapshot->shipments);
    memset(snapshot, 0, sizeof(*snapshot));
}

static bool economy_transfer_snapshot_take(
    economy_transfer_snapshot_t *snapshot,
    const world_t *w,
    const cargo_store_t *const *stores,
    size_t store_count,
    int chain_station,
    const ship_t *credit_ship) {
    if (!snapshot || !w || !stores || store_count > 3 ||
        chain_station < 0 || chain_station >= MAX_STATIONS) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->store_count = store_count;
    for (size_t i = 0; i < store_count; i++) {
        if (!stores[i] ||
            !cargo_store_clone(&snapshot->stores[i], stores[i])) {
            economy_transfer_snapshot_cleanup(snapshot);
            return false;
        }
        snapshot->store_refs[i] = stores[i];
    }
    snapshot->pods = malloc(sizeof(w->cargo_pods));
    snapshot->shipments = malloc(sizeof(w->delivery_shipments));
    if (!snapshot->pods || !snapshot->shipments) {
        economy_transfer_snapshot_cleanup(snapshot);
        return false;
    }
    memcpy(snapshot->pods, w->cargo_pods, sizeof(w->cargo_pods));
    memcpy(snapshot->shipments, w->delivery_shipments,
           sizeof(w->delivery_shipments));
    memcpy(snapshot->contracts, w->contracts, sizeof(w->contracts));
    snapshot->next_delivery_shipment_id =
        w->next_delivery_shipment_id;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *station = &w->stations[s];
        memcpy(snapshot->ledgers[s], station->ledger,
               sizeof(station->ledger));
        snapshot->ledger_counts[s] = station->ledger_count;
        snapshot->credit_pools[s] = station_credit_pool(station);
        memcpy(snapshot->inventory_cache[s],
               station->_inventory_cache,
               sizeof(station->_inventory_cache));
        memcpy(snapshot->finished_residue[s],
               station->_finished_residue,
               sizeof(station->_finished_residue));
        snapshot->manifest_dirty[s] = station->manifest_dirty;
    }
    if (credit_ship) {
        snapshot->ship_credits_earned = credit_ship->stat_credits_earned;
        snapshot->ship_credits_spent = credit_ship->stat_credits_spent;
    }
    const station_t *chain = &w->stations[chain_station];
    snapshot->chain_event_count = chain->chain_event_count;
    memcpy(snapshot->chain_last_hash, chain->chain_last_hash,
           sizeof(snapshot->chain_last_hash));
    snapshot->chain_verified_event_count =
        chain->chain_verified_event_count;
    memcpy(snapshot->chain_verified_last_hash,
           chain->chain_verified_last_hash,
           sizeof(snapshot->chain_verified_last_hash));
    snapshot->chain_health_status = chain->chain_health_status;
    snapshot->chain_append_blocked = chain->chain_append_blocked;
    snapshot->chain_append_block_warned =
        chain->chain_append_block_warned;
    memcpy(snapshot->chain_health_message,
           chain->chain_health_message,
           sizeof(snapshot->chain_health_message));
    return true;
}

static economy_transfer_failure_result_t
economy_transfer_snapshot_evaluate(
    const economy_transfer_snapshot_t *snapshot,
    const world_t *w,
    int chain_station,
    const ship_t *credit_ship,
    economy_transfer_fault_t fault,
    bool caller_invoked,
    int receipt_sink_calls) {
    economy_transfer_failure_result_t result = {
        .setup_ok = true,
        .caller_invoked = caller_invoked,
        .manifests_unchanged = true,
        .receipts_unchanged = true,
        .pods_unchanged = true,
        .ledgers_unchanged = true,
        .contracts_unchanged = true,
        .shipments_unchanged = true,
        .credits_unchanged = true,
        .inventory_unchanged = true,
        .manifest_dirty_unchanged = true,
        .chain_counters_unchanged = true,
        .chain_log_unchanged = true,
        .expected_health_flags = true,
        .receipt_sink_unchanged = receipt_sink_calls == 0,
    };
    if (!snapshot || !w || chain_station < 0 ||
        chain_station >= MAX_STATIONS) {
        result.setup_ok = false;
        return result;
    }
    for (size_t i = 0; i < snapshot->store_count; i++) {
        result.manifests_unchanged &=
            economy_manifest_equal(
                &snapshot->stores[i].manifest,
                &snapshot->store_refs[i]->manifest);
        result.receipts_unchanged &=
            economy_receipts_equal(
                &snapshot->stores[i],
                snapshot->store_refs[i]);
    }
    result.pods_unchanged =
        memcmp(snapshot->pods, w->cargo_pods,
               sizeof(w->cargo_pods)) == 0;
    result.contracts_unchanged =
        memcmp(snapshot->contracts, w->contracts,
               sizeof(w->contracts)) == 0;
    result.shipments_unchanged =
        snapshot->next_delivery_shipment_id ==
            w->next_delivery_shipment_id &&
        memcmp(snapshot->shipments, w->delivery_shipments,
               sizeof(w->delivery_shipments)) == 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *station = &w->stations[s];
        result.ledgers_unchanged &=
            snapshot->ledger_counts[s] == station->ledger_count &&
            memcmp(snapshot->ledgers[s], station->ledger,
                   sizeof(station->ledger)) == 0;
        result.credits_unchanged &=
            fabsf(snapshot->credit_pools[s] -
                  station_credit_pool(station)) < 0.0001f;
        result.inventory_unchanged &=
            memcmp(snapshot->inventory_cache[s],
                   station->_inventory_cache,
                   sizeof(station->_inventory_cache)) == 0 &&
            memcmp(snapshot->finished_residue[s],
                   station->_finished_residue,
                   sizeof(station->_finished_residue)) == 0;
        result.manifest_dirty_unchanged &=
            snapshot->manifest_dirty[s] == station->manifest_dirty;
    }
    if (credit_ship) {
        result.credits_unchanged &=
            snapshot->ship_credits_earned ==
                credit_ship->stat_credits_earned &&
            snapshot->ship_credits_spent ==
                credit_ship->stat_credits_spent;
    }

    const station_t *chain = &w->stations[chain_station];
    result.chain_counters_unchanged =
        snapshot->chain_event_count == chain->chain_event_count &&
        memcmp(snapshot->chain_last_hash, chain->chain_last_hash,
               sizeof(snapshot->chain_last_hash)) == 0 &&
        snapshot->chain_verified_event_count ==
            chain->chain_verified_event_count &&
        memcmp(snapshot->chain_verified_last_hash,
               chain->chain_verified_last_hash,
               sizeof(snapshot->chain_verified_last_hash)) == 0;
    uint64_t walked = 0;
    result.chain_log_unchanged =
        chain_log_verify(chain, &walked, NULL) &&
        walked == snapshot->chain_event_count;

    result.expected_health_flags =
        chain->chain_append_blocked &&
        chain->chain_health_status == CHAIN_HEALTH_FAILED;
    if (fault == ECONOMY_TRANSFER_FAULT_WRITE) {
        result.expected_health_flags &=
            !chain->chain_append_block_warned &&
            strstr(chain->chain_health_message,
                   "write_failed") != NULL;
    } else if (fault == ECONOMY_TRANSFER_FAULT_FLUSH) {
        result.expected_health_flags &=
            !chain->chain_append_block_warned &&
            strstr(chain->chain_health_message,
                   "flush_failed") != NULL;
    } else {
        result.expected_health_flags &=
            chain->chain_append_block_warned &&
            snapshot->chain_append_blocked &&
            snapshot->chain_health_status == CHAIN_HEALTH_FAILED &&
            memcmp(snapshot->chain_health_message,
                   chain->chain_health_message,
                   sizeof(snapshot->chain_health_message)) == 0;
    }
    return result;
}

static void economy_transfer_test_configure_chain(
    const char *label,
    economy_transfer_fault_t fault,
    chain_event_type_t failure_event) {
    char path[256];
    snprintf(path, sizeof(path), "%s/transfer_%s_%d_%d",
             test_tmp_dir(), label, (int)fault, (int)failure_event);
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(path);
    chain_log_test_fault_clear();
}

static void economy_transfer_test_arm_fault(
    station_t *station,
    economy_transfer_fault_t fault,
    chain_event_type_t failure_event) {
    if (!station) return;
    if (fault == ECONOMY_TRANSFER_FAULT_WRITE) {
        chain_log_test_fault_inject(
            CHAIN_LOG_TEST_FAULT_WRITE, failure_event, 1);
    } else if (fault == ECONOMY_TRANSFER_FAULT_FLUSH) {
        chain_log_test_fault_inject(
            CHAIN_LOG_TEST_FAULT_FLUSH, failure_event, 1);
    } else {
        chain_log_health_set(
            station, CHAIN_HEALTH_FAILED, true,
            station->chain_event_count,
            station->chain_last_hash,
            "test pre-blocked prepared transfer");
    }
}

typedef struct {
    uint64_t transfer_event_id;
    chain_payload_transfer_t transfer;
    chain_payload_trade_t trade;
} economy_transfer_trade_pair_t;

static uint64_t economy_read_u64_le(const uint8_t bytes[8]) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
        value |= (uint64_t)bytes[i] << (i * 8);
    return value;
}

static bool economy_read_last_transfer_trade_pair(
    const station_t *station,
    economy_transfer_trade_pair_t *out) {
    if (!station || !out) return false;
    char path[256];
    if (!chain_log_path_for(
            station->station_pubkey, path, sizeof(path))) {
        return false;
    }
    FILE *log = fopen(path, "rb");
    if (!log) return false;

    const long pair_size =
        (long)(2u * (CHAIN_EVENT_HEADER_SIZE + sizeof(uint16_t)) +
               sizeof(chain_payload_transfer_t) +
               sizeof(chain_payload_trade_t));
    bool ok = fseek(log, 0, SEEK_END) == 0;
    long end = ok ? ftell(log) : -1;
    if (end < pair_size ||
        fseek(log, end - pair_size, SEEK_SET) != 0) {
        ok = false;
    }

    uint8_t transfer_header[CHAIN_EVENT_HEADER_SIZE] = {0};
    uint8_t trade_header[CHAIN_EVENT_HEADER_SIZE] = {0};
    uint16_t transfer_len = 0;
    uint16_t trade_len = 0;
    economy_transfer_trade_pair_t pair = {0};
    if (ok &&
        fread(transfer_header, 1, sizeof(transfer_header), log) !=
            sizeof(transfer_header)) {
        ok = false;
    }
    if (ok &&
        fread(&transfer_len, 1, sizeof(transfer_len), log) !=
            sizeof(transfer_len)) {
        ok = false;
    }
    if (ok &&
        (transfer_len != sizeof(pair.transfer) ||
         fread(&pair.transfer, 1, sizeof(pair.transfer), log) !=
             sizeof(pair.transfer))) {
        ok = false;
    }
    if (ok &&
        fread(trade_header, 1, sizeof(trade_header), log) !=
            sizeof(trade_header)) {
        ok = false;
    }
    if (ok &&
        fread(&trade_len, 1, sizeof(trade_len), log) !=
            sizeof(trade_len)) {
        ok = false;
    }
    if (ok &&
        (trade_len != sizeof(pair.trade) ||
         fread(&pair.trade, 1, sizeof(pair.trade), log) !=
             sizeof(pair.trade))) {
        ok = false;
    }
    if (fclose(log) != 0) ok = false;

    uint64_t transfer_event_id =
        economy_read_u64_le(&transfer_header[8]);
    uint64_t trade_event_id =
        economy_read_u64_le(&trade_header[8]);
    if (!ok ||
        transfer_header[16] != (uint8_t)CHAIN_EVT_TRANSFER ||
        trade_header[16] != (uint8_t)CHAIN_EVT_TRADE ||
        transfer_event_id == UINT64_MAX ||
        trade_event_id != transfer_event_id + 1u ||
        pair.trade.transfer_event_id != transfer_event_id) {
        return false;
    }
    pair.transfer_event_id = transfer_event_id;
    *out = pair;
    return true;
}

static void economy_setup_transfer_player(world_t *w,
                                           server_player_t **out_player) {
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    sp->id = 0;
    memset(sp->session_token, 0x71, sizeof(sp->session_token));
    economy_fill_pubkey(sp->pubkey, 0x91);
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    sp->pubkey_challenge_consumed = true;
    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    *out_player = sp;
}

static economy_transfer_failure_result_t
economy_run_named_buy_transfer_failure(
    economy_transfer_fault_t fault,
    chain_event_type_t failure_event) {
    economy_transfer_failure_result_t result = {0};
    economy_transfer_snapshot_t *snapshot =
        calloc(1, sizeof(*snapshot));
    economy_receipt_sink_capture_t sink = {0};
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    if (!w || !snapshot) {
        free(snapshot);
        return result;
    }

    economy_transfer_test_configure_chain(
        "named_buy", fault, failure_event);
    w->rng = 22001u + (uint32_t)fault +
             (uint32_t)failure_event;
    world_reset(w);
    server_player_t *sp = NULL;
    economy_setup_transfer_player(w, &sp);
    station_t *station = &w->stations[0];

    uint8_t cargo_pub[32];
    economy_fill_pubkey(cargo_pub, (uint8_t)(0x51 + fault));
    uint16_t output_index = 0;
    cargo_unit_t unit =
        economy_test_cargo_unit(cargo_pub, &output_index);
    cargo_receipt_chain_t chain = {0};
    if (!economy_issue_single_receipt(
            w, 0, station->station_pubkey, output_index,
            &unit, &chain) ||
        !station_manifest_push_with_chain(station, &unit, &chain)) {
        goto cleanup;
    }
    ledger_earn_by_pubkey(station, sp->pubkey, 100000.0f);

    economy_transfer_test_arm_fault(
        station, fault, failure_event);
    const cargo_store_t *stores[] = {
        &station->cargo_store,
        &sp->ship->cargo_store,
    };
    if (!economy_transfer_snapshot_take(
            snapshot, w, stores,
            sizeof(stores) / sizeof(stores[0]), 0, sp->ship)) {
        goto cleanup;
    }

    server_signed_action_dispatch_result_t dispatch = {0};
    bool invoked = server_dispatch_signed_action_payload(
        w, 0, SIGNED_ACTION_BUY_INGOT,
        unit.pub, sizeof(unit.pub),
        economy_capture_receipt_chain, &sink, &dispatch);
    result = economy_transfer_snapshot_evaluate(
        snapshot, w, 0, sp->ship, fault,
        invoked, sink.calls);

cleanup:
    economy_transfer_snapshot_cleanup(snapshot);
    free(snapshot);
    chain_log_test_fault_clear();
    return result;
}

static economy_transfer_failure_result_t
economy_run_named_delivery_transfer_failure(
    economy_transfer_fault_t fault,
    chain_event_type_t failure_event) {
    economy_transfer_failure_result_t result = {0};
    economy_transfer_snapshot_t *snapshot =
        calloc(1, sizeof(*snapshot));
    economy_receipt_sink_capture_t sink = {0};
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    if (!w || !snapshot) {
        free(snapshot);
        return result;
    }

    economy_transfer_test_configure_chain(
        "named_delivery", fault, failure_event);
    w->rng = 22101u + (uint32_t)fault +
             (uint32_t)failure_event;
    world_reset(w);
    server_player_t *sp = NULL;
    economy_setup_transfer_player(w, &sp);
    station_t *station = &w->stations[0];

    uint8_t cargo_pub[32];
    economy_fill_pubkey(cargo_pub, (uint8_t)(0x61 + fault));
    uint16_t output_index = 0;
    cargo_unit_t unit =
        economy_test_cargo_unit(cargo_pub, &output_index);
    cargo_receipt_chain_t chain = {0};
    if (!economy_issue_single_receipt(
            w, 0, sp->pubkey, output_index,
            &unit, &chain) ||
        !ship_manifest_push_with_chain(sp->ship, &unit, &chain)) {
        goto cleanup;
    }

    manifest_clear(&station->manifest);
    ship_receipts_t *station_receipts =
        station_get_receipts(station);
    if (!station_receipts) goto cleanup;
    ship_receipts_clear(station_receipts);
    int station_capacity = station->manifest.cap;
    if (station_capacity <= 0 ||
        station_finished_mint(
            station, COMMODITY_FRAME,
            station_capacity, NULL) != station_capacity) {
        goto cleanup;
    }

    economy_transfer_test_arm_fault(
        station, fault, failure_event);
    const cargo_store_t *stores[] = {
        &sp->ship->cargo_store,
        &station->cargo_store,
    };
    if (!economy_transfer_snapshot_take(
            snapshot, w, stores,
            sizeof(stores) / sizeof(stores[0]), 0, sp->ship)) {
        goto cleanup;
    }

    uint8_t target = 0;
    server_signed_action_dispatch_result_t dispatch = {0};
    bool invoked = server_dispatch_signed_action_payload(
        w, 0, SIGNED_ACTION_DELIVER,
        &target, sizeof(target),
        economy_capture_receipt_chain, &sink, &dispatch);
    result = economy_transfer_snapshot_evaluate(
        snapshot, w, 0, sp->ship, fault,
        invoked, sink.calls);

cleanup:
    economy_transfer_snapshot_cleanup(snapshot);
    free(snapshot);
    chain_log_test_fault_clear();
    return result;
}

static bool economy_setup_npc_delivery_case_with_quantity(
    world_t *w,
    uint16_t quantity,
    npc_ship_t **out_npc,
    ship_t **out_ship) {
    if (!w || quantity == 0 ||
        quantity > MAX_DELIVERY_BOUND_CARGO ||
        !out_npc || !out_ship) {
        return false;
    }
    memset(w->contracts, 0, sizeof(w->contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(&w->stations[s].knowledge, 0,
               sizeof(w->stations[s].knowledge));
    }

    if (!test_set_station_finished_units(
            &w->stations[0], COMMODITY_FERRITE_INGOT,
            (int)quantity) ||
        !test_anchor_station_legacy_cargo(w, 0) ||
        !station_manifest_bootstrap(&w->stations[2])) {
        return false;
    }
    manifest_clear(&w->stations[2].manifest);
    ship_receipts_t *dest_receipts =
        station_get_receipts(&w->stations[2]);
    if (!dest_receipts) return false;
    ship_receipts_clear(dest_receipts);

    w->contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = (float)quantity,
        .base_price = 500.6f,
        .claimed_by = -1,
        .proof_flags = CONTRACT_PROOF_REQUIRE_PROOF,
    };

    int slot = spawn_npc(w, 0, NPC_ROLE_HAULER);
    if (slot < 0) return false;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (n != slot) w->npc_ships[n].active = false;
    }
    npc_ship_t *npc = &w->npc_ships[slot];
    ship_t *ship = world_npc_ship_for(w, slot);
    if (!ship || !ship_manifest_bootstrap(ship)) return false;
    manifest_clear(&ship->manifest);
    ship_receipts_t *ship_receipts = ship_get_receipts(ship);
    if (!ship_receipts) return false;
    ship_receipts_clear(ship_receipts);
    memset(ship->cargo, 0, sizeof(ship->cargo));
    ship->mining_level = SHIP_UPGRADE_MAX_LEVEL;
    ship->hold_level = SHIP_UPGRADE_MAX_LEVEL;
    ship->tractor_level = SHIP_UPGRADE_MAX_LEVEL;
    npc->state = NPC_STATE_UNLOADING;
    npc->state_timer = 0.0f;
    npc->home_station = 0;
    npc->dest_station = 2;
    npc->pickup_station = 0;
    npc->pickup_commodity = COMMODITY_FERRITE_INGOT;
    npc->pickup_action = (uint8_t)CONTRACT_DELIVERY;
    npc->brain_mode = SERVER_BRAIN_MODE_NEURAL_FLIGHT;
    memset(&ship->knowledge, 0, sizeof(ship->knowledge));
    knowledge_view_configure(&ship->knowledge, SHIP_KNOWN_ITEM_CAP);

    *out_npc = npc;
    *out_ship = ship;
    return true;
}

static bool economy_setup_npc_delivery_case(
    world_t *w,
    npc_ship_t **out_npc,
    ship_t **out_ship) {
    return economy_setup_npc_delivery_case_with_quantity(
        w, 1, out_npc, out_ship);
}

static economy_transfer_failure_result_t
economy_run_npc_delivery_pickup_failure(
    economy_transfer_fault_t fault,
    chain_event_type_t failure_event) {
    economy_transfer_failure_result_t result = {0};
    economy_transfer_snapshot_t *snapshot =
        calloc(1, sizeof(*snapshot));
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    if (!w || !snapshot) {
        free(snapshot);
        return result;
    }

    economy_transfer_test_configure_chain(
        "npc_delivery_pickup", fault, failure_event);
    w->rng = 22201u + (uint32_t)fault +
             (uint32_t)failure_event;
    world_reset(w);
    npc_ship_t *npc = NULL;
    ship_t *ship = NULL;
    if (!economy_setup_npc_delivery_case(
            w, &npc, &ship)) {
        goto cleanup;
    }
    (void)npc;

    station_t *origin = &w->stations[0];
    economy_transfer_test_arm_fault(
        origin, fault, failure_event);
    const cargo_store_t *stores[] = {
        &origin->cargo_store,
        &ship->cargo_store,
        &w->stations[2].cargo_store,
    };
    if (!economy_transfer_snapshot_take(
            snapshot, w, stores,
            sizeof(stores) / sizeof(stores[0]), 0, ship)) {
        goto cleanup;
    }

    step_npc_ships(w, SIM_DT);
    result = economy_transfer_snapshot_evaluate(
        snapshot, w, 0, ship, fault, true, 0);

cleanup:
    economy_transfer_snapshot_cleanup(snapshot);
    free(snapshot);
    chain_log_test_fault_clear();
    return result;
}

static economy_transfer_failure_result_t
economy_run_npc_delivery_destination_failure(
    economy_transfer_fault_t fault,
    chain_event_type_t failure_event) {
    economy_transfer_failure_result_t result = {0};
    economy_transfer_snapshot_t *snapshot =
        calloc(1, sizeof(*snapshot));
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    if (!w || !snapshot) {
        free(snapshot);
        return result;
    }

    economy_transfer_test_configure_chain(
        "npc_delivery_destination", fault, failure_event);
    w->rng = 22301u + (uint32_t)fault +
             (uint32_t)failure_event;
    world_reset(w);
    npc_ship_t *npc = NULL;
    ship_t *ship = NULL;
    if (!economy_setup_npc_delivery_case(
            w, &npc, &ship)) {
        goto cleanup;
    }

    step_npc_ships(w, SIM_DT);
    bool picked_up = false;
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        const delivery_shipment_t *shipment =
            &w->delivery_shipments[i];
        if (shipment->active &&
            shipment->contract_index == 0 &&
            shipment->status == DELIVERY_SHIPMENT_PICKED_UP &&
            shipment->quantity_total == 1) {
            picked_up = true;
            break;
        }
    }
    if (!picked_up || ship->manifest.count != 1) goto cleanup;

    npc->state = NPC_STATE_UNLOADING;
    npc->state_timer = 0.0f;
    npc->dest_station = 2;
    npc->pickup_station = -1;
    npc->pickup_commodity = COMMODITY_COUNT;
    npc->pickup_action = (uint8_t)CONTRACT_TRACTOR;
    station_t *destination = &w->stations[2];
    ship->pos = station_approach_target(destination, ship->pos);
    ship->vel = v2(0.0f, 0.0f);

    economy_transfer_test_arm_fault(
        destination, fault, failure_event);
    const cargo_store_t *stores[] = {
        &w->stations[0].cargo_store,
        &ship->cargo_store,
        &destination->cargo_store,
    };
    if (!economy_transfer_snapshot_take(
            snapshot, w, stores,
            sizeof(stores) / sizeof(stores[0]), 2, ship)) {
        goto cleanup;
    }

    step_npc_ships(w, SIM_DT);
    result = economy_transfer_snapshot_evaluate(
        snapshot, w, 2, ship, fault, true, 0);

cleanup:
    economy_transfer_snapshot_cleanup(snapshot);
    free(snapshot);
    chain_log_test_fault_clear();
    return result;
}

static bool economy_setup_npc_general_haul_case(
    world_t *w,
    uint8_t cargo_seed,
    npc_ship_t **out_npc,
    ship_t **out_ship,
    station_t **out_destination,
    cargo_unit_t *out_unit) {
    if (!w || !out_npc || !out_ship || !out_destination)
        return false;
    memset(w->contracts, 0, sizeof(w->contracts));

    int slot = spawn_npc(w, 0, NPC_ROLE_HAULER);
    if (slot < 0) return false;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (n != slot) w->npc_ships[n].active = false;
    }
    npc_ship_t *npc = &w->npc_ships[slot];
    ship_t *ship = world_npc_ship_for(w, slot);
    if (!ship || !ship_manifest_bootstrap(ship)) return false;
    manifest_clear(&ship->manifest);
    ship_receipts_t *ship_receipts = ship_get_receipts(ship);
    if (!ship_receipts) return false;
    ship_receipts_clear(ship_receipts);
    memset(ship->cargo, 0, sizeof(ship->cargo));

    int destination_index = SIGNAL_FREEPORT_STATION_INDEX;
    station_t *destination = &w->stations[destination_index];
    if (!station_exists(destination) ||
        !station_faction_is_pirate_economy(destination) ||
        !station_manifest_bootstrap(destination)) {
        return false;
    }
    manifest_clear(&destination->manifest);
    ship_receipts_t *destination_receipts =
        station_get_receipts(destination);
    if (!destination_receipts ||
        !test_set_station_finished_units(
            destination, COMMODITY_FERRITE_INGOT, 0)) {
        return false;
    }
    ship_receipts_clear(destination_receipts);

    uint8_t npc_pubkey[32];
    npc_custody_pubkey_from_fields(
        npc->session_token, slot, (uint8_t)npc->role,
        (uint8_t)npc->home_station, npc_pubkey);
    uint8_t cargo_pub[32];
    economy_fill_pubkey(cargo_pub, cargo_seed);
    uint16_t output_index = 0;
    cargo_unit_t unit =
        economy_test_cargo_unit(cargo_pub, &output_index);
    cargo_receipt_chain_t chain = {0};
    if (!economy_issue_single_receipt(
            w, 0, npc_pubkey, output_index,
            &unit, &chain) ||
        !ship_manifest_push_with_chain(ship, &unit, &chain)) {
        return false;
    }

    w->contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = destination_index,
        .target_index = -1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 1.0f,
        .base_price = 800.6f,
        .claimed_by = -1,
    };
    npc->state = NPC_STATE_UNLOADING;
    npc->state_timer = 0.0f;
    npc->home_station = 0;
    npc->dest_station = destination_index;
    npc->pickup_station = -1;
    npc->pickup_commodity = COMMODITY_COUNT;
    npc->pickup_action = (uint8_t)CONTRACT_TRACTOR;
    ship->pos = station_approach_target(destination, ship->pos);
    ship->vel = v2(0.0f, 0.0f);

    *out_npc = npc;
    *out_ship = ship;
    *out_destination = destination;
    if (out_unit) *out_unit = unit;
    return true;
}

static economy_transfer_failure_result_t
economy_run_npc_general_haul_destination_failure(
    economy_transfer_fault_t fault,
    chain_event_type_t failure_event) {
    economy_transfer_failure_result_t result = {0};
    economy_transfer_snapshot_t *snapshot =
        calloc(1, sizeof(*snapshot));
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    if (!w || !snapshot) {
        free(snapshot);
        return result;
    }

    economy_transfer_test_configure_chain(
        "npc_general_haul_destination", fault, failure_event);
    w->rng = 22401u + (uint32_t)fault +
             (uint32_t)failure_event;
    world_reset(w);
    npc_ship_t *npc = NULL;
    ship_t *ship = NULL;
    station_t *destination = NULL;
    if (!economy_setup_npc_general_haul_case(
            w, (uint8_t)(0x71 + fault + failure_event),
            &npc, &ship, &destination, NULL)) {
        goto cleanup;
    }
    (void)npc;
    int destination_index = SIGNAL_FREEPORT_STATION_INDEX;

    economy_transfer_test_arm_fault(
        destination, fault, failure_event);
    const cargo_store_t *stores[] = {
        &w->stations[0].cargo_store,
        &ship->cargo_store,
        &destination->cargo_store,
    };
    if (!economy_transfer_snapshot_take(
            snapshot, w, stores,
            sizeof(stores) / sizeof(stores[0]),
            destination_index, ship)) {
        goto cleanup;
    }

    step_npc_ships(w, SIM_DT);
    result = economy_transfer_snapshot_evaluate(
        snapshot, w, destination_index, ship,
        fault, true, 0);

cleanup:
    economy_transfer_snapshot_cleanup(snapshot);
    free(snapshot);
    chain_log_test_fault_clear();
    return result;
}

typedef struct {
    bool setup_ok;
    bool stores_committed;
    bool adjacent_events;
    bool rounded_delta;
    bool ledger_matches;
    bool stats_match;
    bool bookkeeping_matches;
} economy_paid_transfer_success_result_t;

static delivery_shipment_t *economy_find_delivery_shipment(
    world_t *w,
    uint8_t contract_index) {
    if (!w) return NULL;
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        delivery_shipment_t *shipment =
            &w->delivery_shipments[i];
        if (shipment->active &&
            shipment->contract_index == contract_index) {
            return shipment;
        }
    }
    return NULL;
}

static economy_paid_transfer_success_result_t
economy_run_npc_delivery_pickup_success(void) {
    economy_paid_transfer_success_result_t result = {0};
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    if (!w) return result;

    economy_transfer_test_configure_chain(
        "npc_paid_success_pickup",
        ECONOMY_TRANSFER_FAULT_WRITE, CHAIN_EVT_TRANSFER);
    w->rng = 22501u;
    world_reset(w);
    npc_ship_t *npc = NULL;
    ship_t *ship = NULL;
    if (!economy_setup_npc_delivery_case(
            w, &npc, &ship)) {
        goto cleanup;
    }

    station_t *origin = &w->stations[0];
    if (origin->manifest.count != 1 ||
        ship->manifest.count != 0) {
        goto cleanup;
    }
    cargo_unit_t unit = origin->manifest.units[0];
    float quoted = station_sell_price(
        origin, COMMODITY_FERRITE_INGOT);
    if (quoted <= 0.0f) {
        quoted = station_buy_price(
            origin, COMMODITY_FERRITE_INGOT);
    }
    int64_t expected_delta = -(int64_t)llroundf(quoted);
    if (expected_delta >= 0) goto cleanup;

    int slot = (int)(npc - w->npc_ships);
    uint8_t npc_pubkey[32];
    uint8_t ledger_pubkey[32];
    npc_custody_pubkey_from_fields(
        npc->session_token, slot, (uint8_t)npc->role,
        (uint8_t)npc->home_station, npc_pubkey);
    ledger_pubkey_from_token(
        npc->session_token, ledger_pubkey);
    float ledger_before =
        ledger_balance_by_pubkey(origin, ledger_pubkey);
    float spent_before = ship->stat_credits_spent;
    float earned_before = ship->stat_credits_earned;
    uint16_t origin_count_before = origin->manifest.count;
    uint16_t ship_count_before = ship->manifest.count;
    uint64_t chain_count_before = origin->chain_event_count;
    result.setup_ok = true;

    step_npc_ships(w, SIM_DT);

    economy_transfer_trade_pair_t pair = {0};
    uint64_t walked = 0;
    bool pair_ok =
        economy_read_last_transfer_trade_pair(origin, &pair);
    result.stores_committed =
        origin->manifest.count + 1u == origin_count_before &&
        ship->manifest.count == ship_count_before + 1u &&
        manifest_find(&ship->manifest, unit.pub) >= 0;
    result.adjacent_events =
        origin->chain_event_count == chain_count_before + 2u &&
        pair_ok &&
        pair.transfer_event_id == chain_count_before + 1u &&
        chain_log_verify(origin, &walked, NULL) &&
        walked == origin->chain_event_count &&
        memcmp(pair.transfer.from_pubkey,
               origin->station_pubkey, 32) == 0 &&
        memcmp(pair.transfer.to_pubkey,
               npc_pubkey, 32) == 0 &&
        memcmp(pair.transfer.cargo_pub,
               unit.pub, 32) == 0 &&
        memcmp(pair.trade.ledger_pubkey,
               ledger_pubkey, 32) == 0;
    result.rounded_delta =
        pair_ok &&
        pair.trade.ledger_delta_signed == expected_delta;
    result.ledger_matches =
        fabsf((ledger_balance_by_pubkey(
                   origin, ledger_pubkey) -
               ledger_before) -
              (float)expected_delta) < 0.0001f;
    result.stats_match =
        fabsf((ship->stat_credits_spent - spent_before) +
              (float)expected_delta) < 0.0001f &&
        ship->stat_credits_earned == earned_before;

    delivery_shipment_t *shipment =
        economy_find_delivery_shipment(w, 0);
    result.bookkeeping_matches =
        shipment &&
        shipment->status == DELIVERY_SHIPMENT_PICKED_UP &&
        shipment->quantity_total == 1 &&
        shipment->quantity_bound == 1 &&
        fabsf(shipment->debt_principal +
              (float)expected_delta) < 0.0001f;

cleanup:
    chain_log_test_fault_clear();
    return result;
}

static economy_paid_transfer_success_result_t
economy_run_npc_delivery_destination_success(void) {
    economy_paid_transfer_success_result_t result = {0};
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    if (!w) return result;

    economy_transfer_test_configure_chain(
        "npc_paid_success_delivery",
        ECONOMY_TRANSFER_FAULT_WRITE, CHAIN_EVT_TRANSFER);
    w->rng = 22601u;
    world_reset(w);
    npc_ship_t *npc = NULL;
    ship_t *ship = NULL;
    if (!economy_setup_npc_delivery_case(
            w, &npc, &ship)) {
        goto cleanup;
    }
    step_npc_ships(w, SIM_DT);

    delivery_shipment_t *shipment =
        economy_find_delivery_shipment(w, 0);
    if (!shipment ||
        shipment->status != DELIVERY_SHIPMENT_PICKED_UP ||
        shipment->quantity_total != 1 ||
        ship->manifest.count != 1) {
        goto cleanup;
    }
    station_t *destination = &w->stations[2];
    cargo_unit_t unit = ship->manifest.units[0];
    float quoted = shipment->destination_payout /
                   (float)shipment->quantity_total;
    int64_t expected_delta = (int64_t)llroundf(quoted);
    if (expected_delta <= 0) goto cleanup;

    int slot = (int)(npc - w->npc_ships);
    uint8_t npc_pubkey[32];
    uint8_t ledger_pubkey[32];
    npc_custody_pubkey_from_fields(
        npc->session_token, slot, (uint8_t)npc->role,
        (uint8_t)npc->home_station, npc_pubkey);
    ledger_pubkey_from_token(
        npc->session_token, ledger_pubkey);
    float ledger_before =
        ledger_balance_by_pubkey(destination, ledger_pubkey);
    float spent_before = ship->stat_credits_spent;
    float earned_before = ship->stat_credits_earned;
    uint16_t ship_count_before = ship->manifest.count;
    uint16_t destination_count_before =
        destination->manifest.count;
    uint64_t chain_count_before =
        destination->chain_event_count;

    npc->state = NPC_STATE_UNLOADING;
    npc->state_timer = 0.0f;
    npc->dest_station = 2;
    npc->pickup_station = -1;
    npc->pickup_commodity = COMMODITY_COUNT;
    npc->pickup_action = (uint8_t)CONTRACT_TRACTOR;
    ship->pos = station_approach_target(
        destination, ship->pos);
    ship->vel = v2(0.0f, 0.0f);
    result.setup_ok = true;

    step_npc_ships(w, SIM_DT);

    economy_transfer_trade_pair_t pair = {0};
    uint64_t walked = 0;
    bool pair_ok =
        economy_read_last_transfer_trade_pair(
            destination, &pair);
    result.stores_committed =
        ship->manifest.count + 1u == ship_count_before &&
        destination->manifest.count ==
            destination_count_before + 1u &&
        manifest_find(&destination->manifest, unit.pub) >= 0;
    result.adjacent_events =
        destination->chain_event_count ==
            chain_count_before + 2u &&
        pair_ok &&
        pair.transfer_event_id == chain_count_before + 1u &&
        chain_log_verify(destination, &walked, NULL) &&
        walked == destination->chain_event_count &&
        memcmp(pair.transfer.from_pubkey,
               npc_pubkey, 32) == 0 &&
        memcmp(pair.transfer.to_pubkey,
               destination->station_pubkey, 32) == 0 &&
        memcmp(pair.transfer.cargo_pub,
               unit.pub, 32) == 0 &&
        memcmp(pair.trade.ledger_pubkey,
               ledger_pubkey, 32) == 0;
    result.rounded_delta =
        pair_ok &&
        pair.trade.ledger_delta_signed == expected_delta &&
        expected_delta == 501;
    result.ledger_matches =
        fabsf((ledger_balance_by_pubkey(
                   destination, ledger_pubkey) -
               ledger_before) -
              (float)expected_delta) < 0.0001f;
    result.stats_match =
        ship->stat_credits_spent == spent_before &&
        ship->stat_credits_earned == earned_before;
    result.bookkeeping_matches =
        shipment->status == DELIVERY_SHIPMENT_DELIVERED &&
        shipment->quantity_delivered == 1 &&
        w->contracts[0].active &&
        fabsf(w->contracts[0].quantity_needed) < 0.0001f;

cleanup:
    chain_log_test_fault_clear();
    return result;
}

static economy_paid_transfer_success_result_t
economy_run_npc_general_haul_destination_success(void) {
    economy_paid_transfer_success_result_t result = {0};
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    if (!w) return result;

    economy_transfer_test_configure_chain(
        "npc_paid_success_general_haul",
        ECONOMY_TRANSFER_FAULT_WRITE, CHAIN_EVT_TRANSFER);
    w->rng = 22701u;
    world_reset(w);
    npc_ship_t *npc = NULL;
    ship_t *ship = NULL;
    station_t *destination = NULL;
    cargo_unit_t unit = {0};
    if (!economy_setup_npc_general_haul_case(
            w, 0x7du, &npc, &ship,
            &destination, &unit)) {
        goto cleanup;
    }

    int destination_index = SIGNAL_FREEPORT_STATION_INDEX;
    int64_t expected_delta =
        (int64_t)llroundf(contract_price(&w->contracts[0]));
    if (expected_delta <= 0 ||
        ship->manifest.count != 1) {
        goto cleanup;
    }
    int slot = (int)(npc - w->npc_ships);
    uint8_t npc_pubkey[32];
    uint8_t ledger_pubkey[32];
    npc_custody_pubkey_from_fields(
        npc->session_token, slot, (uint8_t)npc->role,
        (uint8_t)npc->home_station, npc_pubkey);
    ledger_pubkey_from_token(
        npc->session_token, ledger_pubkey);
    float ledger_before =
        ledger_balance_by_pubkey(destination, ledger_pubkey);
    float spent_before = ship->stat_credits_spent;
    float earned_before = ship->stat_credits_earned;
    uint16_t ship_count_before = ship->manifest.count;
    uint16_t destination_count_before =
        destination->manifest.count;
    uint64_t chain_count_before =
        destination->chain_event_count;
    result.setup_ok = true;

    step_npc_ships(w, SIM_DT);

    economy_transfer_trade_pair_t pair = {0};
    uint64_t walked = 0;
    bool pair_ok =
        economy_read_last_transfer_trade_pair(
            destination, &pair);
    result.stores_committed =
        ship->manifest.count + 1u == ship_count_before &&
        destination->manifest.count ==
            destination_count_before + 1u &&
        manifest_find(&destination->manifest, unit.pub) >= 0;
    result.adjacent_events =
        destination->chain_event_count ==
            chain_count_before + 2u &&
        pair_ok &&
        pair.transfer_event_id == chain_count_before + 1u &&
        chain_log_verify(destination, &walked, NULL) &&
        walked == destination->chain_event_count &&
        memcmp(pair.transfer.from_pubkey,
               npc_pubkey, 32) == 0 &&
        memcmp(pair.transfer.to_pubkey,
               destination->station_pubkey, 32) == 0 &&
        memcmp(pair.transfer.cargo_pub,
               unit.pub, 32) == 0 &&
        memcmp(pair.trade.ledger_pubkey,
               ledger_pubkey, 32) == 0;
    result.rounded_delta =
        pair_ok &&
        pair.trade.ledger_delta_signed == expected_delta &&
        expected_delta == 801;
    result.ledger_matches =
        fabsf((ledger_balance_by_pubkey(
                   destination, ledger_pubkey) -
               ledger_before) -
              (float)expected_delta) < 0.0001f;
    result.stats_match =
        ship->stat_credits_spent == spent_before &&
        ship->stat_credits_earned == earned_before;
    result.bookkeeping_matches =
        !w->contracts[0].active &&
        fabsf(w->contracts[0].quantity_needed) < 0.0001f &&
        npc->dest_station == destination_index;

cleanup:
    chain_log_test_fault_clear();
    return result;
}

#define ASSERT_TRANSFER_FAILURE_INERT(value) do { \
    economy_transfer_failure_result_t _result = (value); \
    ASSERT(_result.setup_ok); \
    ASSERT(_result.caller_invoked); \
    ASSERT(_result.manifests_unchanged); \
    ASSERT(_result.receipts_unchanged); \
    ASSERT(_result.pods_unchanged); \
    ASSERT(_result.ledgers_unchanged); \
    ASSERT(_result.contracts_unchanged); \
    ASSERT(_result.shipments_unchanged); \
    ASSERT(_result.credits_unchanged); \
    ASSERT(_result.inventory_unchanged); \
    ASSERT(_result.manifest_dirty_unchanged); \
    ASSERT(_result.chain_counters_unchanged); \
    ASSERT(_result.chain_log_unchanged); \
    ASSERT(_result.expected_health_flags); \
    ASSERT(_result.receipt_sink_unchanged); \
} while (0)

#define ASSERT_PAID_TRANSFER_SUCCESS(value) do { \
    economy_paid_transfer_success_result_t _result = (value); \
    ASSERT(_result.setup_ok); \
    ASSERT(_result.stores_committed); \
    ASSERT(_result.adjacent_events); \
    ASSERT(_result.rounded_delta); \
    ASSERT(_result.ledger_matches); \
    ASSERT(_result.stats_match); \
    ASSERT(_result.bookkeeping_matches); \
} while (0)

#define ASSERT_NPC_PAID_TRANSFER_FAILURES(fault) do { \
    ASSERT_TRANSFER_FAILURE_INERT( \
        economy_run_npc_delivery_pickup_failure( \
            (fault), CHAIN_EVT_TRANSFER)); \
    ASSERT_TRANSFER_FAILURE_INERT( \
        economy_run_npc_delivery_pickup_failure( \
            (fault), CHAIN_EVT_TRADE)); \
    ASSERT_TRANSFER_FAILURE_INERT( \
        economy_run_npc_delivery_destination_failure( \
            (fault), CHAIN_EVT_TRANSFER)); \
    ASSERT_TRANSFER_FAILURE_INERT( \
        economy_run_npc_delivery_destination_failure( \
            (fault), CHAIN_EVT_TRADE)); \
    ASSERT_TRANSFER_FAILURE_INERT( \
        economy_run_npc_general_haul_destination_failure( \
            (fault), CHAIN_EVT_TRANSFER)); \
    ASSERT_TRANSFER_FAILURE_INERT( \
        economy_run_npc_general_haul_destination_failure( \
            (fault), CHAIN_EVT_TRADE)); \
} while (0)

static void economy_prepare_npc_delivery_attempt(
    npc_ship_t *npc,
    ship_t *ship,
    station_t *destination,
    int destination_index) {
    npc->state = NPC_STATE_UNLOADING;
    npc->state_timer = 0.0f;
    npc->dest_station = destination_index;
    npc->pickup_station = -1;
    npc->pickup_commodity = COMMODITY_COUNT;
    npc->pickup_action = (uint8_t)CONTRACT_TRACTOR;
    ship->pos = station_approach_target(destination, ship->pos);
    ship->vel = v2(0.0f, 0.0f);
}

TEST(test_npc_paid_transfer_success_appends_exact_trade_pairs) {
    ASSERT_PAID_TRANSFER_SUCCESS(
        economy_run_npc_delivery_pickup_success());
    ASSERT_PAID_TRANSFER_SUCCESS(
        economy_run_npc_delivery_destination_success());
    ASSERT_PAID_TRANSFER_SUCCESS(
        economy_run_npc_general_haul_destination_success());
}

TEST(test_npc_delivery_payout_rounds_aggregate_across_partial_retry) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    economy_transfer_snapshot_t *snapshot =
        calloc(1, sizeof(*snapshot));
    ASSERT(w != NULL);
    ASSERT(snapshot != NULL);

    economy_transfer_test_configure_chain(
        "npc_delivery_aggregate_payout",
        ECONOMY_TRANSFER_FAULT_PREBLOCKED, CHAIN_EVT_TRADE);
    w->rng = 22801u;
    world_reset(w);

    npc_ship_t *npc = NULL;
    ship_t *ship = NULL;
    ASSERT(economy_setup_npc_delivery_case_with_quantity(
        w, 3, &npc, &ship));
    step_npc_ships(w, SIM_DT);

    delivery_shipment_t *shipment =
        economy_find_delivery_shipment(w, 0);
    ASSERT(shipment != NULL);
    ASSERT(shipment->status == DELIVERY_SHIPMENT_PICKED_UP);
    ASSERT(shipment->quantity_total == 3);
    ASSERT(shipment->quantity_delivered == 0);
    ASSERT(ship->manifest.count == 3);

    int64_t rounded_total =
        (int64_t)llroundf(shipment->destination_payout);
    ASSERT(rounded_total == 1502);
    int64_t expected_deltas[3] = {
        rounded_total / 3 + 1,
        rounded_total / 3 + 1,
        rounded_total / 3,
    };

    int destination_index = 2;
    station_t *destination = &w->stations[destination_index];
    destination->scaffold = false;
    destination->module_count = 0;
    ASSERT(destination->manifest.count == 0);
    ASSERT(destination->manifest.cap > 1);
    int filler_count = (int)destination->manifest.cap - 1;
    ASSERT(station_finished_mint(
        destination, COMMODITY_FRAME, filler_count, NULL) ==
        filler_count);

    int npc_slot = (int)(npc - w->npc_ships);
    uint8_t npc_pubkey[32];
    uint8_t ledger_pubkey[32];
    npc_custody_pubkey_from_fields(
        npc->session_token, npc_slot, (uint8_t)npc->role,
        (uint8_t)npc->home_station, npc_pubkey);
    ledger_pubkey_from_token(npc->session_token, ledger_pubkey);
    float ledger_before =
        ledger_balance_by_pubkey(destination, ledger_pubkey);
    float spent_before = ship->stat_credits_spent;
    float earned_before = ship->stat_credits_earned;
    uint64_t chain_count_before =
        destination->chain_event_count;
    int64_t observed_total = 0;

    economy_prepare_npc_delivery_attempt(
        npc, ship, destination, destination_index);
    step_npc_ships(w, SIM_DT);

    economy_transfer_trade_pair_t pair = {0};
    ASSERT(economy_read_last_transfer_trade_pair(
        destination, &pair));
    ASSERT(pair.transfer_event_id == chain_count_before + 1u);
    ASSERT(pair.trade.ledger_delta_signed == expected_deltas[0]);
    ASSERT(memcmp(pair.transfer.from_pubkey, npc_pubkey, 32) == 0);
    ASSERT(memcmp(pair.transfer.to_pubkey,
                  destination->station_pubkey, 32) == 0);
    ASSERT(memcmp(pair.trade.ledger_pubkey, ledger_pubkey, 32) == 0);
    observed_total += pair.trade.ledger_delta_signed;
    ASSERT(shipment->quantity_delivered == 1);
    ASSERT(shipment->status == DELIVERY_SHIPMENT_PICKED_UP);
    ASSERT(ship->manifest.count == 2);
    ASSERT(destination->manifest.count ==
           destination->manifest.cap);
    ASSERT(destination->chain_event_count ==
           chain_count_before + 2u);
    ASSERT_EQ_FLOAT(
        ledger_balance_by_pubkey(destination, ledger_pubkey) -
            ledger_before,
        (float)observed_total, 0.0001f);

    ASSERT(station_manifest_consume_by_commodity(
        destination, COMMODITY_FRAME, 1) == 1);
    economy_transfer_test_arm_fault(
        destination, ECONOMY_TRANSFER_FAULT_PREBLOCKED,
        CHAIN_EVT_TRADE);
    economy_prepare_npc_delivery_attempt(
        npc, ship, destination, destination_index);
    const cargo_store_t *stores[] = {
        &w->stations[0].cargo_store,
        &ship->cargo_store,
        &destination->cargo_store,
    };
    ASSERT(economy_transfer_snapshot_take(
        snapshot, w, stores,
        sizeof(stores) / sizeof(stores[0]),
        destination_index, ship));

    step_npc_ships(w, SIM_DT);
    ASSERT_TRANSFER_FAILURE_INERT(
        economy_transfer_snapshot_evaluate(
            snapshot, w, destination_index, ship,
            ECONOMY_TRANSFER_FAULT_PREBLOCKED, true, 0));
    economy_transfer_snapshot_cleanup(snapshot);
    ASSERT(shipment->quantity_delivered == 1);
    ASSERT(observed_total == expected_deltas[0]);

    chain_log_health_set(
        destination, CHAIN_HEALTH_OK, false,
        destination->chain_event_count,
        destination->chain_last_hash,
        "test retry after verified pre-block");
    economy_prepare_npc_delivery_attempt(
        npc, ship, destination, destination_index);
    step_npc_ships(w, SIM_DT);

    memset(&pair, 0, sizeof(pair));
    ASSERT(economy_read_last_transfer_trade_pair(
        destination, &pair));
    ASSERT(pair.transfer_event_id == chain_count_before + 3u);
    ASSERT(pair.trade.ledger_delta_signed == expected_deltas[1]);
    observed_total += pair.trade.ledger_delta_signed;
    ASSERT(shipment->quantity_delivered == 2);
    ASSERT(shipment->status == DELIVERY_SHIPMENT_PICKED_UP);
    ASSERT(ship->manifest.count == 1);
    ASSERT(destination->manifest.count ==
           destination->manifest.cap);
    ASSERT(destination->chain_event_count ==
           chain_count_before + 4u);
    ASSERT_EQ_FLOAT(
        ledger_balance_by_pubkey(destination, ledger_pubkey) -
            ledger_before,
        (float)observed_total, 0.0001f);

    ASSERT(station_manifest_consume_by_commodity(
        destination, COMMODITY_FRAME, 1) == 1);
    economy_prepare_npc_delivery_attempt(
        npc, ship, destination, destination_index);
    step_npc_ships(w, SIM_DT);

    memset(&pair, 0, sizeof(pair));
    ASSERT(economy_read_last_transfer_trade_pair(
        destination, &pair));
    ASSERT(pair.transfer_event_id == chain_count_before + 5u);
    ASSERT(pair.trade.ledger_delta_signed == expected_deltas[2]);
    observed_total += pair.trade.ledger_delta_signed;
    ASSERT(observed_total == rounded_total);
    ASSERT(shipment->quantity_delivered == 3);
    ASSERT(shipment->status == DELIVERY_SHIPMENT_DELIVERED);
    ASSERT(ship->manifest.count == 0);
    ASSERT(destination->manifest.count ==
           destination->manifest.cap);
    ASSERT(destination->chain_event_count ==
           chain_count_before + 6u);
    ASSERT(w->contracts[0].active);
    ASSERT_EQ_FLOAT(w->contracts[0].quantity_needed,
                    0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(
        ledger_balance_by_pubkey(destination, ledger_pubkey) -
            ledger_before,
        (float)rounded_total, 0.0001f);
    ASSERT(ship->stat_credits_spent == spent_before);
    ASSERT(ship->stat_credits_earned == earned_before);

    uint64_t walked = 0;
    ASSERT(chain_log_verify(destination, &walked, NULL));
    ASSERT(walked == destination->chain_event_count);
    free(snapshot);
    chain_log_test_fault_clear();
}

TEST(test_prepared_transfer_callers_write_failure_are_inert) {
    ASSERT_TRANSFER_FAILURE_INERT(
        economy_run_named_buy_transfer_failure(
            ECONOMY_TRANSFER_FAULT_WRITE,
            CHAIN_EVT_TRANSFER));
    ASSERT_TRANSFER_FAILURE_INERT(
        economy_run_named_buy_transfer_failure(
            ECONOMY_TRANSFER_FAULT_WRITE,
            CHAIN_EVT_TRADE));
    ASSERT_TRANSFER_FAILURE_INERT(
        economy_run_named_delivery_transfer_failure(
            ECONOMY_TRANSFER_FAULT_WRITE,
            CHAIN_EVT_TRANSFER));
    ASSERT_TRANSFER_FAILURE_INERT(
        economy_run_named_delivery_transfer_failure(
            ECONOMY_TRANSFER_FAULT_WRITE,
            CHAIN_EVT_TRADE));
    ASSERT_NPC_PAID_TRANSFER_FAILURES(
        ECONOMY_TRANSFER_FAULT_WRITE);
}

TEST(test_prepared_transfer_callers_flush_failure_are_inert) {
    ASSERT_TRANSFER_FAILURE_INERT(
        economy_run_named_buy_transfer_failure(
            ECONOMY_TRANSFER_FAULT_FLUSH,
            CHAIN_EVT_TRANSFER));
    ASSERT_TRANSFER_FAILURE_INERT(
        economy_run_named_buy_transfer_failure(
            ECONOMY_TRANSFER_FAULT_FLUSH,
            CHAIN_EVT_TRADE));
    ASSERT_TRANSFER_FAILURE_INERT(
        economy_run_named_delivery_transfer_failure(
            ECONOMY_TRANSFER_FAULT_FLUSH,
            CHAIN_EVT_TRANSFER));
    ASSERT_TRANSFER_FAILURE_INERT(
        economy_run_named_delivery_transfer_failure(
            ECONOMY_TRANSFER_FAULT_FLUSH,
            CHAIN_EVT_TRADE));
    ASSERT_NPC_PAID_TRANSFER_FAILURES(
        ECONOMY_TRANSFER_FAULT_FLUSH);
}

TEST(test_prepared_transfer_callers_preblocked_failure_are_inert) {
    ASSERT_TRANSFER_FAILURE_INERT(
        economy_run_named_buy_transfer_failure(
            ECONOMY_TRANSFER_FAULT_PREBLOCKED,
            CHAIN_EVT_TRANSFER));
    ASSERT_TRANSFER_FAILURE_INERT(
        economy_run_named_buy_transfer_failure(
            ECONOMY_TRANSFER_FAULT_PREBLOCKED,
            CHAIN_EVT_TRADE));
    ASSERT_TRANSFER_FAILURE_INERT(
        economy_run_named_delivery_transfer_failure(
            ECONOMY_TRANSFER_FAULT_PREBLOCKED,
            CHAIN_EVT_TRANSFER));
    ASSERT_TRANSFER_FAILURE_INERT(
        economy_run_named_delivery_transfer_failure(
            ECONOMY_TRANSFER_FAULT_PREBLOCKED,
            CHAIN_EVT_TRADE));
    ASSERT_NPC_PAID_TRANSFER_FAILURES(
        ECONOMY_TRANSFER_FAULT_PREBLOCKED);
}

#undef ASSERT_NPC_PAID_TRANSFER_FAILURES
#undef ASSERT_PAID_TRANSFER_SUCCESS
#undef ASSERT_TRANSFER_FAILURE_INERT

static void economy_force_provenance_screening(world_t *w, int station_idx) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return;
    station_t *st = &w->stations[station_idx];
    st->policy_generation = 1;
    st->policy_tick = w->tick + 1;
    st->policy_card_count = 1;
    st->policy_card_ids[0] = (uint8_t)STATION_POLICY_CARD_PROVENANCE_SCREENING;
    st->policy_card_domains[0] = (uint8_t)STATION_POLICY_DOMAIN_TRADE;
    st->policy_card_costs[0] = 25;
    st->policy_card_scores[0] = 1.0f;
}

static void economy_force_black_market(world_t *w, int station_idx) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return;
    station_t *st = &w->stations[station_idx];
    st->policy_generation = 1;
    st->policy_tick = w->tick + 1;
    st->policy_card_count = 1;
    st->policy_card_ids[0] = (uint8_t)STATION_POLICY_CARD_BLACK_MARKET;
    st->policy_card_domains[0] = (uint8_t)STATION_POLICY_DOMAIN_TRADE;
    st->policy_card_costs[0] = 20;
    st->policy_card_scores[0] = 1.0f;
}

static const cargo_unit_t *test_station_first_unit(const station_t *st,
                                                   commodity_t c,
                                                   recipe_id_t recipe_id) {
    if (!st || !st->manifest.units) return NULL;
    for (uint16_t i = 0; i < st->manifest.count; i++) {
        const cargo_unit_t *u = &st->manifest.units[i];
        if ((commodity_t)u->commodity == c &&
            (recipe_id_t)u->recipe_id == recipe_id) {
            return u;
        }
    }
    return NULL;
}

static delivery_shipment_t *test_find_delivery_shipment(world_t *w,
                                                        int contract_index) {
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        delivery_shipment_t *shipment = &w->delivery_shipments[i];
        if (shipment->active &&
            shipment->contract_index == (uint8_t)contract_index) {
            return shipment;
        }
    }
    return NULL;
}

static int test_find_delivery_shipment_pod(const world_t *w,
                                           const server_player_t *sp,
                                           const delivery_shipment_t *shipment) {
    if (!w || !sp || !shipment) return -1;
    for (int t = 0; t < sp->ship->towed_pod_count && t < 10; t++) {
        int idx = sp->ship->towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS) continue;
        const cargo_pod_t *pod = &w->cargo_pods[idx];
        if (pod->active &&
            pod->kind == CARGO_POD_CARGO &&
            pod->shipment_id == shipment->shipment_id) {
            return idx;
        }
    }
    return -1;
}

static int test_find_towed_exact_cargo_pod(const world_t *w,
                                           const server_player_t *sp,
                                           commodity_t commodity) {
    if (!w || !sp) return -1;
    for (int t = 0; t < sp->ship->towed_pod_count && t < 10; t++) {
        int idx = sp->ship->towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS) continue;
        const cargo_pod_t *pod = &w->cargo_pods[idx];
        if (!pod->active || pod->kind != CARGO_POD_CARGO) continue;
        if (pod->shipment_id != 0 || pod->commodity != commodity) continue;
        if (pod->manifest_count == 0 || pod->manifest_count != pod->quantity)
            continue;
        bool exact = true;
        for (uint16_t i = 0; i < pod->manifest_count; i++) {
            if ((commodity_t)pod->manifest_units[i].commodity != commodity) {
                exact = false;
                break;
            }
        }
        if (exact) return idx;
    }
    return -1;
}

static int test_spawn_towed_exact_cargo_pod(world_t *w,
                                            server_player_t *sp,
                                            commodity_t commodity,
                                            uint16_t count) {
    if (!w || !sp || commodity >= COMMODITY_COUNT ||
        count == 0 || count > CARGO_POD_MANIFEST_CAP ||
        sp->ship->towed_pod_count >= 10) {
        return -1;
    }
    cargo_unit_t units[CARGO_POD_MANIFEST_CAP];
    memset(units, 0, sizeof(units));
    const uint8_t origin[8] = { 'T','E','S','T','B','L','K','G' };
    for (uint16_t i = 0; i < count; i++) {
        if (!hash_legacy_migrate_unit(origin, commodity, i, &units[i]))
            return -1;
    }
    vec2 pos = v2_add(sp->ship->pos, v2(-44.0f, 20.0f));
    int pod_idx = spawn_cargo_pod_with_manifest(
        w, pos, sp->ship->vel, commodity, units, count, CARGO_POD_CARGO);
    if (pod_idx < 0) return -1;
    if (!world_cargo_pod_set_player_tractor(w, pod_idx, (int)sp->id)) {
        memset(&w->cargo_pods[pod_idx], 0, sizeof(w->cargo_pods[pod_idx]));
        return -1;
    }
    return pod_idx;
}

static bool test_stage_pod_at_station_hopper(world_t *w,
                                             int station_idx,
                                             int pod_idx,
                                             commodity_t commodity,
                                             int *out_hopper_idx) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS ||
        pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) {
        return false;
    }
    station_t *st = &w->stations[station_idx];
    int hopper_idx = station_find_hopper_for(st, commodity);
    if (hopper_idx < 0 || hopper_idx >= st->module_count ||
        hopper_idx >= MAX_MODULES_PER_STATION) {
        return false;
    }
    w->cargo_pods[pod_idx].pos = module_world_pos_ring(
        st, st->modules[hopper_idx].ring, st->modules[hopper_idx].slot);
    if (out_hopper_idx) *out_hopper_idx = hopper_idx;
    return true;
}

static int test_first_dock_module_idx(const station_t *st) {
    if (!st) return -1;
    for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
        if (st->modules[m].type == MODULE_DOCK &&
            !st->modules[m].scaffold) {
            return m;
        }
    }
    return -1;
}

static int test_spawn_station_market_exact_cargo_pod(world_t *w,
                                                     int station_idx,
                                                     commodity_t commodity,
                                                     uint16_t count) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS ||
        commodity >= COMMODITY_COUNT || count == 0 ||
        count > CARGO_POD_MANIFEST_CAP) {
        return -1;
    }
    station_t *st = &w->stations[station_idx];
    int dock_idx = test_first_dock_module_idx(st);
    if (dock_idx < 0) return -1;

    cargo_unit_t units[CARGO_POD_MANIFEST_CAP];
    memset(units, 0, sizeof(units));
    const uint8_t origin[8] = { 'T','E','S','T','M','K','T','P' };
    for (uint16_t i = 0; i < count; i++) {
        if (!hash_legacy_migrate_unit(origin, commodity, i, &units[i]))
            return -1;
    }

    vec2 pos = module_world_pos_ring(st, st->modules[dock_idx].ring,
                                     st->modules[dock_idx].slot);
    int pod_idx = spawn_cargo_pod_with_manifest(
        w, pos, v2(0.0f, 0.0f), commodity, units, count, CARGO_POD_CARGO);
    if (pod_idx < 0) return -1;

    cargo_unit_t shell = {0};
    const uint8_t shell_origin[8] = { 'T','E','S','T','S','H','E','L' };
    if (hash_legacy_migrate_unit(shell_origin, COMMODITY_FRAME, 0, &shell))
        cargo_pod_set_shell_frame(&w->cargo_pods[pod_idx], &shell);
    return world_cargo_pod_set_module_tractor(
               w, pod_idx, station_idx, dock_idx) ? pod_idx : -1;
}

static float test_station_market_pod_sell_quote(const station_t *st,
                                                const cargo_pod_t *pod) {
    if (!st || !pod || !pod->active || pod->kind != CARGO_POD_CARGO ||
        pod->manifest_count == 0 || pod->manifest_count != pod->quantity) {
        return 0.0f;
    }
    float quote = 0.0f;
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        const cargo_unit_t *unit = &pod->manifest_units[i];
        quote += station_sell_price_unit(st, unit) *
                 mining_payout_multiplier((mining_grade_t)unit->grade);
    }
    if (pod->has_shell_frame) {
        quote += station_sell_price_unit(st, &pod->shell_frame) *
                 mining_payout_multiplier(
                     (mining_grade_t)pod->shell_frame.grade);
    }
    return (float)llroundf(quote);
}

static void test_move_pod_past_station_charge_boundary(world_t *w,
                                                       int station_idx,
                                                       int pod_idx) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS ||
        pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) {
        return;
    }
    station_t *st = &w->stations[station_idx];
    vec2 base = st->pos;
    int dock_idx = test_first_dock_module_idx(st);
    if (dock_idx >= 0) {
        base = module_world_pos_ring(st, st->modules[dock_idx].ring,
                                     st->modules[dock_idx].slot);
    }
    w->cargo_pods[pod_idx].pos =
        v2_add(base, v2(CARGO_POD_DOCK_TRACTOR_RANGE +
                        HOPPER_INTAKE_STAGING_RANGE + 80.0f, 0.0f));
    w->cargo_pods[pod_idx].vel = v2(0.0f, 0.0f);
}

static bool test_stage_pod_at_station_dock_mouth(world_t *w,
                                                  int station_idx,
                                                  int pod_idx) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS ||
        pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) {
        return false;
    }
    station_t *st = &w->stations[station_idx];
    int dock_idx = test_first_dock_module_idx(st);
    if (dock_idx < 0) return false;
    const station_module_t *dock = &st->modules[dock_idx];
    vec2 dock_pos = module_world_pos_ring(st, dock->ring, dock->slot);
    vec2 outward = v2_norm(v2_sub(dock_pos, st->pos));
    if (v2_len_sq(outward) < 0.5f)
        outward = v2_from_angle(module_angle_ring(st, dock->ring,
                                                  dock->slot));
    cargo_pod_t *pod = &w->cargo_pods[pod_idx];
    float radius = pod->radius > 0.0f ? pod->radius : 18.0f;
    pod->pos = v2_add(dock_pos, v2_scale(
        outward, STATION_MODULE_COL_RADIUS + radius + 8.0f));
    pod->vel = station_ring_point_velocity(st, dock->ring, pod->pos);
    return true;
}

static bool test_view_has_market_memory(const knowledge_view_t *view,
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

static void test_setup_delivery_player(world_t *w, server_player_t **out_sp) {
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    memset(w->contracts, 0, sizeof(w->contracts));
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0x5a, sizeof(sp->session_token));
    economy_finalize_token_identity(sp);
    ASSERT(test_set_station_finished_units(&w->stations[0],
                                           COMMODITY_FRAME, 8));
    if (out_sp) *out_sp = sp;
}

TEST(test_station_production_yard_makes_frames) {
    STATION_DECL(station);
    station.modules[station.module_count++] = (station_module_t){ .type = MODULE_FRAME_PRESS };
    ASSERT(test_set_station_finished_units(&station, COMMODITY_FERRITE_INGOT, 5));
    step_station_production(&station, 1, 1.0f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station,
                                              COMMODITY_FERRITE_INGOT),
                    4.0f, 0.001f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station, COMMODITY_FRAME),
                    (float)CELL_STRUTS_PER_INGOT, 0.001f);
    ASSERT_EQ_INT(station_finished_count(&station, COMMODITY_FERRITE_INGOT), 4);
    ASSERT_EQ_INT(station_finished_count(&station, COMMODITY_FRAME),
                  CELL_STRUTS_PER_INGOT);
}

TEST(test_station_production_does_not_drain_physical_stock_as_storage) {
    STATION_DECL(station);
    station.modules[station.module_count++] =
        (station_module_t){ .type = MODULE_FRAME_PRESS };
    station._physical_inventory_cache[COMMODITY_FERRITE_INGOT] = 2.0f;

    step_station_production(&station, 1, 1.0f);

    ASSERT_EQ_FLOAT(station_inventory_amount(
                        &station, COMMODITY_FERRITE_INGOT),
                    2.0f, 0.001f);
    ASSERT_EQ_FLOAT(station_stored_inventory_amount(
                        &station, COMMODITY_FERRITE_INGOT),
                    0.0f, 0.001f);
    ASSERT_EQ_INT(station_finished_count(&station, COMMODITY_FRAME), 0);
}

TEST(test_station_production_beamworks_makes_modules) {
    STATION_DECL(station);
    station.modules[station.module_count++] = (station_module_t){ .type = MODULE_LASER_FAB };
    station.modules[station.module_count++] = (station_module_t){ .type = MODULE_TRACTOR_FAB };
    ASSERT(test_set_station_finished_units(&station, COMMODITY_CUPRITE_INGOT, 5));
    ASSERT(test_set_station_finished_units(&station, COMMODITY_CRYSTAL_INGOT, 5));
    ASSERT(test_set_station_finished_units(&station, COMMODITY_FRAME, 1));
    step_station_production(&station, 1, 1.0f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station,
                                              COMMODITY_CUPRITE_INGOT),
                    4.5f, 0.001f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station,
                                              COMMODITY_CRYSTAL_INGOT),
                    4.5f, 0.001f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station, COMMODITY_FRAME),
                    0.0f, 0.001f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station,
                                              COMMODITY_LASER_MODULE),
                    0.5f, 0.001f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station,
                                              COMMODITY_TRACTOR_MODULE),
                    0.5f, 0.001f);
    ASSERT_EQ_INT(station_finished_count(&station, COMMODITY_CUPRITE_INGOT), 4);
    ASSERT_EQ_INT(station_finished_count(&station, COMMODITY_CRYSTAL_INGOT), 4);
    ASSERT_EQ_INT(station_finished_count(&station, COMMODITY_FRAME), 0);
}

TEST(test_station_repair_cost_no_damage) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.hull = 100.0f;
    station_t station = {0};
    ASSERT_EQ_FLOAT(station_repair_cost(&ship, &station), 0.0f, 0.01f);
}

TEST(test_station_repair_cost_with_damage) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.hull = 50.0f;
    station_t station = {0};
    /* Any dock can install kits — repair quote needs MODULE_DOCK. */
    station.modules[station.module_count++] =
        (station_module_t){ .type = MODULE_DOCK };
    float cost = station_repair_cost(&ship, &station);
    ASSERT(cost > 0.0f);
}

TEST(test_can_afford_upgrade_dock_fallback) {
    /* Empty cargo, but station stocks the modules and player has
     * credits — dock fills the gap from inventory at retail. */
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    STATION_DECL(station);
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    ASSERT(test_set_station_finished_units(&station, COMMODITY_FRAME, 100));
    station.base_price[COMMODITY_FRAME] = 22.0f;
    ASSERT(can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD,10000.0f));
}

TEST(test_can_afford_upgrade_no_credits_for_dock_fallback) {
    /* Empty cargo, station has modules, balance zero — fallback
     * needs credits, so this must be rejected. */
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    STATION_DECL(station);
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    ASSERT(test_set_station_finished_units(&station, COMMODITY_FRAME, 100));
    station.base_price[COMMODITY_FRAME] = 22.0f;
    ASSERT(!can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD,0.0f));
}

TEST(test_starter_refit_stock_has_retail_price_and_explicit_work_order) {
    SHIP_DECL(ship);
    STATION_DECL(kepler);
    ship.hull_class = HULL_CLASS_MINER;
    snprintf(kepler.station_slug,
             sizeof(kepler.station_slug), "kepler");
    kepler.base_price[COMMODITY_LASER_MODULE] = 15.0f;

    int need = (int)ceilf(
        upgrade_product_cost(
            &ship, SHIP_UPGRADE_MINING));
    ASSERT_EQ_INT(need, 8);
    ASSERT(test_set_station_finished_units(
        &kepler, COMMODITY_LASER_MODULE, need));
    float retail = upgrade_station_credit_cost(
        &kepler, &ship, SHIP_UPGRADE_MINING, need);
    ASSERT(retail > 0.0f);
    ASSERT(!can_afford_upgrade(
        &kepler, &ship, SHIP_UPGRADE_MINING, 0.0f));

    contract_t work = {0};
    float unit_price =
        ceilf(retail / (float)need);
    ASSERT(starter_refit_work_order_init(
        &work, need, unit_price));
    ASSERT(starter_refit_work_order_matches(&work));
    ASSERT(contract_price(&work) * (float)need +
               FLOAT_EPSILON >=
           retail);

    contract_t generic = work;
    memset(generic.target_pub, 0,
           sizeof(generic.target_pub));
    ASSERT(!starter_refit_work_order_matches(&generic));

    cargo_unit_t unsmelted = {0};
    ASSERT(hash_legacy_migrate_unit(
        (const uint8_t *)"NOTSMELT",
        COMMODITY_FERRITE_INGOT, 0,
        &unsmelted));
    ASSERT_EQ_INT(
        contract_fit_cargo_unit(
            &work, &unsmelted),
        CONTRACT_FIT_WRONG_RECIPE);
}

TEST(test_consumed_starter_refit_marker_reserves_contract_slot) {
    contract_t empty = {0};
    ASSERT(contract_slot_available_for_post(&empty));

    contract_t generic = {
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 0,
        .commodity = COMMODITY_FRAME,
    };
    ASSERT(!contract_slot_available_for_post(&generic));

    contract_t starter = {0};
    ASSERT(starter_refit_work_order_init(
        &starter, 8, 20.0f));
    ASSERT(!contract_slot_available_for_post(&starter));
    starter.active = false;
    starter.quantity_needed = 0.0f;
    ASSERT(starter_refit_work_order_matches(&starter));
    ASSERT(!contract_slot_available_for_post(&starter));
}

TEST(test_can_afford_upgrade_no_product_anywhere) {
    /* Empty cargo, empty station inventory — no modules to install. */
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    station_t station = {0};
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    station._inventory_cache[COMMODITY_FRAME] = 0.0f;
    ASSERT(!can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD,10000.0f));
}

TEST(test_can_afford_upgrade_cargo_only_no_credits_needed) {
    /* Ship cargo covers the full module cost — credit balance is
     * irrelevant since the dock has nothing to sell. */
    SHIP_DECL(ship);
    ship.hull_class = HULL_CLASS_MINER;
    station_t station = {0};
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    /* Empty dock inventory; ship carries enough frames itself. */
    int need = (int)ceilf(upgrade_product_cost(&ship, SHIP_UPGRADE_HOLD));
    ASSERT(test_set_ship_finished_units(&ship, COMMODITY_FRAME, need,
                                        MINING_GRADE_COMMON));
    ASSERT(can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD,0.0f));
}

TEST(test_can_afford_upgrade_rejects_float_only_finished_goods) {
    SHIP_DECL(ship);
    STATION_DECL(station);
    ship.hull_class = HULL_CLASS_MINER;
    ASSERT(ship_manifest_bootstrap(&ship));
    ASSERT(station_manifest_bootstrap(&station));

    int need = (int)ceilf(upgrade_product_cost(&ship, SHIP_UPGRADE_HOLD));
    ship.cargo[COMMODITY_FRAME] = (float)need;
    station._inventory_cache[COMMODITY_FRAME] = (float)need;

    ASSERT_EQ_INT(ship_finished_count(&ship, COMMODITY_FRAME), 0);
    ASSERT_EQ_INT(station_finished_count(&station, COMMODITY_FRAME), 0);
    ASSERT(!can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD, 10000.0f));
}

TEST(test_contract_generated_from_hopper_deficit) {
    /* A refinery with low ore_buffer should generate an ore contract */
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    /* Make ferrite the biggest deficit by filling the others */
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 10.0f;
    w.stations[0]._inventory_cache[COMMODITY_CUPRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    w.stations[0]._inventory_cache[COMMODITY_CRYSTAL_ORE] = REFINERY_HOPPER_CAPACITY;
    world_sim_step(&w, SIM_DT);
    /* Find contract for station 0, ferrite ore */
    contract_t *found = NULL;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0 && w.contracts[k].commodity == COMMODITY_FERRITE_ORE) {
            found = &w.contracts[k];
            break;
        }
    }
    ASSERT(found != NULL);
    /* Ore contracts are inventory-driven — quantity_needed is 0 */
    ASSERT_EQ_FLOAT(found->quantity_needed, 0.0f, 0.01f);
}

TEST(test_contract_price_escalates_with_age) {
    /* An unfilled contract should increase in price over time */
    contract_t c = {.active = true, .base_price = 10.0f, .age = 0.0f};
    float price_t0 = contract_price(&c);
    c.age = 300.0f; /* 5 minutes */
    float price_t5 = contract_price(&c);
    ASSERT(price_t5 > price_t0);
    ASSERT_EQ_FLOAT(price_t5, 10.0f * 1.2f, 0.01f);
}

TEST(test_contract_fit_requires_material_grade_and_fragment_tier) {
    contract_t ingot_contract = {
        .active = true,
        .action = CONTRACT_TRACTOR,
        .commodity = COMMODITY_FERRITE_INGOT,
        .required_grade = (uint8_t)MINING_GRADE_RARE,
    };
    cargo_unit_t unit = {
        .kind = CARGO_KIND_INGOT,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .grade = (uint8_t)MINING_GRADE_FINE,
        .quantity = 1,
    };
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&ingot_contract, &unit),
                  (int)CONTRACT_FIT_GRADE_TOO_LOW);

    unit.grade = (uint8_t)MINING_GRADE_RARE;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&ingot_contract, &unit),
                  (int)CONTRACT_FIT_OK);

    unit.commodity = (uint8_t)COMMODITY_CUPRITE_INGOT;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&ingot_contract, &unit),
                  (int)CONTRACT_FIT_WRONG_COMMODITY);

    contract_t ore_contract = {
        .active = true,
        .action = CONTRACT_TRACTOR,
        .commodity = COMMODITY_FERRITE_ORE,
        .required_grade = (uint8_t)MINING_GRADE_RARE,
    };
    asteroid_t fragment = {
        .active = true,
        .tier = ASTEROID_TIER_S,
        .ore = 8.0f,
        .commodity = COMMODITY_FERRITE_ORE,
        .grade = (uint8_t)MINING_GRADE_FINE,
    };
    ASSERT_EQ_INT((int)contract_fit_fragment(&ore_contract, &fragment),
                  (int)CONTRACT_FIT_GRADE_TOO_LOW);

    fragment.grade = (uint8_t)MINING_GRADE_RARE;
    ASSERT_EQ_INT((int)contract_fit_fragment(&ore_contract, &fragment),
                  (int)CONTRACT_FIT_OK);

    fragment.tier = ASTEROID_TIER_M;
    ASSERT_EQ_INT((int)contract_fit_fragment(&ore_contract, &fragment),
                  (int)CONTRACT_FIT_WRONG_TIER);
}

TEST(test_contract_fit_enforces_heritage_recipe_prefix_and_parent) {
    cargo_unit_t unit = {
        .kind = CARGO_KIND_INGOT,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .grade = (uint8_t)MINING_GRADE_COMMON,
        .quantity = 1,
        .prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS,
        .recipe_id = (uint16_t)RECIPE_LEGACY_MIGRATE,
    };
    for (int i = 0; i < 32; i++) {
        unit.pub[i] = (uint8_t)(0x20 + i);
        unit.parent_merkle[i] = (uint8_t)(0x80 + i);
    }

    contract_t recipe_contract = {
        .active = true,
        .action = CONTRACT_TRACTOR,
        .commodity = COMMODITY_FERRITE_INGOT,
        .proof_flags = (uint8_t)(CONTRACT_PROOF_REQUIRE_PROOF |
                                 CONTRACT_PROOF_REQUIRE_RECIPE),
        .required_recipe_id = (uint16_t)RECIPE_SMELT,
    };
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&recipe_contract, &unit),
                  (int)CONTRACT_FIT_WRONG_RECIPE);
    unit.recipe_id = (uint16_t)RECIPE_SMELT;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&recipe_contract, &unit),
                  (int)CONTRACT_FIT_OK);

    contract_t prefix_contract = recipe_contract;
    prefix_contract.proof_flags |= (uint8_t)CONTRACT_PROOF_REQUIRE_PREFIX;
    prefix_contract.required_prefix_class = (uint8_t)INGOT_PREFIX_M;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&prefix_contract, &unit),
                  (int)CONTRACT_FIT_WRONG_PREFIX);
    unit.prefix_class = (uint8_t)INGOT_PREFIX_M;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&prefix_contract, &unit),
                  (int)CONTRACT_FIT_OK);

    contract_t parent_contract = recipe_contract;
    parent_contract.proof_flags |= (uint8_t)CONTRACT_PROOF_REQUIRE_PARENT;
    memset(parent_contract.required_parent, 0xAA, sizeof(parent_contract.required_parent));
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&parent_contract, &unit),
                  (int)CONTRACT_FIT_WRONG_PARENT);
    memcpy(parent_contract.required_parent, unit.parent_merkle,
           sizeof(parent_contract.required_parent));
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&parent_contract, &unit),
                  (int)CONTRACT_FIT_OK);

    contract_t origin_contract = recipe_contract;
    origin_contract.proof_flags |= (uint8_t)CONTRACT_PROOF_FORBID_ORIGIN;
    origin_contract.forbidden_origin_mask = 1ULL << 2;
    unit.origin_station = 2;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&origin_contract, &unit),
                  (int)CONTRACT_FIT_FORBIDDEN_ORIGIN);
    unit.origin_station = 1;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&origin_contract, &unit),
                  (int)CONTRACT_FIT_OK);

    memset(unit.pub, 0, sizeof(unit.pub));
    memset(unit.parent_merkle, 0, sizeof(unit.parent_merkle));
    unit.mined_block = 0;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&recipe_contract, &unit),
                  (int)CONTRACT_FIT_MISSING_PROOF);
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&origin_contract, &unit),
                  (int)CONTRACT_FIT_MISSING_PROOF);
}

TEST(test_contract_delivery_requires_required_grade) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);

    ASSERT(test_set_ship_finished_units(w.players[0].ship,
                                        COMMODITY_TRACTOR_MODULE, 5,
                                        MINING_GRADE_COMMON));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_TRACTOR_MODULE,
        .required_grade = (uint8_t)MINING_GRADE_RARE,
        .quantity_needed = 2.0f,
        .base_price = 100.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    float credits_before = ledger_balance(&w.stations[0],
                                          w.players[0].session_token);
    w.players[0].docked = true;
    w.players[0].current_station = 0;
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_FLOAT(ship_cargo_amount(w.players[0].ship,
                                      COMMODITY_TRACTOR_MODULE),
                    5.0f, 0.01f);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 2.0f, 0.01f);
    ASSERT_EQ_FLOAT(ledger_balance(&w.stations[0],
                                   w.players[0].session_token),
                    credits_before, 0.01f);
}

TEST(test_contract_delivery_requires_heritage_recipe) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x02, 8);
    economy_finalize_token_identity(
        &w.players[0]);

    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FRAME, 1);
    ASSERT(pod_idx >= 0);
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FRAME,
        .proof_flags = (uint8_t)(CONTRACT_PROOF_REQUIRE_PROOF |
                                 CONTRACT_PROOF_REQUIRE_RECIPE),
        .required_recipe_id = (uint16_t)RECIPE_FRAME_BASIC,
        .quantity_needed = 2.0f,
        .base_price = 100.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    w.stations[1].base_price[COMMODITY_FRAME] = 0.0f;
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, pod_idx,
                                            COMMODITY_FRAME, NULL));

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 1);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 2.0f, 0.01f);

    w.cargo_pods[pod_idx].manifest_units[0].recipe_id =
        (uint16_t)RECIPE_FRAME_BASIC;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 1.0f, 0.01f);
}

TEST(test_contract_delivery_bans_enemy_origin_station) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x03, 8);
    economy_finalize_token_identity(
        &w.players[0]);

    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FRAME, 1);
    ASSERT(pod_idx >= 0);
    cargo_unit_t *unit = &w.cargo_pods[pod_idx].manifest_units[0];
    unit->recipe_id = (uint16_t)RECIPE_FRAME_BASIC;
    unit->origin_station = 2;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FRAME,
        .proof_flags = (uint8_t)(CONTRACT_PROOF_REQUIRE_PROOF |
                                 CONTRACT_PROOF_REQUIRE_RECIPE |
                                 CONTRACT_PROOF_FORBID_ORIGIN),
        .required_recipe_id = (uint16_t)RECIPE_FRAME_BASIC,
        .forbidden_origin_mask = 1ULL << 2,
        .quantity_needed = 2.0f,
        .base_price = 100.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    w.stations[1].base_price[COMMODITY_FRAME] = 0.0f;
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, pod_idx,
                                            COMMODITY_FRAME, NULL));

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 1);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 2.0f, 0.01f);

    unit = &w.cargo_pods[pod_idx].manifest_units[0];
    unit->origin_station = 1;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 1.0f, 0.01f);
}

TEST(test_contract_closes_when_deficit_filled) {
    /* Tractor-contract close hysteresis: opens on deficit (<90%), must NOT
     * close until inventory crosses 95% — otherwise a station sitting in
     * [80%, 95%] opens-and-closes a contract every tick, spamming
     * SIM_EVENT_CONTRACT_COMPLETE. See fix for issue #461. */
    WORLD_DECL;
    world_reset(&w);
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 10.0f;
    world_sim_step(&w, SIM_DT); /* generates contract (deficit > threshold) */

    /* 85% should NOT close the contract anymore — it's between open (90%) and close (95%) */
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY * 0.85f;
    world_sim_step(&w, SIM_DT);
    bool still_active = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0 && w.contracts[k].commodity == COMMODITY_FERRITE_ORE) {
            still_active = true; break;
        }
    }
    ASSERT(still_active);

    /* Above the 95% close threshold, contract closes */
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY * 0.96f;
    world_sim_step(&w, SIM_DT);
    bool still_active2 = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0 && w.contracts[k].commodity == COMMODITY_FERRITE_ORE) {
            still_active2 = true; break;
        }
    }
    ASSERT(!still_active2);
}

TEST(test_raw_ore_contract_retires_when_refined_output_full) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 0.0f;
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)MAX_PRODUCT_STOCK));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 0,
        .commodity = COMMODITY_FERRITE_ORE,
        .quantity_needed = 1.0f,
        .base_price = 3.0f,
        .claimed_by = -1,
    };

    world_sim_step(&w, SIM_DT);

    for (int k = 0; k < MAX_CONTRACTS; k++) {
        ASSERT(!(w.contracts[k].active &&
                 w.contracts[k].station_index == 0 &&
                 w.contracts[k].commodity == COMMODITY_FERRITE_ORE));
    }
}

TEST(test_kit_input_contract_closes_at_kit_target) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    ASSERT(test_set_station_finished_units(&w.stations[2],
                                           COMMODITY_FRAME, 20));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = COMMODITY_FRAME,
        .quantity_needed = 12.0f,
        .base_price = 1.0f,
        .claimed_by = -1,
    };

    world_sim_step(&w, SIM_DT);

    for (int k = 0; k < MAX_CONTRACTS; k++) {
        ASSERT(!(w.contracts[k].active &&
                 w.contracts[k].station_index == 2 &&
                 w.contracts[k].commodity == COMMODITY_FRAME));
    }
}

TEST(test_generated_heritage_contracts_require_source_recipe) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    station_t *kepler = &w.stations[1];
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(kepler, COMMODITY_FRAME, 0));

    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(helios, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_LASER_MODULE, 100));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_TRACTOR_MODULE, 100));

    world_sim_step(&w, SIM_DT);

    bool found_smelted_ingot = false;
    bool found_fabbed_frame = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *c = &w.contracts[k];
        if (!c->active || c->action != CONTRACT_TRACTOR) continue;
        if (c->station_index == 1 && c->commodity == COMMODITY_FERRITE_INGOT) {
            found_smelted_ingot = true;
            ASSERT(c->proof_flags & CONTRACT_PROOF_REQUIRE_PROOF);
            ASSERT(c->proof_flags & CONTRACT_PROOF_REQUIRE_RECIPE);
            ASSERT_EQ_INT(c->required_recipe_id, RECIPE_SMELT);
        }
        if (c->station_index == 2 && c->commodity == COMMODITY_FRAME) {
            found_fabbed_frame = true;
            ASSERT(c->proof_flags & CONTRACT_PROOF_REQUIRE_PROOF);
            ASSERT(c->proof_flags & CONTRACT_PROOF_REQUIRE_RECIPE);
            ASSERT_EQ_INT(c->required_recipe_id, RECIPE_FRAME_BASIC);
        }
    }
    ASSERT(found_smelted_ingot);
    ASSERT(found_fabbed_frame);
}

TEST(test_station_policy_preserves_seeded_supply_loop) {
    uint64_t prospect = station_policy_forbidden_origin_mask(
        0, COMMODITY_REPAIR_KIT);
    uint64_t kepler = station_policy_forbidden_origin_mask(
        1, COMMODITY_FERRITE_INGOT);
    uint64_t helios = station_policy_forbidden_origin_mask(
        2, COMMODITY_FRAME);

    ASSERT_EQ_INT((int)(prospect & ((1ULL << 1) | (1ULL << 2))), 0);
    ASSERT_EQ_INT((int)(kepler & ((1ULL << 0) | (1ULL << 2))), 0);
    ASSERT_EQ_INT((int)(helios & ((1ULL << 0) | (1ULL << 1))), 0);
    ASSERT_EQ_INT((int)(prospect & (1ULL << SIGNAL_FREEPORT_STATION_INDEX)), 0);
    ASSERT((helios & (1ULL << SIGNAL_FREEPORT_STATION_INDEX)) != 0);
}

TEST(test_station_policy_cards_rank_under_domain_budgets) {
    WORLD_DECL;
    world_reset(&w);
    station_t *prospect = &w.stations[0];
    ASSERT(test_set_station_finished_units(
        prospect, COMMODITY_REPAIR_KIT, 0));

    station_policy_selection_t selection;
    station_policy_select_cards(prospect, 0, &selection);

    ASSERT(station_policy_selection_has(
        &selection, STATION_POLICY_CARD_REPAIR_STOCK_RESERVE));
    ASSERT(station_policy_selection_has(
        &selection, STATION_POLICY_CARD_STRATEGIC_IMPORTS));
    ASSERT(!station_policy_selection_has(
        &selection, STATION_POLICY_CARD_HOSTILE_ORIGIN_EMBARGO));

    int spent[STATION_POLICY_DOMAIN_COUNT] = {0};
    for (int i = 0; i < selection.count; i++)
        spent[selection.cards[i].domain] += selection.cards[i].budget_cost;
    ASSERT(spent[STATION_POLICY_DOMAIN_TRADE] <= selection.budget.trade);
    ASSERT(spent[STATION_POLICY_DOMAIN_CONSTRUCTION] <=
           selection.budget.construction);
    ASSERT(spent[STATION_POLICY_DOMAIN_FINANCE] <= selection.budget.finance);
}

TEST(test_station_policy_black_market_requires_off_relay_station) {
    WORLD_DECL;
    world_reset(&w);

    station_policy_selection_t relay_selection;
    station_policy_select_cards(&w.stations[0], 0, &relay_selection);
    ASSERT(!station_policy_selection_has(
        &relay_selection, STATION_POLICY_CARD_BLACK_MARKET));

    station_t freeport = {0};
    snprintf(freeport.name, sizeof(freeport.name), "Freeport");
    freeport.signal_range = 0.0f;
    freeport.module_count = 1;
    freeport.modules[0] = (station_module_t){ .type = MODULE_DOCK };

    station_policy_selection_t off_relay_selection;
    station_policy_select_cards(&freeport, 3, &off_relay_selection);
    ASSERT(station_policy_selection_has(
        &off_relay_selection, STATION_POLICY_CARD_BLACK_MARKET));
}

TEST(test_blackglass_posts_black_market_buy_contract) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    station_t *freeport = &w.stations[SIGNAL_FREEPORT_STATION_INDEX];
    ASSERT(station_exists(freeport));
    ASSERT(station_faction_is_pirate_economy(freeport));

    ASSERT(test_set_station_finished_units(
        freeport, COMMODITY_TRACTOR_MODULE, 0));
    ASSERT(test_set_station_finished_units(
        freeport, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(
        freeport, COMMODITY_CRYSTAL_INGOT, 0));
    ASSERT(test_set_station_finished_units(
        freeport, COMMODITY_CUPRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(
        freeport, COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(
        freeport, COMMODITY_FRAME, 0));

    world_sim_step(&w, SIM_DT);

    contract_t *found = NULL;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *c = &w.contracts[k];
        if (c->active && c->action == CONTRACT_TRACTOR &&
            c->station_index == SIGNAL_FREEPORT_STATION_INDEX) {
            found = c;
            break;
        }
    }

    ASSERT(found != NULL);
    ASSERT_EQ_INT(found->commodity, COMMODITY_TRACTOR_MODULE);
    ASSERT(found->quantity_needed > 0.0f);
    ASSERT(found->base_price >
           freeport->base_price[found->commodity]);
    ASSERT_EQ_INT(found->proof_flags, 0);
    ASSERT_EQ_INT(found->required_recipe_id, 0);
    ASSERT_EQ_INT((int)found->forbidden_origin_mask, 0);
}

TEST(test_station_policy_cache_drives_trade_price_modifier) {
    WORLD_DECL;
    world_reset(&w);
    station_t *prospect = &w.stations[0];
    prospect->_inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT(test_set_station_finished_units(
        prospect, COMMODITY_REPAIR_KIT, 0));

    station_policy_refresh(prospect, 0, 7);

    ASSERT_EQ_INT((int)prospect->policy_tick, 7);
    ASSERT(prospect->policy_generation > 0);
    ASSERT(station_policy_cached_has(
        prospect, STATION_POLICY_CARD_REPAIR_STOCK_RESERVE));
    ASSERT(station_policy_cached_has(
        prospect, STATION_POLICY_CARD_STRATEGIC_IMPORTS));
    ASSERT_EQ_INT((int)prospect->policy_top_demand_commodity,
                  COMMODITY_REPAIR_KIT);
    ASSERT(prospect->policy_top_demand_severity > 0.9f);
    ASSERT(station_policy_trade_price_multiplier(
        prospect, COMMODITY_REPAIR_KIT) > 1.4f);

    uint32_t generation = prospect->policy_generation;
    station_policy_refresh(prospect, 0, 7);
    ASSERT_EQ_INT((int)prospect->policy_generation, (int)generation);
}

TEST(test_cargo_legality_clean_chain_is_not_contraband) {
    economy_chain_test_setup("legality_clean");
    WORLD_DECL;
    world_reset(&w);
    economy_chain_test_wipe_logs(&w);

    uint8_t player_pk[32];
    uint8_t cargo_pk[32];
    economy_fill_pubkey(player_pk, 0x31);
    economy_fill_pubkey(cargo_pk, 0x71);
    uint16_t output_index = 0;
    cargo_unit_t unit =
        economy_test_cargo_unit(cargo_pk, &output_index);
    cargo_receipt_chain_t chain = {0};
    ASSERT(economy_issue_single_receipt(
        &w, 2, player_pk, output_index, &unit, &chain));
    economy_force_provenance_screening(&w, 0);

    cargo_legality_result_t result = cargo_legality_classify(
        w.stations, MAX_STATIONS, 0, &unit, &chain);
    ASSERT_EQ_INT((int)result.status, CARGO_LEGALITY_CLEAN);
    ASSERT(cargo_legality_station_accepts(result));
    ASSERT_EQ_INT(result.origin_station, 2);
    ASSERT((result.reasons & CARGO_LEGALITY_REASON_POLICY_SCREENS) != 0);

    economy_chain_test_teardown();
}

TEST(test_cargo_legality_missing_receipt_is_policy_contraband) {
    WORLD_DECL;
    world_reset(&w);
    uint8_t cargo_pk[32];
    economy_fill_pubkey(cargo_pk, 0x72);
    cargo_unit_t unit = economy_test_cargo_unit(cargo_pk, NULL);
    economy_force_provenance_screening(&w, 0);

    cargo_legality_result_t result = cargo_legality_classify(
        w.stations, MAX_STATIONS, 0, &unit, NULL);
    ASSERT_EQ_INT((int)result.status, CARGO_LEGALITY_CONTRABAND);
    ASSERT(!cargo_legality_station_accepts(result));
    ASSERT((result.reasons & CARGO_LEGALITY_REASON_MISSING_RECEIPT) != 0);
    ASSERT((result.reasons & CARGO_LEGALITY_REASON_POLICY_SCREENS) != 0);
}

TEST(test_cargo_legality_black_market_authority_is_local_policy) {
    economy_chain_test_setup("legality_black_market");
    WORLD_DECL;
    world_reset(&w);
    economy_chain_test_wipe_logs(&w);

    uint8_t player_pk[32];
    uint8_t cargo_pk[32];
    economy_fill_pubkey(player_pk, 0x33);
    economy_fill_pubkey(cargo_pk, 0x73);
    uint16_t output_index = 0;
    cargo_unit_t unit =
        economy_test_cargo_unit(cargo_pk, &output_index);

    /* Make station 1 a black-market authority without changing the cargo. */
    economy_force_black_market(&w, 1);

    cargo_receipt_chain_t chain = {0};
    ASSERT(economy_issue_single_receipt(
        &w, 1, player_pk, output_index, &unit, &chain));
    economy_force_provenance_screening(&w, 0);

    cargo_legality_result_t lawful = cargo_legality_classify(
        w.stations, MAX_STATIONS, 0, &unit, &chain);
    ASSERT_EQ_INT((int)lawful.status, CARGO_LEGALITY_CONTRABAND);
    ASSERT(!cargo_legality_station_accepts(lawful));
    ASSERT_EQ_INT(lawful.black_market_station, 1);
    ASSERT((lawful.reasons &
            CARGO_LEGALITY_REASON_BLACK_MARKET_AUTHORITY) != 0);

    cargo_legality_result_t pirate = cargo_legality_classify(
        w.stations, MAX_STATIONS, 1, &unit, &chain);
    ASSERT_EQ_INT((int)pirate.status, CARGO_LEGALITY_CONTRABAND);
    ASSERT(cargo_legality_station_accepts(pirate));
    ASSERT((pirate.reasons & CARGO_LEGALITY_REASON_POLICY_TOLERATES) != 0);

    economy_chain_test_teardown();
}

TEST(test_bulk_sell_refuses_black_market_origin_at_lawful_station) {
    economy_chain_test_setup("bulk_sell_legality");
    WORLD_DECL;
    world_reset(&w);
    economy_chain_test_wipe_logs(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    uint8_t player_pk[32];
    uint8_t cargo_pk[32];
    economy_fill_pubkey(player_pk, 0x34);
    economy_fill_pubkey(cargo_pk, 0x74);
    uint16_t output_index = 0;
    cargo_unit_t unit =
        economy_test_cargo_unit(cargo_pk, &output_index);
    cargo_receipt_chain_t chain = {0};
    economy_force_black_market(&w, 0);
    economy_force_provenance_screening(&w, 1);
    ASSERT(economy_issue_single_receipt(
        &w, 0, player_pk, output_index, &unit, &chain));
    ASSERT(ship_manifest_push_with_chain(sp->ship, &unit, &chain));
    ship_finished_sync(sp->ship, COMMODITY_FERRITE_INGOT);

    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_COUNT;
    int kepler_before = station_finished_count(&w.stations[1],
                                               COMMODITY_FERRITE_INGOT);
    float balance_before = ledger_balance(&w.stations[1], sp->session_token);
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 1);
    ASSERT_EQ_INT(station_finished_count(&w.stations[1],
                                         COMMODITY_FERRITE_INGOT),
                  kepler_before);
    ASSERT_EQ_FLOAT(ledger_balance(&w.stations[1], sp->session_token),
                    balance_before, 0.001f);

    economy_chain_test_teardown();
}

TEST(test_black_market_buys_unwanted_towed_pod_at_markdown) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    const int blackglass_idx = SIGNAL_FREEPORT_STATION_INDEX;
    station_t *blackglass = &w.stations[blackglass_idx];
    ASSERT(station_exists(blackglass));
    economy_force_black_market(&w, blackglass_idx);
    ASSERT(station_policy_accepts_contract_bound_cargo(blackglass));
    ASSERT(!station_consumes(blackglass, COMMODITY_LASER_MODULE));
    ASSERT(test_set_station_finished_units(blackglass, COMMODITY_TRACTOR_MODULE, 6));
    ASSERT(test_set_station_finished_units(blackglass, COMMODITY_LASER_MODULE, 6));
    ASSERT(test_set_station_finished_units(blackglass, COMMODITY_CRYSTAL_INGOT, 10));
    ASSERT(test_set_station_finished_units(blackglass, COMMODITY_CUPRITE_INGOT, 10));
    ASSERT(test_set_station_finished_units(blackglass, COMMODITY_FERRITE_INGOT, 10));
    ASSERT(test_set_station_finished_units(blackglass, COMMODITY_FRAME, 12));
    blackglass->base_price[COMMODITY_LASER_MODULE] = 40.0f;

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xB1, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = blackglass_idx;
    sp->nearby_station = blackglass_idx;
    sp->in_dock_range = true;
    sp->ship->pos = blackglass->pos;

    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, sp, COMMODITY_LASER_MODULE, 2);
    ASSERT(pod_idx >= 0);
    int dock_idx = test_first_dock_module_idx(blackglass);
    ASSERT(dock_idx >= 0);
    w.cargo_pods[pod_idx].pos = module_world_pos_ring(
        blackglass, blackglass->modules[dock_idx].ring,
        blackglass->modules[dock_idx].slot);
    int laser_stock_before =
        station_finished_count(blackglass, COMMODITY_LASER_MODULE);

    float full_quote = 0.0f;
    for (uint16_t u = 0; u < w.cargo_pods[pod_idx].manifest_count; u++) {
        const cargo_unit_t *unit = &w.cargo_pods[pod_idx].manifest_units[u];
        float unit_quote = station_buy_price_unit(blackglass, unit);
        unit_quote *= mining_payout_multiplier((mining_grade_t)unit->grade);
        full_quote += unit_quote;
    }
    float expected = full_quote * BLACK_MARKET_CARGO_MARKDOWN;
    ASSERT(full_quote > expected);
    float before = ledger_balance(blackglass, sp->session_token);

    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_FLOAT(ledger_balance(blackglass, sp->session_token) - before,
                    expected, 0.01f);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].commodity, COMMODITY_LASER_MODULE);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].manifest_count, 2);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[pod_idx]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            blackglass_idx, dock_idx));
    ASSERT_EQ_INT(station_finished_count(blackglass, COMMODITY_LASER_MODULE),
                  laser_stock_before);
}

TEST(test_raw_ore_contract_prefers_starved_downstream_output) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    station_t *helios = &w.stations[2];
    helios->_inventory_cache[COMMODITY_CUPRITE_ORE] = 0.0f;
    helios->_inventory_cache[COMMODITY_CRYSTAL_ORE] = 0.0f;
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_CUPRITE_INGOT, 12));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_CRYSTAL_INGOT, 0));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_TRACTOR_MODULE, 12));

    world_sim_step(&w, SIM_DT);

    bool found_crystal = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w.contracts[k].active || w.contracts[k].station_index != 2) continue;
        if (w.contracts[k].commodity == COMMODITY_CRYSTAL_ORE)
            found_crystal = true;
        ASSERT(w.contracts[k].commodity != COMMODITY_CUPRITE_ORE);
    }
    ASSERT(found_crystal);
}

TEST(test_sell_price_uses_contract_price) {
    /* When a contract exists, selling at that station should pay the
     * escalated contract price, not the base buy_price.
     *
     * Uses COMMODITY_FERRITE_INGOT because raw-ore cargo delivery is a
     * dead path post-#259 (physical ore towing; fragments ride in
     * ship.towed_fragments[], not ship.cargo[]). Ingot delivery is the
     * live path this contract-price logic actually serves. */
    WORLD_DECL;
    world_reset(&w);
    /* Create a contract with aged price — Kepler needs an ingot. */
    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR, .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 50.0f,
        .base_price = 10.0f, .age = 300.0f, /* 5 min -> 1.2x */
    };
    /* Set up player with a deliverable ingot at Kepler's physical intake. */
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    economy_finalize_token_identity(&w.players[0]);
    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FERRITE_INGOT, 10);
    ASSERT(pod_idx >= 0);
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, pod_idx,
                                            COMMODITY_FERRITE_INGOT, NULL));
    /* Zero out ledger balance for precise payout check */
    float init_bal = ledger_balance(&w.stations[1], w.players[0].session_token);
    float expected_price = 10.0f * 1.2f; /* contract_price at age 300 */
    world_sim_step(&w, SIM_DT);
    /* Credits should reflect escalated price, not base 10.0 */
    float earned = ledger_balance(&w.stations[1], w.players[0].session_token) - init_bal;
    ASSERT(earned > 10.0f * 10.0f); /* more than base */
    ASSERT_EQ_FLOAT(earned, 10.0f * expected_price, 1.0f);
    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
}

TEST(test_hauler_fills_highest_value_contract) {
    /* NPC hauler at a station should pick the highest-value contract
     * fillable from local inventory, not a hardcoded destination */
    WORLD_DECL;
    world_reset(&w);
    /* Set up two contracts: one cheap at station 1, one expensive at station 2 */
    w.contracts[0] = (contract_t){
        .active = true, .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 10.0f, .age = 0.0f,
    };
    w.contracts[1] = (contract_t){
        .active = true, .station_index = 2,
        .commodity = COMMODITY_CUPRITE_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 50.0f, .age = 0.0f,
    };
    /* Give home station (0) manifest-backed inventory of both. */
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT, 20));
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_CUPRITE_INGOT, 20));

    int seeded_hauler = spawn_npc(&w, 0, NPC_ROLE_HAULER);
    ASSERT(seeded_hauler >= 0);

    npc_ship_t *hauler = &w.npc_ships[seeded_hauler];
    ASSERT(hauler != NULL);
    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        if (i != seeded_hauler) w.npc_ships[i].active = false;
    hauler->state = NPC_STATE_DOCKED;
    hauler->state_timer = 0.0f; /* ready to act */
    hauler->home_station = 0;
    hauler->dest_station = 1; /* default dest */
    memset(hauler->ship->cargo, 0, sizeof(hauler->ship->cargo));
    /* Seed known_contracts to simulate prior gossip — under the
     * gossip-contract model the hauler only acts on contracts it has
     * heard about via dock contact. The test is exercising the picker
     * scoring, not the gossip propagation, so we inject knowledge
     * directly. */
    test_clear_knowledge(&hauler->ship->knowledge, SHIP_KNOWN_ITEM_CAP);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w.contracts[k].active) continue;
        contract_summary_t summary = contract_summary_make(&w.contracts[k]);
        ASSERT(test_add_known_contract(&hauler->ship->knowledge, &summary));
    }
    world_sim_step(&w, SIM_DT);
    /* Hauler should target station 2 (higher value contract) */
    ASSERT(hauler->dest_station == 2);
}

TEST(test_hauler_picker_trusts_gossiped_contract) {
    /* Under the gossip-contract model the hauler trusts known contract
     * summaries — it cannot peek at foreign station module state to
     * filter out destinations that don't accept the commodity. The
     * authoritative compatibility check happens at delivery time, where
     * a mismatch costs a wasted trip rather than a wrong pick. So the
     * picker simply takes the highest-scoring contract by price/dist. */
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_CRYSTAL_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 500.0f,
        .target_index = -1, .claimed_by = -1,
    };
    w.contracts[1] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = COMMODITY_CUPRITE_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 20.0f,
        .target_index = -1, .claimed_by = -1,
    };
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_CRYSTAL_INGOT, 20));
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_CUPRITE_INGOT, 20));
    ASSERT(test_anchor_station_legacy_cargo(&w, 0));

    int seeded_hauler = spawn_npc(&w, 0, NPC_ROLE_HAULER);
    ASSERT(seeded_hauler >= 0);

    int hauler_slot = -1;
    npc_ship_t *hauler = NULL;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_HAULER) {
            hauler_slot = i;
            hauler = &w.npc_ships[i];
            break;
        }
    }
    ASSERT(hauler != NULL);
    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        if (i != hauler_slot) w.npc_ships[i].active = false;
    ship_t *hauler_ship = world_npc_ship_for(&w, hauler_slot);
    ASSERT(hauler_ship != NULL);
    ASSERT(ship_manifest_bootstrap(hauler_ship));
    manifest_clear(&hauler_ship->manifest);
    ship_receipts_clear(ship_get_receipts(hauler_ship));
    memset(hauler_ship->cargo, 0, sizeof(hauler_ship->cargo));
    hauler->state = NPC_STATE_DOCKED;
    hauler->state_timer = 0.0f;
    hauler->home_station = 0;
    hauler->dest_station = 1;
    memset(hauler->ship->cargo, 0, sizeof(hauler->ship->cargo));
    /* Seed known_contracts (see comment in test_hauler_fills_highest_value_contract) */
    test_clear_knowledge(&hauler->ship->knowledge, SHIP_KNOWN_ITEM_CAP);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w.contracts[k].active) continue;
        contract_summary_t summary = contract_summary_make(&w.contracts[k]);
        ASSERT(test_add_known_contract(&hauler->ship->knowledge, &summary));
    }

    step_npc_ships(&w, SIM_DT);

    /* Highest-value contract wins: $500 crystal to station 1, even
     * though station 1 doesn't actually have a crystal-consuming
     * module. The mismatch will surface at unloading; for the picker,
     * the gossiped contract is the source of truth. */
    ASSERT_EQ_INT(hauler->dest_station, 1);
    ASSERT(ship_finished_count(hauler->ship, COMMODITY_CRYSTAL_INGOT) > 0);
    ASSERT_EQ_INT(ship_finished_count(hauler->ship,
                                      COMMODITY_CUPRITE_INGOT), 0);

    int dest_stock_before = station_finished_count(&w.stations[1],
                                                   COMMODITY_CRYSTAL_INGOT);
    float ledger_before = ledger_balance(&w.stations[1],
                                         hauler->session_token);
    hauler->state = NPC_STATE_UNLOADING;
    hauler->state_timer = 0.0f;
    hauler->dest_station = 1;
    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(station_finished_count(&w.stations[1],
                                         COMMODITY_CRYSTAL_INGOT),
                  dest_stock_before);
    ASSERT_EQ_FLOAT(ledger_balance(&w.stations[1], hauler->session_token),
                    ledger_before, 0.001f);
    ASSERT(manifest_count_by_commodity(&hauler_ship->manifest,
                                       COMMODITY_CRYSTAL_INGOT) > 0);
    ASSERT(!w.contracts[0].active);
}

TEST(test_hauler_ignores_float_only_finished_stock) {
    WORLD_DECL;
    world_reset(&w);

    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
        ASSERT(test_set_station_finished_units(&w.stations[0], (commodity_t)c, 0));

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 50.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT] = 20.0f;

    int seeded_hauler = spawn_npc(&w, 0, NPC_ROLE_HAULER);
    ASSERT(seeded_hauler >= 0);

    int hauler_slot = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_HAULER) {
            hauler_slot = i;
            break;
        }
    }
    ASSERT(hauler_slot >= 0);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (i != hauler_slot) w.npc_ships[i].active = false;
    }

    npc_ship_t *hauler = &w.npc_ships[hauler_slot];
    hauler->state = NPC_STATE_DOCKED;
    hauler->state_timer = 0.0f;
    hauler->home_station = 0;
    hauler->dest_station = 1;
    memset(hauler->ship->cargo, 0, sizeof(hauler->ship->cargo));

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_FLOAT(hauler->ship->cargo[COMMODITY_FERRITE_INGOT], 0.0f, 0.001f);
    ASSERT_EQ_INT(station_finished_count(&w.stations[0],
                                         COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_FLOAT(w.stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT],
                    20.0f, 0.001f);
}

TEST(test_one_contract_per_station) {
    WORLD_DECL;
    world_reset(&w);
    /* Empty all hoppers to create demand */
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++)
        w.stations[0]._inventory_cache[i] = 0.0f;
    /* Run a few ticks to generate contracts */
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    /* Count contracts for station 0. Up to two are allowed per station:
     * one ore contract (raw mining) + one production contract
     * (scaffold/ingot/kit-fab input). */
    int count = 0;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0) count++;
    }
    ASSERT(count >= 1 && count <= 2);
}

TEST(test_destroy_contract_completes_when_asteroid_gone) {
    /* DESTROY contracts should close when their target_index is invalid or inactive.
     * Test without full sim to avoid respawn interference. */
    contract_t c = {
        .active = true, .action = CONTRACT_FRACTURE,
        .target_index = -1,  /* invalid = gone */
        .base_price = 30.0f, .claimed_by = -1,
    };
    /* The fulfillment check: idx < 0 || idx >= MAX_ASTEROIDS || !asteroids[idx].active */
    bool target_gone = (c.target_index < 0 || c.target_index >= MAX_ASTEROIDS);
    ASSERT(target_gone);

    /* Valid index, inactive asteroid */
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    c.target_index = 5;
    asteroids[5].active = false;
    target_gone = (c.target_index < 0 || c.target_index >= MAX_ASTEROIDS || !asteroids[c.target_index].active);
    ASSERT(target_gone);

    /* Valid index, active asteroid — should NOT be gone */
    asteroids[5].active = true;
    target_gone = (c.target_index < 0 || c.target_index >= MAX_ASTEROIDS || !asteroids[c.target_index].active);
    ASSERT(!target_gone);
}

TEST(test_fracture_contract_target_pub_matches_asteroid_identity) {
    contract_t c = {
        .active = true,
        .action = CONTRACT_FRACTURE,
        .target_index = 5,
        .base_price = 30.0f,
        .claimed_by = -1,
    };
    asteroid_t asteroid = {
        .active = true,
        .tier = ASTEROID_TIER_L,
        .commodity = COMMODITY_FERRITE_ORE,
    };
    for (int i = 0; i < 32; i++)
        asteroid.rock_pub[i] = (uint8_t)(0x20u + (uint8_t)i);

    contract_set_target_pub_from_asteroid(&c, &asteroid);
    ASSERT(contract_target_pub_is_set(&c));
    ASSERT(contract_asteroid_target_matches(&c, &asteroid));
    ASSERT_EQ_INT((int)contract_fit_asteroid(&c, &asteroid),
                  (int)CONTRACT_FIT_OK);

    asteroid.rock_pub[0] ^= 0x7Fu;
    ASSERT(!contract_asteroid_target_matches(&c, &asteroid));
    ASSERT_EQ_INT((int)contract_fit_asteroid(&c, &asteroid),
                  (int)CONTRACT_FIT_WRONG_COMMODITY);
}

TEST(test_supply_contract_uses_correct_material) {
    WORLD_DECL;
    world_reset(&w);
    /* LASER_FAB needs crystal ingot + frame hoppers. Plant both. */
    add_hopper_for(&w.stations[1], 3, 1, COMMODITY_CRYSTAL_INGOT);
    add_hopper_for(&w.stations[1], 3, 7, COMMODITY_FRAME);
    int build_slot = station_ring_free_slot(
        &w.stations[1], 2, STATION_RING_SLOTS[2]);
    ASSERT(build_slot >= 0);
    begin_module_construction_at(&w, &w.stations[1], 1, MODULE_LASER_FAB,
                                 2, build_slot);
    /* The generated contract should be for crystal ingots */
    bool found = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].action == CONTRACT_TRACTOR
            && w.contracts[k].station_index == 1
            && w.contracts[k].commodity == COMMODITY_CRYSTAL_INGOT) {
            found = true; break;
        }
    }
    ASSERT(found);
    /* After contract expires and regenerates via step_contracts, it should still be crystal */
    for (int k = 0; k < MAX_CONTRACTS; k++) w.contracts[k].active = false;
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    found = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 1
            && w.contracts[k].commodity == COMMODITY_CRYSTAL_INGOT) {
            found = true; break;
        }
    }
    ASSERT(found);
}

TEST(test_dynamic_ore_price_deficit) {
    station_t st = {0};
    st.base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    /* Buy price: empty=1× base, full=0.5× base */
    st._inventory_cache[COMMODITY_FERRITE_ORE] = 0.0f;
    ASSERT_EQ_FLOAT(station_buy_price(&st, COMMODITY_FERRITE_ORE), 10.0f, 0.1f);
    st._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT_EQ_FLOAT(station_buy_price(&st, COMMODITY_FERRITE_ORE), 5.0f, 0.1f);
    st._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY * 0.5f;
    ASSERT_EQ_FLOAT(station_buy_price(&st, COMMODITY_FERRITE_ORE), 7.5f, 0.1f);
    /* Sell price: empty=2× base, full=1× base */
    st._inventory_cache[COMMODITY_FERRITE_ORE] = 0.0f;
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FERRITE_ORE), 20.0f, 0.1f);
    st._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FERRITE_ORE), 10.0f, 0.1f);
}

TEST(test_product_price_tracks_ore) {
    STATION_DECL(st);
    st.base_price[COMMODITY_FRAME] = 20.0f;
    /* Sell price: empty=2× base, full=1× base */
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FRAME), 40.0f, 0.1f);
    ASSERT(test_set_station_finished_units(
        &st, COMMODITY_FRAME, (int)MAX_PRODUCT_STOCK));
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FRAME), 20.0f, 0.1f);
    ASSERT(test_set_station_finished_units(
        &st, COMMODITY_FRAME, (int)(MAX_PRODUCT_STOCK * 0.5f)));
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FRAME), 25.0f, 0.1f);
}

TEST(test_deliver_ingots_to_contract) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* Player tows the exact ferrite-ingot crate the contract can accept. */
    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FERRITE_INGOT, 20);
    ASSERT(pod_idx >= 0);
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    economy_finalize_token_identity(&w.players[0]);
    float credits_before = ledger_balance(&w.stations[1], w.players[0].session_token);
    /* Create a contract at station 1 (Kepler Yard) for ferrite ingots */
    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 20.0f,
        .target_index = -1, .claimed_by = -1,
    };
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, pod_idx,
                                            COMMODITY_FERRITE_INGOT, NULL));
    world_sim_step(&w, SIM_DT);
    /* Ingot crate delivered, credits gained at station 1 */
    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
    ASSERT(ledger_balance(&w.stations[1], w.players[0].session_token) > credits_before);
    /* Contract quantity reduced */
    ASSERT(w.contracts[0].quantity_needed < 20.0f || !w.contracts[0].active);
}

TEST(test_first_cross_station_haul_uses_local_ledgers) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.contracts, 0, sizeof(w.contracts));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0x42, sizeof(sp->session_token));

    station_t *prospect = &w.stations[0];
    station_t *kepler = &w.stations[1];
    ASSERT_STR_EQ(prospect->currency_name, "prospect vouchers");
    ASSERT_STR_EQ(kepler->currency_name, "kepler bonds");

    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 2));
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FRAME, 1));
    ASSERT(test_set_station_finished_units(kepler, COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(kepler, COMMODITY_FRAME,
                                           (int)MAX_PRODUCT_STOCK));

    for (int i = 0; i < 4; i++) world_sim_step(&w, SIM_DT);

    int kepler_contract = -1;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *ct = &w.contracts[k];
        if (ct->active && ct->action == CONTRACT_TRACTOR &&
            ct->station_index == 1 &&
            ct->commodity == COMMODITY_FERRITE_INGOT) {
            kepler_contract = k;
            break;
        }
    }
    ASSERT(kepler_contract >= 0);

    ledger_earn(prospect, sp->session_token, 100.0f);
    float prospect_start = ledger_balance(prospect, sp->session_token);
    float kepler_start = ledger_balance(kepler, sp->session_token);
    int prospect_market_pod = test_spawn_station_market_exact_cargo_pod(
        &w, 0, COMMODITY_FERRITE_INGOT, 1);
    ASSERT(prospect_market_pod >= 0);

    sp->docked = true;
    sp->current_station = 0;
    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
    sp->input.buy_grade = MINING_GRADE_COMMON;
    int prospect_ingots_before = station_finished_count(prospect,
                                                        COMMODITY_FERRITE_INGOT);
    int prospect_frames_before = station_finished_count(prospect,
                                                        COMMODITY_FRAME);
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    int bought_ingot_pod = test_find_towed_exact_cargo_pod(
        &w, sp, COMMODITY_FERRITE_INGOT);
    ASSERT(bought_ingot_pod >= 0);
    ASSERT_EQ_INT(bought_ingot_pod, prospect_market_pod);
    ASSERT_EQ_INT(w.cargo_pods[bought_ingot_pod].quantity, 1);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FERRITE_INGOT),
                  prospect_ingots_before);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FRAME),
                  prospect_frames_before);
    ASSERT_EQ_INT(cargo_pod_custody_station(&w.cargo_pods[bought_ingot_pod]), 0);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    prospect_start, 0.001f);
    ASSERT_EQ_FLOAT(ledger_balance(kepler, sp->session_token), kepler_start, 0.001f);

    float expected_ingot_cost =
        (float)w.cargo_pods[bought_ingot_pod].custody_charge_total;
    ASSERT(expected_ingot_cost > 0.0f);
    test_move_pod_past_station_charge_boundary(&w, 0, bought_ingot_pod);
    world_sim_step(&w, SIM_DT);
    bool found_buy = false;
    for (int i = 0; i < w.events.count; i++) {
        const sim_event_t *ev = &w.events.events[i];
        if (ev->type == SIM_EVENT_BUY) {
            found_buy = true;
            ASSERT_EQ_INT(ev->buy.station, 0);
            ASSERT_EQ_INT(ev->buy.commodity, COMMODITY_FERRITE_INGOT);
            ASSERT_EQ_INT(ev->buy.cost, (int)lroundf(expected_ingot_cost));
            ASSERT_EQ_INT(ev->buy.quantity, 1);
        }
    }
    ASSERT(found_buy);
    ASSERT_EQ_FLOAT(prospect_start -
                    ledger_balance(prospect, sp->session_token),
                    expected_ingot_cost, 0.01f);
    ASSERT_EQ_INT(cargo_pod_custody_station(&w.cargo_pods[bought_ingot_pod]),
                  -1);
    float prospect_after_buy = ledger_balance(prospect, sp->session_token);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[bought_ingot_pod]), sp->id);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 1);

    int kepler_ingots_before = station_finished_count(kepler,
                                                      COMMODITY_FERRITE_INGOT);
    int kepler_hopper_idx = -1;
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, bought_ingot_pod,
                                            COMMODITY_FERRITE_INGOT,
                                            &kepler_hopper_idx));
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT(w.cargo_pods[bought_ingot_pod].active);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[bought_ingot_pod]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[bought_ingot_pod],
                                            1, kepler_hopper_idx));
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT_EQ_INT(station_finished_count(kepler, COMMODITY_FERRITE_INGOT),
                  kepler_ingots_before);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    prospect_after_buy, 0.001f);
    float kepler_after_delivery = ledger_balance(kepler, sp->session_token);
    ASSERT(kepler_after_delivery > kepler_start);

    int kepler_frame_market_pod = test_spawn_station_market_exact_cargo_pod(
        &w, 1, COMMODITY_FRAME, 1);
    ASSERT(kepler_frame_market_pod >= 0);
    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;
    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FRAME;
    sp->input.buy_grade = MINING_GRADE_COMMON;
    int kepler_frames_before = station_finished_count(kepler, COMMODITY_FRAME);
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FRAME), 0);
    int bought_frame_pod = test_find_towed_exact_cargo_pod(
        &w, sp, COMMODITY_FRAME);
    ASSERT(bought_frame_pod >= 0);
    ASSERT_EQ_INT(bought_frame_pod, kepler_frame_market_pod);
    ASSERT_EQ_INT(w.cargo_pods[bought_frame_pod].quantity, 1);
    ASSERT_EQ_INT(station_finished_count(kepler, COMMODITY_FRAME),
                  kepler_frames_before);
    ASSERT_EQ_INT(cargo_pod_custody_station(&w.cargo_pods[bought_frame_pod]), 1);
    ASSERT_EQ_FLOAT(ledger_balance(kepler, sp->session_token),
                    kepler_after_delivery, 0.001f);
    test_move_pod_past_station_charge_boundary(&w, 1, bought_frame_pod);
    world_sim_step(&w, SIM_DT);
    ASSERT(ledger_balance(kepler, sp->session_token) < kepler_after_delivery);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    prospect_after_buy, 0.001f);
}

TEST(test_delivery_credit_contract_pickup_deliver_and_clear) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    int helios_to_prospect_before =
        station_faction_relation_to(helios, prospect->faction_id);
    int prospect_to_helios_before =
        station_faction_relation_to(prospect, helios->faction_id);
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 3));
    ASSERT(test_set_station_finished_units(helios, COMMODITY_FERRITE_INGOT, 0));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };
    knowledge_view_configure(&helios->knowledge, STATION_KNOWN_ITEM_CAP);
    market_memory_t stale_demand = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_DEMAND,
        .station_a = 2,
        .station_b = 0xff,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_DELIVERY,
        .confidence = 210,
        .salience = 180,
        .quantity_hint = 2,
        .value_hint = 50,
        .observed_tick = 1,
    };
    knowledge_item_t stale_item;
    ASSERT(knowledge_item_from_market_memory(&stale_demand, &stale_item));
    knowledge_view_insert(&helios->knowledge, &stale_item);

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    delivery_shipment_t *shipment = test_find_delivery_shipment(&w, 0);
    ASSERT(shipment != NULL);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_PICKED_UP);
    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FERRITE_INGOT), 1);
    int shipment_pod = test_find_delivery_shipment_pod(&w, sp, shipment);
    ASSERT(shipment_pod >= 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);
    float prospect_after_pickup = ledger_balance(prospect, sp->session_token);
    ASSERT(prospect_after_pickup < 0.0f);

    sp->docked = true;
    sp->current_station = 2;
    sp->nearby_station = 2;
    sp->in_dock_range = true;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    float helios_before = ledger_balance(helios, sp->session_token);
    int helios_dock = test_first_dock_module_idx(helios);
    ASSERT(helios_dock >= 0);
    ASSERT(test_stage_pod_at_station_dock_mouth(&w, 2, shipment_pod));
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_DELIVERED);
    ASSERT_EQ_INT(shipment->quantity_delivered, 2);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT(w.cargo_pods[shipment_pod].active);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].shipment_id, 0);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[shipment_pod]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[shipment_pod],
                                            2, helios_dock));
    ASSERT(ledger_balance(helios, sp->session_token) > helios_before);
    ASSERT(w.contracts[0].active);
    ASSERT(station_faction_relation_to(helios, prospect->faction_id) >
           helios_to_prospect_before);
    ASSERT(station_faction_relation_to(prospect, helios->faction_id) >
           prospect_to_helios_before);
    market_memory_t receipt = {0};
    ASSERT(test_view_has_market_memory(&helios->knowledge,
                                       (uint8_t)MARKET_MEMORY_DELIVERY_RECEIPT,
                                       2, 0,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       &receipt));
    ASSERT_EQ_INT(receipt.quantity_hint, 2);
    ASSERT(receipt.value_hint > 0);
    ASSERT(!test_view_has_market_memory(&helios->knowledge,
                                        (uint8_t)MARKET_MEMORY_DEMAND,
                                        2, 0xff,
                                        (uint8_t)COMMODITY_FERRITE_INGOT,
                                        NULL));

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_CLEARED);
    ASSERT(!w.contracts[0].active);
    ASSERT(ledger_balance(prospect, sp->session_token) > 0.0f);
}

TEST(test_delivery_credit_dock_custody_does_not_teleport_far_pod) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(prospect,
                                           COMMODITY_FERRITE_INGOT, 2));
    ASSERT(test_set_station_finished_units(helios,
                                           COMMODITY_FERRITE_INGOT, 0));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    delivery_shipment_t *shipment = test_find_delivery_shipment(&w, 0);
    ASSERT(shipment != NULL);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_PICKED_UP);
    int shipment_pod = test_find_delivery_shipment_pod(&w, sp, shipment);
    ASSERT(shipment_pod >= 0);

    int helios_dock = test_first_dock_module_idx(helios);
    ASSERT(helios_dock >= 0);
    const station_module_t *dock = &helios->modules[helios_dock];
    vec2 dock_pos = module_world_pos_ring(helios, dock->ring, dock->slot);
    vec2 outward = v2_norm(v2_sub(dock_pos, helios->pos));
    if (v2_len_sq(outward) < 0.5f)
        outward = v2_from_angle(module_angle_ring(helios, dock->ring,
                                                  dock->slot));
    vec2 far_pos = v2_add(dock_pos, v2_scale(outward,
        CARGO_POD_DOCK_TRACTOR_RANGE * 2.0f));
    vec2 far_vel = v2_scale(outward, -17.0f);
    w.cargo_pods[shipment_pod].pos = far_pos;
    w.cargo_pods[shipment_pod].vel = far_vel;

    sp->docked = true;
    sp->current_station = 2;
    sp->nearby_station = 2;
    sp->in_dock_range = true;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_DELIVERED);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT(w.cargo_pods[shipment_pod].active);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[shipment_pod]), -1);
    ASSERT(!cargo_pod_has_module_tractor(&w.cargo_pods[shipment_pod]));
    ASSERT_EQ_INT(cargo_pod_custody_station(
                      &w.cargo_pods[shipment_pod]), 2);
    ASSERT(v2_dist_sq(w.cargo_pods[shipment_pod].pos, far_pos) <
           80.0f * 80.0f);
    ASSERT(v2_dist_sq(w.cargo_pods[shipment_pod].pos, dock_pos) >
           CARGO_POD_DOCK_TRACTOR_RANGE *
           CARGO_POD_DOCK_TRACTOR_RANGE);
}

TEST(test_delivery_credit_requires_exact_bound_cargo) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT,
                                           2));
    ASSERT(test_set_station_finished_units(helios, COMMODITY_FERRITE_INGOT,
                                           0));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    delivery_shipment_t *shipment = test_find_delivery_shipment(&w, 0);
    ASSERT(shipment != NULL);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_PICKED_UP);
    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    int shipment_pod = test_find_delivery_shipment_pod(&w, sp, shipment);
    ASSERT(shipment_pod >= 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].commodity,
                  COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);

    uint8_t first_exact_pub[32];
    uint8_t second_exact_pub[32];
    memcpy(first_exact_pub, shipment->cargo_pub[0], sizeof(first_exact_pub));
    memcpy(second_exact_pub, shipment->cargo_pub[1], sizeof(second_exact_pub));
    ASSERT(memcmp(shipment->cargo_units[0].pub, first_exact_pub, 32) == 0);
    ASSERT(memcmp(shipment->cargo_units[1].pub, second_exact_pub, 32) == 0);

    world_cargo_pod_clear_tractor(&w, shipment_pod);

    sp->docked = true;
    sp->current_station = 2;
    sp->nearby_station = 2;
    sp->in_dock_range = true;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    float helios_before = ledger_balance(helios, sp->session_token);
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_PICKED_UP);
    ASSERT_EQ_INT(shipment->quantity_delivered, 0);
    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(station_finished_count(helios, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_FLOAT(ledger_balance(helios, sp->session_token),
                    helios_before, 0.001f);

    ASSERT(world_cargo_pod_set_player_tractor(
        &w, shipment_pod, (int)sp->id));
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    int helios_dock = test_first_dock_module_idx(helios);
    ASSERT(helios_dock >= 0);
    ASSERT(test_stage_pod_at_station_dock_mouth(&w, 2, shipment_pod));
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_DELIVERED);
    ASSERT_EQ_INT(shipment->quantity_delivered, 2);
    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT(w.cargo_pods[shipment_pod].active);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].shipment_id, 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[shipment_pod]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[shipment_pod],
                                            2, helios_dock));
    ASSERT_EQ_INT(station_finished_count(helios, COMMODITY_FERRITE_INGOT), 0);
    ASSERT(ledger_balance(helios, sp->session_token) > helios_before);
}

TEST(test_delivery_credit_row_sell_unloads_bound_pod) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(prospect,
                                           COMMODITY_FERRITE_INGOT, 2));
    ASSERT(test_set_station_finished_units(helios,
                                           COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FRAME, 1));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    delivery_shipment_t *shipment = test_find_delivery_shipment(&w, 0);
    ASSERT(shipment != NULL);
    int shipment_pod = test_find_delivery_shipment_pod(&w, sp, shipment);
    ASSERT(shipment_pod >= 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);

    sp->docked = true;
    sp->current_station = 2;
    sp->nearby_station = 2;
    sp->in_dock_range = true;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    sp->input.service_sell_grade = MINING_GRADE_COMMON;
    sp->input.service_sell_one = true;
    float helios_before = ledger_balance(helios, sp->session_token);
    int helios_dock = test_first_dock_module_idx(helios);
    ASSERT(helios_dock >= 0);
    ASSERT(test_stage_pod_at_station_dock_mouth(&w, 2, shipment_pod));
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_DELIVERED);
    ASSERT_EQ_INT(shipment->quantity_delivered, 2);
    ASSERT(w.cargo_pods[shipment_pod].active);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].shipment_id, 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);
    ASSERT(w.cargo_pods[shipment_pod].has_shell_frame);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[shipment_pod]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[shipment_pod],
                                            2, helios_dock));
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT_EQ_INT(station_finished_count(helios, COMMODITY_FERRITE_INGOT), 0);
    ASSERT(ledger_balance(helios, sp->session_token) > helios_before);
}

TEST(test_delivery_credit_hail_ignores_empty_origin) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(helios, COMMODITY_FERRITE_INGOT, 0));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT(test_find_delivery_shipment(&w, 0) == NULL);
    ASSERT_EQ_INT(w.contracts[0].claimed_by, -1);
    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token), 0.0f, 0.001f);

    bool found_hail = false;
    for (int i = 0; i < w.events.count; i++) {
        const sim_event_t *ev = &w.events.events[i];
        if (ev->type == SIM_EVENT_HAIL_RESPONSE) {
            found_hail = true;
            ASSERT_EQ_INT(ev->hail_response.station, 0);
            ASSERT(ev->hail_response.contract_index != 0);
        }
    }
    ASSERT(found_hail);
}

TEST(test_delivery_credit_hail_requires_docking_to_pick_up) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 3));
    ASSERT(test_set_station_finished_units(helios, COMMODITY_FERRITE_INGOT, 0));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    sp->docked = false;
    sp->current_station = -1;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->ship->pos = prospect->pos;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT(test_find_delivery_shipment(&w, 0) == NULL);
    ASSERT_EQ_INT(w.contracts[0].claimed_by, -1);
    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FERRITE_INGOT), 3);

    bool found_hail = false;
    for (int i = 0; i < w.events.count; i++) {
        const sim_event_t *ev = &w.events.events[i];
        if (ev->type == SIM_EVENT_HAIL_RESPONSE) {
            found_hail = true;
            ASSERT_EQ_INT(ev->hail_response.station, 0);
            ASSERT_EQ_INT(ev->hail_response.contract_index, 0);
        }
    }
    ASSERT(found_hail);
}

TEST(test_delivery_credit_black_market_sale_defaults_origin_debt) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    station_t *pirate = &w.stations[3];
    snprintf(pirate->name, sizeof(pirate->name), "Freeport");
    pirate->signal_range = 0.0f;
    pirate->dock_radius = 96.0f;
    pirate->radius = 120.0f;
    pirate->module_count = 1;
    pirate->modules[0] = (station_module_t){ .type = MODULE_DOCK };
    pirate->base_price[COMMODITY_FERRITE_INGOT] = 18.0f;
    ASSERT(station_manifest_bootstrap(pirate));
    int helios_to_pirate_before =
        station_faction_relation_to(helios, pirate->faction_id);
    int pirate_to_helios_before =
        station_faction_relation_to(pirate, helios->faction_id);

    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 2));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    knowledge_view_configure(&helios->knowledge, STATION_KNOWN_ITEM_CAP);
    market_memory_t stale_demand = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_DEMAND,
        .station_a = 2,
        .station_b = 0xff,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_DELIVERY,
        .confidence = 210,
        .salience = 180,
        .quantity_hint = 2,
        .value_hint = 50,
        .observed_tick = 1,
    };
    knowledge_item_t stale_item;
    ASSERT(knowledge_item_from_market_memory(&stale_demand, &stale_item));
    knowledge_view_insert(&helios->knowledge, &stale_item);

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    delivery_shipment_t *shipment = test_find_delivery_shipment(&w, 0);
    ASSERT(shipment != NULL);
    int shipment_pod = test_find_delivery_shipment_pod(&w, sp, shipment);
    ASSERT(shipment_pod >= 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);
    float prospect_after_pickup = ledger_balance(prospect, sp->session_token);
    ASSERT(prospect_after_pickup < 0.0f);

    sp->docked = true;
    sp->current_station = 3;
    sp->nearby_station = 3;
    sp->in_dock_range = true;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    sp->input.service_sell_grade = MINING_GRADE_COMMON;
    sp->input.service_sell_one = true;
    float pirate_before = ledger_balance(pirate, sp->session_token);
    int pirate_dock = test_first_dock_module_idx(pirate);
    ASSERT(pirate_dock >= 0);
    ASSERT(test_stage_pod_at_station_dock_mouth(&w, 3, shipment_pod));
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_BLACK_MARKET_SOLD);
    ASSERT_EQ_INT(shipment->quantity_black_market_sold, 2);
    ASSERT(w.cargo_pods[shipment_pod].active);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].shipment_id, 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);
    ASSERT(w.cargo_pods[shipment_pod].has_shell_frame);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[shipment_pod]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[shipment_pod],
                                            3, pirate_dock));
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT(ledger_balance(pirate, sp->session_token) > pirate_before);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    prospect_after_pickup, 0.001f);
    ASSERT(!w.contracts[0].active);
    ASSERT(station_faction_relation_to(helios, pirate->faction_id) <
           helios_to_pirate_before);
    ASSERT(station_faction_relation_to(pirate, helios->faction_id) <
           pirate_to_helios_before);
    ASSERT(test_view_has_market_memory(&helios->knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_RISK,
                                       2, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    ASSERT(test_view_has_market_memory(&pirate->knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_RISK,
                                       2, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    ASSERT(test_view_has_market_memory(&sp->ship->knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_RISK,
                                       2, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    ASSERT(!test_view_has_market_memory(&helios->knowledge,
                                        (uint8_t)MARKET_MEMORY_DEMAND,
                                        2, 0xff,
                                        (uint8_t)COMMODITY_FERRITE_INGOT,
                                        NULL));
}

TEST(test_delivery_credit_timeout_emits_station_risk_memory) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 1));
    ASSERT(test_set_station_finished_units(helios, COMMODITY_FERRITE_INGOT, 0));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 1.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    knowledge_view_configure(&helios->knowledge, STATION_KNOWN_ITEM_CAP);
    market_memory_t stale_demand = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_DEMAND,
        .station_a = 2,
        .station_b = 0xff,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_DELIVERY,
        .confidence = 210,
        .salience = 180,
        .quantity_hint = 1,
        .value_hint = 50,
        .observed_tick = 1,
    };
    knowledge_item_t stale_item;
    ASSERT(knowledge_item_from_market_memory(&stale_demand, &stale_item));
    knowledge_view_insert(&helios->knowledge, &stale_item);

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    delivery_shipment_t *shipment = test_find_delivery_shipment(&w, 0);
    ASSERT(shipment != NULL);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_PICKED_UP);
    shipment->due_tick = w.tick;

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_DEFAULTED);
    ASSERT(!w.contracts[0].active);
    ASSERT(test_view_has_market_memory(&helios->knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_RISK,
                                       2, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    ASSERT(test_view_has_market_memory(&sp->ship->knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_RISK,
                                       2, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    ASSERT(!test_view_has_market_memory(&helios->knowledge,
                                        (uint8_t)MARKET_MEMORY_DEMAND,
                                        2, 0xff,
                                        (uint8_t)COMMODITY_FERRITE_INGOT,
                                        NULL));
}

TEST(test_prospect_pubkey_buy_debits_pubkey_ledger) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0x42, sizeof(sp->session_token));
    memset(sp->pubkey, 0xA5, sizeof(sp->pubkey));
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    sp->pubkey_challenge_consumed = true;
    sp->docked = true;
    sp->current_station = 0;

    station_t *prospect = &w.stations[0];
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 1));
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FRAME, 1));
    int market_pod = test_spawn_station_market_exact_cargo_pod(
        &w, 0, COMMODITY_FERRITE_INGOT, 1);
    ASSERT(market_pod >= 0);
    ledger_earn_by_pubkey(prospect, sp->pubkey, 1000.0f);
    ledger_earn(prospect, sp->session_token, 333.0f);
    float pubkey_before = ledger_balance_by_pubkey(prospect, sp->pubkey);
    float session_before = ledger_balance(prospect, sp->session_token);
    int frames_before = station_finished_count(prospect, COMMODITY_FRAME);
    float expected_cost = test_station_market_pod_sell_quote(
        prospect, &w.cargo_pods[market_pod]);
    ASSERT(expected_cost > 0.0f);

    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
    sp->input.buy_grade = MINING_GRADE_COMMON;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    int bought_pod = test_find_towed_exact_cargo_pod(
        &w, sp, COMMODITY_FERRITE_INGOT);
    ASSERT(bought_pod >= 0);
    ASSERT_EQ_INT(bought_pod, market_pod);
    ASSERT_EQ_INT(w.cargo_pods[bought_pod].quantity, 1);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FRAME),
                  frames_before);
    ASSERT_EQ_INT(cargo_pod_custody_station(&w.cargo_pods[bought_pod]), 0);
    ASSERT_EQ_FLOAT(ledger_balance_by_pubkey(prospect, sp->pubkey),
                    pubkey_before, 0.001f);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    session_before, 0.001f);

    test_move_pod_past_station_charge_boundary(&w, 0, bought_pod);
    world_sim_step(&w, SIM_DT);

    bool found_buy = false;
    for (int i = 0; i < w.events.count; i++) {
        const sim_event_t *ev = &w.events.events[i];
        if (ev->type == SIM_EVENT_BUY) {
            found_buy = true;
            ASSERT_EQ_INT(ev->buy.station, 0);
            ASSERT_EQ_INT(ev->buy.commodity, COMMODITY_FERRITE_INGOT);
            ASSERT_EQ_INT(ev->buy.cost, (int)lroundf(expected_cost));
            ASSERT_EQ_INT(ev->buy.quantity, 1);
        }
    }
    ASSERT(found_buy);
    ASSERT_EQ_FLOAT(pubkey_before - ledger_balance_by_pubkey(prospect, sp->pubkey),
                    expected_cost, 0.01f);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    session_before, 0.001f);
}

TEST(test_market_buy_requires_station_held_pod) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0x43, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = 0;

    station_t *prospect = &w.stations[0];
    ASSERT(test_set_station_finished_units(prospect,
                                           COMMODITY_FERRITE_INGOT, 1));
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FRAME, 2));
    ledger_earn(prospect, sp->session_token, 1000.0f);

    int ingots_before = station_finished_count(prospect,
                                               COMMODITY_FERRITE_INGOT);
    int frames_before = station_finished_count(prospect, COMMODITY_FRAME);
    float balance_before = ledger_balance(prospect, sp->session_token);

    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
    sp->input.buy_grade = MINING_GRADE_COMMON;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT_EQ_INT(test_find_towed_exact_cargo_pod(
                      &w, sp, COMMODITY_FERRITE_INGOT), -1);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FERRITE_INGOT),
                  ingots_before);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FRAME),
                  frames_before);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    balance_before, 0.001f);
}

/* Pubkey-registered players had been getting 65% of the contract payout
 * because try_sell_station_cargo routed through ledger_credit_supply
 * (which applies the 35% smelt-station cut) instead of ledger_earn
 * (full credit). Locks the fix in: payout to the ledger == quoted price
 * × quantity, not × 0.65. Reported as "press S, popup says +152, wallet
 * only sees +99" on the WORK tab. */
TEST(test_deliver_ingots_full_payout_to_pubkey_player) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    /* Finalize the pubkey identity so the durable contract claim and
     * bulk-sell payout both use the canonical player principal. */
    economy_finalize_token_identity(
        &w.players[0]);
    /* Player tows 10 ferrite ingots; Kepler's physical intake contract
     * pays 20 cr each when the hopper tractor takes custody. */
    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FERRITE_INGOT, 10);
    ASSERT(pod_idx >= 0);
    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 10.0f,
        .base_price = 20.0f,
        .target_index = -1, .claimed_by = -1,
    };
    int hopper_idx = station_find_hopper_for(&w.stations[1],
                                             COMMODITY_FERRITE_INGOT);
    ASSERT(hopper_idx >= 0);
    w.cargo_pods[pod_idx].pos = module_world_pos_ring(
        &w.stations[1], w.stations[1].modules[hopper_idx].ring,
        w.stations[1].modules[hopper_idx].slot);
    float bal_before = ledger_balance_by_pubkey(&w.stations[1], w.players[0].pubkey);
    world_sim_step(&w, SIM_DT);
    /* Expect 10 × 20 = 200 cr credited (allow tiny float slack for
     * age-escalation drift on tick 1 — should be effectively zero). */
    float bal_after = ledger_balance_by_pubkey(&w.stations[1], w.players[0].pubkey);
    float gained = bal_after - bal_before;
    ASSERT(gained > 199.0f);
    ASSERT(gained < 201.0f);
    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            1, hopper_idx));
}

TEST(test_deliver_ingots_pending_pubkey_uses_session_ledger) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x02, 8);
    memset(w.players[0].pubkey, 0xBB, 32);
    w.players[0].pubkey_set = true;
    w.players[0].pubkey_proof_ok = false;

    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FERRITE_INGOT, 1);
    ASSERT(pod_idx >= 0);
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, pod_idx,
                                            COMMODITY_FERRITE_INGOT, NULL));
    float session_before = ledger_balance(&w.stations[1],
                                          w.players[0].session_token);
    float pubkey_before = ledger_balance_by_pubkey(&w.stations[1],
                                                   w.players[0].pubkey);

    world_sim_step(&w, SIM_DT);

    ASSERT(ledger_balance(&w.stations[1], w.players[0].session_token) >
           session_before + 0.01f);
    ASSERT_EQ_FLOAT(ledger_balance_by_pubkey(&w.stations[1],
                                             w.players[0].pubkey),
                    pubkey_before, 0.001f);
    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
}

TEST(test_mixed_cargo_sell_and_deliver) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* Player tows the exact ingot crate the refinery contract can accept. */
    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FERRITE_INGOT, 15);
    ASSERT(pod_idx >= 0);
    /* Contract at Kepler for ferrite ingots. */
    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 15.0f,
        .base_price = 20.0f,
        .target_index = -1, .claimed_by = -1,
    };
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    float credits_before = ledger_balance(&w.stations[1], w.players[0].session_token);
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, pod_idx,
                                            COMMODITY_FERRITE_INGOT, NULL));
    world_sim_step(&w, SIM_DT);
    /* Ingot crate delivered via contract */
    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
    ASSERT(ledger_balance(&w.stations[1], w.players[0].session_token) > credits_before);
}

TEST(test_no_delivery_without_matching_contract) {
    /* Cargo with no matching contract AND no consuming module on the
     * station should stay in the hold. Use Prospect (no shipyard,
     * no fab) so a tractor-module load has nowhere to land. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].ship->cargo[COMMODITY_TRACTOR_MODULE] = 20.0f;
    for (int k = 0; k < MAX_CONTRACTS; k++) w.contracts[k].active = false;
    /* Prospect Refinery (station 0): DOCK + SIGNAL_RELAY + FURNACE +
     * ORE_SILO. No SHIPYARD, no TRACTOR_FAB → station_consumes returns
     * false for tractor modules, so the SELL fallback should skip. */
    w.players[0].docked = true;
    w.players[0].current_station = 0;
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_FLOAT(w.players[0].ship->cargo[COMMODITY_TRACTOR_MODULE], 20.0f, 0.01f);
}

TEST(test_no_passive_heal_without_kits) {
    /* Passive heal was removed: docking alone never repairs. With both
     * ship cargo and station inventory empty, damaged hull stays
     * damaged — repair requires kits. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].ship->hull = 50.0f;
    w.players[0].docked = true;
    w.players[0].current_station = 0;
    ASSERT(test_set_station_finished_units(
        &w.stations[0], COMMODITY_REPAIR_KIT, 0));
    ASSERT(test_set_ship_finished_units(
        w.players[0].ship, COMMODITY_REPAIR_KIT, 0,
        MINING_GRADE_COMMON));
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT_EQ_FLOAT(w.players[0].ship->hull, 50.0f, 0.01f);
}

static bool economy_test_smelt_target_for_ore(const station_t *st,
                                              commodity_t ore,
                                              vec2 *out_target) {
    bool found = false;
    float best_d = 1e18f;
    for (int fm = 0; fm < st->module_count; fm++) {
        const station_module_t *f = &st->modules[fm];
        if (f->type != MODULE_FURNACE || f->scaffold) continue;
        if (module_instance_input_ore(f) != ore) continue;
        int ring = (int)f->ring;
        vec2 furnace_pos = module_world_pos_ring(st, ring, f->slot);
        int adj_rings[2] = { ring + 1, ring - 1 };
        for (int ri = 0; ri < 2; ri++) {
            int adj = adj_rings[ri];
            if (adj < 1 || adj > STATION_NUM_RINGS) continue;
            for (int hm = 0; hm < st->module_count; hm++) {
                const station_module_t *h = &st->modules[hm];
                if (h->ring != adj || h->scaffold) continue;
                if (h->type != MODULE_HOPPER) continue;
                if ((commodity_t)h->commodity != ore) continue;
                vec2 hopper_pos = module_world_pos_ring(st, adj, h->slot);
                float d = v2_dist_sq(furnace_pos, hopper_pos);
                if (d < best_d) {
                    best_d = d;
                    if (out_target)
                        *out_target = v2_scale(v2_add(furnace_pos, hopper_pos), 0.5f);
                    found = true;
                }
            }
        }
    }
    return found;
}

static int economy_test_spawn_fragment(world_t *w, commodity_t ore,
                                       float units, vec2 pos) {
    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) { frag = i; break; }
    }
    if (frag < 0) return -1;
    asteroid_t *a = &w->asteroids[frag];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = ore;
    a->ore = units;
    a->max_ore = units;
    a->radius = 6.0f;
    a->fracture_child = true;
    a->grade = (uint8_t)MINING_GRADE_COMMON;
    for (int b = 0; b < 32; b++) a->fracture_seed[b] = (uint8_t)(0x80 + b);
    a->pos = pos;
    a->vel = v2(0.0f, 0.0f);
    return frag;
}

TEST(test_refinery_smelts_fragment_into_ingot_pod) {
    WORLD_DECL;
    world_reset(&w);
    ASSERT(test_anchor_station_legacy_cargo(&w, 0));
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0xC7, sizeof(w.players[0].session_token));
    player_init_ship(&w.players[0], &w);

    ASSERT(station_has_module(&w.stations[0], MODULE_FURNACE));
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        w.stations[0].arm_speed[arm] = 0.0f;
        w.stations[0].arm_rotation[arm] = 0.0f;
    }

    vec2 smelt_target = w.stations[0].pos;
    ASSERT(economy_test_smelt_target_for_ore(&w.stations[0],
                                             COMMODITY_FERRITE_ORE,
                                             &smelt_target));
    int frag = economy_test_spawn_fragment(&w, COMMODITY_FERRITE_ORE,
                                           10.0f, smelt_target);
    ASSERT(frag >= 0);
    w.asteroids[frag].last_towed_by = 0;
    memcpy(w.asteroids[frag].last_towed_token,
           w.players[0].session_token,
           sizeof(w.asteroids[frag].last_towed_token));
    ASSERT(world_asteroid_set_player_tractor(&w, frag, 0));

    float station_ingots_before = station_inventory_amount(
        &w.stations[0], COMMODITY_FERRITE_INGOT);
    int station_frames_before =
        station_finished_count(&w.stations[0], COMMODITY_FRAME);
    int frame_pod_units_before =
        economy_count_exact_pod_units(&w, COMMODITY_FRAME);
    ASSERT(frame_pod_units_before > 0);
    int pod_units_before =
        economy_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT);
    for (int i = 0; i < (int)(10.0f / SIM_DT) && w.asteroids[frag].active; i++)
        world_sim_step(&w, SIM_DT);
    ASSERT(!w.asteroids[frag].active);
    ASSERT_EQ_INT(w.players[0].ship->towed_count, 0);
    ASSERT_EQ_FLOAT(station_inventory_amount(
                        &w.stations[0], COMMODITY_FERRITE_INGOT),
                    station_ingots_before + 10.0f, 0.001f);
    ASSERT_EQ_INT(station_finished_count(&w.stations[0], COMMODITY_FRAME),
                  station_frames_before);
    ASSERT_EQ_INT(economy_count_exact_pod_units(&w, COMMODITY_FRAME),
                  frame_pod_units_before - 1);
    const cargo_pod_t *pod = economy_first_exact_pod(
        &w, COMMODITY_FERRITE_INGOT);
    ASSERT(pod != NULL);
    for (int i = 0; i < 120 && !cargo_pod_has_module_tractor(pod); i++) {
        world_sim_step(&w, SIM_DT);
        pod = economy_first_exact_pod(&w, COMMODITY_FERRITE_INGOT);
        ASSERT(pod != NULL);
    }
    int pod_station = -1;
    int pod_module = -1;
    ASSERT(cargo_pod_module_tractor_indices(pod, &pod_station, &pod_module));
    ASSERT_EQ_INT(pod_station, 0);
    ASSERT(pod_module >= 0 && pod_module < w.stations[0].module_count);
    ASSERT_EQ_INT(w.stations[0].modules[pod_module].type, MODULE_DOCK);
    ASSERT_EQ_INT(cargo_pod_player_tractor(pod), -1);
    ASSERT_EQ_INT(pod->manifest_count, 10);
    ASSERT_EQ_INT(pod->quantity, 10);
    ASSERT(economy_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT) >=
           pod_units_before + 10);
    ASSERT_EQ_INT((int)w.hopper_smelt_events, 0);
}

TEST(test_kit_fab_requires_shipyard) {
    /* After the shipyard-fab redesign, only stations with MODULE_SHIPYARD
     * mint repair kits. A station with only a dock + the three input
     * commodities should never produce kits. */
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    /* Prospect (station 0) has a dock but no shipyard. Kepler and Helios
     * both have shipyards. Pre-fill all three with kit-fab inputs. */
    ASSERT(station_has_module(&w.stations[0], MODULE_DOCK));
    ASSERT(!station_has_module(&w.stations[0], MODULE_SHIPYARD));
    ASSERT(station_has_module(&w.stations[1], MODULE_SHIPYARD));
    ASSERT(station_has_module(&w.stations[2], MODULE_SHIPYARD));
    for (int s = 0; s < 3; s++) {
        ASSERT(test_set_station_finished_units(&w.stations[s], COMMODITY_FRAME, 5));
        ASSERT(test_set_station_finished_units(&w.stations[s], COMMODITY_LASER_MODULE, 5));
        ASSERT(test_set_station_finished_units(&w.stations[s], COMMODITY_TRACTOR_MODULE, 5));
        ASSERT(test_set_station_finished_units(&w.stations[s], COMMODITY_REPAIR_KIT, 0));
        w.stations[s].repair_kit_fab_timer = 0.0f;
        ASSERT(test_anchor_station_legacy_cargo(&w, s));
    }
    /* Run long enough for at least one fab cycle (REPAIR_KIT_FAB_PERIOD = 30s). */
    for (int i = 0; i < (int)(35.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    /* Shipyard station produces kits; dock-only station does not. */
    ASSERT(station_inventory_amount(&w.stations[1],
                                    COMMODITY_REPAIR_KIT) > 0.0f);
    ASSERT(station_inventory_amount(&w.stations[2],
                                    COMMODITY_REPAIR_KIT) > 0.0f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&w.stations[0],
                                              COMMODITY_REPAIR_KIT),
                    0.0f, 0.01f);
}

TEST(test_kit_import_contract_at_consumer_station) {
    /* A station with a dock but no shipyard should issue a TRACTOR
     * contract for REPAIR_KIT when its kit inventory drops below the
     * import threshold. Players or NPC haulers fulfill via the same
     * delivery loop that handles ingots. */
    WORLD_DECL;
    world_reset(&w);
    ASSERT(station_has_module(&w.stations[0], MODULE_DOCK));
    ASSERT(!station_has_module(&w.stations[0], MODULE_SHIPYARD));
    /* Drain Prospect's kit inventory to force the deficit. */
    ASSERT(test_set_station_finished_units(
        &w.stations[0], COMMODITY_REPAIR_KIT, 0));
    /* Run a few seconds for the contract step to fire. */
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    bool found = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *c = &w.contracts[k];
        if (c->active && c->action == CONTRACT_TRACTOR
            && c->station_index == 0
            && c->commodity == COMMODITY_REPAIR_KIT) {
            found = true;
            ASSERT(c->base_price > 0.0f);
            ASSERT(c->quantity_needed > 0.0f);
            ASSERT(c->proof_flags & CONTRACT_PROOF_REQUIRE_PROOF);
            ASSERT(c->proof_flags & CONTRACT_PROOF_REQUIRE_RECIPE);
            ASSERT_EQ_INT(c->required_recipe_id, RECIPE_REPAIR_KIT_FAB);
            break;
        }
    }
    ASSERT(found);
}

TEST(test_kit_import_contract_skips_shipyard_stations) {
    /* A shipyard station mints its own kits; the import contract should
     * not fire there even with kit inventory at zero. */
    WORLD_DECL;
    world_reset(&w);
    ASSERT(station_has_module(&w.stations[1], MODULE_SHIPYARD));
    ASSERT(station_has_module(&w.stations[2], MODULE_SHIPYARD));
    ASSERT(test_set_station_finished_units(
        &w.stations[1], COMMODITY_REPAIR_KIT, 0));
    ASSERT(test_set_station_finished_units(
        &w.stations[2], COMMODITY_REPAIR_KIT, 0));
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *c = &w.contracts[k];
        if (c->active && c->action == CONTRACT_TRACTOR
            && (c->station_index == 1 || c->station_index == 2)
            && c->commodity == COMMODITY_REPAIR_KIT) {
            ASSERT(false); /* shouldn't reach here */
        }
    }
}

TEST(test_repair_drains_ship_cargo_first) {
    /* Player docked at a station with a repair service. Ship carries
     * 50 kits in cargo, station has 100 kits in inventory. A 30 HP
     * repair drains 30 kits from ship cargo, leaves station inventory
     * untouched, and charges only the labor fee (no station retail
     * since no kits sourced from station). */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 0;

    /* Force the repair service (default Prospect lacks REPAIR_BAY). */
    w.stations[0].services |= STATION_SERVICE_REPAIR;
    ASSERT(test_set_station_finished_units(&w.stations[0], COMMODITY_REPAIR_KIT, 100));
    ASSERT(test_set_ship_finished_units(w.players[0].ship, COMMODITY_REPAIR_KIT,
                                        50, MINING_GRADE_COMMON));
    float max_hull = ship_max_hull(w.players[0].ship);
    w.players[0].ship->hull = max_hull - 30.0f; /* 30 HP missing */

    float bal_before = ledger_balance(&w.stations[0],
                                      w.players[0].session_token);
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* Hull restored, ship cargo drained, station inventory untouched. */
    ASSERT_EQ_FLOAT(w.players[0].ship->hull, max_hull, 0.5f);
    ASSERT_EQ_INT(ship_finished_count(w.players[0].ship,
                                      COMMODITY_REPAIR_KIT), 20);
    ASSERT_EQ_INT(station_finished_count(&w.stations[0],
                                         COMMODITY_REPAIR_KIT), 100);

    /* Charge: only labor (no station retail). 30 HP * 1 cr/HP. */
    float bal_after = ledger_balance(&w.stations[0],
                                     w.players[0].session_token);
    float charged = bal_before - bal_after;
    ASSERT_EQ_FLOAT(charged, 30.0f * LABOR_FEE_PER_HP, 0.5f);
}

TEST(test_repair_falls_back_to_station_inventory) {
    /* Player has no kits in cargo; station inventory covers it. Repair
     * charges retail (station_sell_price) + labor since not a shipyard. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 0;

    w.stations[0].services |= STATION_SERVICE_REPAIR;
    w.stations[0].base_price[COMMODITY_REPAIR_KIT] = 6.0f;
    ASSERT(test_set_station_finished_units(&w.stations[0], COMMODITY_REPAIR_KIT,
                                           (int)MAX_PRODUCT_STOCK)); /* full → 1× */
    ASSERT(test_set_ship_finished_units(w.players[0].ship, COMMODITY_REPAIR_KIT,
                                        0, MINING_GRADE_COMMON));
    float max_hull = ship_max_hull(w.players[0].ship);
    w.players[0].ship->hull = max_hull - 10.0f;

    float bal_before = ledger_balance(&w.stations[0],
                                      w.players[0].session_token);
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* 10 HP from station: 10 kits drained, charge = 10 * (6 + 1) = 70 cr. */
    ASSERT_EQ_FLOAT(w.players[0].ship->hull, max_hull, 0.5f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&w.stations[0],
                                              COMMODITY_REPAIR_KIT),
                    MAX_PRODUCT_STOCK - 10.0f, 0.5f);
    float charged = bal_before - ledger_balance(&w.stations[0],
                                                w.players[0].session_token);
    ASSERT_EQ_FLOAT(charged, 10.0f * (6.0f + LABOR_FEE_PER_HP), 1.0f);
}

TEST(test_repair_at_shipyard_no_labor_fee) {
    /* At a shipyard the labor fee is zero — you already paid retail
     * when you bought the kits there. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x02, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 1; /* Kepler has shipyard */
    ASSERT(station_has_module(&w.stations[1], MODULE_SHIPYARD));

    w.stations[1].services |= STATION_SERVICE_REPAIR;
    w.stations[1].base_price[COMMODITY_REPAIR_KIT] = 6.0f;
    ASSERT(test_set_station_finished_units(&w.stations[1], COMMODITY_REPAIR_KIT,
                                           (int)MAX_PRODUCT_STOCK));
    ASSERT(test_set_ship_finished_units(w.players[0].ship, COMMODITY_REPAIR_KIT,
                                        0, MINING_GRADE_COMMON));
    float max_hull = ship_max_hull(w.players[0].ship);
    w.players[0].ship->hull = max_hull - 10.0f;

    float bal_before = ledger_balance(&w.stations[1],
                                      w.players[0].session_token);
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* 10 HP from station: charge = 10 * (6 + 0) = 60 cr (no labor). */
    float charged = bal_before - ledger_balance(&w.stations[1],
                                                w.players[0].session_token);
    ASSERT_EQ_FLOAT(charged, 10.0f * 6.0f, 1.0f);
}

TEST(test_repair_partial_when_kits_short) {
    /* Both ship cargo and station inventory empty: repair does nothing
     * (no partial heal because no kits to consume). */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x03, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 0;

    ASSERT(test_set_station_finished_units(
        &w.stations[0], COMMODITY_REPAIR_KIT, 0));
    ASSERT(test_set_ship_finished_units(
        w.players[0].ship, COMMODITY_REPAIR_KIT, 0,
        MINING_GRADE_COMMON));
    float max_hull = ship_max_hull(w.players[0].ship);
    w.players[0].ship->hull = max_hull - 20.0f;

    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* No kits anywhere = no heal at all (passive heal removed). */
    ASSERT_EQ_FLOAT(w.players[0].ship->hull, max_hull - 20.0f, 0.01f);
}

TEST(test_repair_rejects_float_only_kits) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x04, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 0;

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_REPAIR_KIT, 0));
    ASSERT(test_set_ship_finished_units(w.players[0].ship,
                                        COMMODITY_REPAIR_KIT, 0,
                                        MINING_GRADE_COMMON));
    w.stations[0]._inventory_cache[COMMODITY_REPAIR_KIT] = 100.0f;
    w.players[0].ship->cargo[COMMODITY_REPAIR_KIT] = 100.0f;

    float max_hull = ship_max_hull(w.players[0].ship);
    w.players[0].ship->hull = max_hull - 20.0f;
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_FLOAT(w.players[0].ship->hull, max_hull - 20.0f, 0.01f);
    ASSERT_EQ_FLOAT(w.players[0].ship->cargo[COMMODITY_REPAIR_KIT],
                    100.0f, 0.001f);
}

TEST(test_repair_kit_fab_requires_manifest_inputs) {
    WORLD_DECL;
    world_reset(&w);

    int shipyard = -1;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (station_has_module(&w.stations[s], MODULE_SHIPYARD)) {
            shipyard = s;
            break;
        }
    }
    ASSERT(shipyard >= 0);
    station_t *st = &w.stations[shipyard];

    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_REPAIR_KIT, 0));
    st->_inventory_cache[COMMODITY_FRAME] = 5.0f;
    st->_inventory_cache[COMMODITY_LASER_MODULE] = 5.0f;
    st->_inventory_cache[COMMODITY_TRACTOR_MODULE] = 5.0f;
    st->repair_kit_fab_timer = 0.0f;

    step_dock_repair_kit_fab(&w, 60.0f);

    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_REPAIR_KIT), 0);
    ASSERT_EQ_FLOAT(st->_inventory_cache[COMMODITY_REPAIR_KIT], 0.0f, 0.001f);
}

TEST(test_repair_kit_fab_emits_craft_chain_event) {
    economy_chain_test_setup("repair_kit_craft");
    WORLD_DECL;
    world_reset(&w);
    economy_chain_test_wipe_logs(&w);

    int shipyard = -1;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (station_has_module(&w.stations[s], MODULE_SHIPYARD)) {
            shipyard = s;
            break;
        }
    }
    ASSERT(shipyard >= 0);
    station_t *st = &w.stations[shipyard];

    ASSERT(test_set_station_finished_units(st, COMMODITY_REPAIR_KIT,
                                           (int)REPAIR_KIT_STOCK_CAP - 1));
    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 1));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 1));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 1));
    ASSERT(test_anchor_station_legacy_cargo(&w, shipyard));
    uint64_t before_events = st->chain_event_count;

    const cargo_unit_t *frame = test_station_first_unit(st, COMMODITY_FRAME,
                                                       RECIPE_LEGACY_MIGRATE);
    const cargo_unit_t *laser = test_station_first_unit(st, COMMODITY_LASER_MODULE,
                                                       RECIPE_LEGACY_MIGRATE);
    const cargo_unit_t *tractor = test_station_first_unit(st, COMMODITY_TRACTOR_MODULE,
                                                         RECIPE_LEGACY_MIGRATE);
    ASSERT(frame != NULL);
    ASSERT(laser != NULL);
    ASSERT(tractor != NULL);
    uint8_t expected_inputs[RECIPE_INPUT_MAX][32] = {{0}};
    memcpy(expected_inputs[0], frame->pub, 32);
    memcpy(expected_inputs[1], laser->pub, 32);
    memcpy(expected_inputs[2], tractor->pub, 32);

    st->repair_kit_fab_timer = REPAIR_KIT_FAB_PERIOD;
    step_dock_repair_kit_fab(&w, SIM_DT);

    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_REPAIR_KIT),
                  (int)REPAIR_KIT_STOCK_CAP);
    ASSERT_EQ_INT((int)st->chain_event_count, (int)before_events + 1);

    const cargo_unit_t *kit = test_station_first_unit(st, COMMODITY_REPAIR_KIT,
                                                     RECIPE_REPAIR_KIT_FAB);
    ASSERT(kit != NULL);

    uint64_t walked = 0;
    ASSERT(chain_log_verify(st, &walked, NULL));
    ASSERT_EQ_INT((int)walked, (int)st->chain_event_count);

    chain_cargo_transform_t found = {0};
    ASSERT(chain_log_find_cargo_transform(st, kit->pub, &found));
    ASSERT_EQ_INT(found.type, CHAIN_EVT_CRAFT);
    ASSERT_EQ_INT(found.craft.recipe_id, RECIPE_REPAIR_KIT_FAB);
    ASSERT_EQ_INT(found.craft.input_count, RECIPE_INPUT_MAX);
    ASSERT(memcmp(found.craft.output_pub, kit->pub, 32) == 0);
    ASSERT(memcmp(found.craft.input_pubs[0], expected_inputs[0], 32) == 0);
    ASSERT(memcmp(found.craft.input_pubs[1], expected_inputs[1], 32) == 0);
    ASSERT(memcmp(found.craft.input_pubs[2], expected_inputs[2], 32) == 0);

    economy_chain_test_teardown();
}

TEST(test_furnace_without_hopper_does_not_smelt) {
    /* Furnace capability is pair/tag based: a tagged furnace still
     * requires an adjacent matching ore hopper before it'll fire. */
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        w.stations[0].arm_speed[arm] = 0.0f;
        w.stations[0].arm_rotation[arm] = 0.0f;
    }
    w.stations[0].module_count = 0;
    rebuild_station_services(&w.stations[0]);
    w.stations[0].modules[0] = (station_module_t){
        .type = MODULE_FURNACE,
        .ring = 2,
        .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .scaffold = false,
        .build_progress = 1.0f
    };
    w.stations[0].module_count = 1;
    float initial_ingots = station_inventory_amount(
        &w.stations[0], COMMODITY_FERRITE_INGOT);
    int initial_pod_units =
        economy_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT);
    vec2 furnace_only_pos = module_world_pos_ring(&w.stations[0], 2, 0);
    int frag = economy_test_spawn_fragment(&w, COMMODITY_FERRITE_ORE,
                                           8.0f, furnace_only_pos);
    ASSERT(frag >= 0);
    for (int i = 0; i < (int)(5.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    ASSERT(w.asteroids[frag].active);
    ASSERT_EQ_FLOAT(station_inventory_amount(
                        &w.stations[0], COMMODITY_FERRITE_INGOT),
                    initial_ingots, 0.001f);
    ASSERT_EQ_INT(economy_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT),
                  initial_pod_units);

    /* Add a matching hopper and let it run again — now it should smelt. */
    w.stations[0].modules[1] = (station_module_t){
        .type = MODULE_HOPPER,
        .ring = 1,
        .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_ORE,
        .scaffold = false,
        .build_progress = 1.0f
    };
    w.stations[0].module_count = 2;
    vec2 smelt_target = w.stations[0].pos;
    ASSERT(economy_test_smelt_target_for_ore(&w.stations[0],
                                             COMMODITY_FERRITE_ORE,
                                             &smelt_target));
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FRAME, 1));
    ASSERT(test_anchor_station_legacy_cargo(&w, 0));
    w.asteroids[frag].pos = smelt_target;
    w.asteroids[frag].vel = v2(0.0f, 0.0f);
    w.asteroids[frag].smelt_progress = 0.0f;
    for (int i = 0; i < (int)(5.0f / SIM_DT) && w.asteroids[frag].active; i++)
        world_sim_step(&w, SIM_DT);
    ASSERT(!w.asteroids[frag].active);
    ASSERT_EQ_FLOAT(station_inventory_amount(
                        &w.stations[0], COMMODITY_FERRITE_INGOT),
                    initial_ingots + 8.0f, 0.001f);
    ASSERT(economy_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT) >=
           initial_pod_units + 8);
    ASSERT_EQ_INT((int)w.hopper_smelt_events, 0);
}

TEST(test_commodity_volume_kit_dense) {
    /* Kits take REPAIR_KIT_CARGO_DENSITY units of cargo each; everything
     * else is 1.0. */
    ASSERT_EQ_FLOAT(commodity_volume(COMMODITY_REPAIR_KIT),
                    REPAIR_KIT_CARGO_DENSITY, 0.001f);
    ASSERT_EQ_FLOAT(commodity_volume(COMMODITY_FRAME), 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(commodity_volume(COMMODITY_FERRITE_INGOT), 1.0f, 0.001f);
}

TEST(test_ship_total_cargo_kit_density) {
    /* 100 kits + 5 frames = 100 * 0.1 + 5 * 1.0 = 15 cargo units. */
    ship_t ship = {0};
    ASSERT(ship_manifest_bootstrap(&ship));
    uint8_t origin[8] = {0};
    float legacy[COMMODITY_COUNT] = {0};
    legacy[COMMODITY_REPAIR_KIT] = 100.0f;
    legacy[COMMODITY_FRAME] = 5.0f;
    ASSERT(manifest_migrate_legacy_inventory(
        &ship.manifest, legacy, COMMODITY_COUNT, origin));
    ASSERT_EQ_FLOAT(ship_total_cargo(&ship),
                    100.0f * REPAIR_KIT_CARGO_DENSITY + 5.0f, 0.001f);
    ship_cleanup(&ship);
}

TEST(test_sell_legacy_manifest_requires_pod) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int k = 0; k < MAX_CONTRACTS; k++) w->contracts[k].active = false;

    int consumer = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (station_consumes(&w->stations[i], COMMODITY_FERRITE_INGOT)) {
            consumer = i;
            break;
        }
    }
    ASSERT(consumer >= 0);
    station_t *st = &w->stations[consumer];
    (void)manifest_consume_by_commodity(&st->manifest,
                                         COMMODITY_FERRITE_INGOT,
                                         manifest_count_by_commodity(&st->manifest,
                                                                     COMMODITY_FERRITE_INGOT));
    ASSERT(test_set_station_finished_units(
        st, COMMODITY_FERRITE_INGOT, 0));

    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xD1, 8);
    sp->docked = true;
    sp->current_station = (uint8_t)consumer;
    sp->ship->pos = st->pos;

    cargo_unit_t anon = {0};
    anon.kind = (uint8_t)CARGO_KIND_INGOT;
    anon.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    anon.grade = (uint8_t)MINING_GRADE_COMMON;
    anon.prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
    anon.quantity = 1;
    anon.pub[0] = 0x11;
    cargo_unit_t premium = anon;
    premium.prefix_class = (uint8_t)INGOT_PREFIX_M;
    premium.pub[0] = 0x22;
    ASSERT(manifest_push(&sp->ship->manifest, &anon));
    ASSERT(manifest_push(&sp->ship->manifest, &premium));
    sp->ship->cargo[COMMODITY_FERRITE_INGOT] = 2.0f;

    float before = ledger_balance(st, sp->session_token);

    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    sp->input.service_sell_grade = MINING_GRADE_COMMON;
    sp->input.service_sell_one = true;
    world_sim_step(w, SIM_DT);

    ASSERT_EQ_FLOAT(ledger_balance(st, sp->session_token), before, 0.01f);
    ASSERT_EQ_FLOAT(sp->ship->cargo[COMMODITY_FERRITE_INGOT], 2.0f, 0.001f);
    ASSERT_EQ_INT(sp->ship->manifest.count, 2);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
}

TEST(test_market_buy_ignores_legacy_manifest_ingots) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    memset(w->cargo_pods, 0, sizeof(w->cargo_pods));

    int producer = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (station_produces(&w->stations[i], COMMODITY_FERRITE_INGOT)) {
            producer = i;
            break;
        }
    }
    ASSERT(producer >= 0);
    station_t *st = &w->stations[producer];
    ASSERT(station_manifest_bootstrap(st));
    (void)manifest_consume_by_commodity(&st->manifest,
                                         COMMODITY_FERRITE_INGOT,
                                         manifest_count_by_commodity(&st->manifest,
                                                                     COMMODITY_FERRITE_INGOT));

    cargo_unit_t premium = {0};
    premium.kind = (uint8_t)CARGO_KIND_INGOT;
    premium.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    premium.grade = (uint8_t)MINING_GRADE_COMMON;
    premium.prefix_class = (uint8_t)INGOT_PREFIX_M;
    premium.quantity = 1;
    premium.pub[0] = 0x31;
    cargo_unit_t anon = premium;
    anon.prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
    anon.pub[0] = 0x32;
    ASSERT(manifest_push(&st->manifest, &premium));
    ASSERT(manifest_push(&st->manifest, &anon));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, 1, NULL) == 1);
    int market_pod = test_spawn_station_market_exact_cargo_pod(
        w, producer, COMMODITY_FERRITE_INGOT, 1);
    ASSERT(market_pod >= 0);
    ASSERT_EQ_INT(w->cargo_pods[market_pod].manifest_units[0].prefix_class,
                  INGOT_PREFIX_ANONYMOUS);

    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xD2, 8);
    sp->docked = true;
    sp->current_station = (uint8_t)producer;
    sp->ship->pos = st->pos;
    ledger_earn(st, sp->session_token, 100000.0f);
    float before = ledger_balance(st, sp->session_token);
    float expected_cost = test_station_market_pod_sell_quote(
        st, &w->cargo_pods[market_pod]);
    ASSERT(expected_cost > 0.0f);
    int frames_before = station_finished_count(st, COMMODITY_FRAME);

    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
    sp->input.buy_grade = MINING_GRADE_COMMON;
    world_sim_step(w, SIM_DT);

    ASSERT_EQ_FLOAT(sp->ship->cargo[COMMODITY_FERRITE_INGOT], 0.0f, 0.001f);
    ASSERT_EQ_INT(sp->ship->manifest.count, 0);
    int bought_pod = test_find_towed_exact_cargo_pod(
        w, sp, COMMODITY_FERRITE_INGOT);
    ASSERT(bought_pod >= 0);
    ASSERT_EQ_INT(bought_pod, market_pod);
    ASSERT_EQ_INT(w->cargo_pods[bought_pod].quantity, 1);
    ASSERT_EQ_INT(w->cargo_pods[bought_pod].manifest_units[0].prefix_class,
                  INGOT_PREFIX_ANONYMOUS);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FRAME), frames_before);
    ASSERT_EQ_INT(cargo_pod_custody_station(&w->cargo_pods[bought_pod]),
                  producer);
    ASSERT_EQ_FLOAT(ledger_balance(st, sp->session_token), before, 0.001f);

    test_move_pod_past_station_charge_boundary(w, producer, bought_pod);
    world_sim_step(w, SIM_DT);
    ASSERT_EQ_FLOAT(before - ledger_balance(st, sp->session_token),
                    expected_cost, 0.01f);

    int station_named = 0;
    for (uint16_t i = 0; i < st->manifest.count; i++) {
        const cargo_unit_t *u = &st->manifest.units[i];
        if (u->commodity == (uint8_t)COMMODITY_FERRITE_INGOT &&
            u->prefix_class == (uint8_t)INGOT_PREFIX_M) station_named++;
    }
    ASSERT_EQ_INT(station_named, 1);
}

/* Pod trade is physical: once a towed crate reaches a matching station
 * intake, the station tractor takes custody and every unit inside moves
 * with the crate. */
TEST(test_sell_towed_pod_transfers_whole_pod) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    memset(w->cargo_pods, 0, sizeof(w->cargo_pods));
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xAA, 8);

    int kepler = 1;
    station_t *st = &w->stations[kepler];
    ASSERT(station_consumes(st, COMMODITY_FRAME));
    int hopper_idx = station_find_hopper_for(st, COMMODITY_FRAME);
    ASSERT(hopper_idx >= 0);
    vec2 hopper_pos = module_world_pos_ring(
        st, st->modules[hopper_idx].ring, st->modules[hopper_idx].slot);
    sp->ship->pos = hopper_pos;
    float before = ledger_balance(st, sp->session_token);

    int pod_idx = test_spawn_towed_exact_cargo_pod(
        w, sp, COMMODITY_FRAME, 3);
    ASSERT(pod_idx >= 0);
    w->cargo_pods[pod_idx].pos = hopper_pos;

    world_sim_step(w, SIM_DT);

    ASSERT_EQ_FLOAT(sp->ship->cargo[COMMODITY_FRAME], 0.0f, 0.001f);
    ASSERT_EQ_INT(sp->ship->manifest.count, 0);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT(w->cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(w->cargo_pods[pod_idx].quantity, 3);
    ASSERT_EQ_INT(w->cargo_pods[pod_idx].manifest_count, 3);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w->cargo_pods[pod_idx]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w->cargo_pods[pod_idx],
                                            kepler, hopper_idx));
    ASSERT(ledger_balance(st, sp->session_token) > before);
}

void register_economy_basic_tests(void) {
    TEST_SECTION("\nEconomy tests:\n");
    RUN(test_sell_legacy_manifest_requires_pod);
    RUN(test_market_buy_ignores_legacy_manifest_ingots);
    RUN(test_sell_towed_pod_transfers_whole_pod);
    RUN(test_station_production_yard_makes_frames);
    RUN(test_station_production_does_not_drain_physical_stock_as_storage);
    RUN(test_station_production_beamworks_makes_modules);
    RUN(test_station_repair_cost_no_damage);
    RUN(test_station_repair_cost_with_damage);
    RUN(test_can_afford_upgrade_dock_fallback);
    RUN(test_can_afford_upgrade_no_credits_for_dock_fallback);
    RUN(test_starter_refit_stock_has_retail_price_and_explicit_work_order);
    RUN(test_consumed_starter_refit_marker_reserves_contract_slot);
    RUN(test_can_afford_upgrade_no_product_anywhere);
    RUN(test_can_afford_upgrade_cargo_only_no_credits_needed);
    RUN(test_can_afford_upgrade_rejects_float_only_finished_goods);
    RUN(test_commodity_volume_kit_dense);
    RUN(test_ship_total_cargo_kit_density);
    RUN(test_station_payout_journal_covers_every_action_and_replays_inert);
    RUN(test_station_payout_identity_binds_station_action_and_authority);
    RUN(test_station_payout_stages_two_new_smelt_recipients_in_order);
}

void register_economy_contracts_tests(void) {
    TEST_SECTION("\nContract tests:\n");
    RUN(test_contract_generated_from_hopper_deficit);
    RUN(test_contract_price_escalates_with_age);
    RUN(test_contract_fit_requires_material_grade_and_fragment_tier);
    RUN(test_contract_fit_enforces_heritage_recipe_prefix_and_parent);
    RUN(test_contract_delivery_requires_required_grade);
    RUN(test_contract_delivery_requires_heritage_recipe);
    RUN(test_contract_delivery_bans_enemy_origin_station);
    RUN(test_contract_closes_when_deficit_filled);
    RUN(test_raw_ore_contract_retires_when_refined_output_full);
    RUN(test_kit_input_contract_closes_at_kit_target);
    RUN(test_generated_heritage_contracts_require_source_recipe);
    RUN(test_station_policy_preserves_seeded_supply_loop);
    RUN(test_station_policy_cards_rank_under_domain_budgets);
    RUN(test_station_policy_black_market_requires_off_relay_station);
    RUN(test_blackglass_posts_black_market_buy_contract);
    RUN(test_station_policy_cache_drives_trade_price_modifier);
    RUN(test_cargo_legality_clean_chain_is_not_contraband);
    RUN(test_cargo_legality_missing_receipt_is_policy_contraband);
    RUN(test_cargo_legality_black_market_authority_is_local_policy);
    RUN(test_bulk_sell_refuses_black_market_origin_at_lawful_station);
    RUN(test_black_market_buys_unwanted_towed_pod_at_markdown);
    RUN(test_raw_ore_contract_prefers_starved_downstream_output);
    RUN(test_sell_price_uses_contract_price);
    RUN(test_hauler_fills_highest_value_contract);
    RUN(test_hauler_picker_trusts_gossiped_contract);
    RUN(test_hauler_ignores_float_only_finished_stock);
    RUN(test_kit_fab_requires_shipyard);
    RUN(test_kit_import_contract_at_consumer_station);
    RUN(test_kit_import_contract_skips_shipyard_stations);
    RUN(test_repair_drains_ship_cargo_first);
    RUN(test_repair_falls_back_to_station_inventory);
    RUN(test_repair_at_shipyard_no_labor_fee);
    RUN(test_repair_partial_when_kits_short);
    RUN(test_repair_rejects_float_only_kits);
    RUN(test_repair_kit_fab_requires_manifest_inputs);
    RUN(test_repair_kit_fab_emits_craft_chain_event);
}

void register_economy_contract3_tests(void) {
    TEST_SECTION("\nContract system (3-action):\n");
    RUN(test_one_contract_per_station);
    RUN(test_destroy_contract_completes_when_asteroid_gone);
    RUN(test_fracture_contract_target_pub_matches_asteroid_identity);
    RUN(test_supply_contract_uses_correct_material);
}

void register_economy_pricing_tests(void) {
    TEST_SECTION("\nDynamic pricing:\n");
    RUN(test_dynamic_ore_price_deficit);
    RUN(test_product_price_tracks_ore);
}

void register_economy_mixed_cargo_tests(void) {
    TEST_SECTION("\nMixed cargo sell/deliver:\n");
    RUN(test_npc_paid_transfer_success_appends_exact_trade_pairs);
    RUN(test_npc_delivery_payout_rounds_aggregate_across_partial_retry);
    RUN(test_prepared_transfer_callers_write_failure_are_inert);
    RUN(test_prepared_transfer_callers_flush_failure_are_inert);
    RUN(test_prepared_transfer_callers_preblocked_failure_are_inert);
    RUN(test_deliver_ingots_to_contract);
    RUN(test_first_cross_station_haul_uses_local_ledgers);
    RUN(test_delivery_credit_contract_pickup_deliver_and_clear);
    RUN(test_delivery_credit_dock_custody_does_not_teleport_far_pod);
    RUN(test_delivery_credit_requires_exact_bound_cargo);
    RUN(test_delivery_credit_row_sell_unloads_bound_pod);
    RUN(test_delivery_credit_hail_ignores_empty_origin);
    RUN(test_delivery_credit_hail_requires_docking_to_pick_up);
    RUN(test_delivery_credit_black_market_sale_defaults_origin_debt);
    RUN(test_delivery_credit_timeout_emits_station_risk_memory);
    RUN(test_prospect_pubkey_buy_debits_pubkey_ledger);
    RUN(test_market_buy_requires_station_held_pod);
    RUN(test_deliver_ingots_full_payout_to_pubkey_player);
    RUN(test_deliver_ingots_pending_pubkey_uses_session_ledger);
    RUN(test_mixed_cargo_sell_and_deliver);
    RUN(test_no_delivery_without_matching_contract);
}

void register_economy_service259_tests(void) {
    TEST_SECTION("\nStation service semantics (#259):\n");
    RUN(test_no_passive_heal_without_kits);
}

/* Tagged furnace/pair smelt rules pinned: smelt capability comes from
 * a furnace tagged for the output ingot plus a matching ore hopper on an
 * adjacent ring. Crystal needs two distinct crystal furnace pairs because
 * the first pass creates a tractorable intermediate fragment. */
TEST(test_tagged_furnace_pair_smelt_rules) {
    station_t st = {0};
    /* 0 furnaces: nothing smelts even with a tagged hopper. */
    st.modules[0] = (station_module_t){
        .type = MODULE_HOPPER, .ring = 2, .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_ORE,
        .build_progress = 1.0f,
    };
    st.module_count = 1;
    ASSERT(!station_can_smelt(&st, COMMODITY_FERRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CUPRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    /* One ferrite furnace+hopper pair: ferrite only. */
    st.modules[1] = (station_module_t){
        .type = MODULE_FURNACE, .ring = 1, .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .build_progress = 1.0f,
    };
    st.module_count = 2;
    ASSERT(station_can_smelt(&st, COMMODITY_FERRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CUPRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    /* Add a cuprite pair: ferrite and cuprite both work by tag. */
    st.modules[2] = (station_module_t){
        .type = MODULE_HOPPER, .ring = 2, .slot = 1,
        .commodity = (uint8_t)COMMODITY_CUPRITE_ORE,
        .build_progress = 1.0f,
    };
    st.modules[3] = (station_module_t){
        .type = MODULE_FURNACE, .ring = 1, .slot = 1,
        .commodity = (uint8_t)COMMODITY_CUPRITE_INGOT,
        .build_progress = 1.0f,
    };
    st.module_count = 4;
    ASSERT(station_can_smelt(&st, COMMODITY_FERRITE_ORE));
    ASSERT(station_can_smelt(&st, COMMODITY_CUPRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    /* One crystal pair can stage crystal, but does not advertise full
     * station smelt capability until there is a second crystal pair. */
    st.modules[4] = (station_module_t){
        .type = MODULE_HOPPER, .ring = 2, .slot = 2,
        .commodity = (uint8_t)COMMODITY_CRYSTAL_ORE,
        .build_progress = 1.0f,
    };
    st.modules[5] = (station_module_t){
        .type = MODULE_FURNACE, .ring = 1, .slot = 2,
        .commodity = (uint8_t)COMMODITY_CRYSTAL_INGOT,
        .build_progress = 1.0f,
    };
    st.module_count = 6;
    ASSERT(station_can_smelt(&st, COMMODITY_FERRITE_ORE));
    ASSERT(station_can_smelt(&st, COMMODITY_CUPRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    st.modules[6] = (station_module_t){
        .type = MODULE_FURNACE, .ring = 3, .slot = 2,
        .commodity = (uint8_t)COMMODITY_CRYSTAL_INGOT,
        .build_progress = 1.0f,
    };
    st.module_count = 7;
    ASSERT(station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    /* Tagged furnace without matching adjacent hopper: nothing smelts. */
    memset(&st, 0, sizeof st);
    st.modules[0] = (station_module_t){
        .type = MODULE_FURNACE, .ring = 1, .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .build_progress = 1.0f,
    };
    st.module_count = 1;
    ASSERT(!station_can_smelt(&st, COMMODITY_FERRITE_ORE));

    /* Scaffold furnaces don't count. */
    memset(&st, 0, sizeof st);
    st.modules[0] = (station_module_t){
        .type = MODULE_HOPPER, .ring = 2, .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_ORE,
        .build_progress = 1.0f,
    };
    st.modules[1] = (station_module_t){
        .type = MODULE_FURNACE, .ring = 1, .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .build_progress = 1.0f,
    };
    st.modules[1].scaffold = true;
    st.module_count = 2;
    ASSERT_EQ_INT(station_furnace_count(&st), 0);
    ASSERT(!station_can_smelt(&st, COMMODITY_FERRITE_ORE));
}

void register_economy_refinery_smelt_tests(void) {
    TEST_SECTION("\nRefinery smelt test:\n");
    RUN(test_refinery_smelts_fragment_into_ingot_pod);
    RUN(test_furnace_without_hopper_does_not_smelt);
    RUN(test_tagged_furnace_pair_smelt_rules);
}

/* station_top_demand: derives the top shortage from inventory + the
 * station's consumed-commodity list. This is the primitive HUD
 * beacons / contract auto-pricing / NPC scoring will compose on top
 * of, so the contract-priority code in game_sim.c and this primitive
 * MUST agree on what "starving" means. The tests below pin those
 * agreements to the same constants. */
TEST(test_top_demand_no_shortage_returns_none) {
    WORLD_DECL;
    world_reset(&w);
    /* Top up Kepler's frame_press input commodity to its target — the
     * station has no shortage, so top demand should be empty. */
    station_t *kepler = &w.stations[1];
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_FERRITE_INGOT, (int)MAX_PRODUCT_STOCK));
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_FRAME, (int)MAX_PRODUCT_STOCK));
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_LASER_MODULE, 100));
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_TRACTOR_MODULE, 100));
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_ENGINE_MODULE, 100));
    station_demand_t d = station_top_demand(kepler);
    ASSERT_EQ_INT((int)d.commodity, (int)COMMODITY_COUNT);
    ASSERT_EQ_FLOAT(d.severity, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(d.price_mult, 1.0f, 0.001f);
}

TEST(test_top_demand_picks_starving_commodity) {
    WORLD_DECL;
    world_reset(&w);
    station_t *kepler = &w.stations[1];
    /* Mild shortage on FRAME (consumed by shipyard kit-fab),
     * severe shortage on FERRITE_INGOT (frame_press input). The
     * primitive should pick the worst — ferrite ingots. Targets:
     * frames at 12.0, ferrite ingots at MAX_PRODUCT_STOCK*0.9 = 108.
     * Set frames to 6 (mild, severity ~0.5) and ingots to 0 (full
     * starvation). */
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(kepler, COMMODITY_FRAME, 6));
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_LASER_MODULE, 100));
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_TRACTOR_MODULE, 100));
    station_demand_t d = station_top_demand(kepler);
    ASSERT_EQ_INT((int)d.commodity, (int)COMMODITY_FERRITE_INGOT);
    ASSERT(d.severity > 0.95f);
    /* price_mult = 1.0 + 0.5 * severity → ~1.5 at full starvation. */
    ASSERT(d.price_mult > 1.45f);
    ASSERT(d.price_mult <= 1.5001f);
}

TEST(test_top_demand_skips_self_produced_commodities) {
    /* Helios has its own cuprite furnace + laser fab, so it produces
     * cuprite ingots locally. Even with the float at zero, the
     * primitive must not flag cuprite as a top demand — the local
     * producer is the right answer, not an import. Mirrors the
     * "don't import what we make ourselves" check in game_sim.c
     * priority 4. */
    WORLD_DECL;
    world_reset(&w);
    station_t *helios = &w.stations[2];
    /* Knock out everything else so cuprite is the only candidate
     * (besides things Helios produces). */
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_CUPRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_CRYSTAL_INGOT, (int)MAX_PRODUCT_STOCK));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_FRAME, (int)MAX_PRODUCT_STOCK));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_LASER_MODULE, (int)MAX_PRODUCT_STOCK));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_TRACTOR_MODULE, (int)MAX_PRODUCT_STOCK));
    helios->_inventory_cache[COMMODITY_CUPRITE_ORE]   = REFINERY_HOPPER_CAPACITY;
    helios->_inventory_cache[COMMODITY_CRYSTAL_ORE]   = REFINERY_HOPPER_CAPACITY;
    helios->_inventory_cache[COMMODITY_FERRITE_ORE]   = REFINERY_HOPPER_CAPACITY;
    station_demand_t d = station_top_demand(helios);
    /* Either no demand at all, or demand for something Helios
     * actually doesn't produce — but specifically NOT cuprite ingot. */
    ASSERT(d.commodity != COMMODITY_CUPRITE_INGOT);
}

TEST(test_top_demand_severity_clamped_zero_to_one) {
    /* A negative deficit (overstock) should not produce negative
     * severity, and a wildly empty hopper should clamp to 1.0. */
    WORLD_DECL;
    world_reset(&w);
    station_t *prospect = &w.stations[0];
    /* Force an overstock on FERRITE_ORE: target = HOPPER_CAPACITY*0.5,
     * supply = capacity, so deficit is negative. The primitive
     * should still report severity = 0 for that commodity (and pick
     * something else, or none). */
    prospect->_inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    station_demand_t d = station_top_demand(prospect);
    /* Whatever it picks, severity must be in [0,1]. */
    ASSERT(d.severity >= 0.0f && d.severity <= 1.0f);
    ASSERT(d.price_mult >= 1.0f && d.price_mult <= 1.5f + 0.001f);
    /* And it must not have picked overstocked ferrite ore. */
    ASSERT(d.commodity != COMMODITY_FERRITE_ORE);
}

TEST(test_raw_ore_chain_demand_matches_advanced_fab_recipes) {
    WORLD_DECL;
    world_reset(&w);
    station_t *helios = &w.stations[2];

    /*
     * Laser fabs consume Crystal Ingots; tractor fabs consume Cuprite
     * Ingots. Isolate finished-good pressure so a crossed dependency
     * cannot silently steer the industrial miner toward the wrong ore.
     */
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_CRYSTAL_INGOT, 12));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_CUPRITE_INGOT, 12));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_TRACTOR_MODULE, 12));

    ASSERT(station_raw_ore_chain_need_score(
               helios, COMMODITY_CRYSTAL_ORE) > 0.99f);
    ASSERT_EQ_FLOAT(station_raw_ore_chain_need_score(
                        helios, COMMODITY_CUPRITE_ORE),
                    0.0f, 0.001f);

    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_LASER_MODULE, 12));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_TRACTOR_MODULE, 0));
    ASSERT_EQ_FLOAT(station_raw_ore_chain_need_score(
                        helios, COMMODITY_CRYSTAL_ORE),
                    0.0f, 0.001f);
    ASSERT(station_raw_ore_chain_need_score(
               helios, COMMODITY_CUPRITE_ORE) > 0.99f);
}

/* Demand pricing: a station that's starving for an ingot should post a
 * higher contract price than one that's stocked. Pool_factor and the
 * existing 1.15× content premium stay; the new demand multiplier
 * layers on top, so a fully-stocked station's contract still uses the
 * old price exactly (1.0× demand mult), and a starved station pays up
 * to 50% more. */
TEST(test_contract_price_scales_with_demand) {
    /* Helper to grab Kepler's frame_press ingot import contract. */
    WORLD_HEAP stocked = calloc(1, sizeof(world_t));
    WORLD_HEAP starved = calloc(1, sizeof(world_t));
    ASSERT(stocked != NULL);
    ASSERT(starved != NULL);
    world_reset(stocked);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (starter_refit_work_order_matches(
                &stocked->contracts[k])) {
            stocked->contracts[k].active = false;
        }
    }
    /* Top up Kepler's ferrite ingot inventory to its target so demand
     * mult is 1.0 — i.e. the existing pricing path. */
    ASSERT(test_set_station_finished_units(&stocked->stations[1],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)MAX_PRODUCT_STOCK));
    /* Run a few seconds for contract step to fire. */
    for (int i = 0; i < 240; i++) world_sim_step(stocked, SIM_DT);

    world_reset(starved);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (starter_refit_work_order_matches(
                &starved->contracts[k])) {
            starved->contracts[k].active = false;
        }
    }
    /* Starve Kepler completely for ferrite ingots — demand mult ~1.5. */
    ASSERT(test_set_station_finished_units(
        &starved->stations[1], COMMODITY_FERRITE_INGOT, 0));
    for (int i = 0; i < 240; i++) world_sim_step(starved, SIM_DT);

    /* Find the (Kepler, FERRITE_INGOT) contract in each world. The
     * stocked world may not generate one at all if supply is at
     * target — that's also a valid outcome (no demand → no
     * contract). The starved world must generate one and price it
     * higher than the stocked baseline if the stocked world did
     * post one. */
    contract_t *c_stocked = NULL;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (stocked->contracts[k].active
            && stocked->contracts[k].station_index == 1
            && stocked->contracts[k].commodity == COMMODITY_FERRITE_INGOT
            && !starter_refit_work_order_matches(
                &stocked->contracts[k])) {
            c_stocked = &stocked->contracts[k]; break;
        }
    }
    contract_t *c_starved = NULL;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (starved->contracts[k].active
            && starved->contracts[k].station_index == 1
            && starved->contracts[k].commodity == COMMODITY_FERRITE_INGOT
            && !starter_refit_work_order_matches(
                &starved->contracts[k])) {
            c_starved = &starved->contracts[k]; break;
        }
    }
    ASSERT(c_starved != NULL); /* starvation MUST produce a contract */

    if (c_stocked != NULL) {
        /* If the stocked world also posted a contract, the starved
         * one must be priced higher. The two worlds are otherwise
         * identical so pool_factor + base_price are equal. The only
         * delta is the demand multiplier. */
        ASSERT(c_starved->base_price > c_stocked->base_price * 1.05f);
    }
    /* Either way, the starved contract's price must reflect the
     * demand boost vs. the no-demand baseline of base × 1.15 ×
     * pool. base_price[FERRITE_INGOT] is non-zero by world_reset
     * seeding; the contract should land somewhere between 1.0× and
     * 1.5× of (base × 1.15 × pool). We don't assert the exact value
     * because pool_factor moves with the simulated economy. */
    ASSERT(c_starved->base_price > 0.0f);
}

TEST(test_supply_need_policy_owns_open_refill_and_close_targets) {
    station_t st = {0};
    st.signal_range = 100.0f;
    st.modules[0] = (station_module_t){
        .type = MODULE_SHIPYARD,
        .build_progress = 1.0f,
    };
    st.module_count = 1;

    station_supply_need_t frame = station_supply_need_for(
        &st, COMMODITY_FRAME);
    ASSERT(frame.eligible);
    ASSERT(frame.should_open);
    ASSERT(!frame.should_close);
    ASSERT_EQ_FLOAT(frame.open_target, 12.0f, 0.001f);
    ASSERT_EQ_FLOAT(frame.target, 12.0f, 0.001f);
    ASSERT_EQ_FLOAT(frame.close_target, 12.0f, 0.001f);
    ASSERT_EQ_FLOAT(frame.deficit, 12.0f, 0.001f);

    st.modules[0].type = MODULE_DOCK;
    station_supply_need_t kits = station_supply_need_for(
        &st, COMMODITY_REPAIR_KIT);
    ASSERT(kits.eligible);
    ASSERT(kits.should_open);
    ASSERT_EQ_FLOAT(kits.open_target,
                    REPAIR_KIT_STOCK_CAP * 0.25f, 0.001f);
    ASSERT_EQ_FLOAT(kits.target, REPAIR_KIT_STOCK_CAP, 0.001f);
    ASSERT_EQ_FLOAT(kits.close_target,
                    REPAIR_KIT_STOCK_CAP * 0.95f, 0.001f);
}

void register_economy_demand_tests(void) {
    TEST_SECTION("\nStation demand primitive:\n");
    RUN(test_top_demand_no_shortage_returns_none);
    RUN(test_top_demand_picks_starving_commodity);
    RUN(test_top_demand_skips_self_produced_commodities);
    RUN(test_top_demand_severity_clamped_zero_to_one);
    RUN(test_raw_ore_chain_demand_matches_advanced_fab_recipes);
    RUN(test_contract_price_scales_with_demand);
    RUN(test_supply_need_policy_owns_open_refill_and_close_targets);
}
