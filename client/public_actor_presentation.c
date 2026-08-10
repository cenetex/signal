#include "public_actor_presentation.h"

#include <stdio.h>
#include <string.h>

static size_t presentation_label_len(const char *label)
{
    size_t n = 0;
    if (!label) return 0;
    while (n + 1 < CLIENT_SCOREBOARD_LABEL_CAP && label[n] != '\0') n++;
    return n;
}

static bool presentation_actor_valid(const public_actor_id_t *actor)
{
    return actor &&
        actor->kind == (uint8_t)PUBLIC_ACTOR_ID_DERIVED &&
        public_actor_id_is_canonical(actor);
}

int client_scoreboard_row_for_actor(client_scoreboard_t *scoreboard,
                                    const public_actor_id_t *actor,
                                    const char *label,
                                    bool is_npc)
{
    if (!scoreboard || !presentation_actor_valid(actor)) return -1;
    int count = scoreboard->row_count;
    if (count < 0) count = 0;
    if (count > CLIENT_SCOREBOARD_MAX_ROWS)
        count = CLIENT_SCOREBOARD_MAX_ROWS;
    scoreboard->row_count = count;

    for (int i = 0; i < count; i++) {
        client_scoreboard_row_t *row = &scoreboard->rows[i];
        if (!public_actor_id_equal(&row->actor, actor)) continue;
        if (label && label[0]) {
            snprintf(row->label, sizeof(row->label), "%s", label);
        }
        row->is_npc = is_npc;
        return i;
    }
    if (count >= CLIENT_SCOREBOARD_MAX_ROWS) return -1;

    client_scoreboard_row_t *row = &scoreboard->rows[count];
    memset(row, 0, sizeof(*row));
    row->actor = *actor;
    if (label && label[0]) {
        snprintf(row->label, sizeof(row->label), "%s", label);
    }
    row->is_npc = is_npc;
    scoreboard->row_count = count + 1;
    return count;
}

static bool presentation_labels_equal(const char *a, const char *b)
{
    size_t a_len = presentation_label_len(a);
    size_t b_len = presentation_label_len(b);
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static size_t presentation_unique_prefix_bytes(
    const client_scoreboard_t *scoreboard,
    int row_index)
{
    const client_scoreboard_row_t *row = &scoreboard->rows[row_index];
    bool collision = false;
    for (int i = 0; i < scoreboard->row_count; i++) {
        if (i == row_index) continue;
        const client_scoreboard_row_t *peer = &scoreboard->rows[i];
        if (!presentation_actor_valid(&peer->actor) ||
            public_actor_id_equal(&row->actor, &peer->actor) ||
            !presentation_labels_equal(row->label, peer->label)) {
            continue;
        }
        collision = true;
        break;
    }
    if (!collision) return 0;

    for (size_t prefix = 2; prefix <= PUBLIC_ACTOR_ID_SIZE; prefix++) {
        bool unique = true;
        for (int i = 0; i < scoreboard->row_count; i++) {
            if (i == row_index) continue;
            const client_scoreboard_row_t *peer = &scoreboard->rows[i];
            if (!presentation_actor_valid(&peer->actor) ||
                public_actor_id_equal(&row->actor, &peer->actor) ||
                !presentation_labels_equal(row->label, peer->label)) {
                continue;
            }
            if (memcmp(row->actor.id, peer->actor.id, prefix) == 0) {
                unique = false;
                break;
            }
        }
        if (unique) return prefix;
    }
    return PUBLIC_ACTOR_ID_SIZE;
}

bool client_scoreboard_format_row_label(
    const client_scoreboard_t *scoreboard,
    int row_index,
    char *out,
    size_t out_cap)
{
    static const char hex[] = "0123456789abcdef";
    if (out && out_cap > 0) out[0] = '\0';
    if (!scoreboard || !out || out_cap == 0 ||
        row_index < 0 || row_index >= scoreboard->row_count ||
        row_index >= CLIENT_SCOREBOARD_MAX_ROWS) {
        return false;
    }

    const client_scoreboard_row_t *row = &scoreboard->rows[row_index];
    const char *base = row->label[0] ? row->label : "????";
    size_t base_len = presentation_label_len(base);
    size_t prefix = presentation_actor_valid(&row->actor)
        ? presentation_unique_prefix_bytes(scoreboard, row_index) : 0;
    size_t needed = base_len + (prefix ? 1 + prefix * 2 : 0) + 1;
    if (needed > out_cap) return false;

    memcpy(out, base, base_len);
    size_t offset = base_len;
    if (prefix) {
        out[offset++] = '#';
        for (size_t i = 0; i < prefix; i++) {
            out[offset++] = hex[row->actor.id[i] >> 4];
            out[offset++] = hex[row->actor.id[i] & 0x0fu];
        }
    }
    out[offset] = '\0';
    return true;
}

bool client_scoreboard_format_actor_label(
    const client_scoreboard_t *scoreboard,
    const public_actor_id_t *actor,
    char *out,
    size_t out_cap)
{
    if (out && out_cap > 0) out[0] = '\0';
    if (!scoreboard || !presentation_actor_valid(actor)) return false;
    int count = scoreboard->row_count;
    if (count < 0) return false;
    if (count > CLIENT_SCOREBOARD_MAX_ROWS)
        count = CLIENT_SCOREBOARD_MAX_ROWS;
    for (int i = 0; i < count; i++) {
        if (public_actor_id_equal(&scoreboard->rows[i].actor, actor)) {
            return client_scoreboard_format_row_label(
                scoreboard, i, out, out_cap);
        }
    }
    return false;
}

static bool presentation_actor_matches(
    const public_actor_id_t *actor,
    const public_actor_id_t *expected)
{
    return presentation_actor_valid(actor) &&
        presentation_actor_valid(expected) &&
        public_actor_id_equal(actor, expected);
}

static const char *presentation_event_label(
    bool is_local,
    const char *local_label)
{
    if (is_local && local_label && local_label[0]) return local_label;
    return is_local ? "YOU" : "Pilot";
}

client_scoreboard_event_result_t client_scoreboard_record_death(
    client_scoreboard_t *scoreboard,
    const public_actor_id_t *local_actor,
    const char *local_label,
    const public_actor_id_t *source_actor,
    const public_actor_id_t *subject_actor)
{
    client_scoreboard_event_result_t result = {
        .source_row = -1,
        .subject_row = -1,
    };
    if (!scoreboard) return result;

    result.source_is_local =
        presentation_actor_matches(source_actor, local_actor);
    result.subject_is_local =
        presentation_actor_matches(subject_actor, local_actor);

    /*
     * A self-inflicted death is a death but not a kill. Every other valid
     * source is credited, including a remote source killing the local actor.
     */
    if (!presentation_actor_matches(source_actor, subject_actor)) {
        result.source_row = client_scoreboard_row_for_actor(
            scoreboard,
            source_actor,
            presentation_event_label(
                result.source_is_local, local_label),
            false);
        if (result.source_row >= 0)
            scoreboard->rows[result.source_row].kills++;
    }

    result.subject_row = client_scoreboard_row_for_actor(
        scoreboard,
        subject_actor,
        presentation_event_label(
            result.subject_is_local, local_label),
        false);
    if (result.subject_row >= 0)
        scoreboard->rows[result.subject_row].deaths++;

    return result;
}

client_scoreboard_event_result_t client_scoreboard_record_npc_kill(
    client_scoreboard_t *scoreboard,
    const public_actor_id_t *local_actor,
    const char *local_label,
    const public_actor_id_t *source_actor)
{
    client_scoreboard_event_result_t result = {
        .source_row = -1,
        .subject_row = -1,
    };
    if (!scoreboard) return result;

    result.source_is_local =
        presentation_actor_matches(source_actor, local_actor);
    result.source_row = client_scoreboard_row_for_actor(
        scoreboard,
        source_actor,
        presentation_event_label(
            result.source_is_local, local_label),
        false);
    if (result.source_row >= 0)
        scoreboard->rows[result.source_row].kills++;
    return result;
}
