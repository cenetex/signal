#include "test_harness.h"
#include "gossip.h"

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
}

void register_gossip_tests(void);
void register_gossip_tests(void) {
    TEST_SECTION("\nGossip knowledge:\n");
    RUN(test_knowledge_contract_summary_adapter_round_trips);
    RUN(test_knowledge_view_contract_dedup_keeps_newer_snapshot);
    RUN(test_knowledge_exchange_copies_bounded_items_bidirectionally);
    RUN(test_dock_gossip_dual_writes_contract_knowledge);
}
