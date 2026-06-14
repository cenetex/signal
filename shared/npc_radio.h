#ifndef NPC_RADIO_H
#define NPC_RADIO_H

#include "types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    NPC_RADIO_HAIL_CONVERSATION_LIMIT = 4,
    NPC_RADIO_LINE_LEN = 96,
    NPC_RADIO_CHOICE_COUNT = 3,
    NPC_RADIO_CHOICE_PROMPT_LEN = 1024,
};

typedef struct {
    int npc_index;
    float at_s;
    char line[NPC_RADIO_LINE_LEN];
} npc_radio_hail_entry_t;

bool npc_radio_line(const station_t stations[MAX_STATIONS],
                    const npc_ship_t *npc,
                    int npc_slot,
                    char *out,
                    size_t out_size);

uint8_t npc_radio_player_choice_candidates(
    char out[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN]);

uint8_t npc_radio_player_choice_candidates_for_hail(
    uint32_t hail_salt,
    char out[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN]);

bool npc_radio_player_line(char *out, size_t out_size);

uint8_t npc_radio_choice_candidates(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t *npc,
    int npc_slot,
    char out[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN]);

uint8_t npc_radio_choice_candidates_for_hail(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t *npc,
    int npc_slot,
    uint32_t hail_salt,
    char out[NPC_RADIO_CHOICE_COUNT][NPC_RADIO_LINE_LEN]);

size_t npc_radio_build_choice_prompt(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t npcs[MAX_NPC_SHIPS],
    const npc_radio_hail_entry_t entries[NPC_RADIO_HAIL_CONVERSATION_LIMIT],
    uint8_t entry_count,
    char *out,
    size_t out_size);

size_t npc_radio_build_choice_prompt_for_hail(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t npcs[MAX_NPC_SHIPS],
    const npc_radio_hail_entry_t entries[NPC_RADIO_HAIL_CONVERSATION_LIMIT],
    uint8_t entry_count,
    uint32_t hail_salt,
    char *out,
    size_t out_size);

uint8_t npc_radio_apply_choice_response(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t npcs[MAX_NPC_SHIPS],
    const char *response,
    npc_radio_hail_entry_t entries[NPC_RADIO_HAIL_CONVERSATION_LIMIT],
    uint8_t entry_count);

uint8_t npc_radio_apply_choice_response_for_hail(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t npcs[MAX_NPC_SHIPS],
    const char *response,
    uint32_t hail_salt,
    npc_radio_hail_entry_t entries[NPC_RADIO_HAIL_CONVERSATION_LIMIT],
    uint8_t entry_count);

bool npc_radio_apply_player_choice_response(const char *response,
                                            char *out,
                                            size_t out_size);

bool npc_radio_apply_player_choice_response_for_hail(const char *response,
                                                     uint32_t hail_salt,
                                                     char *out,
                                                     size_t out_size);

uint8_t npc_radio_build_hail_conversation(
    const station_t stations[MAX_STATIONS],
    const npc_ship_t npcs[MAX_NPC_SHIPS],
    vec2 origin,
    float range,
    npc_radio_hail_entry_t out[NPC_RADIO_HAIL_CONVERSATION_LIMIT]);

#endif /* NPC_RADIO_H */
