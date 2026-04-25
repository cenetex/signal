/*
 * highscore.h — persistent global leaderboard.
 *
 * Server keeps the top-N death runs by `credits_earned`. Entries are
 * pushed at death, persisted to disk on change, and broadcast to all
 * connected clients via NET_MSG_HIGHSCORES so the death cinematic can
 * render the current standings.
 */
#ifndef SIGNAL_HIGHSCORE_H
#define SIGNAL_HIGHSCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

typedef struct {
    char  callsign[8];      /* not NUL-terminated if 8 chars used */
    float credits_earned;
} highscore_entry_t;

typedef struct {
    highscore_entry_t entries[HIGHSCORE_TOP_N];
    int               count;
} highscore_table_t;

void highscore_load(highscore_table_t *t, const char *path);
bool highscore_save(const highscore_table_t *t, const char *path);

/* Returns true if the table was mutated (i.e. entry qualified for top-N). */
bool highscore_submit(highscore_table_t *t,
                      const char *callsign, float credits_earned);

/* Serialize the table as a NET_MSG_HIGHSCORES packet. Returns bytes written.
 * buf must be at least HIGHSCORE_HEADER + HIGHSCORE_TOP_N * HIGHSCORE_ENTRY_SIZE. */
int highscore_serialize(uint8_t *buf, const highscore_table_t *t);

#endif /* SIGNAL_HIGHSCORE_H */
