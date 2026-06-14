#include "npc_radio.h"

#include "commodity.h"
#include "module_schema.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int index;
    float dist_sq;
} hail_pick_t;

static void hail_conversation_sort(hail_pick_t *picks, int count) {
    for (int i = 1; i < count; i++) {
        hail_pick_t key = picks[i];
        int j = i - 1;
        while (j >= 0 && picks[j].dist_sq > key.dist_sq) {
            picks[j + 1] = picks[j];
            j--;
        }
        picks[j + 1] = key;
    }
}

static const char *station_label(const station_t stations[MAX_STATIONS],
                                 int station_idx,
                                 char *scratch,
                                 size_t scratch_size) {
    if (!scratch || scratch_size == 0) return "open signal";
    if (!stations || station_idx < 0 || station_idx >= MAX_STATIONS ||
        !stations[station_idx].name[0]) {
        snprintf(scratch, scratch_size, "open signal");
        return scratch;
    }

    const char *name = stations[station_idx].name;
    if (strcmp(name, "Prospect Refinery") == 0) {
        snprintf(scratch, scratch_size, "Prospect Ref");
    } else {
        snprintf(scratch, scratch_size, "%s", name);
    }
    return scratch;
}

static bool market_memory_from_item(const knowledge_item_t *item,
                                    market_memory_t *out) {
    if (!item || !out) return false;
    if (item->payload_kind != (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY)
        return false;
    market_memory_t memory;
    memset(&memory, 0, sizeof(memory));
    memcpy(&memory, item->payload, sizeof(memory));
    if (!memory.active ||
        memory.memory_kind == (uint8_t)MARKET_MEMORY_NONE)
        return false;
    memory.confidence = item->confidence;
    memory.salience = item->salience;
    *out = memory;
    return true;
}

static bool best_market_memory(const npc_ship_t *npc,
                               market_memory_t *out) {
    if (!npc || !out) return false;
    uint8_t count = npc->knowledge.count;
    if (count > npc->knowledge.capacity && npc->knowledge.capacity > 0)
        count = npc->knowledge.capacity;
    if (count > KNOWLEDGE_VIEW_MAX_CAP) count = KNOWLEDGE_VIEW_MAX_CAP;

    int best_score = -1;
    market_memory_t best = {0};
    for (uint8_t i = 0; i < count; i++) {
        market_memory_t memory;
        if (!market_memory_from_item(&npc->knowledge.items[i], &memory))
            continue;
        if (memory.commodity >= (uint8_t)COMMODITY_COUNT &&
            memory.memory_kind != (uint8_t)MARKET_MEMORY_SCAFFOLD_PRESSURE)
            continue;
        int score = (int)memory.confidence * (int)memory.salience;
        if (memory.memory_kind == (uint8_t)MARKET_MEMORY_ORE_PRESSURE)
            score += 2000;
        if (memory.memory_kind == (uint8_t)MARKET_MEMORY_SCAFFOLD_PRESSURE)
            score += 1500;
        if (score > best_score) {
            best_score = score;
            best = memory;
        }
    }
    if (best_score < 0) return false;
    *out = best;
    return true;
}

static const contract_summary_t *first_contract(const npc_ship_t *npc) {
    if (!npc) return NULL;
    uint8_t count = npc->known_contract_count;
    if (count > SHIP_KNOWN_CONTRACT_CAP) count = SHIP_KNOWN_CONTRACT_CAP;
    for (uint8_t i = 0; i < count; i++) {
        if (npc->known_contracts[i].active &&
            npc->known_contracts[i].commodity < (uint8_t)COMMODITY_COUNT) {
            return &npc->known_contracts[i];
        }
    }
    return NULL;
}

static bool station_index_valid(int station_idx) {
    return station_idx >= 0 && station_idx < MAX_STATIONS;
}

static uint8_t format_scaffold_candidates(
    const station_t stations[MAX_STATIONS],
    const market_memory_t *memory,
    char out[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN]) {
    if (!memory ||
        memory->memory_kind != (uint8_t)MARKET_MEMORY_SCAFFOLD_PRESSURE)
        return 0;

    int dest_station = memory->station_a;
    int source_station = memory->station_b == 0xffu ? -1 : memory->station_b;
    module_type_t module_type = MODULE_COUNT;
    if (memory->quantity_hint < (uint16_t)MODULE_COUNT)
        module_type = (module_type_t)memory->quantity_hint;
    if (!station_index_valid(dest_station) || module_type == MODULE_COUNT)
        return 0;

    char dest[32];
    char source[32];
    const char *dest_name = station_label(stations, dest_station,
                                          dest, sizeof(dest));
    const char *source_name = station_label(stations, source_station,
                                            source, sizeof(source));
    const char *module_name = module_type_name(module_type);

    snprintf(out[0], NPC_RADIO_LINE_LEN, "%s scaffold awake at %s.",
             module_name, dest_name);
    if (source_station >= 0)
        snprintf(out[1], NPC_RADIO_LINE_LEN, "%s kit path %s>%s.",
                 module_name, source_name, dest_name);
    else
        snprintf(out[1], NPC_RADIO_LINE_LEN, "%s kit path into %s.",
                 module_name, dest_name);
    snprintf(out[2], NPC_RADIO_LINE_LEN, "%s build signal holds at %s.",
             module_name, dest_name);
    return NPC_RADIO_CHOICE_COUNT;
}

static bool memory_uses_route(const market_memory_t *memory) {
    if (!memory) return false;
    return memory->memory_kind == (uint8_t)MARKET_MEMORY_ROUTE_DANGER ||
           memory->memory_kind == (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS ||
           memory->memory_kind == (uint8_t)MARKET_MEMORY_DELIVERY_RECEIPT ||
           memory->memory_kind == (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION ||
           memory->memory_kind == (uint8_t)MARKET_MEMORY_ROUTE_RISK;
}

static uint8_t format_hauler_candidates(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t *npc,
    const market_memory_t *memory,
    const contract_summary_t *contract,
    char out[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN]) {
    uint8_t commodity = COMMODITY_COUNT;
    int source_station = npc ? npc->home_station : -1;
    int dest_station = npc ? npc->dest_station : -1;
    int quantity = 0;

    uint8_t scaffold_count = format_scaffold_candidates(stations, memory, out);
    if (scaffold_count > 0) return scaffold_count;

    if (contract) {
        commodity = contract->commodity;
        source_station = contract->station_index;
        quantity = (int)(contract->quantity_needed + 0.5f);
    }
    if (memory && memory->commodity < (uint8_t)COMMODITY_COUNT) {
        commodity = memory->commodity;
        if (!contract && memory_uses_route(memory)) {
            source_station = memory->station_b == 0xffu
                ? source_station
                : memory->station_b;
            dest_station = memory->station_a;
            quantity = memory->quantity_hint;
        } else if (!contract) {
            source_station = memory->station_a;
            quantity = memory->quantity_hint;
        }
    }
    if (commodity >= (uint8_t)COMMODITY_COUNT) return 0;

    char source[32];
    char dest[32];
    const char *source_name = station_label(stations, source_station,
                                            source, sizeof(source));
    const char *dest_name = station_label(stations, dest_station,
                                          dest, sizeof(dest));
    const char *code = commodity_code((commodity_t)commodity);

    if (memory && memory->memory_kind == (uint8_t)MARKET_MEMORY_SUPPLY) {
        snprintf(out[0], NPC_RADIO_LINE_LEN, "%s stack warm at %s.",
                 code, source_name);
        snprintf(out[1], NPC_RADIO_LINE_LEN, "%s %s stock is moving.",
                 source_name, code);
        snprintf(out[2], NPC_RADIO_LINE_LEN, "%s supply glows at %s.",
                 code, source_name);
        return NPC_RADIO_CHOICE_COUNT;
    }

    if (memory &&
        (memory->memory_kind == (uint8_t)MARKET_MEMORY_ROUTE_DANGER ||
         memory->memory_kind == (uint8_t)MARKET_MEMORY_ROUTE_RISK)) {
        snprintf(out[0], NPC_RADIO_LINE_LEN, "%s risk %s>%s.",
                 code, source_name, dest_name);
        snprintf(out[1], NPC_RADIO_LINE_LEN, "%s>%s reads rough.",
                 source_name, dest_name);
        snprintf(out[2], NPC_RADIO_LINE_LEN, "%s lane hazard near %s.",
                 code, dest_name);
        return NPC_RADIO_CHOICE_COUNT;
    }

    if (memory &&
        (memory->memory_kind == (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS ||
         memory->memory_kind == (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION)) {
        snprintf(out[0], NPC_RADIO_LINE_LEN, "%s lane %s>%s runs clean.",
                 code, source_name, dest_name);
        snprintf(out[1], NPC_RADIO_LINE_LEN, "%s>%s has clean pay.",
                 source_name, dest_name);
        snprintf(out[2], NPC_RADIO_LINE_LEN, "%s route %s>%s is trusted.",
                 code, source_name, dest_name);
        return NPC_RADIO_CHOICE_COUNT;
    }

    if (memory &&
        memory->memory_kind == (uint8_t)MARKET_MEMORY_DELIVERY_RECEIPT) {
        if (quantity > 0)
            snprintf(out[0], NPC_RADIO_LINE_LEN, "%d %s landed %s>%s.",
                     quantity, code, source_name, dest_name);
        else
            snprintf(out[0], NPC_RADIO_LINE_LEN, "%s landed %s>%s.",
                     code, source_name, dest_name);
        snprintf(out[1], NPC_RADIO_LINE_LEN, "%s receipt %s>%s.",
                 code, source_name, dest_name);
        snprintf(out[2], NPC_RADIO_LINE_LEN, "%s delivery mark at %s.",
                 code, dest_name);
        return NPC_RADIO_CHOICE_COUNT;
    }

    if (memory &&
        memory->memory_kind == (uint8_t)MARKET_MEMORY_STATION_TRUST) {
        snprintf(out[0], NPC_RADIO_LINE_LEN, "%s desk at %s pays clean.",
                 code, source_name);
        snprintf(out[1], NPC_RADIO_LINE_LEN, "%s trusts %s work.",
                 source_name, code);
        snprintf(out[2], NPC_RADIO_LINE_LEN, "%s %s mark is clean.",
                 source_name, code);
        return NPC_RADIO_CHOICE_COUNT;
    }

    if (memory &&
        memory->memory_kind == (uint8_t)MARKET_MEMORY_STATION_RISK) {
        snprintf(out[0], NPC_RADIO_LINE_LEN, "%s desk at %s reads sharp.",
                 code, source_name);
        snprintf(out[1], NPC_RADIO_LINE_LEN, "%s %s work has teeth.",
                 source_name, code);
        snprintf(out[2], NPC_RADIO_LINE_LEN, "%s risk mark at %s.",
                 code, source_name);
        return NPC_RADIO_CHOICE_COUNT;
    }

    if (dest_station >= 0) {
        if (quantity > 0)
            snprintf(out[0], NPC_RADIO_LINE_LEN,
                     "%d %s tagged %s>%s.",
                     quantity, code, source_name, dest_name);
        else
            snprintf(out[0], NPC_RADIO_LINE_LEN,
                     "%s tagged %s>%s.",
                     code, source_name, dest_name);
        snprintf(out[1], NPC_RADIO_LINE_LEN,
                 "%s board at %s; %s wants it.",
                 code, source_name, dest_name);
        snprintf(out[2], NPC_RADIO_LINE_LEN,
                 "%s lane %s>%s lit.",
                 code, source_name, dest_name);
        return NPC_RADIO_CHOICE_COUNT;
    } else {
        snprintf(out[0], NPC_RADIO_LINE_LEN, "%s demand mark holding at %s.",
                 code, source_name);
        if (quantity > 0)
            snprintf(out[1], NPC_RADIO_LINE_LEN,
                     "%d %s haul still open at %s.",
                     quantity, code, source_name);
        else
            snprintf(out[1], NPC_RADIO_LINE_LEN,
                     "%s haul still open at %s.",
                     code, source_name);
        snprintf(out[2], NPC_RADIO_LINE_LEN, "%s %s board is awake.",
                 source_name, code);
        return NPC_RADIO_CHOICE_COUNT;
    }
}

static uint8_t format_miner_candidates(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t *npc,
    const market_memory_t *memory,
    const contract_summary_t *contract,
    char out[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN]) {
    uint8_t commodity = COMMODITY_COUNT;
    int station = npc ? npc->home_station : -1;
    bool pressure = false;

    uint8_t scaffold_count = format_scaffold_candidates(stations, memory, out);
    if (scaffold_count > 0) return scaffold_count;

    if (memory && memory->commodity < (uint8_t)COMMODITY_COUNT) {
        commodity = memory->commodity;
        station = memory->station_a;
        pressure = memory->memory_kind == (uint8_t)MARKET_MEMORY_ORE_PRESSURE;
    } else if (contract) {
        commodity = contract->commodity;
        station = contract->station_index;
    }
    if (commodity >= (uint8_t)COMMODITY_COUNT) return 0;

    char station_buf[32];
    const char *station_name = station_label(stations, station,
                                             station_buf, sizeof(station_buf));
    const char *code = commodity_code((commodity_t)commodity);

    if (pressure) {
        snprintf(out[0], NPC_RADIO_LINE_LEN, "%s pressure bright at %s.",
                 code, station_name);
        snprintf(out[1], NPC_RADIO_LINE_LEN, "%s %s seam is talking.",
                 station_name, code);
        snprintf(out[2], NPC_RADIO_LINE_LEN, "%s pressure mark holding near %s.",
                 code, station_name);
        return NPC_RADIO_CHOICE_COUNT;
    } else {
        snprintf(out[0], NPC_RADIO_LINE_LEN, "%s mark awake near %s.",
                 code, station_name);
        snprintf(out[1], NPC_RADIO_LINE_LEN, "%s wants %s; rock is speaking.",
                 station_name, code);
        snprintf(out[2], NPC_RADIO_LINE_LEN, "%s trace holding near %s.",
                 code, station_name);
        return NPC_RADIO_CHOICE_COUNT;
    }
}

static void rotate_candidates(
    char candidates[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN],
    uint8_t count,
    int offset_seed) {
    if (count <= 1) return;
    int offset = offset_seed < 0 ? 0 : offset_seed % count;
    if (offset <= 0) return;

    char tmp[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN];
    memcpy(tmp, candidates, sizeof(tmp));
    for (uint8_t i = 0; i < count; i++) {
        snprintf(candidates[i], NPC_RADIO_LINE_LEN, "%s",
                 tmp[(i + offset) % count]);
    }
}

static const char *npc_radio_role_label(const npc_ship_t *npc) {
    if (!npc) return "WORKER";
    if (npc->role == NPC_ROLE_MINER) return "MINER";
    if (npc->role == NPC_ROLE_HAULER) return "HAULER";
    return "WORKER";
}

uint8_t npc_radio_player_choice_candidates(
    char out[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN]) {
    if (!out) return 0;
    memset(out, 0, NPC_RADIO_CHOICE_COUNT * NPC_RADIO_LINE_LEN);
    snprintf(out[0], NPC_RADIO_LINE_LEN, "Open hail; local traffic check.");
    snprintf(out[1], NPC_RADIO_LINE_LEN, "Local traffic, sound off.");
    snprintf(out[2], NPC_RADIO_LINE_LEN, "Open channel; nearby traffic check.");
    return NPC_RADIO_CHOICE_COUNT;
}

uint8_t npc_radio_player_choice_candidates_for_hail(
    uint32_t hail_salt,
    char out[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN]) {
    uint8_t count = npc_radio_player_choice_candidates(out);
    rotate_candidates(out, count, (int)(hail_salt % NPC_RADIO_CHOICE_COUNT));
    return count;
}

uint8_t npc_radio_choice_candidates(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t *npc,
    int npc_slot,
    char out[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN]) {
    return npc_radio_choice_candidates_for_hail(stations, npc, npc_slot, 0,
                                               out);
}

uint8_t npc_radio_choice_candidates_for_hail(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t *npc,
    int npc_slot,
    uint32_t hail_salt,
    char out[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN]) {
    if (!npc || !out) return 0;
    memset(out, 0, NPC_RADIO_CHOICE_COUNT * NPC_RADIO_LINE_LEN);

    market_memory_t memory;
    market_memory_t *memory_ptr = NULL;
    if (best_market_memory(npc, &memory)) memory_ptr = &memory;
    const contract_summary_t *contract = first_contract(npc);

    uint8_t count = 0;
    if (npc->role == NPC_ROLE_HAULER) {
        count = format_hauler_candidates(stations, npc, memory_ptr,
                                         contract, out);
    } else if (npc->role == NPC_ROLE_MINER) {
        count = format_miner_candidates(stations, npc, memory_ptr,
                                        contract, out);
    }
    int salt = (int)(hail_salt % NPC_RADIO_CHOICE_COUNT);
    rotate_candidates(out, count, npc_slot + salt);
    return count;
}

static void append_promptf(char *out,
                           size_t out_size,
                           size_t *used,
                           size_t *needed,
                           const char *fmt,
                           ...) {
    if (!used || !needed || !fmt) return;
    va_list args;
    va_start(args, fmt);
    int n = 0;
    if (out && out_size > 0 && *used < out_size) {
        n = vsnprintf(out + *used, out_size - *used, fmt, args);
    } else {
        n = vsnprintf(NULL, 0, fmt, args);
    }
    va_end(args);
    if (n < 0) return;

    *needed += (size_t)n;
    if (out && out_size > 0) {
        size_t available = *used < out_size ? out_size - *used : 0;
        if ((size_t)n < available) {
            *used += (size_t)n;
        } else {
            *used = out_size - 1;
        }
        out[*used] = '\0';
    }
}

size_t npc_radio_build_choice_prompt(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t npcs[MAX_NPC_SHIPS],
    const npc_radio_hail_entry_t entries[NPC_RADIO_HAIL_CONVERSATION_LIMIT],
    uint8_t entry_count,
    char *out,
    size_t out_size) {
    return npc_radio_build_choice_prompt_for_hail(
        stations, npcs, entries, entry_count, 0, out, out_size);
}

size_t npc_radio_build_choice_prompt_for_hail(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t npcs[MAX_NPC_SHIPS],
    const npc_radio_hail_entry_t entries[NPC_RADIO_HAIL_CONVERSATION_LIMIT],
    uint8_t entry_count,
    uint32_t hail_salt,
    char *out,
    size_t out_size) {
    if (out && out_size > 0) out[0] = '\0';
    if (!npcs || !entries) return 0;
    if (entry_count > NPC_RADIO_HAIL_CONVERSATION_LIMIT)
        entry_count = NPC_RADIO_HAIL_CONVERSATION_LIMIT;

    size_t used = 0;
    size_t needed = 0;
    bool included_npcs[MAX_NPC_SHIPS] = {0};
    append_promptf(out, out_size, &used, &needed,
                   "local hail choices\n");
    append_promptf(out, out_size, &used, &needed, "YOU:\n");
    char player_choices[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN];
    uint8_t player_count =
        npc_radio_player_choice_candidates_for_hail(hail_salt,
                                                   player_choices);
    for (uint8_t c = 0; c < player_count; c++) {
        append_promptf(out, out_size, &used, &needed, "%u %s\n",
                       (unsigned)(c + 1), player_choices[c]);
    }

    for (uint8_t i = 0; i < entry_count; i++) {
        int npc_index = entries[i].npc_index;
        if (npc_index < 0 || npc_index >= MAX_NPC_SHIPS) continue;
        const npc_ship_t *npc = &npcs[npc_index];
        char choices[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN];
        uint8_t count = npc_radio_choice_candidates_for_hail(
            stations, npc, npc_index, hail_salt, choices);
        if (count == 0) continue;
        included_npcs[npc_index] = true;
        append_promptf(out, out_size, &used, &needed, "%s N%02d:\n",
                       npc_radio_role_label(npc), npc_index);
        for (uint8_t c = 0; c < count; c++) {
            append_promptf(out, out_size, &used, &needed, "%u %s\n",
                           (unsigned)(c + 1), choices[c]);
        }
    }

    append_promptf(out, out_size, &used, &needed,
                   "Return all speaker keys. No words.\n"
                   "Example: YOU=%u",
                   (unsigned)((hail_salt % NPC_RADIO_CHOICE_COUNT) + 1));
    for (uint8_t i = 0; i < entry_count; i++) {
        int npc_index = entries[i].npc_index;
        if (npc_index < 0 || npc_index >= MAX_NPC_SHIPS) continue;
        if (!included_npcs[npc_index]) continue;
        uint32_t target =
            ((uint32_t)i + hail_salt + NPC_RADIO_CHOICE_COUNT - 1) %
            NPC_RADIO_CHOICE_COUNT;
        uint32_t offset =
            ((uint32_t)npc_index + hail_salt) % NPC_RADIO_CHOICE_COUNT;
        unsigned example_choice =
            (unsigned)(((target + NPC_RADIO_CHOICE_COUNT - offset) %
                        NPC_RADIO_CHOICE_COUNT) + 1);
        append_promptf(out, out_size, &used, &needed, ",N%02d=%u",
                       npc_index, example_choice);
    }
    append_promptf(out, out_size, &used, &needed,
                   "\nANSWER:");
    return needed;
}

static void parse_choice_numbers(const char *response,
                                 int choices[MAX_NPC_SHIPS],
                                 int bare[1 + NPC_RADIO_HAIL_CONVERSATION_LIMIT],
                                 int *bare_count) {
    if (bare_count) *bare_count = 0;
    if (!response) return;

    const char *p = response;
    while (*p) {
        if ((*p == 'N' || *p == 'n') &&
            p[1] >= '0' && p[1] <= '9') {
            const char *q = p + 1;
            int slot = 0;
            int digits = 0;
            while (*q >= '0' && *q <= '9' && digits < 2) {
                slot = slot * 10 + (*q - '0');
                q++;
                digits++;
            }
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                if (*q >= '1' && *q <= '3' && slot >= 0 &&
                    slot < MAX_NPC_SHIPS) {
                    choices[slot] = *q - '0';
                    q++;
                }
            }
            p = q;
            continue;
        }

        if (*p >= '1' && *p <= '3' && bare && bare_count &&
            *bare_count < 1 + NPC_RADIO_HAIL_CONVERSATION_LIMIT) {
            bool part_of_assignment = false;
            const char *q = p + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') part_of_assignment = true;
            if (!part_of_assignment)
                bare[(*bare_count)++] = *p - '0';
        }
        p++;
    }
}

uint8_t npc_radio_apply_choice_response(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t npcs[MAX_NPC_SHIPS],
    const char *response,
    npc_radio_hail_entry_t entries[NPC_RADIO_HAIL_CONVERSATION_LIMIT],
    uint8_t entry_count) {
    return npc_radio_apply_choice_response_for_hail(
        stations, npcs, response, 0, entries, entry_count);
}

uint8_t npc_radio_apply_choice_response_for_hail(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t npcs[MAX_NPC_SHIPS],
    const char *response,
    uint32_t hail_salt,
    npc_radio_hail_entry_t entries[NPC_RADIO_HAIL_CONVERSATION_LIMIT],
    uint8_t entry_count) {
    if (!npcs || !entries || !response) return 0;
    if (entry_count > NPC_RADIO_HAIL_CONVERSATION_LIMIT)
        entry_count = NPC_RADIO_HAIL_CONVERSATION_LIMIT;

    int choices[MAX_NPC_SHIPS];
    for (int i = 0; i < MAX_NPC_SHIPS; i++) choices[i] = 0;
    int bare[1 + NPC_RADIO_HAIL_CONVERSATION_LIMIT] = {0};
    int bare_count = 0;
    parse_choice_numbers(response, choices, bare, &bare_count);

    uint8_t applied = 0;
    for (uint8_t i = 0; i < entry_count; i++) {
        int npc_index = entries[i].npc_index;
        if (npc_index < 0 || npc_index >= MAX_NPC_SHIPS) continue;
        int choice = choices[npc_index];
        if (choice == 0 && bare_count >= (int)entry_count + 1) {
            choice = bare[i + 1]; /* first bare number belongs to YOU */
        } else if (choice == 0 && bare_count >= (int)entry_count) {
            choice = bare[i];
        }
        if (choice < 1 || choice > NPC_RADIO_CHOICE_COUNT) continue;

        char candidates[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN];
        uint8_t count = npc_radio_choice_candidates_for_hail(
            stations, &npcs[npc_index], npc_index, hail_salt, candidates);
        if (choice > (int)count) continue;
        snprintf(entries[i].line, sizeof(entries[i].line), "%s",
                 candidates[choice - 1]);
        applied++;
    }
    return applied;
}

bool npc_radio_player_line(char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    char candidates[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN];
    if (npc_radio_player_choice_candidates(candidates) == 0) return false;
    snprintf(out, out_size, "%s", candidates[0]);
    return true;
}

bool npc_radio_apply_player_choice_response(const char *response,
                                            char *out,
                                            size_t out_size) {
    return npc_radio_apply_player_choice_response_for_hail(response, 0, out,
                                                          out_size);
}

bool npc_radio_apply_player_choice_response_for_hail(const char *response,
                                                     uint32_t hail_salt,
                                                     char *out,
                                                     size_t out_size) {
    if (!response || !out || out_size == 0) return false;
    int choices[MAX_NPC_SHIPS];
    for (int i = 0; i < MAX_NPC_SHIPS; i++) choices[i] = 0;
    int bare[1 + NPC_RADIO_HAIL_CONVERSATION_LIMIT] = {0};
    int bare_count = 0;
    parse_choice_numbers(response, choices, bare, &bare_count);

    int choice = 0;
    const char *p = response;
    while (*p) {
        if ((p[0] == 'Y' || p[0] == 'y') &&
            (p[1] == 'O' || p[1] == 'o') &&
            (p[2] == 'U' || p[2] == 'u')) {
            const char *q = p + 3;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                if (*q >= '1' && *q <= '3') {
                    choice = *q - '0';
                    break;
                }
            }
        }
        p++;
    }
    if (choice == 0 && bare_count > 0) choice = bare[0];
    if (choice < 1 || choice > NPC_RADIO_CHOICE_COUNT) return false;

    char candidates[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN];
    uint8_t count = npc_radio_player_choice_candidates_for_hail(hail_salt,
                                                               candidates);
    if (choice > (int)count) return false;
    snprintf(out, out_size, "%s", candidates[choice - 1]);
    return true;
}

bool npc_radio_line(const station_t stations[MAX_STATIONS],
                    const npc_ship_t *npc,
                    int npc_slot,
                    char *out,
                    size_t out_size) {
    if (!npc || !out || out_size == 0) return false;
    out[0] = '\0';

    char candidates[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN];
    uint8_t count = npc_radio_choice_candidates(stations, npc, npc_slot,
                                                candidates);
    if (count == 0) return false;
    snprintf(out, out_size, "%s", candidates[0]);
    return true;
}

uint8_t npc_radio_build_hail_conversation(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t npcs[MAX_NPC_SHIPS],
    vec2 origin,
    float range,
    npc_radio_hail_entry_t out[NPC_RADIO_HAIL_CONVERSATION_LIMIT]) {
    if (!npcs || !out || range <= 0.0f) return 0;

    hail_pick_t picks[NPC_RADIO_HAIL_CONVERSATION_LIMIT];
    int pick_count = 0;
    float range_sq = range * range;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *npc = &npcs[i];
        if (!npc->active) continue;
        float dist_sq = v2_dist_sq(npc->ship.pos, origin);
        if (dist_sq > range_sq) continue;

        if (pick_count < NPC_RADIO_HAIL_CONVERSATION_LIMIT) {
            picks[pick_count++] = (hail_pick_t){ i, dist_sq };
        } else {
            int worst = 0;
            for (int j = 1; j < NPC_RADIO_HAIL_CONVERSATION_LIMIT; j++) {
                if (picks[j].dist_sq > picks[worst].dist_sq) worst = j;
            }
            if (dist_sq < picks[worst].dist_sq)
                picks[worst] = (hail_pick_t){ i, dist_sq };
        }
    }
    hail_conversation_sort(picks, pick_count);

    for (int i = 0; i < pick_count; i++) {
        const npc_ship_t *npc = &npcs[picks[i].index];
        npc_radio_hail_entry_t *entry = &out[i];
        entry->npc_index = picks[i].index;
        entry->at_s = 1.2f + (float)i * 2.0f;
        if (!npc_radio_line(stations, npc, picks[i].index,
                            entry->line, sizeof(entry->line))) {
            if (npc->role == NPC_ROLE_MINER)
                snprintf(entry->line, sizeof(entry->line),
                         "Miner signal holding.");
            else if (npc->role == NPC_ROLE_HAULER)
                snprintf(entry->line, sizeof(entry->line),
                         "Hauler signal holding.");
            else
                snprintf(entry->line, sizeof(entry->line),
                         "Worker signal holding.");
        }
    }
    return (uint8_t)pick_count;
}
