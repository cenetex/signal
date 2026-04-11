/*
 * sim_asteroid.c -- Asteroid lifecycle: spawning, fracture, field
 * maintenance, and per-frame dynamics.  Extracted from game_sim.c.
 */
#include "sim_asteroid.h"
#include "rng.h"

/* ------------------------------------------------------------------ */
/* RNG wrappers — use underlying randf() with &w->rng                  */
/* ------------------------------------------------------------------ */

static float w_randf(world_t *w)                          { return randf(&w->rng); }
static float w_rand_range(world_t *w, float lo, float hi) { return rand_range(&w->rng, lo, hi); }
static int   w_rand_int(world_t *w, int lo, int hi)       { return rand_int(&w->rng, lo, hi); }

/* ------------------------------------------------------------------ */
/* Signal helpers (local to this file)                                  */
/* ------------------------------------------------------------------ */

static bool point_within_signal_margin(const world_t *w, vec2 pos, float margin) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_provides_signal(&w->stations[s])) continue;
        float range = w->stations[s].signal_range;
        float max_dist = range + margin;
        if (v2_dist_sq(pos, w->stations[s].pos) <= max_dist * max_dist) {
            return true;
        }
    }
    return false;
}

static float max_signal_range(const world_t *w) {
    float best = 0.0f;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (w->stations[i].signal_range > best) best = w->stations[i].signal_range;
    }
    return best > 0.0f ? best : WORLD_RADIUS;
}

/* Pick a random active station (skip empty slots). */
static int pick_active_station(world_t *w) {
    int active[MAX_STATIONS];
    int count = 0;
    for (int s = 0; s < MAX_STATIONS; s++)
        if (station_provides_signal(&w->stations[s])) active[count++] = s;
    if (count == 0) return 0;
    return active[w_rand_int(w, 0, count - 1)];
}

/* ------------------------------------------------------------------ */
/* Asteroid configuration                                              */
/* ------------------------------------------------------------------ */

static void sim_configure_asteroid(world_t *w, asteroid_t *a, asteroid_tier_t tier, commodity_t commodity) {
    float sl = asteroid_spin_limit(tier);
    a->active    = true;
    a->tier      = tier;
    a->commodity = commodity;
    a->radius    = w_rand_range(w, asteroid_radius_min(tier), asteroid_radius_max(tier));
    a->max_hp    = w_rand_range(w, asteroid_hp_min(tier), asteroid_hp_max(tier));
    a->hp        = a->max_hp;
    a->max_ore   = 0.0f;
    a->ore       = 0.0f;
    if (tier == ASTEROID_TIER_S) {
        a->max_ore = w_rand_range(w, 8.0f, 14.0f);
        a->ore     = a->max_ore;
    }
    a->rotation = w_rand_range(w, 0.0f, TWO_PI_F);
    a->spin     = w_rand_range(w, -sl, sl);
    a->seed     = w_rand_range(w, 0.0f, 100.0f);
    a->age      = 0.0f;
    a->net_dirty = true;
}

static asteroid_tier_t random_field_asteroid_tier(world_t *w) {
    float roll = w_randf(w);
    if (roll < 0.03f) return ASTEROID_TIER_XXL;
    if (roll < 0.26f) return ASTEROID_TIER_XL;
    if (roll < 0.70f) return ASTEROID_TIER_L;
    return ASTEROID_TIER_M;
}

/* ------------------------------------------------------------------ */
/* Belt spawning                                                       */
/* ------------------------------------------------------------------ */

/* Find a good clump center in the belt density field near signal-covered space.
 * Uses gradient walk: sample random points, then step toward higher density. */
static vec2 find_belt_clump_center(world_t *w, float *out_density) {
    vec2 best_pos = v2(0.0f, 0.0f);
    float best_density = 0.0f;
    for (int attempt = 0; attempt < 16; attempt++) {
        int stn = pick_active_station(w);
        float angle = w_rand_range(w, 0.0f, TWO_PI_F);
        float distance = w_rand_range(w, 200.0f, w->stations[stn].signal_range * 0.85f);
        vec2 pos = v2_add(w->stations[stn].pos, v2(cosf(angle) * distance, sinf(angle) * distance));
        float d = belt_density_at(&w->belt, pos.x, pos.y);
        /* Gradient walk: take 4 steps toward higher density */
        float step = 200.0f;
        for (int g = 0; g < 4; g++) {
            float dx = belt_density_at(&w->belt, pos.x + step, pos.y) - belt_density_at(&w->belt, pos.x - step, pos.y);
            float dy = belt_density_at(&w->belt, pos.x, pos.y + step) - belt_density_at(&w->belt, pos.x, pos.y - step);
            float glen = sqrtf(dx * dx + dy * dy);
            if (glen > 0.001f) {
                pos.x += dx / glen * step;
                pos.y += dy / glen * step;
            }
            d = belt_density_at(&w->belt, pos.x, pos.y);
            step *= 0.6f;
        }
        if (d > best_density) {
            best_density = d;
            best_pos = pos;
        }
        if (d > 0.3f) break;
    }
    if (out_density) *out_density = best_density;
    return best_pos;
}

/*
 * Seed a clump of asteroids at a belt position.
 * Clumps are irregular blobs: 1 anchor (XL/XXL), several medium, debris fill.
 * Returns number of asteroids placed.
 */
int seed_asteroid_clump(world_t *w, int first_slot) {
    float density = 0.0f;
    vec2 center = find_belt_clump_center(w, &density);
    if (density < 0.05f) return 0;

    commodity_t ore = belt_ore_at(&w->belt, center.x, center.y);

    /* Clump size scales with density: 3-12 rocks */
    int clump_size = 3 + (int)(density * 9.0f);
    float clump_radius = 200.0f + density * 400.0f;

    /* Elongation: stretch the clump along a random axis */
    float stretch_angle = w_rand_range(w, 0.0f, TWO_PI_F);
    float stretch_factor = w_rand_range(w, 1.0f, 2.5f);
    float cos_s = cosf(stretch_angle);
    float sin_s = sinf(stretch_angle);

    /* Shared drift velocity for the clump */
    vec2 drift = v2(w_rand_range(w, -3.0f, 3.0f), w_rand_range(w, -3.0f, 3.0f));

    int placed = 0;
    for (int i = 0; i < clump_size && (first_slot + placed) < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[first_slot + placed];
        if (a->active) continue;

        /* Pick tier: first rock is the anchor, rest are smaller */
        asteroid_tier_t tier;
        if (i == 0) {
            tier = (w_randf(w) < 0.15f) ? ASTEROID_TIER_XXL : ASTEROID_TIER_XL;
        } else if (i <= 3) {
            tier = (w_randf(w) < 0.4f) ? ASTEROID_TIER_L : ASTEROID_TIER_M;
        } else {
            tier = (w_randf(w) < 0.3f) ? ASTEROID_TIER_L : ASTEROID_TIER_M;
        }

        clear_asteroid(a);
        sim_configure_asteroid(w, a, tier, ore);
        a->fracture_child = false;

        /* Scatter around center with elongation */
        float r = w_rand_range(w, 0.0f, clump_radius) * sqrtf(w_randf(w)); /* sqrt for uniform disk */
        float theta = w_rand_range(w, 0.0f, TWO_PI_F);
        float lx = cosf(theta) * r;
        float ly = sinf(theta) * r;
        /* Apply stretch */
        float sx = lx * cos_s - ly * sin_s;
        float sy = lx * sin_s + ly * cos_s;
        sx *= stretch_factor;
        float fx = sx * cos_s + sy * sin_s;
        float fy = -sx * sin_s + sy * cos_s;

        a->pos = v2_add(center, v2(fx, fy));
        a->vel = v2_add(drift, v2(w_rand_range(w, -2.0f, 2.0f), w_rand_range(w, -2.0f, 2.0f)));
        placed++;
    }
    return placed;
}

/* Seed a single asteroid at a belt position (for respawn/compat). */
void seed_field_asteroid_of_tier(world_t *w, asteroid_t *a, asteroid_tier_t tier) {
    float density = 0.0f;
    vec2 pos = find_belt_clump_center(w, &density);
    commodity_t ore = belt_ore_at(&w->belt, pos.x, pos.y);
    clear_asteroid(a);
    sim_configure_asteroid(w, a, tier, ore);
    a->fracture_child = false;
    a->pos = pos;
    a->vel = v2(w_rand_range(w, -4.0f, 4.0f), w_rand_range(w, -4.0f, 4.0f));
}

void seed_random_field_asteroid(world_t *w, asteroid_t *a) {
    seed_field_asteroid_of_tier(w, a, random_field_asteroid_tier(w));
}

static void set_inbound_field_velocity(world_t *w, asteroid_t *a, vec2 inward) {
    float speed_lo = 12.0f, speed_hi = 20.0f, tangent_jitter = 6.0f;
    switch (a->tier) {
    case ASTEROID_TIER_XXL:
        speed_lo = 18.0f; speed_hi = 30.0f; tangent_jitter = 4.0f;
        break;
    case ASTEROID_TIER_XL:
        speed_lo = 16.0f; speed_hi = 26.0f; tangent_jitter = 5.0f;
        break;
    case ASTEROID_TIER_L:
        speed_lo = 14.0f; speed_hi = 22.0f; tangent_jitter = 6.0f;
        break;
    case ASTEROID_TIER_M:
        speed_lo = 12.0f; speed_hi = 18.0f; tangent_jitter = 7.0f;
        break;
    case ASTEROID_TIER_S:
    default:
        break;
    }
    vec2 tangent = v2_perp(inward);
    a->vel = v2_add(v2_scale(inward, w_rand_range(w, speed_lo, speed_hi)),
                    v2_scale(tangent, w_rand_range(w, -tangent_jitter, tangent_jitter)));
}

static void spawn_inbound_field_asteroid_of_tier(world_t *w, asteroid_t *a, asteroid_tier_t tier) {
    clear_asteroid(a);
    a->fracture_child = false;

    /* Spawn at 30-60% of signal range -- close enough to reach the action
     * in 1-3 minutes, not 10-15. Prefer belt-dense positions. */
    int stn = pick_active_station(w);
    vec2 center = w->stations[stn].pos;
    float sr = w->stations[stn].signal_range;
    if (sr <= 0.0f) sr = max_signal_range(w);

    vec2 spawn_pos = center;
    vec2 inward = v2(-1.0f, 0.0f);
    float best_density = 0.0f;

    for (int attempt = 0; attempt < 32; attempt++) {
        float angle = w_rand_range(w, 0.0f, TWO_PI_F);
        vec2 outward = v2_from_angle(angle);
        float dist = w_rand_range(w, sr * 0.30f, sr * 0.60f);
        vec2 pos = v2_add(center, v2_scale(outward, dist));
        float d = belt_density_at(&w->belt, pos.x, pos.y);
        if (d > best_density) {
            best_density = d;
            spawn_pos = pos;
            inward = v2_scale(outward, -1.0f);
        }
        if (d > 0.15f) break;
    }

    commodity_t ore = belt_ore_at(&w->belt, spawn_pos.x, spawn_pos.y);
    sim_configure_asteroid(w, a, tier, ore);
    a->pos = spawn_pos;
    set_inbound_field_velocity(w, a, inward);
}

static void spawn_field_asteroid(world_t *w, asteroid_t *a) {
    spawn_inbound_field_asteroid_of_tier(w, a, random_field_asteroid_tier(w));
}

static void spawn_child_asteroid(world_t *w, asteroid_t *a, asteroid_tier_t tier, commodity_t commodity, vec2 pos, vec2 vel) {
    clear_asteroid(a);
    sim_configure_asteroid(w, a, tier, commodity);
    a->fracture_child = true;
    a->pos = pos;
    a->vel = vel;
}

static int desired_child_count(world_t *w, asteroid_tier_t tier) {
    switch (tier) {
    case ASTEROID_TIER_XXL: return w_rand_int(w, 8, 14);
    case ASTEROID_TIER_XL: return w_rand_int(w, 2, 3);
    case ASTEROID_TIER_L:  return w_rand_int(w, 2, 3);
    case ASTEROID_TIER_M:  return w_rand_int(w, 2, 4);
    default: return 0;
    }
}

static void inspect_asteroid_field(world_t *w, int *seeded_count, int *first_inactive_slot) {
    *seeded_count = 0;
    *first_inactive_slot = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) {
            if (*first_inactive_slot < 0) *first_inactive_slot = i;
            continue;
        }
        if (!w->asteroids[i].fracture_child) (*seeded_count)++;
    }
}

/* ------------------------------------------------------------------ */
/* Fracture                                                            */
/* ------------------------------------------------------------------ */

void fracture_asteroid(world_t *w, int idx, vec2 outward_dir, int8_t fractured_by) {
    asteroid_t parent = w->asteroids[idx];
    asteroid_tier_t child_tier = asteroid_next_tier(parent.tier);
    int desired = desired_child_count(w, parent.tier);
    int child_slots[16] = { idx, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    int child_count = 1;

    for (int i = 0; i < MAX_ASTEROIDS && child_count < desired; i++) {
        if (i == idx || w->asteroids[i].active) continue;
        child_slots[child_count++] = i;
    }

    float base_angle = atan2f(outward_dir.y, outward_dir.x);
    for (int i = 0; i < child_count; i++) {
        float spread_t = (child_count == 1) ? 0.0f : (((float)i / (float)(child_count - 1)) - 0.5f);
        float child_angle = base_angle + (spread_t * 1.35f) + w_rand_range(w, -0.14f, 0.14f);
        vec2 dir = v2_from_angle(child_angle);
        vec2 tangent = v2_perp(dir);
        asteroid_t *child = &w->asteroids[child_slots[i]];
        spawn_child_asteroid(w, child, child_tier, parent.commodity, parent.pos, parent.vel);
        vec2 cpos = v2_add(parent.pos, v2_scale(dir, (parent.radius * 0.28f) + (child->radius * 0.85f)));
        float drift = w_rand_range(w, 22.0f, 56.0f);
        vec2 cvel = v2_add(parent.vel, v2_add(v2_scale(dir, drift), v2_scale(tangent, w_rand_range(w, -10.0f, 10.0f))));
        child->pos = cpos;
        child->vel = cvel;
        child->last_fractured_by = fractured_by;
    }

    /* audio_play_fracture removed */
    SIM_LOG("[sim] asteroid %d fractured into %d children\n", idx, child_count);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_FRACTURE, .fracture.tier = parent.tier});
}

/* ------------------------------------------------------------------ */
/* Per-frame asteroid dynamics                                         */
/* ------------------------------------------------------------------ */

void sim_step_asteroid_dynamics(world_t *w, float dt) {
    float cleanup_d_sq = FRACTURE_CHILD_CLEANUP_DISTANCE * FRACTURE_CHILD_CLEANUP_DISTANCE;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;

        a->rotation += a->spin * dt;
        a->pos = v2_add(a->pos, v2_scale(a->vel, dt));
        a->vel = v2_scale(a->vel, 1.0f / (1.0f + (0.42f * dt)));
        a->age += dt;

        /* Despawn asteroids that leave station-supported space. */
        if (!point_within_signal_margin(w, a->pos, a->radius + 260.0f)) {
            clear_asteroid(a);
            continue;
        }

        /* Cleanup old fracture children far from ALL players */
        if (a->fracture_child && a->age >= FRACTURE_CHILD_CLEANUP_AGE) {
            bool near_player = false;
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (!w->players[p].connected) continue;
                if (v2_dist_sq(a->pos, w->players[p].ship.pos) <= cleanup_d_sq) {
                    near_player = true;
                    break;
                }
            }
            if (!near_player) clear_asteroid(a);
        }

        /* Station vortex: asteroids near stations get caught in orbit.
         * Large asteroids orbit outside the perimeter.
         * S fragments spiral inward toward hoppers. */
        for (int s = 0; s < MAX_STATIONS; s++) {
            const station_t *st = &w->stations[s];
            if (!station_exists(st)) continue;
            float d_sq = v2_dist_sq(a->pos, st->pos);
            float vortex_range = st->dock_radius * 2.0f;
            if (d_sq > vortex_range * vortex_range || d_sq < 1.0f) continue;
            float d = sqrtf(d_sq);
            vec2 radial = v2_scale(v2_sub(a->pos, st->pos), 1.0f / d);
            vec2 tangent = v2(-radial.y, radial.x);
            if (a->tier == ASTEROID_TIER_S) {
                /* Fragments: strong spiral inward toward center/hoppers */
                a->vel = v2_add(a->vel, v2_scale(tangent, 12.0f * dt));
                a->vel = v2_sub(a->vel, v2_scale(radial, 6.0f * dt));
            } else {
                /* Large asteroids: gentle orbit outside perimeter */
                a->vel = v2_add(a->vel, v2_scale(tangent, 4.0f * dt));
                /* Push outward if inside dock radius (keep clear of station) */
                if (d < st->dock_radius)
                    a->vel = v2_add(a->vel, v2_scale(radial, 15.0f * dt));
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Field maintenance                                                   */
/* ------------------------------------------------------------------ */

void maintain_asteroid_field(world_t *w, float dt) {
    int seeded = 0, first_slot = -1;
    inspect_asteroid_field(w, &seeded, &first_slot);
    if (seeded >= FIELD_ASTEROID_TARGET) { w->field_spawn_timer = 0.0f; return; }
    w->field_spawn_timer += dt;
    if (w->field_spawn_timer < FIELD_ASTEROID_RESPAWN_DELAY) return;
    if (first_slot >= 0) {
        /* Spawn a small wave of 2-4 inbound rocks from the belt edge */
        int wave = 2 + w_rand_int(w, 0, 2);
        int spawned = 0;
        for (int i = first_slot; i < MAX_ASTEROIDS && spawned < wave; i++) {
            if (w->asteroids[i].active) continue;
            spawn_field_asteroid(w, &w->asteroids[i]);
            spawned++;
        }
    }
    w->field_spawn_timer = 0.0f;
}
