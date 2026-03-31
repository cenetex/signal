/*
 * game_sim.c -- Game simulation for Signal Space Miner.
 * Used by both the authoritative server and the client (local sim).
 * All rendering, audio, and sokol references are excluded.
 * Global state replaced with world_t *w and server_player_t *sp parameters.
 */
#include "game_sim.h"
#include <stdlib.h>

#ifdef GAME_SIM_VERBOSE
#define SIM_LOG(...) printf(__VA_ARGS__)
#else
#define SIM_LOG(...) ((void)0)
#endif

static const float INGOT_BUFFER_CAPACITY = 50.0f;

static void emit_event(world_t *w, sim_event_t ev) {
    if (w->events.count < SIM_MAX_EVENTS) {
        w->events.events[w->events.count++] = ev;
    }
}

/* ================================================================== */
/* Hull definitions                                                   */
/* ================================================================== */

const hull_def_t HULL_DEFS[HULL_CLASS_COUNT] = {
    [HULL_CLASS_MINER] = {
        .name          = "Mining Cutter",
        .max_hull      = 100.0f,
        .accel         = 300.0f,
        .turn_speed    = 2.75f,
        .drag          = 0.45f,
        .ore_capacity  = 120.0f,
        .ingot_capacity= 0.0f,
        .mining_rate   = 28.0f,
        .tractor_range = 150.0f,
        .ship_radius   = 16.0f,
        .render_scale  = 1.0f,
    },
    [HULL_CLASS_HAULER] = {
        .name          = "Cargo Hauler",
        .max_hull      = 150.0f,
        .accel         = 140.0f,
        .turn_speed    = 1.6f,
        .drag          = 0.55f,
        .ore_capacity  = 0.0f,
        .ingot_capacity= 40.0f,
        .mining_rate   = 0.0f,
        .tractor_range = 0.0f,
        .ship_radius   = 18.0f,
        .render_scale  = 0.85f,
    },
    [HULL_CLASS_NPC_MINER] = {
        .name          = "Mining Drone",
        .max_hull      = 80.0f,
        .accel         = 180.0f,
        .turn_speed    = 2.0f,
        .drag          = 0.5f,
        .ore_capacity  = 60.0f,
        .ingot_capacity= 0.0f,
        .mining_rate   = 14.0f,
        .tractor_range = 0.0f,
        .ship_radius   = 12.0f,
        .render_scale  = 0.7f,
    },
};

/* ================================================================== */
/* Math / utility                                                     */
/* ================================================================== */

/* ================================================================== */
/* RNG -- world-local xorshift                                        */
/* ================================================================== */

static uint32_t rng_next(world_t *w) {
    if (w->rng == 0) w->rng = 0xA341316Cu;
    uint32_t x = w->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    w->rng = x;
    return x;
}

static float randf(world_t *w) {
    return (float)(rng_next(w) & 0x00FFFFFFu) / 16777215.0f;
}

static float rand_range(world_t *w, float lo, float hi) {
    return lerpf(lo, hi, randf(w));
}

static int rand_int(world_t *w, int lo, int hi) {
    return lo + (int)(rng_next(w) % (uint32_t)(hi - lo + 1));
}

/* ================================================================== */
/* Commodity / ship helpers                                           */
/* ================================================================== */

static asteroid_tier_t asteroid_next_tier(asteroid_tier_t tier) {
    if (tier >= ASTEROID_TIER_S) return ASTEROID_TIER_S;
    return (asteroid_tier_t)(tier + 1);
}
/* Note: XXL -> XL is handled naturally by (tier + 1) since XXL=0, XL=1 */

static bool asteroid_is_collectible(const asteroid_t *a) {
    return a->active && (a->tier == ASTEROID_TIER_S);
}

static float asteroid_progress_ratio(const asteroid_t *a) {
    if (asteroid_is_collectible(a) && a->max_ore > 0.0f)
        return clampf(a->ore / a->max_ore, 0.0f, 1.0f);
    if (a->max_hp > 0.0f)
        return clampf(a->hp / a->max_hp, 0.0f, 1.0f);
    return 0.0f;
}

static commodity_t commodity_refined_form(commodity_t c) {
    switch (c) {
    case COMMODITY_FERRITE_ORE: return COMMODITY_FRAME_INGOT;
    case COMMODITY_CUPRITE_ORE: return COMMODITY_CONDUCTOR_INGOT;
    case COMMODITY_CRYSTAL_ORE: return COMMODITY_LENS_INGOT;
    default: return c;
    }
}

static commodity_t random_raw_ore(world_t *w) {
    return (commodity_t)rand_int(w, (int)COMMODITY_FERRITE_ORE, (int)COMMODITY_CRYSTAL_ORE);
}

static float ship_total_cargo(const ship_t *s) {
    float t = 0.0f;
    for (int i = 0; i < COMMODITY_COUNT; i++) t += s->cargo[i];
    return t;
}

static float ship_raw_ore_total(const ship_t *s) {
    float t = 0.0f;
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++) t += s->cargo[i];
    return t;
}

static void clear_ship_cargo(ship_t *s) {
    memset(s->cargo, 0, sizeof(s->cargo));
}

static const hull_def_t *ship_hull_def_ptr(const ship_t *s) {
    return &HULL_DEFS[s->hull_class];
}

static const hull_def_t *npc_hull_def(const npc_ship_t *npc) {
    return &HULL_DEFS[npc->hull_class];
}

static float ship_max_hull(const ship_t *s) {
    return ship_hull_def_ptr(s)->max_hull;
}

static float ship_cargo_capacity(const ship_t *s) {
    return ship_hull_def_ptr(s)->ore_capacity + ((float)s->hold_level * SHIP_HOLD_UPGRADE_STEP);
}

static float ship_mining_rate(const ship_t *s) {
    return ship_hull_def_ptr(s)->mining_rate + ((float)s->mining_level * SHIP_MINING_UPGRADE_STEP);
}

static float ship_tractor_range(const ship_t *s) {
    return ship_hull_def_ptr(s)->tractor_range + ((float)s->tractor_level * SHIP_TRACTOR_UPGRADE_STEP);
}

static float ship_collect_radius(const ship_t *s) {
    return SHIP_BASE_COLLECT_RADIUS + ((float)s->tractor_level * SHIP_COLLECT_UPGRADE_STEP);
}

static int ship_upgrade_level(const ship_t *s, ship_upgrade_t upgrade) {
    switch (upgrade) {
    case SHIP_UPGRADE_MINING:  return s->mining_level;
    case SHIP_UPGRADE_HOLD:    return s->hold_level;
    case SHIP_UPGRADE_TRACTOR: return s->tractor_level;
    default: return 0;
    }
}

static bool ship_upgrade_maxed(const ship_t *s, ship_upgrade_t upgrade) {
    return ship_upgrade_level(s, upgrade) >= SHIP_UPGRADE_MAX_LEVEL;
}

static int ship_upgrade_cost(const ship_t *s, ship_upgrade_t upgrade) {
    int level = ship_upgrade_level(s, upgrade);
    int tier = level + 1;
    switch (upgrade) {
    case SHIP_UPGRADE_MINING:  return 180 + (tier * 110) + (level * level * 120);
    case SHIP_UPGRADE_HOLD:    return 210 + (tier * 120) + (level * level * 135);
    case SHIP_UPGRADE_TRACTOR: return 160 + (tier * 100) + (level * level * 110);
    default: return 0;
    }
}

static product_t upgrade_required_product(ship_upgrade_t upgrade) {
    switch (upgrade) {
    case SHIP_UPGRADE_HOLD:    return PRODUCT_FRAME;
    case SHIP_UPGRADE_MINING:  return PRODUCT_LASER_MODULE;
    case SHIP_UPGRADE_TRACTOR: return PRODUCT_TRACTOR_MODULE;
    default: return PRODUCT_FRAME;
    }
}

static float upgrade_product_cost(const ship_t *s, ship_upgrade_t upgrade) {
    int level = ship_upgrade_level(s, upgrade);
    int next = level + 1;
    return UPGRADE_BASE_PRODUCT * (float)next;
}

static float ship_cargo_space(const ship_t *s) {
    return fmaxf(0.0f, ship_cargo_capacity(s) - ship_total_cargo(s));
}

static uint32_t station_upgrade_service(ship_upgrade_t upgrade) {
    switch (upgrade) {
    case SHIP_UPGRADE_MINING:  return STATION_SERVICE_UPGRADE_LASER;
    case SHIP_UPGRADE_HOLD:    return STATION_SERVICE_UPGRADE_HOLD;
    case SHIP_UPGRADE_TRACTOR: return STATION_SERVICE_UPGRADE_TRACTOR;
    default: return 0;
    }
}

/* ================================================================== */
/* Station helpers                                                    */
/* ================================================================== */

static int nearest_station_index(world_t *w, vec2 pos) {
    float best_d = 0.0f;
    int best = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        float d = v2_dist_sq(pos, w->stations[i].pos);
        if (best < 0 || d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static vec2 station_dock_anchor(const station_t *station, const hull_def_t *hull) {
    if (!station) return v2(0.0f, 0.0f);
    return v2_add(station->pos, v2(0.0f, -(station->radius + hull->ship_radius + STATION_DOCK_APPROACH_OFFSET)));
}

static bool station_has_service(const station_t *station, uint32_t service) {
    return station && ((station->services & service) != 0);
}

static float station_cargo_sale_value(const station_t *station, const ship_t *s) {
    float total = 0.0f;
    if (!station) return 0.0f;
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++) {
        total += s->cargo[i] * station->buy_price[i];
    }
    return total;
}

static float station_repair_cost(const ship_t *s) {
    float missing = fmaxf(0.0f, ship_max_hull(s) - s->hull);
    return ceilf(missing * STATION_REPAIR_COST_PER_HULL);
}

/* ================================================================== */
/* Asteroid tier property functions                                   */
/* ================================================================== */

static float asteroid_spin_limit(asteroid_tier_t tier) {
    switch (tier) {
    case ASTEROID_TIER_XXL: return 0.06f;
    case ASTEROID_TIER_XL: return 0.16f;
    case ASTEROID_TIER_L:  return 0.24f;
    case ASTEROID_TIER_M:  return 0.38f;
    case ASTEROID_TIER_S:  return 0.62f;
    default: return 0.2f;
    }
}

static float asteroid_radius_min(asteroid_tier_t tier) {
    switch (tier) {
    case ASTEROID_TIER_XXL: return 180.0f;
    case ASTEROID_TIER_XL: return 54.0f;
    case ASTEROID_TIER_L:  return 34.0f;
    case ASTEROID_TIER_M:  return 20.0f;
    case ASTEROID_TIER_S:  return 11.0f;
    default: return 16.0f;
    }
}

static float asteroid_radius_max(asteroid_tier_t tier) {
    switch (tier) {
    case ASTEROID_TIER_XXL: return 350.0f;
    case ASTEROID_TIER_XL: return 78.0f;
    case ASTEROID_TIER_L:  return 48.0f;
    case ASTEROID_TIER_M:  return 30.0f;
    case ASTEROID_TIER_S:  return 16.0f;
    default: return 18.0f;
    }
}

static float asteroid_hp_min(asteroid_tier_t tier) {
    switch (tier) {
    case ASTEROID_TIER_XXL: return 800.0f;
    case ASTEROID_TIER_XL: return 120.0f;
    case ASTEROID_TIER_L:  return 68.0f;
    case ASTEROID_TIER_M:  return 32.0f;
    case ASTEROID_TIER_S:  return 10.0f;
    default: return 8.0f;
    }
}

static float asteroid_hp_max(asteroid_tier_t tier) {
    switch (tier) {
    case ASTEROID_TIER_XXL: return 1400.0f;
    case ASTEROID_TIER_XL: return 170.0f;
    case ASTEROID_TIER_L:  return 96.0f;
    case ASTEROID_TIER_M:  return 46.0f;
    case ASTEROID_TIER_S:  return 18.0f;
    default: return 12.0f;
    }
}

/* ================================================================== */
/* Asteroid lifecycle                                                 */
/* ================================================================== */

static void clear_asteroid(asteroid_t *a) {
    memset(a, 0, sizeof(*a));
}

static void configure_asteroid_tier(world_t *w, asteroid_t *a, asteroid_tier_t tier, commodity_t commodity) {
    float sl = asteroid_spin_limit(tier);
    a->active    = true;
    a->tier      = tier;
    a->commodity = commodity;
    a->radius    = rand_range(w, asteroid_radius_min(tier), asteroid_radius_max(tier));
    a->max_hp    = rand_range(w, asteroid_hp_min(tier), asteroid_hp_max(tier));
    a->hp        = a->max_hp;
    a->max_ore   = 0.0f;
    a->ore       = 0.0f;
    if (tier == ASTEROID_TIER_S) {
        a->max_ore = rand_range(w, 8.0f, 14.0f);
        a->ore     = a->max_ore;
    }
    a->rotation = rand_range(w, 0.0f, TWO_PI_F);
    a->spin     = rand_range(w, -sl, sl);
    a->seed     = rand_range(w, 0.0f, 100.0f);
    a->age      = 0.0f;
}

static asteroid_tier_t random_field_asteroid_tier(world_t *w) {
    float roll = randf(w);
    if (roll < 0.03f) return ASTEROID_TIER_XXL;
    if (roll < 0.26f) return ASTEROID_TIER_XL;
    if (roll < 0.70f) return ASTEROID_TIER_L;
    return ASTEROID_TIER_M;
}

static void spawn_field_asteroid_of_tier(world_t *w, asteroid_t *a, asteroid_tier_t tier) {
    float angle = rand_range(w, 0.0f, TWO_PI_F);
    clear_asteroid(a);
    configure_asteroid_tier(w, a, tier, random_raw_ore(w));
    a->fracture_child = false;
    if (tier == ASTEROID_TIER_XXL) {
        /* XXL asteroids spawn at world edge and drift inward */
        a->pos = v2(cosf(angle) * WORLD_RADIUS, sinf(angle) * WORLD_RADIUS);
        float inward_speed = rand_range(w, 15.0f, 30.0f);
        a->vel = v2(-cosf(angle) * inward_speed, -sinf(angle) * inward_speed);
    } else {
        float distance = rand_range(w, 420.0f, WORLD_RADIUS - 180.0f);
        a->pos = v2(cosf(angle) * distance, sinf(angle) * distance);
        a->vel = v2(rand_range(w, -4.0f, 4.0f), rand_range(w, -4.0f, 4.0f));
    }
}

static void spawn_field_asteroid(world_t *w, asteroid_t *a) {
    spawn_field_asteroid_of_tier(w, a, random_field_asteroid_tier(w));
}

static void spawn_child_asteroid(world_t *w, asteroid_t *a, asteroid_tier_t tier, commodity_t commodity, vec2 pos, vec2 vel) {
    clear_asteroid(a);
    configure_asteroid_tier(w, a, tier, commodity);
    a->fracture_child = true;
    a->pos = pos;
    a->vel = vel;
}

static int desired_child_count(world_t *w, asteroid_tier_t tier) {
    switch (tier) {
    case ASTEROID_TIER_XXL: return rand_int(w, 8, 14);
    case ASTEROID_TIER_XL: return rand_int(w, 2, 3);
    case ASTEROID_TIER_L:  return rand_int(w, 2, 3);
    case ASTEROID_TIER_M:  return rand_int(w, 2, 4);
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

static void fracture_asteroid(world_t *w, int idx, vec2 outward_dir) {
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
        float child_angle = base_angle + (spread_t * 1.35f) + rand_range(w, -0.14f, 0.14f);
        vec2 dir = v2_from_angle(child_angle);
        vec2 tangent = v2_perp(dir);
        asteroid_t *child = &w->asteroids[child_slots[i]];
        spawn_child_asteroid(w, child, child_tier, parent.commodity, parent.pos, parent.vel);
        vec2 cpos = v2_add(parent.pos, v2_scale(dir, (parent.radius * 0.28f) + (child->radius * 0.85f)));
        float drift = rand_range(w, 22.0f, 56.0f);
        vec2 cvel = v2_add(parent.vel, v2_add(v2_scale(dir, drift), v2_scale(tangent, rand_range(w, -10.0f, 10.0f))));
        child->pos = cpos;
        child->vel = cvel;
    }

    /* audio_play_fracture removed */
    SIM_LOG("[sim] asteroid %d fractured into %d children\n", idx, child_count);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_FRACTURE, .fracture.tier = parent.tier});
}

/* ================================================================== */
/* Per-frame world systems                                            */
/* ================================================================== */

static void step_asteroid_dynamics(world_t *w, float dt) {
    float cleanup_d_sq = FRACTURE_CHILD_CLEANUP_DISTANCE * FRACTURE_CHILD_CLEANUP_DISTANCE;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;

        a->rotation += a->spin * dt;
        a->pos = v2_add(a->pos, v2_scale(a->vel, dt));
        a->vel = v2_scale(a->vel, 1.0f / (1.0f + (0.42f * dt)));
        a->age += dt;

        float max_d = WORLD_RADIUS + a->radius + 260.0f;
        if (v2_len_sq(a->pos) > (max_d * max_d)) {
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
    }
}

static void maintain_asteroid_field(world_t *w, float dt) {
    int seeded = 0, first_slot = -1;
    inspect_asteroid_field(w, &seeded, &first_slot);
    if (seeded >= FIELD_ASTEROID_TARGET) { w->field_spawn_timer = 0.0f; return; }
    w->field_spawn_timer += dt;
    if (w->field_spawn_timer < FIELD_ASTEROID_RESPAWN_DELAY) return;
    if (first_slot >= 0) spawn_field_asteroid(w, &w->asteroids[first_slot]);
    w->field_spawn_timer = 0.0f;
}

static void step_refinery_production(world_t *w, float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (st->role != STATION_ROLE_REFINERY) continue;

        int active = 0;
        for (int i = COMMODITY_FERRITE_ORE; i < COMMODITY_RAW_ORE_COUNT; i++)
            if (st->ore_buffer[i] > 0.01f) active++;
        if (active == 0) continue;
        if (active > REFINERY_MAX_FURNACES) active = REFINERY_MAX_FURNACES;
        float rate = REFINERY_BASE_SMELT_RATE / (float)active;

        for (int i = COMMODITY_FERRITE_ORE; i < COMMODITY_RAW_ORE_COUNT; i++) {
            commodity_t ore = (commodity_t)i;
            if (st->ore_buffer[ore] <= 0.01f) continue;
            float consume = fminf(st->ore_buffer[ore], rate * dt);
            st->ore_buffer[ore] -= consume;
            st->inventory[commodity_refined_form(ore)] += consume;
        }
    }
}

static void step_station_production(world_t *w, float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (st->role == STATION_ROLE_YARD) {
            if (st->product_stock[PRODUCT_FRAME] < MAX_PRODUCT_STOCK) {
                float buf = st->ingot_buffer[INGOT_IDX(COMMODITY_FRAME_INGOT)];
                if (buf > 0.01f) {
                    float room = MAX_PRODUCT_STOCK - st->product_stock[PRODUCT_FRAME];
                    float consume = fminf(buf, fminf(STATION_PRODUCTION_RATE * dt, room));
                    st->ingot_buffer[INGOT_IDX(COMMODITY_FRAME_INGOT)] -= consume;
                    st->product_stock[PRODUCT_FRAME] += consume;
                }
            }
        } else if (st->role == STATION_ROLE_BEAMWORKS) {
            if (st->product_stock[PRODUCT_LASER_MODULE] < MAX_PRODUCT_STOCK) {
                float buf_co = st->ingot_buffer[INGOT_IDX(COMMODITY_CONDUCTOR_INGOT)];
                if (buf_co > 0.01f) {
                    float room = MAX_PRODUCT_STOCK - st->product_stock[PRODUCT_LASER_MODULE];
                    float consume = fminf(buf_co, fminf(STATION_PRODUCTION_RATE * dt, room));
                    st->ingot_buffer[INGOT_IDX(COMMODITY_CONDUCTOR_INGOT)] -= consume;
                    st->product_stock[PRODUCT_LASER_MODULE] += consume;
                }
            }
            if (st->product_stock[PRODUCT_TRACTOR_MODULE] < MAX_PRODUCT_STOCK) {
                float buf_ln = st->ingot_buffer[INGOT_IDX(COMMODITY_LENS_INGOT)];
                if (buf_ln > 0.01f) {
                    float room = MAX_PRODUCT_STOCK - st->product_stock[PRODUCT_TRACTOR_MODULE];
                    float consume = fminf(buf_ln, fminf(STATION_PRODUCTION_RATE * dt, room));
                    st->ingot_buffer[INGOT_IDX(COMMODITY_LENS_INGOT)] -= consume;
                    st->product_stock[PRODUCT_TRACTOR_MODULE] += consume;
                }
            }
        }
    }
}

/* ================================================================== */
/* NPC ships                                                          */
/* ================================================================== */

static float npc_total_cargo(const npc_ship_t *npc) {
    float t = 0.0f;
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++) t += npc->cargo[i];
    return t;
}

static bool npc_target_valid(const world_t *w, const npc_ship_t *npc) {
    if (npc->target_asteroid < 0 || npc->target_asteroid >= MAX_ASTEROIDS) return false;
    const asteroid_t *a = &w->asteroids[npc->target_asteroid];
    return a->active && a->tier != ASTEROID_TIER_S;
}

static int npc_find_mineable_asteroid(const world_t *w, const npc_ship_t *npc) {
    int best = -1;
    float best_d = 1e18f;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;
        float d = v2_dist_sq(npc->pos, a->pos);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static void npc_steer_toward(npc_ship_t *npc, vec2 target, float accel, float turn_speed, float dt) {
    vec2 delta = v2_sub(target, npc->pos);
    float desired = atan2f(delta.y, delta.x);
    float diff = wrap_angle(desired - npc->angle);
    float max_turn = turn_speed * dt;
    if (diff > max_turn) diff = max_turn;
    else if (diff < -max_turn) diff = -max_turn;
    npc->angle = wrap_angle(npc->angle + diff);
    vec2 fwd = v2_from_angle(npc->angle);
    npc->vel = v2_add(npc->vel, v2_scale(fwd, accel * dt));
    npc->thrusting = accel > 0.0f;
}

static void npc_apply_physics(npc_ship_t *npc, float drag, float dt) {
    npc->vel = v2_scale(npc->vel, 1.0f / (1.0f + (drag * dt)));
    npc->pos = v2_add(npc->pos, v2_scale(npc->vel, dt));
    /* World boundary push-back (same as step_ship_motion) */
    float d_sq = v2_len_sq(npc->pos);
    float wr_sq = WORLD_RADIUS * WORLD_RADIUS;
    if (d_sq > wr_sq) {
        float d = sqrtf(d_sq);
        vec2 push = v2_scale(npc->pos, -(d - WORLD_RADIUS) * 0.08f / d);
        npc->vel = v2_add(npc->vel, push);
    }
}

static void npc_resolve_station_collisions(world_t *w, npc_ship_t *npc) {
    const hull_def_t *hull = npc_hull_def(npc);
    for (int i = 0; i < MAX_STATIONS; i++) {
        station_t *st = &w->stations[i];
        float minimum = st->radius + 4.0f + hull->ship_radius;
        vec2 delta = v2_sub(npc->pos, st->pos);
        float d_sq = v2_len_sq(delta);
        if (d_sq >= minimum * minimum) continue;
        float d = sqrtf(d_sq);
        vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
        npc->pos = v2_add(st->pos, v2_scale(normal, minimum));
        float vel_toward = v2_dot(npc->vel, normal);
        if (vel_toward < 0.0f)
            npc->vel = v2_sub(npc->vel, v2_scale(normal, vel_toward * 1.2f));
    }
}

static void step_hauler(world_t *w, npc_ship_t *npc, int n, float dt) {
    const hull_def_t *hull = npc_hull_def(npc);
    switch (npc->state) {
    case NPC_STATE_DOCKED: {
        npc->state_timer -= dt;
        npc->vel = v2(0.0f, 0.0f);
        if (npc->state_timer <= 0.0f) {
            station_t *home = &w->stations[npc->home_station];
            station_t *dest = &w->stations[npc->dest_station];
            float space = hull->ingot_capacity;
            bool loaded = false;
            if (dest->role == STATION_ROLE_YARD) {
                commodity_t ingot = COMMODITY_FRAME_INGOT;
                float take = fminf(home->inventory[ingot], space);
                if (take > 0.5f) {
                    npc->ingots[INGOT_IDX(ingot)] += take;
                    home->inventory[ingot] -= take;
                    loaded = true;
                }
            } else if (dest->role == STATION_ROLE_BEAMWORKS) {
                commodity_t ingots[2] = { COMMODITY_CONDUCTOR_INGOT, COMMODITY_LENS_INGOT };
                for (int i = 0; i < 2; i++) {
                    float take = fminf(home->inventory[ingots[i]], space / 2.0f);
                    if (take > 0.01f) {
                        npc->ingots[INGOT_IDX(ingots[i])] += take;
                        home->inventory[ingots[i]] -= take;
                        space -= take;
                        loaded = true;
                    }
                }
            }
            if (loaded) {
                npc->state = NPC_STATE_TRAVEL_TO_DEST;
            } else {
                npc->state = NPC_STATE_TRAVEL_TO_DEST;
            }
        }
        break;
    }
    case NPC_STATE_TRAVEL_TO_DEST: {
        station_t *dest = &w->stations[npc->dest_station];
        npc_steer_toward(npc, dest->pos, hull->accel, hull->turn_speed, dt);
        npc_apply_physics(npc, hull->drag, dt);
        float dock_r = dest->dock_radius * 0.7f;
        if (v2_dist_sq(npc->pos, dest->pos) < dock_r * dock_r) {
            npc->vel = v2(0.0f, 0.0f);
            npc->pos = v2_add(dest->pos, v2(30.0f * (float)(n % 2 == 0 ? -1 : 1), -(dest->radius + hull->ship_radius + 50.0f)));
            npc->state = NPC_STATE_UNLOADING;
            npc->state_timer = HAULER_LOAD_TIME;
        }
        break;
    }
    case NPC_STATE_UNLOADING: {
        npc->state_timer -= dt;
        npc->vel = v2(0.0f, 0.0f);
        if (npc->state_timer <= 0.0f) {
            station_t *dest = &w->stations[npc->dest_station];
            for (int i = 0; i < INGOT_COUNT; i++) {
                dest->ingot_buffer[i] += npc->ingots[i];
                if (dest->ingot_buffer[i] > INGOT_BUFFER_CAPACITY)
                    dest->ingot_buffer[i] = INGOT_BUFFER_CAPACITY;
                npc->ingots[i] = 0.0f;
            }
            npc->state = NPC_STATE_RETURN_TO_STATION;
        }
        break;
    }
    case NPC_STATE_RETURN_TO_STATION: {
        station_t *home = &w->stations[npc->home_station];
        npc_steer_toward(npc, home->pos, hull->accel, hull->turn_speed, dt);
        npc_apply_physics(npc, hull->drag, dt);
        float dock_r = home->dock_radius * 0.7f;
        if (v2_dist_sq(npc->pos, home->pos) < dock_r * dock_r) {
            npc->vel = v2(0.0f, 0.0f);
            npc->pos = v2_add(home->pos, v2(50.0f * (float)(n % 2 == 0 ? -1 : 1), -(home->radius + hull->ship_radius + 70.0f)));
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
        }
        break;
    }
    default:
        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = HAULER_DOCK_TIME;
        break;
    }
}

static void step_npc_ships(world_t *w, float dt) {
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active) continue;
        npc->thrusting = false;

        if (npc->role == NPC_ROLE_HAULER) {
            step_hauler(w, npc, n, dt);
            if (npc->state != NPC_STATE_DOCKED)
                npc_resolve_station_collisions(w, npc);
            continue;
        }

        const hull_def_t *hull = npc_hull_def(npc);
        switch (npc->state) {
        case NPC_STATE_DOCKED: {
            npc->state_timer -= dt;
            npc->vel = v2(0.0f, 0.0f);
            if (npc->state_timer <= 0.0f) {
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) {
                    npc->target_asteroid = target;
                    npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
                } else {
                    npc->state = NPC_STATE_IDLE;
                    npc->state_timer = 2.0f;
                }
            }
            break;
        }
        case NPC_STATE_TRAVEL_TO_ASTEROID: {
            if (!npc_target_valid(w, npc)) {
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) npc->target_asteroid = target;
                else { npc->target_asteroid = -1; npc->state = NPC_STATE_RETURN_TO_STATION; break; }
            }
            asteroid_t *a = &w->asteroids[npc->target_asteroid];
            npc_steer_toward(npc, a->pos, hull->accel, hull->turn_speed, dt);
            npc_apply_physics(npc, hull->drag, dt);
            if (v2_dist_sq(npc->pos, a->pos) < MINING_RANGE * MINING_RANGE)
                npc->state = NPC_STATE_MINING;
            break;
        }
        case NPC_STATE_MINING: {
            if (!npc_target_valid(w, npc)) {
                if (npc_total_cargo(npc) > 0.5f) {
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                } else {
                    int target = npc_find_mineable_asteroid(w, npc);
                    if (target >= 0) { npc->target_asteroid = target; npc->state = NPC_STATE_TRAVEL_TO_ASTEROID; }
                    else npc->state = NPC_STATE_RETURN_TO_STATION;
                }
                break;
            }
            asteroid_t *a = &w->asteroids[npc->target_asteroid];
            float dist_sq = v2_dist_sq(npc->pos, a->pos);
            float standoff = a->radius + 60.0f;
            float approach = standoff + 20.0f;

            if (dist_sq > approach * approach) {
                npc_steer_toward(npc, a->pos, hull->accel, hull->turn_speed, dt);
                npc_apply_physics(npc, hull->drag, dt);
                break;
            }

            vec2 face_dir = v2_sub(a->pos, npc->pos);
            float desired = atan2f(face_dir.y, face_dir.x);
            float diff = wrap_angle(desired - npc->angle);
            float max_turn = hull->turn_speed * dt;
            if (diff > max_turn) diff = max_turn;
            else if (diff < -max_turn) diff = -max_turn;
            npc->angle = wrap_angle(npc->angle + diff);

            if (dist_sq < standoff * standoff) {
                vec2 away = v2_norm(v2_sub(npc->pos, a->pos));
                npc->vel = v2_add(npc->vel, v2_scale(away, hull->accel * 0.5f * dt));
            }
            npc->vel = v2_scale(npc->vel, 1.0f / (1.0f + (4.0f * dt)));
            npc_apply_physics(npc, hull->drag, dt);

            float mined = hull->mining_rate * dt;
            mined = fminf(mined, a->hp);
            a->hp -= mined;

            float cs = hull->ore_capacity - npc_total_cargo(npc);
            float ore_gained = fminf(mined * 0.15f, cs);
            if (ore_gained > 0.0f) npc->cargo[a->commodity] += ore_gained;

            if (a->hp <= 0.01f) {
                vec2 outward = v2_norm(v2_sub(a->pos, npc->pos));
                fracture_asteroid(w, npc->target_asteroid, outward);
                npc->target_asteroid = -1;
            }
            if (cs <= 0.5f) {
                npc->state = NPC_STATE_RETURN_TO_STATION;
                npc->target_asteroid = -1;
            }
            break;
        }
        case NPC_STATE_RETURN_TO_STATION: {
            station_t *home = &w->stations[npc->home_station];
            npc_steer_toward(npc, home->pos, hull->accel, hull->turn_speed, dt);
            npc_apply_physics(npc, hull->drag, dt);
            float dock_r = home->dock_radius * 0.7f;
            if (v2_dist_sq(npc->pos, home->pos) < dock_r * dock_r) {
                npc->vel = v2(0.0f, 0.0f);
                npc->pos = v2_add(home->pos, v2(30.0f * (float)(n % 3 - 1), -(home->radius + hull->ship_radius + 50.0f)));
                if (home->role == STATION_ROLE_REFINERY) {
                    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++) {
                        float space = REFINERY_HOPPER_CAPACITY - home->ore_buffer[i];
                        float deposit = fminf(npc->cargo[i], fmaxf(0.0f, space));
                        home->ore_buffer[i] += deposit;
                        npc->cargo[i] -= deposit;
                    }
                }
                npc->state = NPC_STATE_DOCKED;
                npc->state_timer = NPC_DOCK_TIME;
                npc->target_asteroid = -1;
            }
            break;
        }
        case NPC_STATE_IDLE: {
            npc_apply_physics(npc, hull->drag, dt);
            npc->state_timer -= dt;
            if (npc->state_timer <= 0.0f) {
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) { npc->target_asteroid = target; npc->state = NPC_STATE_TRAVEL_TO_ASTEROID; }
                else npc->state_timer = 3.0f;
            }
            break;
        }
        default: break;
        }

        /* NPC station collision (Bug 34) */
        if (npc->state != NPC_STATE_DOCKED)
            npc_resolve_station_collisions(w, npc);
    }
}

/* ================================================================== */
/* Player ship helpers                                                */
/* ================================================================== */

static vec2 ship_forward(const ship_t *s) {
    return v2_from_angle(s->angle);
}

static vec2 ship_muzzle(const ship_t *s, vec2 forward) {
    return v2_add(s->pos, v2_scale(forward, ship_hull_def_ptr(s)->ship_radius + 8.0f));
}

static bool try_spend_credits(ship_t *s, float amount) {
    if (amount <= 0.0f) return true;
    if (s->credits + 0.01f < amount) return false;
    s->credits = fmaxf(0.0f, s->credits - amount);
    return true;
}

static void anchor_ship_in_station(server_player_t *sp, world_t *w) {
    const station_t *st = &w->stations[sp->current_station];
    const hull_def_t *hull = ship_hull_def_ptr(&sp->ship);
    vec2 base = station_dock_anchor(st, hull);
    /* Offset by player ID so multiple docked players don't overlap */
    float offset = (float)(sp->id % 4) * (hull->ship_radius * 2.5f) - (hull->ship_radius * 3.75f);
    sp->ship.pos = v2_add(base, v2(offset, 0.0f));
    sp->ship.vel = v2(0.0f, 0.0f);
}

static void apply_ship_damage(world_t *w, server_player_t *sp, float damage);

static void dock_ship(world_t *w, server_player_t *sp) {
    if (sp->nearby_station >= 0) sp->current_station = sp->nearby_station;
    sp->docked = true;
    sp->in_dock_range = true;
    anchor_ship_in_station(sp, w);
    SIM_LOG("[sim] player %d docked at station %d\n", sp->id, sp->current_station);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_DOCK, .player_id = sp->id});
}

static void launch_ship(world_t *w, server_player_t *sp) {
    sp->docked = false;
    sp->nearby_station = sp->current_station;
    sp->in_dock_range = true;
    anchor_ship_in_station(sp, w);
    SIM_LOG("[sim] player %d launched\n", sp->id);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_LAUNCH, .player_id = sp->id});
}

static void emergency_recover_ship(world_t *w, server_player_t *sp) {
    clear_ship_cargo(&sp->ship);
    sp->ship.hull = ship_max_hull(&sp->ship);
    sp->ship.angle = PI_F * 0.5f;
    dock_ship(w, sp);
    SIM_LOG("[sim] player %d emergency recovered\n", sp->id);
}

static void apply_ship_damage(world_t *w, server_player_t *sp, float damage) {
    if (damage <= 0.0f) return;
    sp->ship.hull = fmaxf(0.0f, sp->ship.hull - damage);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_DAMAGE, .player_id = sp->id, .damage.amount = damage});
    if (sp->ship.hull <= 0.01f) emergency_recover_ship(w, sp);
}

/* ================================================================== */
/* Ship collision                                                     */
/* ================================================================== */

static void resolve_ship_circle(world_t *w, server_player_t *sp, vec2 center, float radius) {
    float minimum = radius + ship_hull_def_ptr(&sp->ship)->ship_radius;
    vec2 delta = v2_sub(sp->ship.pos, center);
    float d_sq = v2_len_sq(delta);
    if (d_sq >= minimum * minimum) return;
    float d = sqrtf(d_sq);
    vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
    sp->ship.pos = v2_add(center, v2_scale(normal, minimum));
    float vel_toward = v2_dot(sp->ship.vel, normal);
    if (vel_toward < 0.0f) {
        float impact = -vel_toward;
        if (!sp->docked && impact > SHIP_COLLISION_DAMAGE_THRESHOLD)
            apply_ship_damage(w, sp, (impact - SHIP_COLLISION_DAMAGE_THRESHOLD) * SHIP_COLLISION_DAMAGE_SCALE);
        sp->ship.vel = v2_sub(sp->ship.vel, v2_scale(normal, vel_toward * 1.2f));
    }
}

/* ================================================================== */
/* Mining target                                                      */
/* ================================================================== */

static int find_mining_target(const world_t *w, vec2 origin, vec2 forward) {
    int best = -1;
    float best_proj = MINING_RANGE + 1.0f;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active || asteroid_is_collectible(a)) continue;
        vec2 to_a = v2_sub(a->pos, origin);
        float proj = v2_dot(to_a, forward);
        if (proj < 0.0f || proj > MINING_RANGE) continue;
        float dist_from_ray = fabsf(v2_cross(to_a, forward));
        if (dist_from_ray > a->radius + 9.0f) continue;
        if (proj < best_proj) { best_proj = proj; best = i; }
    }
    return best;
}

/* ================================================================== */
/* Station interactions                                               */
/* ================================================================== */

static void try_sell_station_cargo(world_t *w, server_player_t *sp) {
    station_t *st = &w->stations[sp->current_station];
    if (!station_has_service(st, STATION_SERVICE_ORE_BUYER)) return;
    if (ship_raw_ore_total(&sp->ship) <= 0.01f) return;
    float payout = 0.0f;
    for (int i = COMMODITY_FERRITE_ORE; i < COMMODITY_RAW_ORE_COUNT; i++) {
        commodity_t ore = (commodity_t)i;
        float amount = sp->ship.cargo[ore];
        if (amount <= 0.01f) continue;
        float hopper_space = REFINERY_HOPPER_CAPACITY - st->ore_buffer[ore];
        if (hopper_space <= 0.01f) continue;
        float accepted = fminf(amount, hopper_space);
        payout += accepted * st->buy_price[ore];
        st->ore_buffer[ore] += accepted;
        sp->ship.cargo[ore] -= accepted;
    }
    sp->ship.credits += payout;
    SIM_LOG("[sim] player %d sold ore for %.0f cr\n", sp->id, payout);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_SELL, .player_id = sp->id});
}

static void try_repair_ship(world_t *w, server_player_t *sp) {
    station_t *st = &w->stations[sp->current_station];
    if (!station_has_service(st, STATION_SERVICE_REPAIR)) return;
    float cost = station_repair_cost(&sp->ship);
    if (cost <= 0.0f) return;
    if (!try_spend_credits(&sp->ship, cost)) return;
    sp->ship.hull = ship_max_hull(&sp->ship);
    SIM_LOG("[sim] player %d repaired for %.0f cr\n", sp->id, cost);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_REPAIR, .player_id = sp->id});
}

static void try_apply_ship_upgrade(world_t *w, server_player_t *sp, ship_upgrade_t upgrade) {
    station_t *st = &w->stations[sp->current_station];
    uint32_t req_svc = station_upgrade_service(upgrade);
    if (!station_has_service(st, req_svc)) return;
    if (ship_upgrade_maxed(&sp->ship, upgrade)) return;

    product_t required = upgrade_required_product(upgrade);
    float pcost = upgrade_product_cost(&sp->ship, upgrade);
    if (st->product_stock[required] < pcost - 0.01f) return;
    int cost = ship_upgrade_cost(&sp->ship, upgrade);
    if (!try_spend_credits(&sp->ship, (float)cost)) return;
    st->product_stock[required] -= pcost;

    switch (upgrade) {
    case SHIP_UPGRADE_MINING:  sp->ship.mining_level++;  break;
    case SHIP_UPGRADE_HOLD:    sp->ship.hold_level++;    break;
    case SHIP_UPGRADE_TRACTOR: sp->ship.tractor_level++; break;
    default: break;
    }
    SIM_LOG("[sim] player %d upgraded %d to level %d\n", sp->id, (int)upgrade,
           ship_upgrade_level(&sp->ship, upgrade));
    emit_event(w, (sim_event_t){.type = SIM_EVENT_UPGRADE, .player_id = sp->id, .upgrade.upgrade = upgrade});
}

/* ================================================================== */
/* Per-player per-step functions                                      */
/* ================================================================== */

static void step_ship_rotation(ship_t *s, float dt, float turn_input) {
    s->angle = wrap_angle(s->angle + (turn_input * ship_hull_def_ptr(s)->turn_speed * dt));
}

static void step_ship_thrust(ship_t *s, float dt, float thrust_input, vec2 forward) {
    const hull_def_t *hull = ship_hull_def_ptr(s);
    if (thrust_input > 0.0f) {
        s->vel = v2_add(s->vel, v2_scale(forward, hull->accel * thrust_input * dt));
    } else if (thrust_input < 0.0f) {
        s->vel = v2_add(s->vel, v2_scale(forward, SHIP_BRAKE * thrust_input * dt));
    }
}

static void step_ship_motion(ship_t *s, float dt) {
    s->vel = v2_scale(s->vel, 1.0f / (1.0f + (ship_hull_def_ptr(s)->drag * dt)));
    s->pos = v2_add(s->pos, v2_scale(s->vel, dt));
    float d_sq = v2_len_sq(s->pos);
    float wr_sq = WORLD_RADIUS * WORLD_RADIUS;
    if (d_sq > wr_sq) {
        float d = sqrtf(d_sq);
        vec2 push = v2_scale(s->pos, -(d - WORLD_RADIUS) * 0.08f / d);
        s->vel = v2_add(s->vel, push);
    }
}

static void resolve_world_collisions(world_t *w, server_player_t *sp) {
    for (int i = 0; i < MAX_STATIONS; i++)
        resolve_ship_circle(w, sp, w->stations[i].pos, w->stations[i].radius + 4.0f);
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active || asteroid_is_collectible(&w->asteroids[i])) continue;
        resolve_ship_circle(w, sp, w->asteroids[i].pos, w->asteroids[i].radius);
    }
}

static void update_docking_state(world_t *w, server_player_t *sp, float dt) {
    if (sp->docked) {
        sp->in_dock_range = true;
        sp->nearby_station = sp->current_station;
        anchor_ship_in_station(sp, w);
        return;
    }
    float best_d = 0.0f;
    sp->nearby_station = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        float dr_sq = w->stations[i].dock_radius * w->stations[i].dock_radius;
        float d = v2_dist_sq(sp->ship.pos, w->stations[i].pos);
        if (d <= dr_sq && (sp->nearby_station < 0 || d < best_d)) { best_d = d; sp->nearby_station = i; }
    }
    sp->in_dock_range = sp->nearby_station >= 0;
    if (sp->in_dock_range)
        sp->ship.vel = v2_scale(sp->ship.vel, 1.0f / (1.0f + (dt * 2.2f)));
}

static void update_targeting_state(world_t *w, server_player_t *sp, vec2 forward) {
    sp->hover_asteroid = find_mining_target(w, ship_muzzle(&sp->ship, forward), forward);
}

static void step_fragment_collection(world_t *w, server_player_t *sp, float dt) {
    float nearby_sq = FRAGMENT_NEARBY_RANGE * FRAGMENT_NEARBY_RANGE;
    float tr = ship_tractor_range(&sp->ship);
    float tr_sq = tr * tr;
    float cs = ship_cargo_space(&sp->ship);
    sp->nearby_fragments = 0;
    sp->tractor_fragments = 0;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!asteroid_is_collectible(a)) continue;
        vec2 to_ship = v2_sub(sp->ship.pos, a->pos);
        float d_sq = v2_len_sq(to_ship);
        if (d_sq <= nearby_sq) sp->nearby_fragments++;
        if (cs <= 0.0f) continue;
        if (d_sq <= tr_sq) {
            float d = sqrtf(d_sq);
            float pull = 1.0f - clampf(d / tr, 0.0f, 1.0f);
            vec2 pull_dir = d > 0.001f ? v2_scale(to_ship, 1.0f / d) : ship_forward(&sp->ship);
            sp->tractor_fragments++;
            a->vel = v2_add(a->vel, v2_scale(pull_dir, FRAGMENT_TRACTOR_ACCEL * lerpf(0.35f, 1.0f, pull) * dt));
            float speed = v2_len(a->vel);
            if (speed > FRAGMENT_MAX_SPEED) a->vel = v2_scale(v2_norm(a->vel), FRAGMENT_MAX_SPEED);
        }
        float cr = ship_collect_radius(&sp->ship) + a->radius;
        if (d_sq <= cr * cr) {
            float recovered = fminf(a->ore, cs);
            if (recovered <= 0.0f) continue;
            sp->ship.cargo[a->commodity] += recovered;
            cs -= recovered;
            a->ore -= recovered;
            if (a->ore <= 0.01f) {
                clear_asteroid(a);
            } else if (a->max_ore > 0.0f) {
                a->radius = lerpf(asteroid_radius_min(ASTEROID_TIER_S) * 0.72f,
                                  asteroid_radius_max(ASTEROID_TIER_S),
                                  asteroid_progress_ratio(a));
            }
        }
    }
}

static void step_mining_system(world_t *w, server_player_t *sp, float dt, bool mining, vec2 forward) {
    sp->beam_active = false;
    sp->beam_hit = false;
    if (!mining) return;

    vec2 muzzle = ship_muzzle(&sp->ship, forward);
    sp->beam_active = true;
    sp->beam_start = muzzle;

    if (sp->hover_asteroid >= 0) {
        asteroid_t *a = &w->asteroids[sp->hover_asteroid];
        vec2 to_a = v2_sub(a->pos, muzzle);
        vec2 normal = v2_norm(to_a);
        sp->beam_end = v2_sub(a->pos, v2_scale(normal, a->radius * 0.85f));
        sp->beam_hit = true;
        emit_event(w, (sim_event_t){.type = SIM_EVENT_MINING_TICK, .player_id = sp->id});
        if (!w->player_only_mode) {
            float mined = ship_mining_rate(&sp->ship) * dt;
            mined = fminf(mined, a->hp);
            a->hp -= mined;
            if (a->hp <= 0.01f)
                fracture_asteroid(w, sp->hover_asteroid, normal);
        }
    } else {
        sp->beam_end = v2_add(muzzle, v2_scale(forward, MINING_RANGE));
    }
}

static void step_station_interaction_system(world_t *w, server_player_t *sp, const input_intent_t *intent) {
    if (intent->interact) {
        if (sp->docked) { launch_ship(w, sp); return; }
        if (!sp->in_dock_range) return;
        dock_ship(w, sp);
        return;
    }
    if (!sp->docked) return;
    if (intent->service_sell)        try_sell_station_cargo(w, sp);
    else if (intent->service_repair) try_repair_ship(w, sp);
    else if (intent->upgrade_mining) try_apply_ship_upgrade(w, sp, SHIP_UPGRADE_MINING);
    else if (intent->upgrade_hold)   try_apply_ship_upgrade(w, sp, SHIP_UPGRADE_HOLD);
    else if (intent->upgrade_tractor)try_apply_ship_upgrade(w, sp, SHIP_UPGRADE_TRACTOR);
}

/* ================================================================== */
/* step_player -- one player per tick                                 */
/* ================================================================== */

/* Calculate signal interference from nearby objects.  Returns 0..1
 * where 0 = clean signal, 1 = maximum interference. */
static float calc_signal_interference(const world_t *w, const server_player_t *sp) {
    float interference = 0.0f;
    vec2 pos = sp->ship.pos;

    /* Other players — strong interference at close range */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!w->players[i].connected || w->players[i].docked) continue;
        if (&w->players[i] == sp) continue;
        float d = sqrtf(v2_dist_sq(pos, w->players[i].ship.pos));
        if (d < 200.0f) {
            float strength = (200.0f - d) / 200.0f;
            interference += strength * 0.5f;
        }
    }

    /* Large asteroids — mass creates interference */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;
        float range = a->radius * 3.0f;
        float d = sqrtf(v2_dist_sq(pos, a->pos));
        if (d < range) {
            float strength = (range - d) / range;
            float mass_factor = a->radius / 80.0f;  /* bigger = more interference */
            interference += strength * mass_factor * 0.15f;
        }
    }

    return clampf(interference, 0.0f, 0.7f);  /* cap at 70% interference */
}

static void step_player(world_t *w, server_player_t *sp, float dt) {
    sp->hover_asteroid = -1;
    sp->nearby_fragments = 0;
    sp->tractor_fragments = 0;

    if (!sp->docked) {
        /* Signal interference: nearby objects add noise to controls */
        float interference = calc_signal_interference(w, sp);
        float turn_input = sp->input.turn;
        float thrust_input = sp->input.thrust;
        if (interference > 0.01f) {
            /* Add jitter to controls proportional to interference.
             * Use a local RNG seeded from player position to avoid
             * mutating world RNG state (bug 47). */
            uint32_t local_rng = (uint32_t)(sp->ship.pos.x * 1000.0f) ^ (uint32_t)(sp->ship.pos.y * 1000.0f) ^ (uint32_t)(w->time * 1000.0f);
            if (local_rng == 0) local_rng = 0xA341316Cu;
            local_rng ^= local_rng << 13; local_rng ^= local_rng >> 17; local_rng ^= local_rng << 5;
            float r1 = (float)(local_rng & 0x00FFFFFFu) / 16777215.0f;
            local_rng ^= local_rng << 13; local_rng ^= local_rng >> 17; local_rng ^= local_rng << 5;
            float r2 = (float)(local_rng & 0x00FFFFFFu) / 16777215.0f;
            float noise_turn = (r1 - 0.5f) * 2.0f * interference;
            float noise_thrust = (r2 - 0.5f) * 0.6f * interference;
            turn_input += noise_turn;
            thrust_input = clampf(thrust_input + noise_thrust, -1.0f, 1.0f);
        }

        vec2 forward = ship_forward(&sp->ship);
        step_ship_rotation(&sp->ship, dt, turn_input);
        forward = ship_forward(&sp->ship);           /* refresh after rotation */
        step_ship_thrust(&sp->ship, dt, thrust_input, forward);
        step_ship_motion(&sp->ship, dt);
        resolve_world_collisions(w, sp);
        update_docking_state(w, sp, dt);
        step_station_interaction_system(w, sp, &sp->input);
        if (!sp->docked) {
            update_targeting_state(w, sp, forward);
            step_mining_system(w, sp, dt, sp->input.mine, forward);
            step_fragment_collection(w, sp, dt);
        }
    } else {
        update_docking_state(w, sp, dt);
        step_station_interaction_system(w, sp, &sp->input);
    }

    /* Clear one-shot action flags after the sim has consumed them. */
    sp->input.interact = false;
    sp->input.service_sell = false;
    sp->input.service_repair = false;
    sp->input.upgrade_mining = false;
    sp->input.upgrade_hold = false;
    sp->input.upgrade_tractor = false;
}

/* ================================================================== */
/* Asteroid-asteroid gravity                                          */
/* ================================================================== */

static void step_asteroid_gravity(world_t *w, float dt) {
    /* Asteroid-asteroid attraction (non-S tier, within 400 units) */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;
        for (int j = i + 1; j < MAX_ASTEROIDS; j++) {
            asteroid_t *b = &w->asteroids[j];
            if (!b->active || b->tier == ASTEROID_TIER_S) continue;
            vec2 delta = v2_sub(b->pos, a->pos);
            float dist_sq = v2_len_sq(delta);
            if (dist_sq > 400.0f * 400.0f || dist_sq < 1.0f) continue;
            float dist = sqrtf(dist_sq);
            vec2 normal = v2_scale(delta, 1.0f / dist);
            float mass_a = a->radius * a->radius;
            float mass_b = b->radius * b->radius;
            /* Gravitational force proportional to both masses */
            float force_mag = (mass_a * mass_b) / dist_sq * 8.0f;
            if (force_mag > 60.0f * mass_a) force_mag = 60.0f * mass_a; /* clamp */
            /* F = ma, so acceleration = force / mass */
            vec2 accel_a = v2_scale(normal, (force_mag / mass_a) * dt);
            vec2 accel_b = v2_scale(normal, -(force_mag / mass_b) * dt);
            a->vel = v2_add(a->vel, accel_a);
            b->vel = v2_add(b->vel, accel_b);
        }
    }

    /* Station attraction (asteroids within 800 units of a station) */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        for (int s = 0; s < MAX_STATIONS; s++) {
            vec2 delta = v2_sub(w->stations[s].pos, a->pos);
            float dist_sq = v2_len_sq(delta);
            if (dist_sq > 800.0f * 800.0f || dist_sq < 1.0f) continue;
            float dist = sqrtf(dist_sq);
            /* Don't attract asteroids that are at or inside collision boundary */
            float min_dist = a->radius + w->stations[s].radius;
            if (dist < min_dist + 10.0f) continue;
            vec2 normal = v2_scale(delta, 1.0f / dist);
            float force = w->stations[s].radius / (dist * 0.8f) * 2.0f;
            float mass_a = a->radius * a->radius;
            float accel = force / mass_a;
            a->vel = v2_add(a->vel, v2_scale(normal, accel * dt));
        }
    }
}

/* ================================================================== */
/* Asteroid-asteroid collision                                        */
/* ================================================================== */

static void resolve_asteroid_collisions(world_t *w) {
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        for (int j = i + 1; j < MAX_ASTEROIDS; j++) {
            asteroid_t *b = &w->asteroids[j];
            if (!b->active) continue;
            /* Skip if both are S tier */
            if (a->tier == ASTEROID_TIER_S && b->tier == ASTEROID_TIER_S) continue;
            float min_dist = a->radius + b->radius;
            vec2 delta = v2_sub(a->pos, b->pos);
            float dist_sq = v2_len_sq(delta);
            if (dist_sq >= min_dist * min_dist) continue;
            float dist = sqrtf(dist_sq);
            if (dist < 0.001f) { dist = 0.001f; delta = v2(1.0f, 0.0f); }
            vec2 normal = v2_scale(delta, 1.0f / dist);
            float overlap = min_dist - dist;
            /* Push apart: heavier (larger radius) moves less */
            float mass_a = a->radius * a->radius;
            float mass_b = b->radius * b->radius;
            float total_mass = mass_a + mass_b;
            float ratio_a = mass_b / total_mass; /* a moves proportional to b's mass */
            float ratio_b = mass_a / total_mass;
            a->pos = v2_add(a->pos, v2_scale(normal, overlap * ratio_a));
            b->pos = v2_sub(b->pos, v2_scale(normal, overlap * ratio_b));
            /* Transfer velocity along collision normal */
            float rel_vel = v2_dot(v2_sub(a->vel, b->vel), normal);
            if (rel_vel < 0.0f) {
                vec2 impulse_a = v2_scale(normal, rel_vel * ratio_a);
                vec2 impulse_b = v2_scale(normal, rel_vel * ratio_b);
                a->vel = v2_sub(a->vel, impulse_a);
                b->vel = v2_add(b->vel, impulse_b);
            }
        }
    }
}

/* ================================================================== */
/* Asteroid-station collision                                         */
/* ================================================================== */

static void resolve_asteroid_station_collisions(world_t *w) {
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        for (int s = 0; s < MAX_STATIONS; s++) {
            float min_dist = a->radius + w->stations[s].radius;
            vec2 delta = v2_sub(a->pos, w->stations[s].pos);
            float dist_sq = v2_len_sq(delta);
            if (dist_sq >= min_dist * min_dist) continue;
            float dist = sqrtf(dist_sq);
            if (dist < 0.001f) { dist = 0.001f; delta = v2(1.0f, 0.0f); }
            vec2 normal = v2_scale(delta, 1.0f / dist);
            float overlap = min_dist - dist;
            /* Push asteroid out (station is immovable) with extra margin */
            float push = overlap + 8.0f;
            a->pos = v2_add(a->pos, v2_scale(normal, push));
            /* Bounce velocity with restitution 0.6 */
            float vel_along = v2_dot(a->vel, normal);
            float impact_speed = fabsf(vel_along);
            if (vel_along < 0.0f) {
                a->vel = v2_sub(a->vel, v2_scale(normal, vel_along * 1.6f));
            }
            /* Always give a minimum outward velocity to prevent sticking */
            if (vel_along < 20.0f) {
                a->vel = v2_add(a->vel, v2_scale(normal, 20.0f));
            }
            /* High-speed impact damages asteroid */
            if (impact_speed > 100.0f) {
                float damage = impact_speed * 0.3f;
                a->hp -= damage;
                if (a->hp <= 0.0f) {
                    /* Fracture the asteroid */
                    vec2 outward = v2_scale(normal, -1.0f);
                    fracture_asteroid(w, i, outward);
                }
            }
        }
    }
}

/* ================================================================== */
/* Public: world_sim_step                                             */
/* ================================================================== */

void world_sim_step(world_t *w, float dt) {
    w->events.count = 0;
    w->time += dt;
    step_asteroid_dynamics(w, dt);
    maintain_asteroid_field(w, dt);
    step_asteroid_gravity(w, dt);
    resolve_asteroid_collisions(w);
    resolve_asteroid_station_collisions(w);
    step_refinery_production(w, dt);
    step_station_production(w, dt);
    step_npc_ships(w, dt);
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (!w->players[p].connected) continue;
        step_player(w, &w->players[p], dt);
    }

    /* Player-player collision: ramming damage + signal interference */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!w->players[i].connected || w->players[i].docked) continue;
        for (int j = i + 1; j < MAX_PLAYERS; j++) {
            if (!w->players[j].connected || w->players[j].docked) continue;
            float ri = ship_hull_def_ptr(&w->players[i].ship)->ship_radius;
            float rj = ship_hull_def_ptr(&w->players[j].ship)->ship_radius;
            float minimum = ri + rj;
            vec2 delta = v2_sub(w->players[i].ship.pos, w->players[j].ship.pos);
            float d_sq = v2_len_sq(delta);
            if (d_sq >= minimum * minimum) continue;
            float d = sqrtf(d_sq);
            vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
            float overlap = minimum - d;
            w->players[i].ship.pos = v2_add(w->players[i].ship.pos, v2_scale(normal, overlap * 0.5f));
            w->players[j].ship.pos = v2_sub(w->players[j].ship.pos, v2_scale(normal, overlap * 0.5f));
            float rel_vel = v2_dot(v2_sub(w->players[i].ship.vel, w->players[j].ship.vel), normal);
            if (rel_vel < 0.0f) {
                float impact = -rel_vel;
                vec2 impulse = v2_scale(normal, rel_vel * 0.6f);
                w->players[i].ship.vel = v2_sub(w->players[i].ship.vel, impulse);
                w->players[j].ship.vel = v2_add(w->players[j].ship.vel, impulse);
                /* Ramming damage — both ships take damage based on impact speed */
                if (impact > SHIP_COLLISION_DAMAGE_THRESHOLD * 0.7f) {
                    float dmg = (impact - SHIP_COLLISION_DAMAGE_THRESHOLD * 0.7f) * SHIP_COLLISION_DAMAGE_SCALE;
                    apply_ship_damage(w, &w->players[i], dmg);
                    apply_ship_damage(w, &w->players[j], dmg);
                }
            }
        }
    }
}

/* ================================================================== */
/* Public: world_sim_step_player_only                                 */
/* ================================================================== */

void world_sim_step_player_only(world_t *w, int player_idx, float dt) {
    w->events.count = 0;
    /* Do NOT advance w->time — world time is server-authoritative (bug 46) */
    if (player_idx < 0 || player_idx >= MAX_PLAYERS) return;
    server_player_t *sp = &w->players[player_idx];
    if (!sp->connected) return;
    w->player_only_mode = true;  /* suppress mining HP and world RNG mutation */
    step_player(w, sp, dt);
    w->player_only_mode = false;
}

/* ================================================================== */
/* Public: world_reset                                                */
/* ================================================================== */

void world_reset(world_t *w) {
    memset(w, 0, sizeof(*w));
    w->rng = 0xC0FFEE12u;

    /* --- Stations --- */
    snprintf(w->stations[0].name, sizeof(w->stations[0].name), "%s", "Prospect Refinery");
    w->stations[0].role        = STATION_ROLE_REFINERY;
    w->stations[0].pos         = v2(0.0f, -240.0f);
    w->stations[0].radius      = 62.0f;
    w->stations[0].dock_radius = 132.0f;
    w->stations[0].buy_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w->stations[0].buy_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    w->stations[0].buy_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    w->stations[0].services    = STATION_SERVICE_ORE_BUYER | STATION_SERVICE_REPAIR;

    snprintf(w->stations[1].name, sizeof(w->stations[1].name), "%s", "Kepler Yard");
    w->stations[1].role        = STATION_ROLE_YARD;
    w->stations[1].pos         = v2(-320.0f, 230.0f);
    w->stations[1].radius      = 56.0f;
    w->stations[1].dock_radius = 124.0f;
    w->stations[1].services    = STATION_SERVICE_REPAIR | STATION_SERVICE_UPGRADE_HOLD;

    snprintf(w->stations[2].name, sizeof(w->stations[2].name), "%s", "Helios Works");
    w->stations[2].role        = STATION_ROLE_BEAMWORKS;
    w->stations[2].pos         = v2(320.0f, 230.0f);
    w->stations[2].radius      = 56.0f;
    w->stations[2].dock_radius = 124.0f;
    w->stations[2].services    = STATION_SERVICE_REPAIR | STATION_SERVICE_UPGRADE_LASER | STATION_SERVICE_UPGRADE_TRACTOR;

    /* --- Initial asteroid field --- */
    if (FIELD_ASTEROID_TARGET > 0) spawn_field_asteroid_of_tier(w, &w->asteroids[0], ASTEROID_TIER_XL);
    if (FIELD_ASTEROID_TARGET > 1) spawn_field_asteroid_of_tier(w, &w->asteroids[1], ASTEROID_TIER_L);
    for (int i = 2; i < FIELD_ASTEROID_TARGET && i < MAX_ASTEROIDS; i++)
        spawn_field_asteroid(w, &w->asteroids[i]);

    /* --- NPC ships (3 miners + 2 haulers) --- */
    for (int i = 0; i < 3; i++) {
        npc_ship_t *npc = &w->npc_ships[i];
        npc->active       = true;
        npc->role          = NPC_ROLE_MINER;
        npc->hull_class    = HULL_CLASS_NPC_MINER;
        npc->state         = NPC_STATE_DOCKED;
        npc->pos           = v2_add(w->stations[0].pos, v2(30.0f * (float)(i - 1), -(w->stations[0].radius + HULL_DEFS[HULL_CLASS_NPC_MINER].ship_radius + 50.0f)));
        npc->vel           = v2(0.0f, 0.0f);
        npc->angle         = PI_F * 0.5f;
        npc->target_asteroid = -1;
        npc->home_station  = 0;
        npc->state_timer   = NPC_DOCK_TIME + (float)i * 2.0f;
        npc->thrusting     = false;
    }
    for (int i = 0; i < 2; i++) {
        npc_ship_t *npc = &w->npc_ships[3 + i];
        npc->active       = true;
        npc->role          = NPC_ROLE_HAULER;
        npc->hull_class    = HULL_CLASS_HAULER;
        npc->state         = NPC_STATE_DOCKED;
        npc->pos           = v2_add(w->stations[0].pos, v2(50.0f * (float)(i == 0 ? -1 : 1), -(w->stations[0].radius + HULL_DEFS[HULL_CLASS_HAULER].ship_radius + 70.0f)));
        npc->vel           = v2(0.0f, 0.0f);
        npc->angle         = PI_F * 0.5f;
        npc->target_asteroid = -1;
        npc->home_station  = 0;
        npc->dest_station  = 1 + i;
        npc->state_timer   = HAULER_DOCK_TIME + (float)i * 3.0f;
        npc->thrusting     = false;
    }

    SIM_LOG("[sim] world reset complete (%d asteroids, %d NPCs)\n", FIELD_ASTEROID_TARGET, 5);
}

/* ================================================================== */
/* Public: player_init_ship                                           */
/* ================================================================== */

void player_init_ship(server_player_t *sp, world_t *w) {
    memset(&sp->ship, 0, sizeof(sp->ship));
    sp->ship.hull_class = HULL_CLASS_MINER;
    sp->ship.hull       = HULL_DEFS[HULL_CLASS_MINER].max_hull;
    sp->ship.credits    = 0.0f;
    sp->ship.angle      = PI_F * 0.5f;
    sp->docked          = true;
    sp->current_station = 0;
    sp->nearby_station  = 0;
    sp->in_dock_range   = true;
    sp->hover_asteroid  = -1;
    anchor_ship_in_station(sp, w);
}
