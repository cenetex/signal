#ifndef PUBLIC_ACTOR_PRESENTATION_H
#define PUBLIC_ACTOR_PRESENTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "public_actor_id.h"

enum {
    CLIENT_SCOREBOARD_MAX_ROWS = 16,
    CLIENT_SCOREBOARD_LABEL_CAP = 16,
    /*
     * A normal collision uses callsign + '#' + four hex digits. The larger
     * bound lets an adversarial common prefix expand until it is unique.
     */
    CLIENT_PUBLIC_ACTOR_DISPLAY_LABEL_CAP =
        CLIENT_SCOREBOARD_LABEL_CAP + 1 + PUBLIC_ACTOR_ID_SIZE * 2,
};

typedef struct {
    public_actor_id_t actor;
    char label[CLIENT_SCOREBOARD_LABEL_CAP];
    uint16_t kills;
    uint16_t deaths;
    bool is_npc;
} client_scoreboard_row_t;

typedef struct {
    bool show;
    client_scoreboard_row_t rows[CLIENT_SCOREBOARD_MAX_ROWS];
    int row_count;
} client_scoreboard_t;

typedef struct {
    bool source_is_local;
    bool subject_is_local;
    int source_row;
    int subject_row;
} client_scoreboard_event_result_t;

/*
 * Find/create by full public actor ID. `label` is mutable presentation only:
 * changing it updates the matching actor row and can never merge actors.
 */
int client_scoreboard_row_for_actor(client_scoreboard_t *scoreboard,
                                    const public_actor_id_t *actor,
                                    const char *label,
                                    bool is_npc);

/*
 * Format a row for human presentation. A unique callsign is returned
 * unchanged. If another full actor ID has the same callsign, append the
 * shortest public-ID hex prefix (minimum two bytes) that distinguishes this
 * row from every colliding row. This stays compact normally and expands
 * deterministically for adversarial common prefixes.
 */
bool client_scoreboard_format_row_label(
    const client_scoreboard_t *scoreboard,
    int row,
    char *out,
    size_t out_cap);

/* Find a full actor ID and format its current row label. */
bool client_scoreboard_format_actor_label(
    const client_scoreboard_t *scoreboard,
    const public_actor_id_t *actor,
    char *out,
    size_t out_cap);

/*
 * Apply public combat attribution without consulting a transport slot or
 * callsign-bearing interpolation snapshot. `local_label` is used only when
 * the full event actor equals `local_actor`; all remote actors receive the
 * generic presentation label.
 */
client_scoreboard_event_result_t client_scoreboard_record_death(
    client_scoreboard_t *scoreboard,
    const public_actor_id_t *local_actor,
    const char *local_label,
    const public_actor_id_t *source_actor,
    const public_actor_id_t *subject_actor);

client_scoreboard_event_result_t client_scoreboard_record_npc_kill(
    client_scoreboard_t *scoreboard,
    const public_actor_id_t *local_actor,
    const char *local_label,
    const public_actor_id_t *source_actor);

#endif /* PUBLIC_ACTOR_PRESENTATION_H */
