/*
 * sim_physics.c -- Asteroid physics: N-body gravity, asteroid-asteroid
 * collision, and asteroid-station collision.  Extracted from game_sim.c.
 */
#include "sim_physics.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <string.h>

_Static_assert(MAX_ASTEROIDS <= INT16_MAX,
               "asteroid pair plan indices must fit in int16_t");
_Static_assert(ASTEROID_PAIR_MAX_CANDIDATES < UINT32_MAX,
               "asteroid pair count must fit in uint32_t");

static bool bytes_have_any_bit(const uint8_t *bytes, size_t len) {
    uint8_t any = 0;
    for (size_t i = 0; i < len; i++) any |= bytes[i];
    return any != 0;
}

static int compare_bytes(const uint8_t *a, const uint8_t *b, size_t len) {
    int cmp = memcmp(a, b, len);
    return (cmp > 0) - (cmp < 0);
}

static int compare_u32(uint32_t a, uint32_t b) {
    return (a > b) - (a < b);
}

static int compare_i32(int32_t a, int32_t b) {
    return (a > b) - (a < b);
}

/* IEEE-754 total-order key: numeric order for finite values, deterministic
 * bit order for signed zero, infinities, and any corrupted NaN payload. */
static uint32_t ordered_float_key(float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x80000000u) ? ~bits : (bits ^ 0x80000000u);
}

static int compare_float(float a, float b) {
    return compare_u32(ordered_float_key(a), ordered_float_key(b));
}

static int asteroid_identity_kind(const asteroid_t *asteroid) {
    if (bytes_have_any_bit(asteroid->rock_pub,
                           sizeof(asteroid->rock_pub))) {
        return 0;
    }
    if (bytes_have_any_bit(asteroid->fracture_seed,
                           sizeof(asteroid->fracture_seed))) {
        return 1;
    }
    if (bytes_have_any_bit(asteroid->fragment_pub,
                           sizeof(asteroid->fragment_pub))) {
        return 2;
    }
    return 3;
}

/* Identity is the first body-order key. The explicit semantic tie-break list
 * covers every asteroid_t field without reading struct padding. Slot is used
 * only for an exact asteroid-record tie. Normal live bodies have a unique
 * rock/fragment identity; the anonymous fallback is limited to legacy and
 * test fixtures whose slot metadata/references must also be exchangeable. */
static int asteroid_body_compare(const world_t *w, int left, int right) {
    const asteroid_t *a = &w->asteroids[left];
    const asteroid_t *b = &w->asteroids[right];
    int cmp = compare_i32(asteroid_identity_kind(a),
                          asteroid_identity_kind(b));
    if (cmp != 0) return cmp;

#define COMPARE_BYTES(field) do { \
    cmp = compare_bytes(a->field, b->field, sizeof(a->field)); \
    if (cmp != 0) return cmp; \
} while (0)
#define COMPARE_I32(field) do { \
    cmp = compare_i32((int32_t)a->field, (int32_t)b->field); \
    if (cmp != 0) return cmp; \
} while (0)
#define COMPARE_U32(field) do { \
    cmp = compare_u32((uint32_t)a->field, (uint32_t)b->field); \
    if (cmp != 0) return cmp; \
} while (0)
#define COMPARE_FLOAT(field) do { \
    cmp = compare_float(a->field, b->field); \
    if (cmp != 0) return cmp; \
} while (0)

    switch (asteroid_identity_kind(a)) {
    case 0: COMPARE_BYTES(rock_pub); break;
    case 1:
        COMPARE_BYTES(fracture_seed);
        COMPARE_BYTES(fragment_pub);
        break;
    case 2: COMPARE_BYTES(fragment_pub); break;
    default: break;
    }

    COMPARE_U32(active);
    COMPARE_U32(fracture_child);
    COMPARE_I32(tier);
    COMPARE_FLOAT(pos.x);
    COMPARE_FLOAT(pos.y);
    COMPARE_FLOAT(vel.x);
    COMPARE_FLOAT(vel.y);
    COMPARE_FLOAT(radius);
    COMPARE_FLOAT(hp);
    COMPARE_FLOAT(max_hp);
    COMPARE_FLOAT(ore);
    COMPARE_FLOAT(max_ore);
    COMPARE_I32(commodity);
    COMPARE_FLOAT(rotation);
    COMPARE_FLOAT(spin);
    COMPARE_FLOAT(seed);
    COMPARE_FLOAT(age);
    COMPARE_I32(tractor.kind);
    COMPARE_I32(tractor.source_index);
    COMPARE_I32(tractor.source_part);
    COMPARE_U32(tractor.source_generation);
    COMPARE_I32(last_towed_by);
    COMPARE_I32(last_fractured_by);
    COMPARE_FLOAT(smelt_progress);
    COMPARE_U32(crystal_stage);
    COMPARE_U32(crystal_stage_station);
    COMPARE_U32(crystal_stage_module);
    COMPARE_U32(phase);
    COMPARE_FLOAT(gas_emit_timer);
    COMPARE_U32(net_dirty);
    COMPARE_BYTES(last_towed_token);
    COMPARE_BYTES(thrown_by_token);
    COMPARE_U32(thrown_timer_q);
    COMPARE_BYTES(last_fractured_token);
    COMPARE_BYTES(fracture_seed);
    COMPARE_BYTES(fragment_pub);
    COMPARE_U32(grade);
    COMPARE_BYTES(rock_pub);

#undef COMPARE_FLOAT
#undef COMPARE_U32
#undef COMPARE_I32
#undef COMPARE_BYTES

    return compare_i32(left, right);
}

static int asteroid_pair_cell_compare(const asteroid_pair_cell_t *a,
                                      const asteroid_pair_cell_t *b) {
    int cmp = compare_i32(a->cell_y, b->cell_y);
    return cmp != 0 ? cmp : compare_i32(a->cell_x, b->cell_x);
}

static void asteroid_pair_sort_cells(asteroid_pair_plan_t *plan) {
    asteroid_pair_cell_t scratch[MAX_ASTEROIDS];
    uint32_t count = plan->cell_count;
    for (uint32_t width = 1; width < count; width *= 2u) {
        for (uint32_t left = 0; left < count; left += width * 2u) {
            uint32_t mid = left + width;
            uint32_t end = left + width * 2u;
            if (mid > count) mid = count;
            if (end > count) end = count;
            uint32_t i = left;
            uint32_t j = mid;
            for (uint32_t out = left; out < end; out++) {
                if (i < mid &&
                    (j >= end ||
                     asteroid_pair_cell_compare(
                         &plan->cells[i], &plan->cells[j]) <= 0)) {
                    scratch[out] = plan->cells[i++];
                } else {
                    scratch[out] = plan->cells[j++];
                }
            }
        }
        memcpy(plan->cells, scratch, count * sizeof(plan->cells[0]));
    }
}

static void asteroid_pair_sort_body_span(
    const world_t *w, asteroid_pair_plan_t *plan,
    uint16_t begin, uint16_t count) {
    int16_t scratch[MAX_ASTEROIDS];
    uint32_t first = begin;
    uint32_t limit = first + count;
    for (uint32_t width = 1; width < count; width *= 2u) {
        for (uint32_t left = first; left < limit; left += width * 2u) {
            uint32_t mid = left + width;
            uint32_t end = left + width * 2u;
            if (mid > limit) mid = limit;
            if (end > limit) end = limit;
            uint32_t i = left;
            uint32_t j = mid;
            for (uint32_t out = left; out < end; out++) {
                if (i < mid &&
                    (j >= end ||
                     asteroid_body_compare(
                         w, plan->indices[i], plan->indices[j]) <= 0)) {
                    scratch[out] = plan->indices[i++];
                } else {
                    scratch[out] = plan->indices[j++];
                }
            }
        }
        memcpy(&plan->indices[first], &scratch[first],
               count * sizeof(plan->indices[0]));
    }
}

static int asteroid_pair_find_cell(const asteroid_pair_plan_t *plan,
                                   int32_t cell_x, int32_t cell_y) {
    int lo = 0;
    int hi = (int)plan->cell_count - 1;
    asteroid_pair_cell_t key = {
        .cell_x = cell_x,
        .cell_y = cell_y,
    };
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = asteroid_pair_cell_compare(&plan->cells[mid], &key);
        if (cmp < 0) {
            lo = mid + 1;
        } else if (cmp > 0) {
            hi = mid - 1;
        } else {
            return mid;
        }
    }
    return -1;
}

static void asteroid_pair_emit(
    const asteroid_pair_plan_t *plan,
    uint16_t left_pos, uint16_t right_pos,
    asteroid_pair_visitor_t visitor, void *context,
    uint32_t *count) {
    assert(left_pos < plan->active_count);
    assert(right_pos < plan->active_count);
    assert(left_pos != right_pos);
    if (visitor) {
        visitor(plan->indices[left_pos], plan->indices[right_pos], context);
    }
    (*count)++;
}

static void asteroid_pair_visit_self_cell(
    const asteroid_pair_plan_t *plan,
    const asteroid_pair_cell_t *cell,
    asteroid_pair_visitor_t visitor, void *context,
    uint32_t *pair_count) {
    uint32_t count = cell->count;
    if (count < 2u) return;

    if (count <= ASTEROID_PAIR_EXHAUSTIVE_CELL_LIMIT) {
        for (uint32_t i = 0; i < count; i++) {
            for (uint32_t j = i + 1u; j < count; j++) {
                asteroid_pair_emit(
                    plan, (uint16_t)(cell->begin + i),
                    (uint16_t)(cell->begin + j),
                    visitor, context, pair_count);
            }
        }
        return;
    }

    uint32_t distance_count = count / 2u;
    uint32_t window = asteroid_pair_self_revisit_epochs((uint16_t)count);
    uint32_t phase = (uint32_t)(plan->epoch % window);
    uint32_t start = phase * ASTEROID_PAIR_DISTANCE_BANDS;
    uint32_t remaining = distance_count - start;
    uint32_t bands = ASTEROID_PAIR_DISTANCE_BANDS;
    if (bands > remaining) bands = remaining;
    for (uint32_t band = 0; band < bands; band++) {
        uint32_t distance = start + band + 1u;
        uint32_t owners =
            (count % 2u == 0u && distance == count / 2u)
                ? count / 2u : count;
        for (uint32_t i = 0; i < owners; i++) {
            uint32_t j = (i + distance) % count;
            asteroid_pair_emit(
                plan, (uint16_t)(cell->begin + i),
                (uint16_t)(cell->begin + j),
                visitor, context, pair_count);
        }
    }
}

static void asteroid_pair_visit_cross_cells(
    const asteroid_pair_plan_t *plan,
    const asteroid_pair_cell_t *left,
    const asteroid_pair_cell_t *right,
    asteroid_pair_visitor_t visitor, void *context,
    uint32_t *pair_count) {
    uint32_t left_count = left->count;
    uint32_t right_count = right->count;
    if (left_count == 0u || right_count == 0u) return;

    if (left_count * right_count <=
        ASTEROID_PAIR_EXHAUSTIVE_CROSS_LIMIT) {
        for (uint32_t i = 0; i < left_count; i++) {
            for (uint32_t j = 0; j < right_count; j++) {
                asteroid_pair_emit(
                    plan, (uint16_t)(left->begin + i),
                    (uint16_t)(right->begin + j),
                    visitor, context, pair_count);
            }
        }
        return;
    }

    const asteroid_pair_cell_t *outer =
        left_count >= right_count ? left : right;
    const asteroid_pair_cell_t *inner =
        left_count >= right_count ? right : left;
    uint32_t window = asteroid_pair_cross_revisit_epochs(
        left->count, right->count);
    uint32_t phase = (uint32_t)(plan->epoch % window);
    uint32_t start = phase * ASTEROID_PAIR_DISTANCE_BANDS;
    uint32_t remaining = inner->count - start;
    uint32_t bands = ASTEROID_PAIR_DISTANCE_BANDS;
    if (bands > remaining) bands = remaining;
    for (uint32_t band = 0; band < bands; band++) {
        uint32_t offset = start + band;
        for (uint32_t i = 0; i < outer->count; i++) {
            uint32_t j = (i + offset) % inner->count;
            asteroid_pair_emit(
                plan, (uint16_t)(outer->begin + i),
                (uint16_t)(inner->begin + j),
                visitor, context, pair_count);
        }
    }
}

uint32_t asteroid_pair_plan_visit(
    const asteroid_pair_plan_t *plan,
    asteroid_pair_visitor_t visitor,
    void *context) {
    if (!plan) return 0;
    static const int8_t FORWARD_NEIGHBORS[4][2] = {
        { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 },
    };
    uint32_t pair_count = 0;
    for (uint16_t ci = 0; ci < plan->cell_count; ci++) {
        const asteroid_pair_cell_t *cell = &plan->cells[ci];
        asteroid_pair_visit_self_cell(
            plan, cell, visitor, context, &pair_count);
        for (size_t n = 0;
             n < sizeof(FORWARD_NEIGHBORS) /
                 sizeof(FORWARD_NEIGHBORS[0]);
             n++) {
            int neighbor_index = asteroid_pair_find_cell(
                plan,
                cell->cell_x + FORWARD_NEIGHBORS[n][0],
                cell->cell_y + FORWARD_NEIGHBORS[n][1]);
            if (neighbor_index < 0) continue;
            asteroid_pair_visit_cross_cells(
                plan, cell, &plan->cells[neighbor_index],
                visitor, context, &pair_count);
        }
    }
    return pair_count;
}

uint32_t asteroid_pair_self_revisit_epochs(uint16_t body_count) {
    if (body_count < 2u) return 0;
    if (body_count <= ASTEROID_PAIR_EXHAUSTIVE_CELL_LIMIT) return 1;
    uint32_t distances = body_count / 2u;
    return (distances + ASTEROID_PAIR_DISTANCE_BANDS - 1u) /
           ASTEROID_PAIR_DISTANCE_BANDS;
}

uint32_t asteroid_pair_cross_revisit_epochs(
    uint16_t body_count_a, uint16_t body_count_b) {
    if (body_count_a == 0u || body_count_b == 0u) return 0;
    uint32_t product = (uint32_t)body_count_a * body_count_b;
    if (product <= ASTEROID_PAIR_EXHAUSTIVE_CROSS_LIMIT) return 1;
    uint32_t smaller =
        body_count_a < body_count_b ? body_count_a : body_count_b;
    return (smaller + ASTEROID_PAIR_DISTANCE_BANDS - 1u) /
           ASTEROID_PAIR_DISTANCE_BANDS;
}

static uint32_t asteroid_pair_world_active_count(const world_t *w) {
    uint32_t active_count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (w->asteroids[i].active) active_count++;
    }
    return active_count;
}

static bool asteroid_pair_plan_finalize(
    const world_t *w, asteroid_pair_plan_t *plan, uint32_t active_count) {
    if (active_count > MAX_ASTEROIDS || active_count > UINT16_MAX)
        return false;
    plan->active_count = (uint16_t)active_count;
    plan->epoch = w->tick / ASTEROID_PAIR_TICKS_PER_EPOCH;
    plan->candidate_pair_count =
        asteroid_pair_plan_visit(plan, NULL, NULL);
    if (plan->candidate_pair_count > ASTEROID_PAIR_MAX_CANDIDATES)
        return false;
    assert(plan->candidate_pair_count <= ASTEROID_PAIR_MAX_CANDIDATES);
    return true;
}

static bool asteroid_pair_plan_build_from_grid(
    const world_t *w, asteroid_pair_plan_t *plan,
    uint32_t world_active_count) {
    const spatial_grid_t *grid = &w->asteroid_grid;
    if (!grid->entries && (grid->capacity != 0u || grid->occupied != 0u))
        return false;

    for (uint32_t i = 0; i < grid->capacity; i++) {
        const sparse_cell_entry_t *entry = &grid->entries[i];
        if (entry->key_x == INT32_MIN || entry->cell.count == 0u) continue;
        if (plan->cell_count >= MAX_ASTEROIDS) return false;
        plan->cells[plan->cell_count++] = (asteroid_pair_cell_t) {
            .cell_x = entry->key_x,
            .cell_y = entry->key_y,
        };
    }
    asteroid_pair_sort_cells(plan);

    bool seen[MAX_ASTEROIDS] = {0};
    uint32_t active_count = 0;
    for (uint16_t ci = 0; ci < plan->cell_count; ci++) {
        asteroid_pair_cell_t *planned_cell = &plan->cells[ci];
        const spatial_cell_t *source_cell = spatial_grid_lookup(
            grid, planned_cell->cell_x, planned_cell->cell_y);
        if (!source_cell) return false;
        planned_cell->begin = (uint16_t)active_count;
        for (uint16_t i = 0; i < source_cell->count; i++) {
            int asteroid_index = source_cell->indices[i];
            if (asteroid_index < 0 || asteroid_index >= MAX_ASTEROIDS ||
                !w->asteroids[asteroid_index].active ||
                seen[asteroid_index] ||
                active_count >= MAX_ASTEROIDS) {
                return false;
            }
            seen[asteroid_index] = true;
            plan->indices[active_count++] = (int16_t)asteroid_index;
        }
        planned_cell->count =
            (uint16_t)(active_count - planned_cell->begin);
        if (planned_cell->count > plan->max_cell_count)
            plan->max_cell_count = planned_cell->count;
        asteroid_pair_sort_body_span(
            w, plan, planned_cell->begin, planned_cell->count);
    }

    if (active_count != world_active_count) return false;
    return asteroid_pair_plan_finalize(w, plan, active_count);
}

/* Allocation failure can leave one or more active bodies out of the sparse
 * grid. Rebuild only the immutable gravity/collision pair snapshot directly
 * from the bounded asteroid array in that exceptional case; non-pair grid
 * consumers continue to see the partial acceleration grid for this tick.
 * Unique-cell discovery is deterministic O(active * unique_cells), bounded
 * by MAX_ASTEROIDS squared, and allocation-free. The ordinary 30 Hz path
 * remains the O(active + cells) sparse-grid walk above. */
static bool asteroid_pair_plan_build_from_active_bodies(
    const world_t *w, asteroid_pair_plan_t *plan,
    uint32_t world_active_count) {
    if (world_active_count > MAX_ASTEROIDS ||
        world_active_count > UINT16_MAX) {
        return false;
    }

    for (int asteroid_index = 0;
         asteroid_index < MAX_ASTEROIDS;
         asteroid_index++) {
        const asteroid_t *asteroid = &w->asteroids[asteroid_index];
        if (!asteroid->active) continue;

        int cell_x;
        int cell_y;
        spatial_grid_cell(
            &w->asteroid_grid, asteroid->pos, &cell_x, &cell_y);
        int cell_index = -1;
        for (uint16_t ci = 0; ci < plan->cell_count; ci++) {
            if (plan->cells[ci].cell_x == cell_x &&
                plan->cells[ci].cell_y == cell_y) {
                cell_index = (int)ci;
                break;
            }
        }
        if (cell_index < 0) {
            if (plan->cell_count >= MAX_ASTEROIDS) return false;
            cell_index = (int)plan->cell_count++;
            plan->cells[cell_index] = (asteroid_pair_cell_t) {
                .cell_x = cell_x,
                .cell_y = cell_y,
            };
        }
        if (plan->cells[cell_index].count == UINT16_MAX) return false;
        plan->cells[cell_index].count++;
    }

    asteroid_pair_sort_cells(plan);
    uint32_t next_begin = 0;
    for (uint16_t ci = 0; ci < plan->cell_count; ci++) {
        asteroid_pair_cell_t *cell = &plan->cells[ci];
        uint16_t body_count = cell->count;
        cell->begin = (uint16_t)next_begin;
        next_begin += body_count;
        if (next_begin > world_active_count) return false;
        cell->count = 0;
    }
    if (next_begin != world_active_count) return false;

    for (int asteroid_index = 0;
         asteroid_index < MAX_ASTEROIDS;
         asteroid_index++) {
        const asteroid_t *asteroid = &w->asteroids[asteroid_index];
        if (!asteroid->active) continue;

        int cell_x;
        int cell_y;
        spatial_grid_cell(
            &w->asteroid_grid, asteroid->pos, &cell_x, &cell_y);
        int cell_index =
            asteroid_pair_find_cell(plan, cell_x, cell_y);
        if (cell_index < 0) return false;
        asteroid_pair_cell_t *cell = &plan->cells[cell_index];
        uint32_t body_pos = (uint32_t)cell->begin + cell->count;
        if (body_pos >= world_active_count ||
            body_pos >= MAX_ASTEROIDS) {
            return false;
        }
        plan->indices[body_pos] = (int16_t)asteroid_index;
        cell->count++;
    }

    for (uint16_t ci = 0; ci < plan->cell_count; ci++) {
        asteroid_pair_cell_t *cell = &plan->cells[ci];
        if (cell->count > plan->max_cell_count)
            plan->max_cell_count = cell->count;
        asteroid_pair_sort_body_span(
            w, plan, cell->begin, cell->count);
    }
    return asteroid_pair_plan_finalize(
        w, plan, world_active_count);
}

bool asteroid_pair_plan_build(const world_t *w, asteroid_pair_plan_t *plan) {
    if (!w || !plan) return false;
    uint32_t world_active_count =
        asteroid_pair_world_active_count(w);

    memset(plan, 0, sizeof(*plan));
    if (w->asteroid_grid.overflow_count == 0u &&
        asteroid_pair_plan_build_from_grid(
            w, plan, world_active_count)) {
        return true;
    }

    memset(plan, 0, sizeof(*plan));
    return asteroid_pair_plan_build_from_active_bodies(
        w, plan, world_active_count);
}

static bool token_has_any_bit(const uint8_t token[8]) {
    return bytes_have_any_bit(token, 8);
}

void sim_body_integrate(sim_body_t body, float dt,
                        float velocity_multiplier) {
    if (!body.pos || !body.vel || !isfinite(dt) || dt <= 0.0f) return;
    if (!isfinite(velocity_multiplier) || velocity_multiplier < 0.0f)
        velocity_multiplier = 0.0f;
    *body.pos = v2_add(*body.pos, v2_scale(*body.vel, dt));
    *body.vel = v2_scale(*body.vel, velocity_multiplier);
}

void sim_body_advance(sim_body_t body, float dt) {
    if (!isfinite(dt) || dt <= 0.0f) return;
    if ((body.flags & SIM_BODY_FLAG_SPIN) && body.rotation && body.spin)
        *body.rotation += *body.spin * dt;
    if ((body.flags & SIM_BODY_FLAG_AGE) && body.age)
        *body.age += dt;
    if ((body.flags & SIM_BODY_FLAG_DYNAMIC) &&
        !(body.flags & SIM_BODY_FLAG_KINEMATIC)) {
        sim_body_integrate(body, dt,
            (body.flags & SIM_BODY_FLAG_DRAG)
                ? body.velocity_multiplier : 1.0f);
    }
}

sim_body_t sim_body_from_asteroid(asteroid_t *asteroid) {
    if (!asteroid) return (sim_body_t){0};
    return (sim_body_t){
        .pos = &asteroid->pos,
        .vel = &asteroid->vel,
        .rotation = &asteroid->rotation,
        .spin = &asteroid->spin,
        .age = &asteroid->age,
        .radius = asteroid->radius,
        .velocity_multiplier = 1.0f,
        .flags = SIM_BODY_FLAG_DYNAMIC | SIM_BODY_FLAG_SPIN |
                 SIM_BODY_FLAG_AGE,
        .collision_mask = SIM_BODY_COLLIDE_SHIP |
                          SIM_BODY_COLLIDE_STATION |
                          SIM_BODY_COLLIDE_ASTEROID |
                          SIM_BODY_COLLIDE_CARGO,
    };
}

sim_body_t sim_body_from_cargo_pod(cargo_pod_t *pod) {
    if (!pod) return (sim_body_t){0};
    return (sim_body_t){
        .pos = &pod->pos,
        .vel = &pod->vel,
        .rotation = &pod->rotation,
        .spin = &pod->spin,
        .age = &pod->age,
        .radius = pod->radius,
        .velocity_multiplier = 1.0f,
        .flags = SIM_BODY_FLAG_DYNAMIC | SIM_BODY_FLAG_SPIN |
                 SIM_BODY_FLAG_AGE,
        .collision_mask = SIM_BODY_COLLIDE_SHIP |
                          SIM_BODY_COLLIDE_STATION |
                          SIM_BODY_COLLIDE_ASTEROID |
                          SIM_BODY_COLLIDE_CARGO,
    };
}

sim_body_t sim_body_from_scaffold(scaffold_t *scaffold) {
    if (!scaffold) return (sim_body_t){0};
    sim_body_t body = {
        .pos = &scaffold->pos,
        .vel = &scaffold->vel,
        .rotation = &scaffold->rotation,
        .spin = &scaffold->spin,
        .age = &scaffold->age,
        .radius = scaffold->radius,
        .velocity_multiplier = 1.0f,
        .flags = SIM_BODY_FLAG_SPIN | SIM_BODY_FLAG_AGE,
        .collision_mask = SIM_BODY_COLLIDE_SHIP |
                          SIM_BODY_COLLIDE_STATION |
                          SIM_BODY_COLLIDE_ASTEROID,
    };
    if (scaffold->state == SCAFFOLD_LOOSE ||
        scaffold->state == SCAFFOLD_SNAPPING) {
        body.flags |= SIM_BODY_FLAG_DYNAMIC;
    } else {
        body.flags |= SIM_BODY_FLAG_KINEMATIC;
    }
    if (scaffold_has_tractor(scaffold)) body.flags |= SIM_BODY_FLAG_TOWED;
    return body;
}

void sim_world_integrate_bodies(world_t *w, sim_body_phase_t phase,
                                float dt) {
    if (!w || !isfinite(dt) || dt <= 0.0f) return;
    if (phase == SIM_BODY_PHASE_ASTEROIDS) {
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            asteroid_t *asteroid = &w->asteroids[i];
            if (!asteroid->active) continue;
            sim_body_t body = sim_body_from_asteroid(asteroid);
            entity_ref_t target = world_entity_ref_for_slot(
                w, ENTITY_KIND_ASTEROID, i, -1);
            if (world_tow_link_for_target_const(w, target))
                body.flags |= SIM_BODY_FLAG_TOWED;
            else {
                body.flags |= SIM_BODY_FLAG_DRAG;
                body.velocity_multiplier =
                    1.0f / (1.0f + ASTEROID_AMBIENT_DRAG * dt);
            }
            sim_body_advance(body, dt);
        }
        return;
    }
    if (phase == SIM_BODY_PHASE_CARGO_PODS) {
        for (int i = 0; i < MAX_CARGO_PODS; i++) {
            cargo_pod_t *pod = &w->cargo_pods[i];
            if (!pod->active) continue;
            sim_body_t body = sim_body_from_cargo_pod(pod);
            body.flags |= SIM_BODY_FLAG_DRAG;
            body.velocity_multiplier = 1.0f / (1.0f + 0.35f * dt);
            if (cargo_pod_has_player_tractor(pod) ||
                cargo_pod_has_module_tractor(pod))
                body.flags |= SIM_BODY_FLAG_TOWED;
            sim_body_advance(body, dt);
        }
        return;
    }
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        scaffold_t *scaffold = &w->scaffolds[i];
        if (!scaffold->active) continue;
        sim_body_t body = sim_body_from_scaffold(scaffold);
        if (phase == SIM_BODY_PHASE_SCAFFOLD_AMBIENT) {
            /* Spin and age every live scaffold; only loose bodies drift. */
            if (scaffold->state != SCAFFOLD_LOOSE)
                body.flags &=
                    (uint16_t)(UINT16_MAX ^ SIM_BODY_FLAG_DYNAMIC);
            else {
                body.flags |= SIM_BODY_FLAG_DRAG;
                body.velocity_multiplier = 0.98f;
            }
        } else if (phase == SIM_BODY_PHASE_SCAFFOLD_SNAPPING) {
            if (scaffold->state != SCAFFOLD_SNAPPING) continue;
            body.flags &= (uint16_t)(UINT16_MAX ^
                                     (SIM_BODY_FLAG_SPIN |
                                      SIM_BODY_FLAG_AGE));
        } else {
            continue;
        }
        sim_body_advance(body, dt);
    }
}

sim_body_contact_t sim_body_resolve_static_circle(
    sim_body_t body, vec2 center, float obstacle_radius,
    vec2 obstacle_vel, float bounce_scale, float skin) {
    sim_body_contact_t contact = {0};
    if (!body.pos || !body.vel || body.radius < 0.0f ||
        obstacle_radius < 0.0f) {
        return contact;
    }
    float min_dist = body.radius + obstacle_radius;
    vec2 delta = v2_sub(*body.pos, center);
    float dist_sq = v2_len_sq(delta);
    if (dist_sq >= min_dist * min_dist) return contact;

    float dist = v2_len(delta);
    if (dist < 0.001f) {
        dist = 0.001f;
        delta = v2(1.0f, 0.0f);
    }
    contact.collided = true;
    contact.normal = v2_scale(delta, 1.0f / dist);
    vec2 relative_vel = v2_sub(*body.vel, obstacle_vel);
    contact.closing_speed = -v2_dot(relative_vel, contact.normal);

    float overlap = min_dist - dist;
    *body.pos = v2_add(*body.pos,
                       v2_scale(contact.normal, overlap + skin));
    float vel_along = v2_dot(relative_vel, contact.normal);
    if (vel_along < 0.0f) {
        *body.vel = v2_sub(*body.vel,
                           v2_scale(contact.normal,
                                    vel_along * bounce_scale));
    }
    return contact;
}

void asteroid_clear_thrown(asteroid_t *a) {
    if (!a) return;
    bool changed = a->thrown_timer_q > 0 || token_has_any_bit(a->thrown_by_token);
    memset(a->thrown_by_token, 0, sizeof(a->thrown_by_token));
    a->thrown_timer_q = 0;
    if (changed) a->net_dirty = true;
}

void asteroid_mark_thrown(asteroid_t *a, const uint8_t token[8], float seconds) {
    if (!a) return;
    if (!token || !token_has_any_bit(token) || seconds <= 0.0f || !isfinite(seconds)) {
        asteroid_clear_thrown(a);
        return;
    }
    unsigned q = (unsigned)ceilf(seconds * 10.0f);
    if (q == 0) q = 1;
    if (q > 255u) q = 255u;
    memcpy(a->thrown_by_token, token, sizeof(a->thrown_by_token));
    a->thrown_timer_q = (uint8_t)q;
    a->net_dirty = true;
}

bool asteroid_is_ballistic(const asteroid_t *a) {
    return a && a->thrown_timer_q > 0 && token_has_any_bit(a->thrown_by_token);
}

void asteroid_step_thrown_state(world_t *w) {
    if (!w || w->tick == 0 || (w->tick % ASTEROID_THROW_TIMER_TICKS) != 0) return;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->thrown_timer_q == 0) continue;
        a->thrown_timer_q--;
        if (a->thrown_timer_q == 0)
            memset(a->thrown_by_token, 0, sizeof(a->thrown_by_token));
        a->net_dirty = true;
    }
}

typedef struct {
    vec2 pos;
    float radius;
    int intake_modules;
} asteroid_pull_station_t;

static int collect_asteroid_pull_stations(const world_t *w,
                                          asteroid_pull_station_t out[MAX_STATIONS]) {
    int count = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (st->scaffold) continue;
        int intake_modules = 0;
        for (int m = 0; m < st->module_count; m++) {
            if (st->modules[m].scaffold) continue;
            module_type_t mt = st->modules[m].type;
            if (mt == MODULE_HOPPER || mt == MODULE_FURNACE)
                intake_modules++;
        }
        if (intake_modules == 0) continue;
        out[count++] = (asteroid_pull_station_t) {
            .pos = st->pos,
            .radius = st->radius,
            .intake_modules = intake_modules,
        };
    }
    return count;
}

/* ================================================================== */
/* Asteroid-asteroid gravity                                          */
/* ================================================================== */

typedef struct {
    world_t *world;
    float dt;
} asteroid_gravity_pair_context_t;

static void step_asteroid_gravity_pair(
    int asteroid_a, int asteroid_b, void *opaque) {
    asteroid_gravity_pair_context_t *context = opaque;
    world_t *w = context->world;
    asteroid_t *a = &w->asteroids[asteroid_a];
    asteroid_t *b = &w->asteroids[asteroid_b];
    if (!a->active || !b->active ||
        a->tier == ASTEROID_TIER_S || b->tier == ASTEROID_TIER_S) {
        return;
    }
    vec2 delta = v2_sub(b->pos, a->pos);
    float dist_sq = v2_len_sq(delta);
    if (dist_sq > 800.0f * 800.0f || dist_sq < 1.0f) return;
    float dist = v2_len(delta);
    /* Don't attract asteroids at or inside collision boundary. */
    float min_dist = a->radius + b->radius;
    if (dist < min_dist * 1.3f) return;
    vec2 normal = v2_scale(delta, 1.0f / dist);
    float mass_a = a->radius * a->radius;
    float mass_b = b->radius * b->radius;
    /* Gravitational force proportional to both masses. Clamp against the
     * lighter body so exchanging plan operands preserves equal/opposite
     * force and the canonical result. */
    float force_mag = (mass_a * mass_b) / dist_sq * 14.0f;
    float max_force = 60.0f * fminf(mass_a, mass_b);
    if (force_mag > max_force) force_mag = max_force;
    vec2 accel_a =
        v2_scale(normal, (force_mag / mass_a) * context->dt);
    vec2 accel_b =
        v2_scale(normal, -(force_mag / mass_b) * context->dt);
    a->vel = v2_add(a->vel, accel_a);
    b->vel = v2_add(b->vel, accel_b);
}

void step_asteroid_gravity(
    world_t *w, float dt, const asteroid_pair_plan_t *plan) {
    /* world_sim_step builds this once after asteroid integration so cargo,
     * gravity, and asteroid collision all share the same tick snapshot. */
    const spatial_grid_t *g = &w->asteroid_grid;

    /* Asteroid-asteroid attraction consumes the same immutable pair
     * ownership plan as collision; no slot-order filter lives here. */
    asteroid_gravity_pair_context_t gravity_context = {
        .world = w,
        .dt = dt,
    };
    asteroid_pair_plan_visit(
        plan, step_asteroid_gravity_pair, &gravity_context);

    /* Industrial pull: only stations with active intake/processing modules
     * generate asteroid attraction. Pull scales with industrial activity
     * and inversely with asteroid size (fragments pulled strongly). */
    asteroid_pull_station_t pull_stations[MAX_STATIONS];
    int pull_station_count = collect_asteroid_pull_stations(w, pull_stations);
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        for (int s = 0; s < pull_station_count; s++) {
            const asteroid_pull_station_t *st = &pull_stations[s];
            int intake_modules = st->intake_modules;
            vec2 delta = v2_sub(st->pos, a->pos);
            float dist_sq = v2_len_sq(delta);
            float pull_range = 600.0f + (float)intake_modules * 100.0f;
            if (dist_sq > pull_range * pull_range || dist_sq < 1.0f) continue;
            float dist = v2_len(delta);
            float min_dist = a->radius + st->radius;
            if (dist < min_dist + 10.0f) continue;
            vec2 normal = v2_scale(delta, 1.0f / dist);
            /* Tier-dependent: smaller = more pulled. radius^2 inversely scales force */
            float mass_a = a->radius * a->radius;
            float base_force = (float)intake_modules * 2.5f;
            float force = base_force * st->radius / (dist * 0.8f);
            /* TIER_S fragments get extra pull for hopper feeding */
            if (a->tier == ASTEROID_TIER_S) force *= 3.0f;
            float accel = force / mass_a;
            a->vel = v2_add(a->vel, v2_scale(normal, accel * dt));
        }
    }

    /* Signal pressure: strong relay coverage pushes isolated field rocks
     * down the signal gradient. That makes mined-out/high-signal cores
     * shed terrain toward the frontier/fringe instead of pulling the
     * frontier empty. */

    /* Prebuild connected player positions once — avoids scanning all
     * MAX_PLAYERS (32) for every asteroid in the weak-signal loop. */
    vec2 player_positions[MAX_PLAYERS];
    int player_count = 0;
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (server_player_is_gameplay_ready(&w->players[p]))
            player_positions[player_count++] = w->players[p].ship->pos;
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;

        bool near_player = false;
        for (int p = 0; p < player_count; p++) {
            if (v2_dist_sq(a->pos, player_positions[p]) <= 600.0f * 600.0f) {
                near_player = true;
                break;
            }
        }
        if (near_player) continue;

        bool near_asteroid = false;
        {
            int acx, acy;
            spatial_grid_cell(g, a->pos, &acx, &acy);
            for (int gy = acy - 1; gy <= acy + 1 && !near_asteroid; gy++) {
                for (int gx = acx - 1; gx <= acx + 1 && !near_asteroid; gx++) {
                    const spatial_cell_t *cell = spatial_grid_lookup(g, gx, gy);
                    if (!cell) continue;
                    for (uint16_t ci = 0; ci < cell->count; ci++) {
                        int j = cell->indices[ci];
                        if (j == i || !w->asteroids[j].active) continue;
                        if (v2_dist_sq(a->pos, w->asteroids[j].pos) <= 400.0f * 400.0f) {
                            near_asteroid = true;
                            break;
                        }
                    }
                }
            }
        }
        if (near_asteroid) continue;

        float sig_here = signal_strength_at(w, a->pos);
        if (sig_here <= SIGNAL_BAND_FRONTIER) continue;

        const float step = 300.0f;
        float sxp = signal_strength_at(w, v2_add(a->pos, v2(step, 0.0f)));
        float sxm = signal_strength_at(w, v2_add(a->pos, v2(-step, 0.0f)));
        float syp = signal_strength_at(w, v2_add(a->pos, v2(0.0f, step)));
        float sym = signal_strength_at(w, v2_add(a->pos, v2(0.0f, -step)));
        vec2 uphill = v2(sxp - sxm, syp - sym);
        float glen_sq = v2_len_sq(uphill);
        if (glen_sq < 0.000001f) continue;

        vec2 downhill = v2_scale(uphill, -1.0f / v2_len(uphill));
        float pressure = (sig_here - SIGNAL_BAND_FRONTIER) /
                         (SIGNAL_BAND_OPERATIONAL - SIGNAL_BAND_FRONTIER);
        if (pressure > 1.0f) pressure = 1.0f;
        a->vel = v2_add(a->vel, v2_scale(downhill, 3.0f * pressure * dt));
    }
}

/* ================================================================== */
/* Asteroid-asteroid collision                                        */
/* ================================================================== */

static void resolve_asteroid_collision_pair(
    int asteroid_a, int asteroid_b, void *opaque) {
    world_t *w = opaque;
    asteroid_t *a = &w->asteroids[asteroid_a];
    asteroid_t *b = &w->asteroids[asteroid_b];
    if (!a->active || !b->active) return;
    if (a->tier == ASTEROID_TIER_S && b->tier == ASTEROID_TIER_S) return;

    float min_dist = a->radius + b->radius;
    vec2 delta = v2_sub(a->pos, b->pos);
    float dist_sq = v2_len_sq(delta);
    if (dist_sq >= min_dist * min_dist) return;
    float dist = v2_len(delta);
    if (dist < 0.001f) {
        dist = 0.001f;
        delta = v2(1.0f, 0.0f);
    }
    vec2 normal = v2_scale(delta, 1.0f / dist);
    float overlap = min_dist - dist;
    /* Push apart: heavier (larger radius) moves less. */
    float mass_a = a->radius * a->radius;
    float mass_b = b->radius * b->radius;
    float total_mass = mass_a + mass_b;
    float ratio_a = mass_b / total_mass;
    float ratio_b = mass_a / total_mass;
    a->pos = v2_add(a->pos, v2_scale(normal, overlap * ratio_a));
    b->pos = v2_sub(b->pos, v2_scale(normal, overlap * ratio_b));
    /* Transfer velocity along collision normal. */
    float rel_vel = v2_dot(v2_sub(a->vel, b->vel), normal);
    if (rel_vel < 0.0f) {
        vec2 impulse_a = v2_scale(normal, rel_vel * ratio_a);
        vec2 impulse_b = v2_scale(normal, rel_vel * ratio_b);
        a->vel = v2_sub(a->vel, impulse_a);
        b->vel = v2_add(b->vel, impulse_b);
    }
}

void resolve_asteroid_collisions(
    world_t *w, const asteroid_pair_plan_t *plan) {
    asteroid_pair_plan_visit(
        plan, resolve_asteroid_collision_pair, w);
}

/* ================================================================== */
/* Asteroid-station collision                                         */
/* ================================================================== */

static void resolve_asteroid_module_collision(asteroid_t *a, vec2 mod_pos, float mod_radius) {
    sim_body_contact_t contact = sim_body_resolve_static_circle(
        (sim_body_t){ .pos = &a->pos, .vel = &a->vel, .radius = a->radius },
        mod_pos, mod_radius, v2(0.0f, 0.0f), 1.0f, 1.0f);
    if (contact.collided) a->net_dirty = true;
}

static void resolve_asteroid_corridor_collision(asteroid_t *a, vec2 center,
                                                float ring_r, float angle_a,
                                                float arc_delta) {
    vec2 delta = v2_sub(a->pos, center);
    float dist = v2_len(delta);
    if (dist < 1.0f) return;

    float r_inner = ring_r - STATION_CORRIDOR_HW - a->radius;
    float r_outer = ring_r + STATION_CORRIDOR_HW + a->radius;
    if (dist <= r_inner || dist >= r_outer) return;

    float ast_angle = fixp_atan2f(delta.y, delta.x);
    float angular_margin = fixp_asinf(fminf(a->radius / dist, 1.0f));
    float expanded_start = angle_a - angular_margin;
    float expanded_delta = arc_delta + 2.0f * angular_margin;
    if (angle_in_arc(ast_angle, expanded_start, expanded_delta) < 0.0f) return;

    vec2 radial = v2_scale(delta, 1.0f / dist);
    vec2 push_normal;
    float d_inner = dist - (ring_r - STATION_CORRIDOR_HW);
    float d_outer = (ring_r + STATION_CORRIDOR_HW) - dist;
    if (d_inner < d_outer) {
        a->pos = v2_add(center, v2_scale(radial,
            ring_r - STATION_CORRIDOR_HW - a->radius - 1.0f));
        push_normal = v2_scale(radial, -1.0f);
    } else {
        a->pos = v2_add(center, v2_scale(radial,
            ring_r + STATION_CORRIDOR_HW + a->radius + 1.0f));
        push_normal = radial;
    }

    float vel_along = v2_dot(a->vel, push_normal);
    if (vel_along < 0.0f)
        a->vel = v2_sub(a->vel, v2_scale(push_normal, vel_along));
    a->net_dirty = true;
}

static bool asteroid_near_corridor_module(const asteroid_t *a,
                                          const station_geom_t *geom,
                                          const geom_corridor_t *cor) {
    vec2 delta = v2_sub(a->pos, geom->center);
    float dist = v2_len(delta);
    if (fabsf(dist - cor->ring_radius) >=
        STATION_CORRIDOR_HW + a->radius + STATION_MODULE_COL_RADIUS)
        return false;

    float ast_ang = fixp_atan2f(delta.y, delta.x);
    for (int mi = 0; mi < geom->circle_count; mi++) {
        const geom_circle_t *circle = &geom->circles[mi];
        if (circle->ring != cor->ring) continue;
        float angular_size = (cor->ring_radius > 1.0f)
            ? (STATION_MODULE_COL_RADIUS + a->radius) / cor->ring_radius
            : 0.0f;
        if (fabsf(wrap_angle(ast_ang - circle->angle)) < angular_size)
            return true;
    }
    return false;
}

static bool asteroid_near_station_collision_envelope(const asteroid_t *a,
                                                     const station_t *st) {
    float reach = station_collision_envelope_radius(st) + a->radius;
    return v2_dist_sq(a->pos, st->pos) <= reach * reach;
}

void resolve_asteroid_station_collisions(world_t *w) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_collides(st)) continue;

        bool any_near = false;
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            asteroid_t *a = &w->asteroids[i];
            if (!a->active) continue;
            if (asteroid_near_station_collision_envelope(a, st)) {
                any_near = true;
                break;
            }
        }
        if (!any_near) continue;

        /* A full MAX_STATIONS geometry cache is ~320 KiB with the expanded
         * station cap, which overflows the browser WASM stack. Keep only one
         * station's geometry live while walking all asteroids. */
        station_geom_t geom;
        station_build_geom(st, &geom);

        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            asteroid_t *a = &w->asteroids[i];
            if (!a->active) continue;
            if (!asteroid_near_station_collision_envelope(a, st)) continue;
            /* Core collision */
            if (geom.has_core)
                resolve_asteroid_module_collision(a, geom.core.center, geom.core.radius);
            /* Module and dock collision circles. */
            for (int ci = 0; ci < geom.circle_count; ci++)
                resolve_asteroid_module_collision(a, geom.circles[ci].center, geom.circles[ci].radius);
            /* Corridor arcs form the visible station wall bands between modules. */
            for (int ci = 0; ci < geom.corridor_count; ci++) {
                const geom_corridor_t *cor = &geom.corridors[ci];
                if (asteroid_near_corridor_module(a, &geom, cor)) continue;
                resolve_asteroid_corridor_collision(a, geom.center,
                    cor->ring_radius, cor->angle_a, cor->arc_delta);
            }
        }
    }
}
