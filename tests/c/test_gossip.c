#include "test_harness.h"
#include "chain_log.h"
#include "gossip.h"
#include "sim_ai.h"

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
    st->known_contract_count = 0;
    memset(st->known_contracts, 0, sizeof(st->known_contracts));

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

    gossip_dock_handshake(&w, 3,
                          ship.known_contracts,
                          &ship.known_contract_count,
                          SHIP_KNOWN_CONTRACT_CAP,
                          &ship.knowledge);
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

    gossip_dock_handshake(&w, 3,
                          ship.known_contracts,
                          &ship.known_contract_count,
                          SHIP_KNOWN_CONTRACT_CAP,
                          &ship.knowledge);

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

    gossip_dock_handshake(&w, 3,
                          ship.known_contracts,
                          &ship.known_contract_count,
                          SHIP_KNOWN_CONTRACT_CAP,
                          &ship.knowledge);
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
    memset(w.stations[0].known_contracts, 0, sizeof(w.stations[0].known_contracts));
    w.stations[0].known_contract_count = 0;
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
    gossip_dock_handshake(&w, 0,
                          ship.known_contracts,
                          &ship.known_contract_count,
                          SHIP_KNOWN_CONTRACT_CAP,
                          &ship.knowledge);

    ASSERT_EQ_INT(ship.known_contract_count, 1);
    ASSERT_EQ_INT(w.stations[0].known_contract_count, 1);
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
    memset(w.stations[2].known_contracts, 0,
           sizeof(w.stations[2].known_contracts));
    w.stations[2].known_contract_count = 0;
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
    gossip_dock_handshake(&w, 2,
                          ship.known_contracts,
                          &ship.known_contract_count,
                          SHIP_KNOWN_CONTRACT_CAP,
                          &ship.knowledge);

    ASSERT_EQ_INT(ship.known_contract_count, 1);
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
    memset(w.contracts, 0, sizeof(w.contracts));
    memset(w.stations[0].known_contracts, 0,
           sizeof(w.stations[0].known_contracts));
    w.stations[0].known_contract_count = 0;
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

    gossip_dock_handshake(&w, 0,
                          ship.known_contracts,
                          &ship.known_contract_count,
                          SHIP_KNOWN_CONTRACT_CAP,
                          &ship.knowledge);

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
}

TEST(test_dock_gossip_dual_writes_station_supply_memory) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    memset(w.stations[0].known_contracts, 0,
           sizeof(w.stations[0].known_contracts));
    w.stations[0].known_contract_count = 0;
    memset(&w.stations[0].knowledge, 0, sizeof(w.stations[0].knowledge));

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT, 5));

    SHIP_DECL(ship);
    gossip_dock_handshake(&w, 0,
                          ship.known_contracts,
                          &ship.known_contract_count,
                          SHIP_KNOWN_CONTRACT_CAP,
                          &ship.knowledge);

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
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    int a_slot = spawn_npc(&w, 0, NPC_ROLE_MINER);
    int b_slot = spawn_npc(&w, 0, NPC_ROLE_MINER);
    ASSERT(a_slot >= 0);
    ASSERT(b_slot >= 0);
    npc_ship_t *a = &w.npc_ships[a_slot];
    npc_ship_t *b = &w.npc_ships[b_slot];

    a->state = NPC_STATE_IDLE;
    b->state = NPC_STATE_IDLE;
    a->ship.pos = v2(100.0f, 100.0f);
    b->ship.pos = v2(220.0f, 100.0f);
    memset(a->known_contracts, 0, sizeof(a->known_contracts));
    memset(b->known_contracts, 0, sizeof(b->known_contracts));
    a->known_contract_count = 0;
    b->known_contract_count = 0;
    memset(&a->knowledge, 0, sizeof(a->knowledge));
    memset(&b->knowledge, 0, sizeof(b->knowledge));
    knowledge_view_configure(&a->knowledge, SHIP_KNOWN_ITEM_CAP);
    knowledge_view_configure(&b->knowledge, SHIP_KNOWN_ITEM_CAP);
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
    contract_pool_insert(a->known_contracts, &a->known_contract_count,
                         SHIP_KNOWN_CONTRACT_CAP, &contract);

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
    knowledge_view_insert(&a->knowledge, &item);
    ASSERT(gossip_hnn_store_market_memory(&a->hnn_market_mem, &memory));

    ASSERT_EQ_INT(gossip_ship_contact_exchange(&w), 1);

    ASSERT_EQ_INT(b->known_contract_count, 1);
    ASSERT(view_has_market_memory(&b->knowledge,
                                  (uint8_t)MARKET_MEMORY_DEMAND,
                                  2, 0xff,
                                  (uint8_t)COMMODITY_FERRITE_INGOT,
                                  NULL));
    ASSERT(b->hnn_market_mem.experience_count > 0);
    ASSERT(gossip_hnn_market_resonance(&b->hnn_market_mem,
                                       &memory,
                                       GOSSIP_HNN_JOB_HAUL) > 0.05f);
}

TEST(test_bootstrap_seeds_station_local_contracts_only) {
    WORLD_DECL;
    world_reset(&w);

    memset(w.contracts, 0, sizeof(w.contracts));
    for (int i = 0; i < MAX_STATIONS; i++) {
        memset(w.stations[i].known_contracts, 0,
               sizeof(w.stations[i].known_contracts));
        w.stations[i].known_contract_count = 0;
        memset(&w.stations[i].knowledge, 0, sizeof(w.stations[i].knowledge));
    }

    w.contracts[0].active = true;
    w.contracts[0].action = CONTRACT_TRACTOR;
    w.contracts[0].station_index = 2;
    w.contracts[0].commodity = COMMODITY_FERRITE_INGOT;
    w.contracts[0].quantity_needed = 4.0f;
    w.contracts[0].base_price = 80.0f;

    gossip_bootstrap_world_stations(&w);

    ASSERT_EQ_INT(w.stations[2].known_contract_count, 1);
    ASSERT_EQ_INT(w.stations[2].known_contracts[0].station_index, 2);
    ASSERT_EQ_INT(w.stations[2].known_contracts[0].commodity,
                  COMMODITY_FERRITE_INGOT);
    ASSERT(view_has_contract(&w.stations[2].knowledge,
                             (uint8_t)CONTRACT_TRACTOR,
                             2,
                             (uint8_t)COMMODITY_FERRITE_INGOT,
                             NULL));

    ASSERT_EQ_INT(w.stations[0].known_contract_count, 0);
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
    RUN(test_dock_gossip_dual_writes_station_supply_memory);
    RUN(test_ship_contact_gossip_exchanges_memory_and_holograms);
    RUN(test_bootstrap_seeds_station_local_contracts_only);
}
