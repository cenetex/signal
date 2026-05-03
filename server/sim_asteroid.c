/*
 * sim_asteroid.c -- Asteroid lifecycle: spawning, fracture, field
 * maintenance, and per-frame dynamics.  Extracted from game_sim.c.
 */
#include "sim_asteroid.h"
#include "chunk.h"
#include "rng.h"
#include "mining.h"  /* fracture_seed_compute */
#include "protocol.h"
#include "sha256.h"  /* rock_pub derivation (#285 slice 1) */
#include "chain_log.h" /* signed event emission (#479 C) */

/* ------------------------------------------------------------------ */
/* RNG wrappers — use underlying randf() with &w->rng                  */
/* ------------------------------------------------------------------ */

static float w_randf(world_t *w)                          { return randf(&w->rng); }
static float w_rand_range(world_t *w, float lo, float hi) { return rand_range(&w->rng, lo, hi); }
static int   w_rand_int(world_t *w, int lo, int hi)       { return rand_int(&w->rng, lo, hi); }

/* Permanent floating terrain (#285 slice 1) — forward decls; bodies
 * live with the chunk-materialize block below. */
static bool rock_pub_is_destroyed(const world_t *w, const uint8_t pub[32]);
static void mark_rock_destroyed(world_t *w, const uint8_t pub[32]);
static void compute_rock_pub(uint32_t belt_seed, int32_t cx, int32_t cy,
                              uint16_t slot, uint8_t out[32]);

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

/* max_signal_range removed — chunk materialization uses per-station checks */

/* Pick a random active station (skip empty slots). */
static int pick_active_station(world_t *w) {
    int active[MAX_STATIONS];
    int count = 0;
    for (int s = 0; s < MAX_STATIONS; s++)
        if (station_provides_signal(&w->stations[s])) active[count++] = s;
    if (count == 0) return 0;
    return active[w_rand_int(w, 0, count - 1)];
}

/* fracture_claim_state_reset lives in sim_asteroid.h as a shared
 * static inline so sim_production.c (smelt completion) agrees on the
 * exact reset semantics. */

static bool fracture_claim_pub_is_zero(const uint8_t pub[32]) {
    static const uint8_t zero_pub[32] = {0};
    return !pub || memcmp(pub, zero_pub, sizeof(zero_pub)) == 0;
}

static bool fracture_claim_token_is_zero(const uint8_t token[8]) {
    static const uint8_t zero_token[8] = {0};
    return !token || memcmp(token, zero_token, sizeof(zero_token)) == 0;
}

static bool fracture_claim_has_best(const fracture_claim_state_t *state) {
    return state && !fracture_claim_pub_is_zero(state->best_player_pub);
}

static bool fracture_claim_seen_token(const fracture_claim_state_t *state,
                                      const uint8_t token[8]) {
    if (!state || fracture_claim_token_is_zero(token)) return false;
    for (uint8_t i = 0; i < state->seen_claimant_count && i < MAX_PLAYERS; i++) {
        if (memcmp(state->seen_claimant_tokens[i], token, 8) == 0)
            return true;
    }
    return false;
}

static void fracture_claim_mark_seen_token(fracture_claim_state_t *state,
                                           const uint8_t token[8]) {
    if (!state || fracture_claim_token_is_zero(token)) return;
    if (fracture_claim_seen_token(state, token)) return;
    if (state->seen_claimant_count >= MAX_PLAYERS) return;
    memcpy(state->seen_claimant_tokens[state->seen_claimant_count], token, 8);
    state->seen_claimant_count++;
}

static fracture_claim_state_t *fracture_claim_state_for(world_t *w, int idx) {
    if (!w || idx < 0 || idx >= MAX_ASTEROIDS) return NULL;
    return &w->fracture_claims[idx];
}

static void clear_asteroid_slot(world_t *w, int idx) {
    fracture_claim_state_t *state = fracture_claim_state_for(w, idx);
    if (!w || idx < 0 || idx >= MAX_ASTEROIDS) return;
    clear_asteroid(&w->asteroids[idx]);
    fracture_claim_state_reset(state);
}

static float fracture_signal_radius(const world_t *w, vec2 pos) {
    float radius = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_provides_signal(st)) continue;
        if (v2_dist_sq(pos, st->pos) <= st->signal_range * st->signal_range &&
            st->signal_range > radius)
            radius = st->signal_range;
    }
    return radius;
}

static bool player_can_claim_fracture(const world_t *w, int player_id, int asteroid_idx) {
    const server_player_t *sp;
    float radius;
    if (!w || player_id < 0 || player_id >= MAX_PLAYERS ||
        asteroid_idx < 0 || asteroid_idx >= MAX_ASTEROIDS)
        return false;
    sp = &w->players[player_id];
    if (!sp->connected || !sp->session_ready) return false;
    if (!w->asteroids[asteroid_idx].active) return false;
    radius = fracture_signal_radius(w, w->asteroids[asteroid_idx].pos);
    if (radius <= 0.0f) return false;
    return v2_dist_sq(sp->ship.pos, w->asteroids[asteroid_idx].pos) <= radius * radius;
}

static int fracture_find_by_id(const world_t *w, uint32_t fracture_id) {
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const fracture_claim_state_t *state = &w->fracture_claims[i];
        if (!state->fracture_id) continue;
        if (state->fracture_id == fracture_id) return i;
    }
    return -1;
}

/* Distance-graded burst cap: rocks near the spawn cluster (origin)
 * fracture with the baseline FRACTURE_CHALLENGE_BURST_CAP attempts;
 * rocks further out get more attempts, which raises the probability
 * tail on rare prefix classes (RATi etc.). The driver: pushing the
 * frontier outward — planting outposts, hauling deeper — should pay
 * better-graded ore, not just more of the same. The scaling is gentle
 * (50 → 200 over 30 kU) so the existing prefix distribution stays
 * dominant near the seed cluster and the dial only matters as players
 * actually expand. Wire byte 41 already carries burst_cap, so clients
 * search the matching range with no protocol change. */
uint16_t mining_burst_cap_for_position(vec2 pos) {
    const float SCALE_START = 5000.0f;     /* below this — baseline */
    const float SCALE_FULL  = 30000.0f;    /* above this — clamped at max */
    const uint16_t CAP_MIN  = FRACTURE_CHALLENGE_BURST_CAP;     /* 50 */
    const uint16_t CAP_MAX  = FRACTURE_CHALLENGE_BURST_CAP * 4; /* 200 */
    float dist = sqrtf(pos.x * pos.x + pos.y * pos.y);
    if (dist <= SCALE_START) return CAP_MIN;
    if (dist >= SCALE_FULL)  return CAP_MAX;
    float t = (dist - SCALE_START) / (SCALE_FULL - SCALE_START);
    return (uint16_t)(CAP_MIN + t * (float)(CAP_MAX - CAP_MIN));
}

static void fracture_begin_claim_window(world_t *w, int asteroid_idx) {
    fracture_claim_state_t *state;
    asteroid_t *a;
    if (!w || asteroid_idx < 0 || asteroid_idx >= MAX_ASTEROIDS) return;
    state = &w->fracture_claims[asteroid_idx];
    a = &w->asteroids[asteroid_idx];
    fracture_claim_state_reset(state);
    if (++w->next_fracture_id == 0) ++w->next_fracture_id;
    state->active = true;
    state->challenge_dirty = true;
    state->fracture_id = w->next_fracture_id;
    state->deadline_ms = (uint32_t)(w->time * 1000.0f) + 500u;
    state->burst_cap = mining_burst_cap_for_position(a->pos);
    a->grade = MINING_GRADE_COMMON;
    memset(a->fragment_pub, 0, sizeof(a->fragment_pub));
    a->net_dirty = true;
}

static void fracture_commit_resolution(world_t *w, int asteroid_idx,
                                       const uint8_t winner_pub[32],
                                       uint32_t best_nonce,
                                       mining_grade_t best_grade) {
    fracture_claim_state_t *state;
    asteroid_t *a;
    if (!w || asteroid_idx < 0 || asteroid_idx >= MAX_ASTEROIDS) return;
    state = &w->fracture_claims[asteroid_idx];
    a = &w->asteroids[asteroid_idx];
    state->active = false;
    state->resolved = true;
    state->resolved_dirty = true;
    state->best_nonce = best_nonce;
    state->best_grade = (uint8_t)best_grade;
    if (winner_pub) memcpy(state->best_player_pub, winner_pub, 32);
    else memset(state->best_player_pub, 0, sizeof(state->best_player_pub));
    mining_fragment_pub_compute(a->fracture_seed, state->best_player_pub,
                                best_nonce, a->fragment_pub);
    a->grade = (uint8_t)best_grade;
    a->net_dirty = true;

    /* Push onto the resolve broadcast queue. The claim state's
     * resolved_dirty flag can get wiped by step_furnace_smelting in
     * the same tick if the rock was already in the furnace beam when
     * it resolved — the queue entry outlives that clear so transport
     * can still deliver NET_MSG_FRACTURE_RESOLVED to everyone. */
    for (int p = 0; p < MAX_PENDING_RESOLVES; p++) {
        pending_resolve_t *pr = &w->pending_resolves[p];
        if (pr->active) continue;
        memset(pr, 0, sizeof(*pr));
        pr->active = true;
        pr->fracture_id = state->fracture_id;
        pr->grade = state->best_grade;
        memcpy(pr->fragment_pub, a->fragment_pub, 32);
        memcpy(pr->winner_pub, state->best_player_pub, 32);
        break;
    }
}

static void fracture_resolve_fallback(world_t *w, int asteroid_idx) {
    fracture_claim_state_t *state;
    asteroid_t *a;
    uint8_t zero_pub[32] = {0};
    mining_grade_t best = MINING_GRADE_COMMON;
    uint32_t best_nonce = 0;
    bool have_best = false;
    uint16_t fallback_cap;

    if (!w || asteroid_idx < 0 || asteroid_idx >= MAX_ASTEROIDS) return;
    a = &w->asteroids[asteroid_idx];
    state = &w->fracture_claims[asteroid_idx];
    /* Match the challenge's burst_cap so unclaimed fractures can
     * reach the same grade ceiling a claimant would have. Previously
     * this iterated MINING_BURST_PER_FRAGMENT (20) while clients
     * searched FRACTURE_CHALLENGE_BURST_CAP (50) — silent asymmetry
     * that quietly capped "nobody-in-range" rocks at a lower quality. */
    fallback_cap = state->burst_cap ? state->burst_cap : FRACTURE_CHALLENGE_BURST_CAP;
    for (uint32_t i = 0; i < fallback_cap; i++) {
        mining_keypair_t kp;
        char callsign[8];
        mining_grade_t grade;
        mining_keypair_derive(a->fracture_seed, zero_pub, i, &kp);
        mining_callsign_from_pubkey(kp.pub, callsign);
        grade = mining_classify_base58(callsign);
        if (!have_best || grade > best) {
            best = grade;
            best_nonce = i;
            have_best = true;
        }
    }
    fracture_commit_resolution(w, asteroid_idx, zero_pub, best_nonce, best);
}

/* ------------------------------------------------------------------ */
/* Asteroid configuration                                              */
/* ------------------------------------------------------------------ */

static void sim_configure_asteroid(world_t *w, asteroid_t *a, asteroid_tier_t tier, commodity_t commodity) {
    float sl = asteroid_spin_limit(tier);
    fracture_claim_state_reset(fracture_claim_state_for(w, (int)(a - w->asteroids)));
    a->active    = true;
    a->tier      = tier;
    a->commodity = commodity;
    a->radius    = w_rand_range(w, asteroid_radius_min(tier), asteroid_radius_max(tier));
    a->max_hp    = w_rand_range(w, asteroid_hp_min(tier), asteroid_hp_max(tier));
    a->hp        = a->max_hp;
    a->max_ore   = 0.0f;
    a->ore       = 0.0f;
    if (tier == ASTEROID_TIER_S) {
        /* S-tier fragments carry integer ore so every smelt mints exactly
         * floor(a->ore) manifest units against this fragment's pub —
         * closes the H2 partial-boundary provenance loss the earlier bug
         * audit flagged. Keeps the 8-14 range as integers 8..14. */
        int min_ore = 8;
        int max_ore = 14;
        int n = min_ore + (int)floorf(w_randf(w) * (float)(max_ore - min_ore + 1));
        if (n < min_ore) n = min_ore;
        if (n > max_ore) n = max_ore;
        a->max_ore = (float)n;
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

        clear_asteroid_slot(w, first_slot + placed);
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
    clear_asteroid_slot(w, (int)(a - w->asteroids));
    sim_configure_asteroid(w, a, tier, ore);
    a->fracture_child = false;
    a->pos = pos;
    a->vel = v2(w_rand_range(w, -4.0f, 4.0f), w_rand_range(w, -4.0f, 4.0f));
}

void seed_random_field_asteroid(world_t *w, asteroid_t *a) {
    seed_field_asteroid_of_tier(w, a, random_field_asteroid_tier(w));
}

/* set_inbound_field_velocity, spawn_inbound_field_asteroid_of_tier,
 * and spawn_field_asteroid removed — chunk materialization replaces it */

static void spawn_child_asteroid(world_t *w, asteroid_t *a, asteroid_tier_t tier, commodity_t commodity, vec2 pos, vec2 vel) {
    clear_asteroid_slot(w, (int)(a - w->asteroids));
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

/* inspect_asteroid_field removed — chunk materialization replaces it */

/* ------------------------------------------------------------------ */
/* Fracture                                                            */
/* ------------------------------------------------------------------ */

void fracture_asteroid(world_t *w, int idx, vec2 outward_dir, int8_t fractured_by) {
    asteroid_t parent = w->asteroids[idx];
    /* Permanent floating terrain (#285 slice 1): retire the parent's
     * rock_pub forever. Children inherit fracture_seed but get fresh
     * fragment_pubs once their claim resolves; the parent's pub is a
     * tombstone in the destroyed-records ledger so re-visits to the
     * same chunk skip this slot's seed roster entry. No-op for non-
     * seed parents (fracture children re-fracturing) — their rock_pub
     * is zero, so mark_rock_destroyed early-returns. */
    mark_rock_destroyed(w, parent.rock_pub);
    /* Layer C of #479: emit a signed EVT_ROCK_DESTROY by the witnessing
     * station (the closest one whose signal range covers the parent
     * rock's position). If no station witnessed, skip — this is rare
     * (out-of-signal fracture) but defensively allowed. */
    {
        static const uint8_t zero_pub[32] = {0};
        if (memcmp(parent.rock_pub, zero_pub, 32) != 0) {
            int witness = -1;
            float best_d2 = 0.0f;
            for (int s = 0; s < MAX_STATIONS; s++) {
                const station_t *st = &w->stations[s];
                if (!station_provides_signal(st)) continue;
                float sr = st->signal_range;
                float d2 = v2_dist_sq(parent.pos, st->pos);
                if (d2 <= sr * sr && (witness < 0 || d2 < best_d2)) {
                    witness = s; best_d2 = d2;
                }
            }
            if (witness >= 0) {
                /* fractured_by is a player slot index (or -1). Look up
                 * their pubkey if we have one; otherwise leave zero. */
                uint8_t player_pub[32] = {0};
                if (fractured_by >= 0 && fractured_by < MAX_PLAYERS &&
                    w->players[fractured_by].connected) {
                    memcpy(player_pub, w->players[fractured_by].pubkey, 32);
                }
                chain_payload_rock_destroy_t payload = {0};
                memcpy(payload.rock_pub, parent.rock_pub, 32);
                memcpy(payload.fracturing_player_pub, player_pub, 32);
                memcpy(payload.station_pubkey,
                       w->stations[witness].station_pubkey, 32);
                (void)chain_log_emit(w, &w->stations[witness],
                                     CHAIN_EVT_ROCK_DESTROY,
                                     &payload, (uint16_t)sizeof(payload));
            } else {
                SIM_LOG("[chain] rock destroyed out of signal range — "
                        "no witness, no event emitted\n");
            }
        }
    }
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
        child->last_towed_by = -1;
        memset(child->last_towed_token, 0, sizeof(child->last_towed_token));
        memset(child->last_fractured_token, 0, sizeof(child->last_fractured_token));
        child->grade = MINING_GRADE_COMMON;
        memset(child->fragment_pub, 0, sizeof(child->fragment_pub));
        if (fractured_by >= 0 && fractured_by < MAX_PLAYERS &&
            w->players[fractured_by].connected &&
            w->players[fractured_by].session_ready) {
            memcpy(child->last_fractured_token,
                   w->players[fractured_by].session_token,
                   sizeof(child->last_fractured_token));
        }

        /* Stamp the fracture_seed on each child. Deterministic from
         * the child's birth state — every observer reproduces it. */
        mining_fracture_inputs_t mi;
        memset(&mi, 0, sizeof(mi));
        mi.asteroid_id         = (uint16_t)child_slots[i];
        mi.asteroid_pos_x_q    = mining_q100_(child->pos.x);
        mi.asteroid_pos_y_q    = mining_q100_(child->pos.y);
        mi.asteroid_rotation_q = mining_q1000_(child->rotation);
        mi.ship_pos_x_q        = mining_q100_(parent.pos.x);
        mi.ship_pos_y_q        = mining_q100_(parent.pos.y);
        mi.world_time_ms       = (uint64_t)(w->time * 1000.0);
        mi.fractured_by        = (uint8_t)fractured_by;
        mining_fracture_seed_compute(&mi, child->fracture_seed);

        if (child->tier == ASTEROID_TIER_S && child->ore > 0.0f) {
            fracture_begin_claim_window(w, child_slots[i]);
        }
    }

    /* audio_play_fracture removed */
    SIM_LOG("[sim] asteroid %d fractured into %d children\n", idx, child_count);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_FRACTURE, .player_id = fractured_by,
                                  .fracture = { .tier = parent.tier, .asteroid_id = idx }});
}

bool submit_fracture_claim(world_t *w, int player_id, uint32_t fracture_id,
                           uint32_t burst_nonce, uint8_t claimed_grade) {
    fracture_claim_state_t *state;
    asteroid_t *a;
    const uint8_t *session_token;
    uint8_t player_pub[32];
    mining_keypair_t kp;
    char callsign[8];
    mining_grade_t actual_grade;
    uint32_t now_ms;
    int asteroid_idx;

    asteroid_idx = fracture_find_by_id(w, fracture_id);
    if (asteroid_idx < 0) return false;
    state = &w->fracture_claims[asteroid_idx];
    a = &w->asteroids[asteroid_idx];
    if (!a->active || !state->active || state->resolved) return false;
    /* Explicit deadline enforcement — don't rely on step_fracture_claims
     * having run this tick. Without this, a claim arriving between the
     * deadline passing and the next step runs could still count, which
     * is race-dependent behaviour. */
    now_ms = (uint32_t)(w->time * 1000.0f);
    if (now_ms >= state->deadline_ms) return false;
    if (player_id < 0 || player_id >= MAX_PLAYERS) return false;
    if (burst_nonce >= state->burst_cap) return false;
    if (!player_can_claim_fracture(w, player_id, asteroid_idx)) return false;
    session_token = w->players[player_id].session_token;
    /* Session-token dedup already covers "same player claims twice" —
     * a redundant winner-pub memcmp used to live below; dropped since
     * pub = sha256(token) is injective so the token check is exact. */
    if (fracture_claim_seen_token(state, session_token)) return false;

    sha256_bytes(session_token, 8, player_pub);
    mining_keypair_derive(a->fracture_seed, player_pub, burst_nonce, &kp);
    mining_callsign_from_pubkey(kp.pub, callsign);
    actual_grade = mining_classify_base58(callsign);
    if (actual_grade < (mining_grade_t)claimed_grade) return false;

    if (!fracture_claim_has_best(state) ||
        actual_grade > (mining_grade_t)state->best_grade) {
        state->best_grade = (uint8_t)actual_grade;
        state->best_nonce = burst_nonce;
        memcpy(state->best_player_pub, player_pub, sizeof(state->best_player_pub));
    }
    fracture_claim_mark_seen_token(state, session_token);
    return true;
}

void step_fracture_claims(world_t *w) {
    uint32_t now_ms = (uint32_t)(w->time * 1000.0f);

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        fracture_claim_state_t *state = &w->fracture_claims[i];
        if (!state->active) continue;
        if (!w->asteroids[i].active) {
            fracture_claim_state_reset(state);
            continue;
        }
        /* Rebroadcast active challenges at FRACTURE_CHALLENGE_REBROADCAST_MS
         * cadence so players who enter signal range mid-window (or had
         * the first packet dropped) still get a chance to race. The
         * initial broadcast is driven by challenge_dirty being set in
         * fracture_begin_claim_window; subsequent re-arms happen here. */
        if (!state->resolved &&
            now_ms >= state->challenge_last_ms + FRACTURE_CHALLENGE_REBROADCAST_MS) {
            state->challenge_dirty = true;
            state->challenge_last_ms = now_ms;
        }
        if (now_ms < state->deadline_ms) continue;
        if (fracture_claim_has_best(state))
            fracture_commit_resolution(w, i, state->best_player_pub,
                                       state->best_nonce,
                                       (mining_grade_t)state->best_grade);
        else
            fracture_resolve_fallback(w, i);
    }
}

/* ------------------------------------------------------------------ */
/* Per-frame asteroid dynamics                                         */
/* ------------------------------------------------------------------ */

void sim_step_asteroid_dynamics(world_t *w, float dt) {
    float cleanup_d_sq = FRACTURE_CHILD_CLEANUP_DISTANCE * FRACTURE_CHILD_CLEANUP_DISTANCE;

    /* Build "currently towed" set so we can skip ambient drag on them.
     * Towed fragments have their own drag in the tractor physics. */
    bool towed[MAX_ASTEROIDS];
    memset(towed, 0, sizeof(towed));
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (!w->players[p].connected) continue;
        for (int t = 0; t < w->players[p].ship.towed_count; t++) {
            int idx = w->players[p].ship.towed_fragments[t];
            if (idx >= 0 && idx < MAX_ASTEROIDS) towed[idx] = true;
        }
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        int idx = w->npc_ships[n].towed_fragment;
        if (idx >= 0 && idx < MAX_ASTEROIDS) towed[idx] = true;
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;

        a->rotation += a->spin * dt;
        a->pos = v2_add(a->pos, v2_scale(a->vel, dt));
        /* Ambient drag — skip towed fragments (they have their own drag) */
        if (!towed[i])
            a->vel = v2_scale(a->vel, 1.0f / (1.0f + (0.42f * dt)));
        a->age += dt;

        /* Despawn asteroids that leave station-supported space. */
        if (!point_within_signal_margin(w, a->pos, a->radius + 260.0f)) {
            clear_asteroid_slot(w, i);
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
            if (!near_player) clear_asteroid_slot(w, i);
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

/* ------------------------------------------------------------------ */
/* Chunk-based terrain materialization                                 */
/* ------------------------------------------------------------------ */

/* Materialize radius: slightly larger than view radius so rocks appear
 * before the player reaches the edge. */
#define MATERIALIZE_RADIUS 3500.0f

/* ------------------------------------------------------------------ */
/* Permanent floating terrain (#285 slice 1)                          */
/* ------------------------------------------------------------------ */

/* rock_pub = SHA256("rock-v1" || belt_seed || cx || cy || slot).
 * Deterministic from belt seed + chunk coordinates + per-chunk slot
 * index, so two clients independently arrive at the same identity
 * without any shared mutable state. */
static void compute_rock_pub(uint32_t belt_seed, int32_t cx, int32_t cy,
                              uint16_t slot, uint8_t out[32]) {
    uint8_t buf[7 + 4 + 4 + 4 + 2];
    size_t o = 0;
    memcpy(buf + o, "rock-v1", 7); o += 7;
    buf[o++] = (uint8_t)(belt_seed       & 0xFF);
    buf[o++] = (uint8_t)((belt_seed >> 8)  & 0xFF);
    buf[o++] = (uint8_t)((belt_seed >> 16) & 0xFF);
    buf[o++] = (uint8_t)((belt_seed >> 24) & 0xFF);
    uint32_t ucx = (uint32_t)cx;
    uint32_t ucy = (uint32_t)cy;
    buf[o++] = (uint8_t)(ucx       & 0xFF);
    buf[o++] = (uint8_t)((ucx >> 8)  & 0xFF);
    buf[o++] = (uint8_t)((ucx >> 16) & 0xFF);
    buf[o++] = (uint8_t)((ucx >> 24) & 0xFF);
    buf[o++] = (uint8_t)(ucy       & 0xFF);
    buf[o++] = (uint8_t)((ucy >> 8)  & 0xFF);
    buf[o++] = (uint8_t)((ucy >> 16) & 0xFF);
    buf[o++] = (uint8_t)((ucy >> 24) & 0xFF);
    buf[o++] = (uint8_t)(slot       & 0xFF);
    buf[o++] = (uint8_t)((slot >> 8)  & 0xFF);
    sha256_bytes(buf, o, out);
}

/* destroyed_rocks is kept sorted ascending by rock_pub (lexicographic
 * byte order) so membership is bsearch in O(log n) — three to nine
 * memcmp(32B) on a 256-entry table. Slice 2 of #285 lays the right
 * shape; slice 3 hangs Binary Fuse off the same key set for O(1)
 * filter probes when cardinality climbs. */

/* Branchless return: index of the first entry whose rock_pub >= pub,
 * matching the C++ lower_bound contract. The caller checks whether
 * that entry's pub equals the search key to decide membership. */
static int destroyed_rocks_lower_bound(const world_t *w, const uint8_t pub[32]) {
    int lo = 0;
    int hi = (int)w->destroyed_rock_count;
    while (lo < hi) {
        int mid = lo + ((hi - lo) >> 1);
        if (memcmp(w->destroyed_rocks[mid].rock_pub, pub, 32) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static bool rock_pub_is_destroyed(const world_t *w, const uint8_t pub[32]) {
    int idx = destroyed_rocks_lower_bound(w, pub);
    if (idx >= (int)w->destroyed_rock_count) return false;
    return memcmp(w->destroyed_rocks[idx].rock_pub, pub, 32) == 0;
}

/* Idempotent insert at the lower-bound index; shifts the tail forward
 * with memmove. Records the world-clock millisecond of the fracture
 * so closed-epoch snapshots can bound "destroyed before epoch N"
 * proofs.
 *
 * Cap-overflow policy: at 256 entries the rock is still removed from
 * the active pool (the player-facing outcome stays "destroyed
 * forever"), but the tombstone is dropped — the verifier loses a
 * row of provenance. This gap closes when slice 3 swaps to the
 * append-only side-file. The cap is loud (SIM_LOG) so we'll know if
 * it ever binds in practice. */
static void mark_rock_destroyed(world_t *w, const uint8_t pub[32]) {
    static const uint8_t zero[32] = {0};
    if (memcmp(pub, zero, 32) == 0) return;  /* unstamped (fracture child) */
    int idx = destroyed_rocks_lower_bound(w, pub);
    if (idx < (int)w->destroyed_rock_count &&
        memcmp(w->destroyed_rocks[idx].rock_pub, pub, 32) == 0) {
        return; /* already retired, idempotent */
    }
    int cap = (int)(sizeof(w->destroyed_rocks) / sizeof(w->destroyed_rocks[0]));
    if ((int)w->destroyed_rock_count >= cap) {
        SIM_LOG("[sim] destroyed_rocks ledger full (%d) — tombstone dropped\n", cap);
        return;
    }
    int tail = (int)w->destroyed_rock_count - idx;
    if (tail > 0) {
        memmove(&w->destroyed_rocks[idx + 1],
                &w->destroyed_rocks[idx],
                (size_t)tail * sizeof(w->destroyed_rocks[0]));
    }
    memcpy(w->destroyed_rocks[idx].rock_pub, pub, 32);
    w->destroyed_rocks[idx].destroyed_at_ms = (uint64_t)(w->time * 1000.0f);
    w->destroyed_rock_count++;
}

/* Check if a chunk center is within signal coverage of any station. */
static bool chunk_in_signal(const world_t *w, int32_t cx, int32_t cy) {
    float wx = ((float)cx + 0.5f) * CHUNK_SIZE;
    float wy = ((float)cy + 0.5f) * CHUNK_SIZE;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_provides_signal(&w->stations[s])) continue;
        float sr = w->stations[s].signal_range;
        if (v2_dist_sq(w->stations[s].pos, v2(wx, wy)) <= sr * sr)
            return true;
    }
    return false;
}

/* Find a free asteroid slot. Returns -1 if pool full. */
static int find_free_slot(const world_t *w) {
    for (int i = 0; i < MAX_ASTEROIDS; i++)
        if (!w->asteroids[i].active) return i;
    return -1;
}

/* Materialize a chunk_asteroid_t into a world asteroid slot.
 *
 * `seed_slot` is the rock's index within its source chunk's roster;
 * combined with (cx, cy, belt_seed) it derives the rock's permanent
 * identity (rock_pub). Caller iterates the chunk's seed roster and
 * passes r=0..N-1; the resulting rock_pub survives drift, save/load,
 * and any future migration that reshuffles pool slots. */
void materialize_asteroid(world_t *w, int slot, const chunk_asteroid_t *ca,
                           int32_t cx, int32_t cy, uint16_t seed_slot) {
    asteroid_t *a = &w->asteroids[slot];
    memset(a, 0, sizeof(*a));
    fracture_claim_state_reset(&w->fracture_claims[slot]);
    a->active = true;
    a->tier = ca->tier;
    a->commodity = ca->commodity;
    a->pos = ca->pos;
    a->vel = v2(0.0f, 0.0f); /* terrain is stationary until disturbed */
    a->radius = ca->radius;
    a->hp = ca->hp;
    a->max_hp = ca->hp;
    a->ore = ca->ore;
    a->max_ore = ca->ore;
    a->rotation = ca->rotation;
    a->spin = ca->spin;
    a->seed = ca->seed;
    a->last_towed_by = -1;
    a->last_fractured_by = -1;
    a->net_dirty = true;
    compute_rock_pub(w->belt_seed, cx, cy, seed_slot, a->rock_pub);
    w->asteroid_origin[slot].chunk_x = cx;
    w->asteroid_origin[slot].chunk_y = cy;
    w->asteroid_origin[slot].from_chunk = true;
}

/* Check if a chunk is already materialized (has at least one active terrain
 * asteroid from this chunk). */
static bool chunk_materialized(const world_t *w, int32_t cx, int32_t cy) {
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) continue;
        if (w->asteroid_origin[i].from_chunk &&
            w->asteroid_origin[i].chunk_x == cx &&
            w->asteroid_origin[i].chunk_y == cy)
            return true;
    }
    return false;
}

/* Is this asteroid "disturbed" (moved by gameplay, shouldn't auto-despawn)? */
static bool asteroid_disturbed(const asteroid_t *a) {
    return v2_len_sq(a->vel) > 4.0f || /* velocity > 2 u/s */
           a->hp < a->max_hp ||        /* damaged */
           a->last_towed_by >= 0;       /* being towed */
}

void maintain_asteroid_field(world_t *w, float dt) {
    w->field_spawn_timer += dt;
    if (w->field_spawn_timer < FIELD_ASTEROID_RESPAWN_DELAY) return;
    w->field_spawn_timer = 0.0f;

    /* --- Compute needed chunks from player + NPC viewports --- */

    /* Collect viewport centers */
    vec2 viewports[MAX_PLAYERS + MAX_NPC_SHIPS];
    int nv = 0;
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (!w->players[p].connected) continue;
        viewports[nv++] = w->players[p].ship.pos;
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        viewports[nv++] = w->npc_ships[n].ship.pos;
    }
    if (nv == 0) return;

    /* --- Materialize needed chunks --- */
    float mr = MATERIALIZE_RADIUS;
    int chunks_in_radius = (int)ceilf(mr / CHUNK_SIZE);

    for (int vi = 0; vi < nv; vi++) {
        int32_t vcx, vcy;
        chunk_coord(viewports[vi].x, viewports[vi].y, &vcx, &vcy);

        for (int dy = -chunks_in_radius; dy <= chunks_in_radius; dy++) {
            for (int dx = -chunks_in_radius; dx <= chunks_in_radius; dx++) {
                int32_t cx = vcx + dx;
                int32_t cy = vcy + dy;

                /* Skip if already materialized */
                if (chunk_materialized(w, cx, cy)) continue;

                /* Skip if outside signal coverage */
                if (!chunk_in_signal(w, cx, cy)) continue;

                /* Generate and place. Each rock gets its permanent
                 * identity from (belt_seed, cx, cy, r); slots whose
                 * rock_pub is in the destroyed-records ledger were
                 * mined to gravel in some past visit and don't come
                 * back — that's the "permanent floating terrain"
                 * invariant from #285. */
                chunk_asteroid_t rocks[CHUNK_MAX_ASTEROIDS];
                int count = chunk_generate(&w->belt, w->rng, cx, cy,
                                            rocks, CHUNK_MAX_ASTEROIDS);
                for (int r = 0; r < count; r++) {
                    uint8_t pub[32];
                    compute_rock_pub(w->belt_seed, cx, cy, (uint16_t)r, pub);
                    if (rock_pub_is_destroyed(w, pub)) continue;
                    int slot = find_free_slot(w);
                    if (slot < 0) goto pool_full;
                    materialize_asteroid(w, slot, &rocks[r], cx, cy, (uint16_t)r);
                }
            }
        }
    }
    pool_full:

    /* --- Despawn terrain asteroids in chunks no longer needed --- */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) continue;
        if (!w->asteroid_origin[i].from_chunk) continue;
        if (asteroid_disturbed(&w->asteroids[i])) continue;

        /* Check if any viewport still needs this chunk */
        bool needed = false;
        for (int vi = 0; vi < nv; vi++) {
            float dx = w->asteroids[i].pos.x - viewports[vi].x;
            float dy = w->asteroids[i].pos.y - viewports[vi].y;
            if (dx * dx + dy * dy < (mr + 500.0f) * (mr + 500.0f)) {
                needed = true;
                break;
            }
        }
        if (!needed) {
            /* Also check signal coverage — keep if still in signal */
            if (!point_within_signal_margin(w, w->asteroids[i].pos, 260.0f))
                clear_asteroid_slot(w, i);
        }
    }
}
