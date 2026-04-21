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

bool mining_client_search_fracture(uint32_t fracture_id, const uint8_t seed[32],
                                   uint32_t deadline_ms, uint16_t burst_cap,
                                   mining_client_claim_t *out_claim) {
    mining_grade_t best_grade = MINING_GRADE_COMMON;
    uint32_t best_nonce = 0;

    (void)deadline_ms; /* Server enforces the deadline; client races to beat it. */
    if (!out_claim || !seed || !g_mining.player_ready || burst_cap == 0) return false;
    for (uint32_t n = 0; n < burst_cap; n++) {
        mining_keypair_t kp;
        char callsign[8];
        mining_grade_t grade;

        mining_keypair_derive(seed, g_mining.player_key, n, &kp);
        mining_callsign_from_pubkey(kp.pub, callsign);
        grade = mining_classify_base58(callsign);
        if (grade > best_grade) {
            best_grade = grade;
            best_nonce = n;
        }
    }

    out_claim->fracture_id = fracture_id;
    out_claim->burst_nonce = best_nonce;
    out_claim->claimed_grade = best_grade;
    /* Track the active fracture so resolve can clear the HUD state on arrival.
     * The 0.6s timer is a cosmetic cap; if the server answer beats it,
     * mining_client_resolve_fracture clears both. */
    g_mining.fracture_search_id = fracture_id;
    if (g_mining.fracture_search_timer < 0.6f)
        g_mining.fracture_search_timer = 0.6f;
    return true;
}

void mining_client_resolve_fracture(uint32_t fracture_id, mining_grade_t grade) {
    (void)grade;
    /* Clear MINING... // CLAIM WINDOW the instant the server's resolve
     * arrives — without this, the badge could linger past a sub-second
     * resolution and leave the player thinking a claim is still open.
     * Only clear if this is the fracture we were racing; stale resolves
     * (e.g. from a different nearby fragment) are ignored. */
    if (g_mining.fracture_search_id && g_mining.fracture_search_id == fracture_id) {
        g_mining.fracture_search_id = 0;
        g_mining.fracture_search_timer = 0.0f;
    }
}

void mining_client_tick(float dt) {
    if (g_mining.recent_strike_timer > 0.0f) {
        g_mining.recent_strike_timer -= dt;
        if (g_mining.recent_strike_timer < 0.0f) g_mining.recent_strike_timer = 0.0f;
    }
    if (g_mining.fracture_search_timer > 0.0f) {
        g_mining.fracture_search_timer -= dt;
        if (g_mining.fracture_search_timer < 0.0f) g_mining.fracture_search_timer = 0.0f;
    }
}

const mining_client_t *mining_client_get(void) {
    return &g_mining;
}
