/*
 * sim_scent.c -- emission and sampling of physical scent traces.
 * See sim_scent.h for why this exists and what replaced it.
 */
#include "sim_scent.h"

#include "signal_model.h"

#include <math.h>

/* Rock scent is refreshed a slice at a time. Every rock still emits many
 * times a second at 120 Hz, but the per-tick cost is a fraction of the
 * belt rather than all of it. */
#define SCENT_ROCK_STRIDE 8

/* Per-deposit strengths. signal_field_observe saturates -- it moves the
 * cell a fraction of the way to 1.0 -- so these are deliberately small:
 * a lone rock never smells like a field of them, and a cell's steady
 * state is set by how many sources keep feeding it against decay. */
#define SCENT_ROCK_GAIN   0.012f
#define SCENT_WAKE_GAIN   0.020f

/* Sight. Generous at full signal, and it collapses out past the relay
 * chain, which is what pushes a fly back onto its nose. */
#define SCENT_SIGHT_BASE  2600.0f
#define SCENT_SIGHT_FLOOR 420.0f

/* Rarer ore is louder. Grades come from base58 patterns in the fragment
 * pub, so this is the smell of something worth hauling home, not just of
 * rock -- which is what makes a fly prefer a good patch over a near one. */
static float scent_grade_gain(uint8_t grade)
{
    switch (grade) {
    case MINING_GRADE_FINE:         return 1.4f;
    case MINING_GRADE_RARE:         return 2.0f;
    case MINING_GRADE_RATI:         return 3.0f;
    case MINING_GRADE_COMMISSIONED: return 3.5f;
    default:                        return 1.0f;   /* common */
    }
}

/* What a rock is worth to a nose.
 *
 * Only S-tier fragments carry `ore` at all -- everything bigger holds its
 * ore behind a fracture and reads 0.0f -- so keying emission on that field
 * would have left every actual mining target odourless while the loose
 * shards did all the smelling. A whole rock smells of what is locked
 * inside it, scaled by how much tier implies; a fragment smells of what it
 * is genuinely still carrying, and goes quiet as it is smelted down. */
static float scent_rock_yield(const asteroid_t *a)
{
    if (a->tier == ASTEROID_TIER_S)
        return (a->max_ore > 0.0f) ? (a->ore / a->max_ore) : 0.0f;
    switch (a->tier) {
    case ASTEROID_TIER_XXL: return 1.00f;
    case ASTEROID_TIER_XL:  return 0.80f;
    case ASTEROID_TIER_L:   return 0.60f;
    case ASTEROID_TIER_M:   return 0.40f;
    default:                return 0.20f;
    }
}

static void scent_emit_rocks(world_t *w)
{
    uint32_t phase = w->tick % SCENT_ROCK_STRIDE;
    for (int i = (int)phase; i < MAX_ASTEROIDS; i += SCENT_ROCK_STRIDE) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;

        /* A worked-out patch stops smelling because its rocks are gone,
         * not because anything clears the field: emission simply stops and
         * decay does the rest. */
        float load = scent_rock_yield(a);
        if (load <= 0.0f) continue;
        float gain = SCENT_ROCK_GAIN * load * scent_grade_gain(a->grade);
        (void)signal_field_observe(&w->signal_field, a->pos,
                                   SIGNAL_FIELD_KIND_ORE_SCENT,
                                   gain, w->tick);
    }
}

/* A ship's wake scales with what it is hauling, so an empty hull is
 * nearly silent and a loaded one drags a bright line across the field.
 * Being followable while laden is the point, not a side effect. */
static float scent_ship_load(const ship_t *s)
{
    if (!s) return 0.0f;
    float load = 0.0f;
    load += 1.0f * (float)s->towed_count;
    load += 1.6f * (float)s->towed_pod_count;
    load += 3.0f * (s->towed_scaffold >= 0 ? 1.0f : 0.0f);
    return load;
}

static void scent_emit_wake(world_t *w)
{
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *npc = &w->npc_ships[i];
        if (!npc->active || !npc->ship) continue;
        float load = scent_ship_load(npc->ship);
        if (load <= 0.0f) continue;
        float gain = SCENT_WAKE_GAIN * fminf(load, 4.0f) * 0.25f;
        (void)signal_field_observe(&w->signal_field, npc->ship->pos,
                                   SIGNAL_FIELD_KIND_CARGO_WAKE,
                                   gain, w->tick);
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const server_player_t *p = &w->players[i];
        if (!p->connected || !p->ship) continue;
        float load = scent_ship_load(p->ship);
        if (load <= 0.0f) continue;
        float gain = SCENT_WAKE_GAIN * fminf(load, 4.0f) * 0.25f;
        (void)signal_field_observe(&w->signal_field, p->ship->pos,
                                   SIGNAL_FIELD_KIND_CARGO_WAKE,
                                   gain, w->tick);
    }
}

void scent_step(world_t *w)
{
    if (!w) return;
    scent_emit_rocks(w);
    scent_emit_wake(w);
}

float scent_sample(const world_t *w, vec2 pos, signal_field_kind_t kind)
{
    if (!w) return 0.0f;
    return signal_field_query(&w->signal_field, pos, kind, 1);
}

bool scent_gradient(const world_t *w,
                    vec2 pos,
                    signal_field_kind_t kind,
                    vec2 *out_dir,
                    float *out_strength)
{
    if (!w) return false;
    float here = signal_field_query(&w->signal_field, pos, kind, 1);
    if (out_strength) *out_strength = here;

    /* Probe one cell out along both axes and take the steepest ascent.
     * A cell is 4096 units, so this resolves a patch, never a rock --
     * which is the honest limit of a nose at this range. */
    const float step = SIGNAL_FIELD_CELL_SIZE;
    float east  = signal_field_query(&w->signal_field,
                                     v2(pos.x + step, pos.y), kind, 1);
    float west  = signal_field_query(&w->signal_field,
                                     v2(pos.x - step, pos.y), kind, 1);
    float north = signal_field_query(&w->signal_field,
                                     v2(pos.x, pos.y + step), kind, 1);
    float south = signal_field_query(&w->signal_field,
                                     v2(pos.x, pos.y - step), kind, 1);

    vec2 g = v2(east - west, north - south);
    float mag = v2_len(g);
    /* Below this the field is flat to within its own quantisation and any
     * "direction" would be an artifact of cell boundaries. */
    if (mag < 1e-4f) return false;
    if (out_dir) *out_dir = v2(g.x / mag, g.y / mag);
    return true;
}

float scent_sight_radius(const world_t *w, const ship_t *ship)
{
    if (!w || !ship) return SCENT_SIGHT_FLOOR;
    float q = signal_npc_confidence(signal_strength_at(w, ship->pos));
    return SCENT_SIGHT_FLOOR + (SCENT_SIGHT_BASE - SCENT_SIGHT_FLOOR) * q;
}
