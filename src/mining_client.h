/*
 * mining_client.h -- Client-side RATi session state.
 *
 * "RATi IS the ore." There is no separate keypair inventory — the
 * grade roll is authoritative on the server at tractor time, keyed
 * to (fragment.fracture_seed, player.session_token). The client just
 * displays what it sees on the wire.
 *
 * Player identity = sha256(session_token). The 7-char callsign is
 * the first 7 chars of base58 of that hash. Server and client agree
 * because they derive from the same input — no separate keypair to
 * store, no localStorage needed for identity.
 */
#ifndef CLIENT_MINING_H
#define CLIENT_MINING_H

#include <stdbool.h>
#include <stdint.h>

#include "mining.h"     /* shared/mining.h — grade enum, payout_multiplier */

typedef struct {
    bool     player_ready;
    uint8_t  player_key[32];          /* sha256(session_token) */
    char     player_callsign[8];

    /* Per-session stats: how many smelts of each grade this session,
     * and the total bonus credits earned above the baseline ore price. */
    int      strikes_by_grade[MINING_GRADE_COUNT];
    int      bonus_cr_total;

    /* HUD feedback for the most recent rare strike. */
    float          recent_strike_timer;
    mining_grade_t recent_strike_grade;
    int            recent_strike_bonus;
} mining_client_t;

/* Reset to empty state. Called once at startup. */
void mining_client_init(void);

/* Bind to the server-issued session_token. Called from the JOIN /
 * SESSION handlers as soon as the wire delivers it. Recomputes
 * player_key + player_callsign. */
void mining_client_set_session_token(const uint8_t token[8]);

/* Record a strike reported by SIM_EVENT_SELL. */
void mining_client_record_strike(mining_grade_t grade, int bonus_cr);

/* Per-frame timers (fades the recent-strike HUD badge). */
void mining_client_tick(float dt);

/* Read-only accessor for HUD / station UI. */
const mining_client_t *mining_client_get(void);

#endif /* CLIENT_MINING_H */
