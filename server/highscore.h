/*
 * highscore.h — in-memory leaderboard projected from the chain log.
 *
 * The leaderboard is no longer persisted to a flat file. Per-station
 * chain logs (server/chain_log.{c,h}) are the canonical record:
 * SIM_EVENT_DEATH emits a CHAIN_EVT_DEATH event signed by station 0,
 * and at server boot highscore_replay_from_chain() walks every
 * `chain/<base58(pubkey)>.log` file and projects death events into
 * the in-memory highscore_table_t. Old chain files from worlds with
 * different belt_seeds survive as orphans and contribute alongside
 * the current world's runs (each row is tagged with its world_id).
 */
#ifndef SIGNAL_HIGHSCORE_H
#define SIGNAL_HIGHSCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

typedef struct {
    char     callsign[8];        /* not NUL-terminated if 8 chars used */
    float    credits_earned;
    /* Phase 2: world + build identity, killer attribution. */
    uint32_t world_id;           /* low 32 bits of belt_seed (or station-pub-derived for orphans) */
    uint32_t world_seq;          /* monotonic world id; newer-world-wins dedup */
    uint32_t build_id;           /* low 32 bits of build SHA at run time */
    uint64_t epoch_tick;         /* sim tick at death */
    uint8_t  killed_by[8];       /* killer callsign, all-zero if unresolved */
} highscore_entry_t;

typedef struct {
    highscore_entry_t entries[HIGHSCORE_TOP_N];
    int               count;
} highscore_table_t;

/* Submit a candidate run. Returns true if the table was mutated.
 * Dedup is by callsign only: a newer world (greater world_seq) replaces
 * any prior entry for the same callsign regardless of credits; same-seq
 * resubmissions promote only when credits beat the existing score; an
 * older world cannot displace a newer one. */
bool highscore_submit(highscore_table_t *t,
                      const char *callsign, float credits_earned,
                      uint32_t world_id, uint32_t world_seq,
                      uint32_t build_id,
                      uint64_t epoch_tick, const uint8_t killed_by[8]);

/* Walk every `*.log` file in `chain_dir`, parse death events out of each,
 * and project them into the leaderboard. Single-pass: each DEATH event
 * carries its killer callsign directly (resolved at emit time). A
 * fallback victim-token-to-callsign map is built first to fill in
 * historical events that predate the killed_by_callsign field. The
 * table is reset to empty before replay. */
void highscore_replay_from_chain(highscore_table_t *t, const char *chain_dir);

/* Serialize the table as a NET_MSG_HIGHSCORES packet. Returns bytes written.
 * buf must be at least HIGHSCORE_HEADER + HIGHSCORE_TOP_N * HIGHSCORE_ENTRY_SIZE. */
int highscore_serialize(uint8_t *buf, const highscore_table_t *t);

#endif /* SIGNAL_HIGHSCORE_H */
