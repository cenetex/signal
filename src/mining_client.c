/*
 * mining_client.c -- Client-side RATi session state.
 *
 * Identity is derived from the server-issued session_token, so the
 * client and server agree on the player's mining key without any
 * separate keypair store. Roll outcomes ride the wire as part of
 * SIM_EVENT_SELL; this TU just keeps a session tally.
 */
#include "mining_client.h"

#include <string.h>

static mining_client_t g_mining;

void mining_client_init(void) {
    memset(&g_mining, 0, sizeof(g_mining));
    /* player_ready stays false until set_session_token is called. */
}

void mining_client_set_session_token(const uint8_t token[8]) {
    sha256_bytes(token, 8, g_mining.player_key);
    mining_callsign_from_pubkey(g_mining.player_key, g_mining.player_callsign);
    g_mining.player_ready = true;
}

void mining_client_record_strike(mining_grade_t grade, int bonus_cr) {
    if ((int)grade < 0 || grade >= MINING_GRADE_COUNT) return;
    g_mining.strikes_by_grade[grade]++;
    g_mining.bonus_cr_total += bonus_cr;
    if (grade >= MINING_GRADE_RARE) {
        g_mining.recent_strike_timer = 3.0f;
        g_mining.recent_strike_grade = grade;
        g_mining.recent_strike_bonus = bonus_cr;
    }
}

void mining_client_tick(float dt) {
    if (g_mining.recent_strike_timer > 0.0f) {
        g_mining.recent_strike_timer -= dt;
        if (g_mining.recent_strike_timer < 0.0f) g_mining.recent_strike_timer = 0.0f;
    }
}

const mining_client_t *mining_client_get(void) {
    return &g_mining;
}
