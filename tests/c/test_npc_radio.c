#include "test_harness.h"

#include "npc_radio.h"
#include "gossip.h"

static void add_memory(npc_ship_t *npc,
                       market_memory_kind_t kind,
                       uint8_t station_a,
                       uint8_t station_b,
                       commodity_t commodity,
                       uint16_t quantity_hint) {
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);
    ASSERT(npc->knowledge.count < SHIP_KNOWN_ITEM_CAP);

    market_memory_t memory = {0};
    memory.active = true;
    memory.memory_kind = (uint8_t)kind;
    memory.station_a = station_a;
    memory.station_b = station_b;
    memory.commodity = (uint8_t)commodity;
    memory.action = (uint8_t)CONTRACT_TRACTOR;
    memory.confidence = 220;
    memory.salience = 210;
    memory.quantity_hint = quantity_hint;

    knowledge_item_t *item = &npc->knowledge.items[npc->knowledge.count++];
    memset(item, 0, sizeof(*item));
    item->kind = (uint8_t)KNOW_MARKET;
    item->confidence = memory.confidence;
    item->salience = memory.salience;
    item->payload_kind = (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY;
    memcpy(item->payload, &memory, sizeof(memory));
}

static void add_haul_contract(npc_ship_t *npc,
                              uint8_t station_index,
                              commodity_t commodity,
                              float quantity_needed) {
    ASSERT(npc->known_contract_count < SHIP_KNOWN_CONTRACT_CAP);
    contract_summary_t *contract =
        &npc->known_contracts[npc->known_contract_count++];
    memset(contract, 0, sizeof(*contract));
    contract->active = true;
    contract->action = (uint8_t)CONTRACT_TRACTOR;
    contract->station_index = station_index;
    contract->commodity = (uint8_t)commodity;
    contract->quantity_needed = quantity_needed;
}

static npc_ship_t *place_npc(world_t *w,
                             int index,
                             npc_role_t role,
                             vec2 pos) {
    if (!w || index < 0 || index >= MAX_NPC_SHIPS) return NULL;
    npc_ship_t *npc = &w->npc_ships[index];
    memset(npc, 0, sizeof(*npc));
    npc->active = true;
    npc->role = role;
    npc->ship.pos = pos;
    npc->home_station = 0;
    npc->dest_station = 2;
    return npc;
}

static bool line_seen_before(char seen[][NPC_RADIO_LINE_LEN],
                             int seen_count,
                             const char *line) {
    for (int i = 0; i < seen_count; i++) {
        if (strcmp(seen[i], line) == 0) return true;
    }
    return false;
}

TEST(test_npc_radio_miner_uses_ore_pressure_memory) {
    WORLD_DECL;
    world_reset(&w);

    npc_ship_t npc = {0};
    npc.role = NPC_ROLE_MINER;
    npc.home_station = 0;
    add_memory(&npc, MARKET_MEMORY_ORE_PRESSURE, 0, 0xff,
               COMMODITY_FERRITE_ORE, 84);

    char line[96];
    ASSERT(npc_radio_line(w.stations, &npc, 0, line, sizeof(line)));
    ASSERT_STR_EQ(line, "FE pressure bright at Prospect Ref.");

    ASSERT(npc_radio_line(w.stations, &npc, 1, line, sizeof(line)));
    ASSERT_STR_EQ(line, "Prospect Ref FE seam is talking.");
}

TEST(test_npc_radio_hauler_uses_contract_and_destination) {
    WORLD_DECL;
    world_reset(&w);

    npc_ship_t npc = {0};
    npc.role = NPC_ROLE_HAULER;
    npc.home_station = 1;
    npc.dest_station = 2;
    add_haul_contract(&npc, 1, COMMODITY_FERRITE_INGOT, 108.0f);
    add_memory(&npc, MARKET_MEMORY_DEMAND, 1, 0xff,
               COMMODITY_FERRITE_INGOT, 108);

    char line[96];
    ASSERT(npc_radio_line(w.stations, &npc, 2, line, sizeof(line)));
    ASSERT_STR_EQ(line, "FR lane Kepler Yard>Helios Works lit.");

    ASSERT(npc_radio_line(w.stations, &npc, 3, line, sizeof(line)));
    ASSERT_STR_EQ(line, "108 FR tagged Kepler Yard>Helios Works.");
}

TEST(test_npc_radio_returns_false_without_grounding) {
    WORLD_DECL;
    world_reset(&w);

    npc_ship_t npc = {0};
    npc.role = NPC_ROLE_HAULER;
    npc.home_station = 1;
    npc.dest_station = 2;

    char line[96];
    ASSERT(!npc_radio_line(w.stations, &npc, 2, line, sizeof(line)));
    ASSERT_STR_EQ(line, "");
}

TEST(test_npc_radio_choice_candidates_rotate_by_slot) {
    WORLD_DECL;
    world_reset(&w);

    npc_ship_t hauler = {0};
    hauler.role = NPC_ROLE_HAULER;
    hauler.home_station = 1;
    hauler.dest_station = 2;
    add_haul_contract(&hauler, 1, COMMODITY_FERRITE_INGOT, 108.0f);
    add_memory(&hauler, MARKET_MEMORY_DEMAND, 1, 0xff,
               COMMODITY_FERRITE_INGOT, 108);

    char choices[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN];
    uint8_t count = npc_radio_choice_candidates(w.stations, &hauler, 2,
                                                choices);
    ASSERT_EQ_INT(count, 3);
    ASSERT_STR_EQ(choices[0], "FR lane Kepler Yard>Helios Works lit.");
    ASSERT_STR_EQ(choices[1], "108 FR tagged Kepler Yard>Helios Works.");
    ASSERT_STR_EQ(choices[2], "FR board at Kepler Yard; Helios Works wants it.");

    npc_ship_t miner = {0};
    miner.role = NPC_ROLE_MINER;
    miner.home_station = 0;
    add_memory(&miner, MARKET_MEMORY_ORE_PRESSURE, 0, 0xff,
               COMMODITY_FERRITE_ORE, 84);
    count = npc_radio_choice_candidates(w.stations, &miner, 1, choices);
    ASSERT_EQ_INT(count, 3);
    ASSERT_STR_EQ(choices[0], "Prospect Ref FE seam is talking.");
    ASSERT_STR_EQ(choices[1], "FE pressure mark holding near Prospect Ref.");
    ASSERT_STR_EQ(choices[2], "FE pressure bright at Prospect Ref.");
}

TEST(test_npc_radio_formats_supply_route_and_scaffold_memories) {
    WORLD_DECL;
    world_reset(&w);

    char choices[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN];

    npc_ship_t supply = {0};
    supply.role = NPC_ROLE_HAULER;
    supply.home_station = 1;
    supply.dest_station = 2;
    add_memory(&supply, MARKET_MEMORY_SUPPLY, 1, 0xff,
               COMMODITY_FERRITE_INGOT, 12);
    uint8_t count = npc_radio_choice_candidates(w.stations, &supply, 0,
                                                choices);
    ASSERT_EQ_INT(count, 3);
    ASSERT_STR_EQ(choices[0], "FR stack warm at Kepler Yard.");
    ASSERT_STR_EQ(choices[1], "Kepler Yard FR stock is moving.");
    ASSERT_STR_EQ(choices[2], "FR supply glows at Kepler Yard.");

    npc_ship_t risk = {0};
    risk.role = NPC_ROLE_HAULER;
    risk.home_station = 0;
    risk.dest_station = 0;
    add_memory(&risk, MARKET_MEMORY_ROUTE_RISK, 2, 1,
               COMMODITY_FERRITE_INGOT, 4);
    count = npc_radio_choice_candidates(w.stations, &risk, 0, choices);
    ASSERT_EQ_INT(count, 3);
    ASSERT_STR_EQ(choices[0], "FR risk Kepler Yard>Helios Works.");
    ASSERT_STR_EQ(choices[1], "Kepler Yard>Helios Works reads rough.");
    ASSERT_STR_EQ(choices[2], "FR lane hazard near Helios Works.");

    npc_ship_t scaffold = {0};
    scaffold.role = NPC_ROLE_MINER;
    scaffold.home_station = 0;
    scaffold.dest_station = 0;
    add_memory(&scaffold, MARKET_MEMORY_SCAFFOLD_PRESSURE, 2, 1,
               COMMODITY_COUNT, MODULE_FRAME_PRESS);
    count = npc_radio_choice_candidates(w.stations, &scaffold, 0, choices);
    ASSERT_EQ_INT(count, 3);
    ASSERT_STR_EQ(choices[0], "Frame Press scaffold awake at Helios Works.");
    ASSERT_STR_EQ(choices[1], "Frame Press kit path Kepler Yard>Helios Works.");
    ASSERT_STR_EQ(choices[2], "Frame Press build signal holds at Helios Works.");
}

TEST(test_npc_radio_choice_prompt_and_response_apply) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.npc_ships, 0, sizeof(w.npc_ships));

    npc_ship_t *hauler = place_npc(&w, 2, NPC_ROLE_HAULER, v2(10.0f, 0.0f));
    ASSERT(hauler != NULL);
    hauler->home_station = 1;
    hauler->dest_station = 2;
    add_haul_contract(hauler, 1, COMMODITY_FERRITE_INGOT, 108.0f);
    add_memory(hauler, MARKET_MEMORY_DEMAND, 1, 0xff,
               COMMODITY_FERRITE_INGOT, 108);

    npc_ship_t *miner = place_npc(&w, 7, NPC_ROLE_MINER, v2(30.0f, 0.0f));
    ASSERT(miner != NULL);
    add_memory(miner, MARKET_MEMORY_ORE_PRESSURE, 0, 0xff,
               COMMODITY_FERRITE_ORE, 84);

    npc_radio_hail_entry_t entries[NPC_RADIO_HAIL_CONVERSATION_LIMIT] = {0};
    uint8_t count = npc_radio_build_hail_conversation(
        w.stations, w.npc_ships, v2(0.0f, 0.0f), 60.0f, entries);
    ASSERT_EQ_INT(count, 2);

    char prompt[1024];
    size_t needed = npc_radio_build_choice_prompt(
        w.stations, w.npc_ships, entries, count, prompt, sizeof(prompt));
    ASSERT(needed > 0);
    ASSERT(strstr(prompt, "local hail choices") != NULL);
    ASSERT(strstr(prompt, "YOU:") != NULL);
    ASSERT(strstr(prompt, "1 Open hail; local traffic check.") != NULL);
    ASSERT(strstr(prompt, "2 Local traffic, sound off.") != NULL);
    ASSERT(strstr(prompt, "HAULER N02:") != NULL);
    ASSERT(strstr(prompt, "1 FR lane Kepler Yard>Helios Works lit.") != NULL);
    ASSERT(strstr(prompt, "MINER N07:") != NULL);
    ASSERT(strstr(prompt, "Example: YOU=1,N02=1,N07=3") != NULL);
    ASSERT(strstr(prompt, "ANSWER:") != NULL);

    uint8_t applied = npc_radio_apply_choice_response(
        w.stations, w.npc_ships, "YOU=1,N02=2,N07=3", entries, count);
    ASSERT_EQ_INT(applied, 2);
    ASSERT_STR_EQ(entries[0].line, "108 FR tagged Kepler Yard>Helios Works.");
    ASSERT_STR_EQ(entries[1].line, "FE pressure bright at Prospect Ref.");

    applied = npc_radio_apply_choice_response(
        w.stations, w.npc_ships, "HAULER N02=3, MINER N07=1",
        entries, count);
    ASSERT_EQ_INT(applied, 2);
    ASSERT_STR_EQ(entries[0].line, "FR board at Kepler Yard; Helios Works wants it.");
    ASSERT_STR_EQ(entries[1].line, "Prospect Ref FE seam is talking.");

    applied = npc_radio_apply_choice_response(
        w.stations, w.npc_ships, "1,3,2", entries, count);
    ASSERT_EQ_INT(applied, 2);
    ASSERT_STR_EQ(entries[0].line, "FR board at Kepler Yard; Helios Works wants it.");
    ASSERT_STR_EQ(entries[1].line, "FE pressure mark holding near Prospect Ref.");

    applied = npc_radio_apply_choice_response(
        w.stations, w.npc_ships, "N02=4,N07=1", entries, count);
    ASSERT_EQ_INT(applied, 1);
    ASSERT_STR_EQ(entries[0].line, "FR board at Kepler Yard; Helios Works wants it.");
    ASSERT_STR_EQ(entries[1].line, "Prospect Ref FE seam is talking.");
}

TEST(test_npc_radio_choice_prompt_varies_by_hail_request) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.npc_ships, 0, sizeof(w.npc_ships));

    npc_ship_t *hauler = place_npc(&w, 2, NPC_ROLE_HAULER, v2(10.0f, 0.0f));
    ASSERT(hauler != NULL);
    hauler->home_station = 1;
    hauler->dest_station = 2;
    add_haul_contract(hauler, 1, COMMODITY_FERRITE_INGOT, 108.0f);
    add_memory(hauler, MARKET_MEMORY_DEMAND, 1, 0xff,
               COMMODITY_FERRITE_INGOT, 108);

    npc_radio_hail_entry_t entries[NPC_RADIO_HAIL_CONVERSATION_LIMIT] = {0};
    uint8_t count = npc_radio_build_hail_conversation(
        w.stations, w.npc_ships, v2(0.0f, 0.0f), 60.0f, entries);
    ASSERT_EQ_INT(count, 1);

    char prompt[1024];
    size_t needed = npc_radio_build_choice_prompt_for_hail(
        w.stations, w.npc_ships, entries, count, 1,
        prompt, sizeof(prompt));
    ASSERT(needed > 0);
    ASSERT(strstr(prompt, "YOU:") != NULL);
    ASSERT(strstr(prompt, "1 Local traffic, sound off.") != NULL);
    ASSERT(strstr(prompt, "HAULER N02:") != NULL);
    ASSERT(strstr(prompt, "1 108 FR tagged Kepler Yard>Helios Works.") != NULL);
    ASSERT(strstr(prompt, "Example: YOU=2,N02=1") != NULL);

    char player_line[NPC_RADIO_LINE_LEN];
    ASSERT(npc_radio_apply_player_choice_response_for_hail(
        "YOU=1,N02=1", 1, player_line, sizeof(player_line)));
    ASSERT_STR_EQ(player_line, "Local traffic, sound off.");

    uint8_t applied = npc_radio_apply_choice_response_for_hail(
        w.stations, w.npc_ships, "YOU=1,N02=1", 1, entries, count);
    ASSERT_EQ_INT(applied, 1);
    ASSERT_STR_EQ(entries[0].line, "108 FR tagged Kepler Yard>Helios Works.");
}

TEST(test_npc_radio_choice_prompt_omits_ungrounded_example_keys) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.npc_ships, 0, sizeof(w.npc_ships));

    npc_ship_t *ungrounded =
        place_npc(&w, 1, NPC_ROLE_HAULER, v2(10.0f, 0.0f));
    ASSERT(ungrounded != NULL);
    ungrounded->home_station = 1;
    ungrounded->dest_station = 2;

    npc_ship_t *hauler = place_npc(&w, 2, NPC_ROLE_HAULER, v2(20.0f, 0.0f));
    ASSERT(hauler != NULL);
    hauler->home_station = 1;
    hauler->dest_station = 2;
    add_haul_contract(hauler, 1, COMMODITY_FERRITE_INGOT, 108.0f);
    add_memory(hauler, MARKET_MEMORY_DEMAND, 1, 0xff,
               COMMODITY_FERRITE_INGOT, 108);

    npc_radio_hail_entry_t entries[NPC_RADIO_HAIL_CONVERSATION_LIMIT] = {0};
    uint8_t count = npc_radio_build_hail_conversation(
        w.stations, w.npc_ships, v2(0.0f, 0.0f), 60.0f, entries);
    ASSERT_EQ_INT(count, 2);

    char prompt[1024];
    size_t needed = npc_radio_build_choice_prompt_for_hail(
        w.stations, w.npc_ships, entries, count, 1,
        prompt, sizeof(prompt));
    ASSERT(needed > 0);
    ASSERT(strstr(prompt, "HAULER N01:") == NULL);
    ASSERT(strstr(prompt, "HAULER N02:") != NULL);
    ASSERT(strstr(prompt, "N01=") == NULL);
    ASSERT(strstr(prompt, "N02=") != NULL);
}

TEST(test_npc_radio_player_choices_apply) {
    char choices[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN];
    uint8_t count = npc_radio_player_choice_candidates(choices);
    ASSERT_EQ_INT(count, 3);
    ASSERT_STR_EQ(choices[0], "Open hail; local traffic check.");
    ASSERT_STR_EQ(choices[1], "Local traffic, sound off.");
    ASSERT_STR_EQ(choices[2], "Open channel; nearby traffic check.");

    char line[NPC_RADIO_LINE_LEN];
    ASSERT(npc_radio_player_line(line, sizeof(line)));
    ASSERT_STR_EQ(line, "Open hail; local traffic check.");

    ASSERT(npc_radio_apply_player_choice_response("YOU=2,N00=1",
                                                  line, sizeof(line)));
    ASSERT_STR_EQ(line, "Local traffic, sound off.");

    ASSERT(npc_radio_apply_player_choice_response("3,1,2",
                                                  line, sizeof(line)));
    ASSERT_STR_EQ(line, "Open channel; nearby traffic check.");

    ASSERT(!npc_radio_apply_player_choice_response("YOU=4",
                                                   line, sizeof(line)));
}

TEST(test_npc_radio_hail_conversation_orders_by_proximity) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.npc_ships, 0, sizeof(w.npc_ships));

    npc_ship_t *far_miner = place_npc(&w, 4, NPC_ROLE_MINER, v2(50.0f, 0.0f));
    ASSERT(far_miner != NULL);
    add_memory(far_miner, MARKET_MEMORY_ORE_PRESSURE, 0, 0xff,
               COMMODITY_FERRITE_ORE, 84);

    npc_ship_t *near_hauler = place_npc(&w, 2, NPC_ROLE_HAULER, v2(10.0f, 0.0f));
    ASSERT(near_hauler != NULL);
    near_hauler->home_station = 1;
    near_hauler->dest_station = 2;
    add_haul_contract(near_hauler, 1, COMMODITY_FERRITE_INGOT, 108.0f);
    add_memory(near_hauler, MARKET_MEMORY_DEMAND, 1, 0xff,
               COMMODITY_FERRITE_INGOT, 108);

    npc_ship_t *mid_miner = place_npc(&w, 7, NPC_ROLE_MINER, v2(30.0f, 0.0f));
    ASSERT(mid_miner != NULL);
    add_memory(mid_miner, MARKET_MEMORY_ORE_PRESSURE, 0, 0xff,
               COMMODITY_FERRITE_ORE, 84);

    npc_radio_hail_entry_t entries[NPC_RADIO_HAIL_CONVERSATION_LIMIT] = {0};
    uint8_t count = npc_radio_build_hail_conversation(
        w.stations, w.npc_ships, v2(0.0f, 0.0f), 60.0f, entries);

    ASSERT_EQ_INT(count, 3);
    ASSERT_EQ_INT(entries[0].npc_index, 2);
    ASSERT_EQ_INT(entries[1].npc_index, 7);
    ASSERT_EQ_INT(entries[2].npc_index, 4);
    ASSERT_EQ_FLOAT(entries[0].at_s, 1.2f, 0.001f);
    ASSERT_EQ_FLOAT(entries[1].at_s, 3.2f, 0.001f);
    ASSERT_EQ_FLOAT(entries[2].at_s, 5.2f, 0.001f);
    ASSERT_STR_EQ(entries[0].line, "FR lane Kepler Yard>Helios Works lit.");
    ASSERT_STR_EQ(entries[1].line, "Prospect Ref FE seam is talking.");
    ASSERT_STR_EQ(entries[2].line, "Prospect Ref FE seam is talking.");
}

TEST(test_npc_radio_hail_conversation_caps_nearest_four) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.npc_ships, 0, sizeof(w.npc_ships));

    for (int i = 0; i < 6; i++) {
        npc_ship_t *npc = place_npc(&w, i, NPC_ROLE_MINER,
                                    v2(10.0f + (float)i * 10.0f, 0.0f));
        ASSERT(npc != NULL);
        add_memory(npc, MARKET_MEMORY_ORE_PRESSURE, 0, 0xff,
                   COMMODITY_FERRITE_ORE, 84);
    }

    npc_radio_hail_entry_t entries[NPC_RADIO_HAIL_CONVERSATION_LIMIT] = {0};
    uint8_t count = npc_radio_build_hail_conversation(
        w.stations, w.npc_ships, v2(0.0f, 0.0f), 55.0f, entries);

    ASSERT_EQ_INT(count, NPC_RADIO_HAIL_CONVERSATION_LIMIT);
    ASSERT_EQ_INT(entries[0].npc_index, 0);
    ASSERT_EQ_INT(entries[1].npc_index, 1);
    ASSERT_EQ_INT(entries[2].npc_index, 2);
    ASSERT_EQ_INT(entries[3].npc_index, 3);
}

TEST(test_npc_radio_formats_real_simulated_npc_memories) {
    WORLD_DECL;
    world_reset(&w);

    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        w.npc_ships[i].active = false;

    int slots[3] = {
        spawn_npc(&w, 0, NPC_ROLE_MINER),
        spawn_npc(&w, 1, NPC_ROLE_HAULER),
        spawn_npc(&w, 2, NPC_ROLE_HAULER),
    };
    for (int i = 0; i < 3; i++) {
        ASSERT(slots[i] >= 0);
        npc_ship_t *npc = &w.npc_ships[slots[i]];
        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = 0.0f;
        memset(&npc->knowledge, 0, sizeof(npc->knowledge));
        knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);
        add_memory(npc,
                   i == 0 ? MARKET_MEMORY_ORE_PRESSURE :
                   i == 1 ? MARKET_MEMORY_DEMAND :
                            MARKET_MEMORY_ROUTE_DANGER,
                   i == 0 ? 0 : (uint8_t)i,
                   i == 2 ? 1 : 0xff,
                   i == 0 ? COMMODITY_FERRITE_ORE : COMMODITY_FERRITE_INGOT,
                   (uint16_t)(30 + i * 10));
    }

    int grounded_npcs = 0;
    int radio_ready_npcs = 0;
    char seen[16][NPC_RADIO_LINE_LEN] = {{0}};
    int seen_count = 0;

    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *npc = &w.npc_ships[i];
        if (!npc->active) continue;

        bool has_market_memory = false;
        uint8_t knowledge_count = npc->knowledge.count;
        if (knowledge_count > KNOWLEDGE_VIEW_MAX_CAP)
            knowledge_count = KNOWLEDGE_VIEW_MAX_CAP;
        for (uint8_t k = 0; k < knowledge_count; k++) {
            market_memory_t memory;
            if (market_memory_from_knowledge_item(&npc->knowledge.items[k],
                                                  &memory)) {
                has_market_memory = true;
                break;
            }
        }
        if (!has_market_memory) continue;
        grounded_npcs++;

        char choices[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN];
        uint8_t count = npc_radio_choice_candidates_for_hail(
            w.stations, npc, i, 1, choices);
        ASSERT_EQ_INT(count, NPC_RADIO_CHOICE_COUNT);
        for (uint8_t c = 0; c < count; c++) {
            ASSERT(strlen(choices[c]) > 8);
            ASSERT(strlen(choices[c]) < NPC_RADIO_LINE_LEN);
            ASSERT(strstr(choices[c], "open signal") == NULL);
        }
        if (!line_seen_before(seen, seen_count, choices[0]) &&
            seen_count < 16) {
            snprintf(seen[seen_count++], NPC_RADIO_LINE_LEN, "%s",
                     choices[0]);
        }
        radio_ready_npcs++;
    }

    ASSERT(grounded_npcs >= 3);
    ASSERT(radio_ready_npcs >= 3);
    ASSERT(seen_count >= 2);
}

void register_npc_radio_tests(void) {
    RUN(test_npc_radio_miner_uses_ore_pressure_memory);
    RUN(test_npc_radio_hauler_uses_contract_and_destination);
    RUN(test_npc_radio_returns_false_without_grounding);
    RUN(test_npc_radio_choice_candidates_rotate_by_slot);
    RUN(test_npc_radio_formats_supply_route_and_scaffold_memories);
    RUN(test_npc_radio_choice_prompt_and_response_apply);
    RUN(test_npc_radio_choice_prompt_varies_by_hail_request);
    RUN(test_npc_radio_choice_prompt_omits_ungrounded_example_keys);
    RUN(test_npc_radio_player_choices_apply);
    RUN(test_npc_radio_hail_conversation_orders_by_proximity);
    RUN(test_npc_radio_hail_conversation_caps_nearest_four);
    RUN(test_npc_radio_formats_real_simulated_npc_memories);
}
