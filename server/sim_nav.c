/*
 * sim_nav.c — A* pathfinding and station nav mesh for Signal Space Miner.
 * Extracted from game_sim.c (#272 slice).
 */
#include "sim_nav.h"
#include <stdlib.h>


/* (Reactive avoidance deleted — see git history for compute_path_avoidance.
 * All navigation now uses A* paths + nav_forward_clearance.) */

/* ================================================================== */
/* A* Pathfinding — sparse navigation graph through station docks     */
/* ================================================================== */

enum {
    NAV_MAX_NODES = 96,
};

typedef struct {
    vec2 pos;
} nav_node_t;

/* Extended graph node: position + optional precomputed edge info. */
typedef struct {
    nav_node_t nodes[NAV_MAX_NODES];
    int count;
    /* Precomputed edge bitfield: edge_ok[i] has bit j set if nodes
     * i and j are pre-validated (skip nav_line_clear). Only used for
     * station-internal edges injected by the precomputed nav mesh. */
    uint64_t edge_ok[NAV_MAX_NODES]; /* NAV_MAX_NODES <= 96, need 2 words */
    uint64_t edge_ok_hi[NAV_MAX_NODES]; /* bits 64..95 */
} nav_graph_t;

/* ------------------------------------------------------------------ */
/* Precomputed station nav mesh ("roads" between rings)               */
/* ------------------------------------------------------------------ */
/* Station geometry is semi-static (changes only when modules are
 * added/activated). We precompute the navigable road network once and
 * inject it into the A* graph at query time. Node positions are stored
 * in local polar coords so ring rotation is handled at conversion. */

enum {
    SNAV_MAX_NODES = 28,  /* center + 8 exterior + up to ~9 dock pairs */
    SNAV_MAX_EDGES = 64,
};

typedef enum {
    SNAV_FIXED,   /* position is fixed angle+radius from station center */
    SNAV_RING,    /* position tracks a ring slot (rotates with ring) */
} snav_node_kind_t;

typedef struct {
    snav_node_kind_t kind;
    /* SNAV_FIXED: angle + radius from station center */
    /* SNAV_RING:  ring + slot, r_offset from ring radius */
    float angle;      /* fixed angle, or unused for RING */
    float radius;     /* fixed radius from center, or r_offset for RING */
    int   ring;       /* 0 for FIXED, 1-3 for RING */
    int   slot;       /* slot index for RING nodes */
} snav_node_t;

typedef struct {
    uint8_t a, b;
} snav_edge_t;

typedef struct {
    snav_node_t nodes[SNAV_MAX_NODES];
    int         node_count;
    snav_edge_t edges[SNAV_MAX_EDGES];
    int         edge_count;
    bool        valid;  /* false = needs rebuild */
} station_nav_t;

static station_nav_t s_station_nav[MAX_STATIONS];

/* Convert a precomputed nav node to world-space for a given station. */
static vec2 snav_node_world_pos(const station_t *st, const snav_node_t *n) {
    if (n->kind == SNAV_RING) {
        float ang = module_angle_ring(st, n->ring, n->slot);
        float r = STATION_RING_RADIUS[n->ring] + n->radius;
        return v2_add(st->pos, v2(cosf(ang) * r, sinf(ang) * r));
    }
    /* SNAV_FIXED */
    return v2_add(st->pos, v2(cosf(n->angle) * n->radius,
                               sinf(n->angle) * n->radius));
}

static nav_path_t s_npc_paths[MAX_NPC_SHIPS];
static nav_path_t s_player_paths[MAX_PLAYERS];

nav_path_t *nav_npc_path(int npc_idx) {
    if (npc_idx < 0 || npc_idx >= MAX_NPC_SHIPS) {
        /* Out-of-bounds: return slot 0 with a forced replan so it
         * doesn't silently alias another actor's path. */
        s_npc_paths[0].age = 999.0f;
        return &s_npc_paths[0];
    }
    return &s_npc_paths[npc_idx];
}

nav_path_t *nav_player_path(int player_id) {
    if (player_id < 0 || player_id >= MAX_PLAYERS) {
        s_player_paths[0].age = 999.0f;
        return &s_player_paths[0];
    }
    return &s_player_paths[player_id];
}

void nav_force_replan(nav_path_t *path) {
    path->age = 999.0f;
}

/* Test if a line segment a->b is clear of station ring walls and large
 * asteroids. Returns a simple boolean.
 *
 * Asteroid checks use the spatial grid to avoid scanning ALL asteroids.
 * We walk cells along the segment's bounding box (expanded by the max
 * asteroid radius + clearance margin) and only test asteroids in those
 * cells. */
static bool nav_line_clear(const world_t *w, vec2 a, vec2 b, float clearance) {
    vec2 delta = v2_sub(b, a);
    float seg_len = v2_len(delta);
    if (seg_len < 1.0f) return true;
    vec2 fwd = v2_scale(delta, 1.0f / seg_len);

    /* Check large asteroids via spatial grid. Expand the AABB by the
     * max possible asteroid radius (~120u for XL) + 2x clearance so we
     * don't miss rocks whose center is outside the tight corridor but
     * whose body overlaps. */
    {
        const spatial_grid_t *g = &w->asteroid_grid;
        const float margin = 150.0f + clearance * 2.0f;
        float min_x = fminf(a.x, b.x) - margin;
        float min_y = fminf(a.y, b.y) - margin;
        float max_x = fmaxf(a.x, b.x) + margin;
        float max_y = fmaxf(a.y, b.y) + margin;
        int cx0, cy0, cx1, cy1;
        spatial_grid_cell(g, v2(min_x, min_y), &cx0, &cy0);
        spatial_grid_cell(g, v2(max_x, max_y), &cx1, &cy1);
        /* Deduplicate: track which asteroid indices we've already tested.
         * Use a small bitset — MAX_ASTEROIDS is typically <= 512. */
        uint64_t checked[MAX_ASTEROIDS / 64 + 1];
        memset(checked, 0, sizeof(checked));
        for (int cy = cy0; cy <= cy1; cy++) {
            for (int cx = cx0; cx <= cx1; cx++) {
                const spatial_cell_t *cell = spatial_grid_lookup(g, cx, cy);
                if (!cell) continue;
                for (int ci = 0; ci < cell->count; ci++) {
                    int idx = cell->indices[ci];
                    if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
                    /* Dedup check */
                    int word = idx / 64, bit = idx % 64;
                    if (checked[word] & (1ULL << bit)) continue;
                    checked[word] |= (1ULL << bit);
                    const asteroid_t *ast = &w->asteroids[idx];
                    if (!ast->active || ast->tier == ASTEROID_TIER_S) continue;
                    vec2 to_a = v2_sub(ast->pos, a);
                    float proj = v2_dot(to_a, fwd);
                    if (proj < -ast->radius || proj > seg_len + ast->radius) continue;
                    float perp = fabsf(v2_cross(to_a, fwd));
                    if (perp < ast->radius + clearance * 2.0f) return false;
                }
            }
        }
    }

    /* Check station structures */
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_collides(st)) continue;
        float st_d = v2_dist_sq(st->pos, a);
        float max_r = 600.0f; /* conservative max ring + margin */
        if (st_d > (seg_len + max_r) * (seg_len + max_r)) continue;

        /* Core circle */
        if (st->radius > 0.0f) {
            vec2 to_c = v2_sub(st->pos, a);
            float proj = v2_dot(to_c, fwd);
            if (proj > -st->radius && proj < seg_len + st->radius) {
                float perp = fabsf(v2_cross(to_c, fwd));
                if (perp < st->radius + clearance) return false;
            }
        }

        /* Ring walls: ray-circle intersection for each ring */
        for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
            float ring_r = STATION_RING_RADIUS[ring];
            /* Check if any module exists on this ring */
            bool has_modules = false;
            for (int mi = 0; mi < st->module_count; mi++)
                if (st->modules[mi].ring == ring) { has_modules = true; break; }
            if (!has_modules) continue;

            vec2 lp = v2_sub(a, st->pos);
            float bb = 2.0f * v2_dot(lp, fwd);
            float cc = v2_len_sq(lp) - ring_r * ring_r;
            float disc = bb * bb - 4.0f * cc;
            if (disc < 0.0f) continue;
            float sqd = sqrtf(disc);
            float t1 = (-bb - sqd) * 0.5f;
            float t2 = (-bb + sqd) * 0.5f;
            /* Check both intersection points */
            for (int ti = 0; ti < 2; ti++) {
                float t = (ti == 0) ? t1 : t2;
                if (t < 0.0f || t > seg_len) continue;
                vec2 cross_local = v2_add(lp, v2_scale(fwd, t));
                float cross_ang = atan2f(cross_local.y, cross_local.x);
                /* Is this a dock slot? If so, passable. */
                bool dock_slot = false;
                for (int mi = 0; mi < st->module_count; mi++) {
                    if (st->modules[mi].ring != ring) continue;
                    if (st->modules[mi].type != MODULE_DOCK) continue;
                    if (st->modules[mi].scaffold) continue;
                    float dock_ang = module_angle_ring(st, ring, st->modules[mi].slot);
                    int slots_n = STATION_RING_SLOTS[ring];
                    float slot_arc = (slots_n > 0) ? (TWO_PI_F / (float)slots_n) : TWO_PI_F;
                    if (fabsf(wrap_angle(cross_ang - dock_ang)) < slot_arc * 0.35f) {
                        dock_slot = true; break;
                    }
                }
                if (dock_slot) continue;
                /* Check if crossing hits a corridor or non-dock module */
                bool in_wall = false;
                for (int mi = 0; mi < st->module_count; mi++) {
                    if (st->modules[mi].ring != ring) continue;
                    if (st->modules[mi].type == MODULE_DOCK) continue;
                    if (st->modules[mi].scaffold) continue;
                    float mod_ang = module_angle_ring(st, ring, st->modules[mi].slot);
                    float ang_size = (STATION_MODULE_COL_RADIUS + clearance) / ring_r;
                    if (fabsf(wrap_angle(cross_ang - mod_ang)) < ang_size) {
                        in_wall = true; break;
                    }
                }
                if (!in_wall) {
                    /* Check corridor arcs between modules */
                    station_geom_t geom;
                    station_build_geom(st, &geom);
                    for (int co = 0; co < geom.corridor_count; co++) {
                        if (geom.corridors[co].ring != ring) continue;
                        float a0 = geom.corridors[co].angle_a;
                        float a1 = geom.corridors[co].angle_b;
                        float da = wrap_angle(a1 - a0);
                        if (angle_in_arc(cross_ang, a0, da) >= 0.0f) {
                            in_wall = true; break;
                        }
                    }
                }
                if (in_wall) return false;
            }
        }
    }
    return true;
}

bool nav_segment_clear(const world_t *w, vec2 start, vec2 goal, float clearance) {
    return nav_line_clear(w, start, goal, clearance);
}

/* ------------------------------------------------------------------ */
/* station_build_nav — precompute navigable road network for a station */
/* ------------------------------------------------------------------ */
/* Nodes:
 *   - Station center (FIXED)
 *   - Per dock: outer waypoint (ring_r + 100) and inner (ring_r - 100), both RING
 *   - 8 exterior waypoints around the outermost ring (FIXED, every 45 deg)
 *
 * Edges: every pair of nodes is tested with nav_line_clear. Validated
 * pairs are stored so the A* graph can skip the expensive check later.
 *
 * Must be called after module changes (activation, placement, init). */
void station_build_nav(const world_t *w, int station_idx) {
    const station_t *st = &w->stations[station_idx];
    station_nav_t *nav = &s_station_nav[station_idx];
    nav->node_count = 0;
    nav->edge_count = 0;
    nav->valid = false;

    if (!station_collides(st)) return;

    /* --- Add center node (FIXED) --- */
    if (nav->node_count < SNAV_MAX_NODES) {
        snav_node_t *n = &nav->nodes[nav->node_count++];
        n->kind = SNAV_FIXED;
        n->angle = 0.0f;
        n->radius = 0.0f;
        n->ring = 0;
        n->slot = 0;
    }

    /* --- Add dock inner/outer pairs (RING) --- */
    for (int mi = 0; mi < st->module_count && nav->node_count < SNAV_MAX_NODES - 2; mi++) {
        if (st->modules[mi].type != MODULE_DOCK) continue;
        if (st->modules[mi].scaffold) continue;
        int ring = st->modules[mi].ring;
        int slot = st->modules[mi].slot;

        /* Outer: 100u outside the ring */
        snav_node_t *outer = &nav->nodes[nav->node_count++];
        outer->kind = SNAV_RING;
        outer->angle = 0.0f;
        outer->radius = 100.0f;  /* offset from ring radius */
        outer->ring = ring;
        outer->slot = slot;

        /* Inner: 100u inside the ring */
        if (nav->node_count < SNAV_MAX_NODES) {
            snav_node_t *inner = &nav->nodes[nav->node_count++];
            inner->kind = SNAV_RING;
            inner->angle = 0.0f;
            inner->radius = -100.0f;
            inner->ring = ring;
            inner->slot = slot;
        }
    }

    /* --- Add 8 exterior waypoints around outermost occupied ring (FIXED) --- */
    float outer_r = 0.0f;
    for (int mi = 0; mi < st->module_count; mi++) {
        int r = st->modules[mi].ring;
        if (r >= 1 && r <= STATION_NUM_RINGS && STATION_RING_RADIUS[r] > outer_r)
            outer_r = STATION_RING_RADIUS[r];
    }
    if (outer_r > 0.0f) {
        for (int q = 0; q < 8 && nav->node_count < SNAV_MAX_NODES; q++) {
            float ang = (float)q * (PI_F * 0.25f);
            snav_node_t *n = &nav->nodes[nav->node_count++];
            n->kind = SNAV_FIXED;
            n->angle = ang;
            n->radius = outer_r + 140.0f;
            n->ring = 0;
            n->slot = 0;
        }
    }

    /* --- Validate edges: test all pairs with nav_line_clear --- */
    /* Only precompute edges where both nodes are rotation-invariant:
     * RING<->RING (all rings share one rotation speed, so relative
     * geometry is constant) or FIXED<->FIXED. Mixed FIXED<->RING edges
     * go stale as rings rotate, so those are left for runtime
     * nav_line_clear during A* expansion. */
    const float clearance = 46.0f; /* 16 ship_radius + 30 margin */
    for (int i = 0; i < nav->node_count && nav->edge_count < SNAV_MAX_EDGES; i++) {
        for (int j = i + 1; j < nav->node_count && nav->edge_count < SNAV_MAX_EDGES; j++) {
            bool same_kind = (nav->nodes[i].kind == nav->nodes[j].kind);
            if (!same_kind) continue; /* skip FIXED<->RING — stale after rotation */
            vec2 pi = snav_node_world_pos(st, &nav->nodes[i]);
            vec2 pj = snav_node_world_pos(st, &nav->nodes[j]);
            if (nav_line_clear(w, pi, pj, clearance)) {
                nav->edges[nav->edge_count].a = (uint8_t)i;
                nav->edges[nav->edge_count].b = (uint8_t)j;
                nav->edge_count++;
            }
        }
    }

    nav->valid = true;
}

/* Rebuild nav for all stations. Called at world init and after save load. */
void station_rebuild_all_nav(const world_t *w) {
    for (int s = 0; s < MAX_STATIONS; s++)
        station_build_nav(w, s);
}

/* Helper: set prevalidated edge bit in both directions. */
static void nav_graph_set_edge(nav_graph_t *g, int i, int j) {
    if (j < 64) g->edge_ok[i] |= (1ULL << j);
    else        g->edge_ok_hi[i] |= (1ULL << (j - 64));
    if (i < 64) g->edge_ok[j] |= (1ULL << i);
    else        g->edge_ok_hi[j] |= (1ULL << (i - 64));
}

static bool nav_graph_has_edge(const nav_graph_t *g, int i, int j) {
    if (j < 64) return (g->edge_ok[i] >> j) & 1;
    return (g->edge_ok_hi[i] >> (j - 64)) & 1;
}

/* Build a sparse navigation graph with precomputed station roads +
 * dynamic asteroid bypass waypoints. */
static void nav_build_graph(const world_t *w, vec2 start, vec2 goal,
                            float clearance, nav_graph_t *g) {
    g->count = 0;
    memset(g->edge_ok, 0, sizeof(g->edge_ok));
    memset(g->edge_ok_hi, 0, sizeof(g->edge_ok_hi));

    /* Node 0 = start, Node 1 = goal */
    g->nodes[g->count++].pos = start;
    g->nodes[g->count++].pos = goal;

    /* Inject precomputed station nav meshes. Convert local coords to
     * world-space and translate precomputed edges into graph edge bits. */
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        const station_nav_t *nav = &s_station_nav[s];
        if (!nav->valid || nav->node_count == 0) continue;
        /* Only include stations near the travel corridor */
        vec2 mid = v2_scale(v2_add(start, goal), 0.5f);
        float corridor_sq = v2_dist_sq(start, goal);
        if (v2_dist_sq(st->pos, mid) > corridor_sq + 2000.0f * 2000.0f) continue;

        /* Map: snav node index -> graph node index */
        int base = g->count;
        for (int ni = 0; ni < nav->node_count && g->count < NAV_MAX_NODES; ni++)
            g->nodes[g->count++].pos = snav_node_world_pos(st, &nav->nodes[ni]);

        /* Translate precomputed edges into graph edge bitfield */
        for (int ei = 0; ei < nav->edge_count; ei++) {
            int ga = base + nav->edges[ei].a;
            int gb = base + nav->edges[ei].b;
            if (ga < g->count && gb < g->count)
                nav_graph_set_edge(g, ga, gb);
        }
    }

    /* Asteroid bypass waypoints: for each large asteroid that blocks
     * the direct start->goal line, add two waypoints perpendicular to
     * the travel direction, letting A* route around the obstacle. */
    vec2 sg_delta = v2_sub(goal, start);
    float sg_len = v2_len(sg_delta);
    if (sg_len > 1.0f) {
        vec2 sg_fwd = v2_scale(sg_delta, 1.0f / sg_len);
        vec2 sg_perp = v2(-sg_fwd.y, sg_fwd.x);
        for (int i = 0; i < MAX_ASTEROIDS && g->count < NAV_MAX_NODES - 2; i++) {
            const asteroid_t *a = &w->asteroids[i];
            if (!a->active || a->tier == ASTEROID_TIER_S) continue;
            if (a->radius < 30.0f) continue; /* skip small rocks */
            vec2 to_a = v2_sub(a->pos, start);
            float proj = v2_dot(to_a, sg_fwd);
            if (proj < 0.0f || proj > sg_len) continue;
            float lat = fabsf(v2_dot(to_a, sg_perp));
            float margin = a->radius + clearance * 2.0f;
            if (lat > margin) continue; /* not blocking */
            /* Add waypoints on both sides of the asteroid */
            float bypass = a->radius + clearance * 2.5f;
            g->nodes[g->count++].pos = v2_add(a->pos, v2_scale(sg_perp, bypass));
            if (g->count < NAV_MAX_NODES)
                g->nodes[g->count++].pos = v2_add(a->pos, v2_scale(sg_perp, -bypass));
        }
    }
}

/* A* search through the navigation graph. Returns true if a path was found. */
bool nav_find_path(const world_t *w, vec2 start, vec2 goal,
                   float clearance, nav_path_t *out) {
    out->count = 0;
    out->current = 0;
    out->age = 0.0f;
    out->goal = goal;

    /* Fast path: direct line is clear AND no station is between
     * start and goal -> skip the graph entirely. We check for nearby
     * stations so the A* always kicks in when approaching a station. */
    if (nav_line_clear(w, start, goal, clearance)) {
        bool station_nearby = false;
        vec2 mid = v2_scale(v2_add(start, goal), 0.5f);
        float half_len = sqrtf(v2_dist_sq(start, goal)) * 0.5f + 400.0f;
        for (int s = 0; s < MAX_STATIONS; s++) {
            const station_t *st = &w->stations[s];
            if (!station_collides(st)) continue;
            if (v2_dist_sq(st->pos, mid) < half_len * half_len) {
                station_nearby = true; break;
            }
        }
        if (!station_nearby) return true;
    }

    nav_graph_t graph;
    nav_build_graph(w, start, goal, clearance, &graph);
    int n = graph.count;
    if (n < 2) return false;

    float g_cost[NAV_MAX_NODES];
    float f_cost[NAV_MAX_NODES];
    int   came_from[NAV_MAX_NODES];
    bool  closed[NAV_MAX_NODES];
    bool  in_open[NAV_MAX_NODES];
    for (int i = 0; i < n; i++) {
        g_cost[i] = 1e18f;
        f_cost[i] = 1e18f;
        came_from[i] = -1;
        closed[i] = false;
        in_open[i] = false;
    }
    g_cost[0] = 0.0f;
    f_cost[0] = v2_dist_sq(start, goal); /* use dist_sq as heuristic for speed */
    in_open[0] = true;

    for (int iter = 0; iter < n * n; iter++) {
        /* Find open node with lowest f_cost */
        int cur = -1;
        float best_f = 1e18f;
        for (int i = 0; i < n; i++) {
            if (in_open[i] && !closed[i] && f_cost[i] < best_f) {
                best_f = f_cost[i]; cur = i;
            }
        }
        if (cur < 0) break; /* no path */
        if (cur == 1) {     /* reached goal */
            /* Reconstruct path */
            int path[NAV_MAX_NODES];
            int path_len = 0;
            int c = 1;
            while (c >= 0 && path_len < NAV_MAX_NODES) {
                path[path_len++] = c;
                c = came_from[c];
            }
            /* Reverse and skip node 0 (start) and node 1 (goal is final target) */
            out->count = 0;
            for (int i = path_len - 2; i >= 0; i--) {
                if (out->count < NAV_MAX_PATH)
                    out->waypoints[out->count++] = graph.nodes[path[i]].pos;
            }
            return true;
        }

        closed[cur] = true;
        in_open[cur] = false;

        /* Expand neighbors: use precomputed edges when available,
         * fall back to nav_line_clear for dynamic connections. */
        for (int nb = 0; nb < n; nb++) {
            if (nb == cur || closed[nb]) continue;
            if (!nav_graph_has_edge(&graph, cur, nb) &&
                !nav_line_clear(w, graph.nodes[cur].pos, graph.nodes[nb].pos, clearance))
                continue;
            float edge = sqrtf(v2_dist_sq(graph.nodes[cur].pos, graph.nodes[nb].pos));
            float new_g = g_cost[cur] + edge;
            if (new_g < g_cost[nb]) {
                g_cost[nb] = new_g;
                f_cost[nb] = new_g + sqrtf(v2_dist_sq(graph.nodes[nb].pos, goal));
                came_from[nb] = cur;
                in_open[nb] = true;
            }
        }
    }
    return false; /* no path found */
}

/* Advance along a computed path, returning the next waypoint to steer toward. */
vec2 nav_next_waypoint(nav_path_t *path, vec2 ship_pos, vec2 final_target, float dt) {
    path->age += dt;
    if (path->count == 0 || path->current >= path->count)
        return final_target;
    /* Advance when within 80u of the current waypoint */
    while (path->current < path->count &&
           v2_dist_sq(ship_pos, path->waypoints[path->current]) < 80.0f * 80.0f) {
        path->current++;
    }
    if (path->current >= path->count) return final_target;
    return path->waypoints[path->current];
}

/* ================================================================== */

int nav_compute_path(const world_t *w, vec2 start, vec2 goal, float clearance,
                     vec2 *out_waypoints, int max_count) {
    nav_path_t p;
    nav_find_path(w, start, goal, clearance, &p);
    int n = p.count < max_count ? p.count : max_count;
    for (int i = 0; i < n; i++) out_waypoints[i] = p.waypoints[i];
    return n;
}

int nav_get_player_path(int player_id, vec2 *out_waypoints, int max_count, int *out_current) {
    if (player_id < 0 || player_id >= MAX_PLAYERS) return 0;
    const nav_path_t *p = &s_player_paths[player_id];
    int n = p->count < max_count ? p->count : max_count;
    for (int i = 0; i < n; i++) out_waypoints[i] = p->waypoints[i];
    if (out_current) *out_current = p->current;
    return n;
}

/* ================================================================== */
/* Shared flight helpers                                               */
/* ================================================================== */

vec2 nav_follow_path(const world_t *w, nav_path_t *path,
                     vec2 ship_pos, vec2 destination,
                     float clearance, float dt) {
    bool dest_changed = v2_dist_sq(path->goal, destination) > 100.0f * 100.0f;
    if (dest_changed || path->age > 5.0f || (path->count == 0 && path->age > 0.5f)) {
        nav_find_path(w, ship_pos, destination, clearance, path);
    }
    return nav_next_waypoint(path, ship_pos, destination, dt);
}

nav_steer_t nav_steer_toward_waypoint(nav_path_t *path, vec2 ship_pos,
                                       vec2 destination, float dt) {
    (void)dt;
    vec2 wp = (path->count > 0 && path->current < path->count)
        ? path->waypoints[path->current]
        : destination;
    vec2 to_wp = v2_sub(wp, ship_pos);
    float dist = v2_len(to_wp);
    nav_steer_t out;
    out.desired_heading = (dist > 0.001f)
        ? atan2f(to_wp.y, to_wp.x)
        : 0.0f;
    out.wp_dist = dist;
    out.at_intermediate = (path->current < path->count);
    return out;
}

float nav_approach_speed(float dist, float max_speed) {
    const float decel = 150.0f;
    float v = sqrtf(2.0f * decel * fmaxf(dist, 0.0f));
    if (v > max_speed) v = max_speed;
    if (v < 30.0f && dist > 5.0f) v = 30.0f;
    return v;
}

float nav_speed_control(float current_speed, float target_speed) {
    if (current_speed > target_speed * 1.10f) return -1.0f;
    if (current_speed < target_speed * 0.85f) return  1.0f;
    return 0.0f;
}

float nav_forward_clearance(const world_t *w, vec2 pos, vec2 vel,
                            float ship_radius, float heading) {
    vec2 fwd = v2(cosf(heading), sinf(heading));
    float speed = v2_len(vel);
    /* Lookahead: 1.5s of travel, min 100u, max 500u */
    float lookahead = fmaxf(100.0f, fminf(speed * 1.5f, 500.0f));
    float worst = 1.0f; /* 1.0 = fully clear */

    /* Use spatial grid for efficiency */
    const spatial_grid_t *g = &w->asteroid_grid;
    float margin = 150.0f + ship_radius;
    vec2 probe_end = v2_add(pos, v2_scale(fwd, lookahead));
    float min_x = fminf(pos.x, probe_end.x) - margin;
    float min_y = fminf(pos.y, probe_end.y) - margin;
    float max_x = fmaxf(pos.x, probe_end.x) + margin;
    float max_y = fmaxf(pos.y, probe_end.y) + margin;
    int cx0, cy0, cx1, cy1;
    spatial_grid_cell(g, v2(min_x, min_y), &cx0, &cy0);
    spatial_grid_cell(g, v2(max_x, max_y), &cx1, &cy1);
    uint64_t checked[MAX_ASTEROIDS / 64 + 1];
    memset(checked, 0, sizeof(checked));

    for (int cy = cy0; cy <= cy1; cy++) {
        for (int cx = cx0; cx <= cx1; cx++) {
            const spatial_cell_t *cell = spatial_grid_lookup(g, cx, cy);
            if (!cell) continue;
            for (int ci = 0; ci < cell->count; ci++) {
                int idx = cell->indices[ci];
                if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
                int word = idx / 64, bit = idx % 64;
                if (checked[word] & (1ULL << bit)) continue;
                checked[word] |= (1ULL << bit);
                const asteroid_t *a = &w->asteroids[idx];
                if (!a->active || a->tier == ASTEROID_TIER_S) continue;
                vec2 to_a = v2_sub(a->pos, pos);
                float fd = v2_dot(to_a, fwd);
                if (fd < -a->radius || fd > lookahead) continue;
                vec2 perp = v2(-fwd.y, fwd.x);
                float lat = fabsf(v2_dot(to_a, perp));
                float clearance = a->radius + ship_radius + 20.0f;
                if (lat < clearance) {
                    /* How urgent: 1.0 at ship, 0.0 at lookahead edge */
                    float urgency = 1.0f - (fd / lookahead);
                    float scale = 1.0f - urgency;
                    if (scale < worst) worst = scale;
                }
            }
        }
    }
    /* Check station structures — core, ring walls, and modules */
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_collides(st)) continue;
        float st_dsq = v2_dist_sq(st->pos, pos);
        float max_r = 600.0f;
        if (st_dsq > (lookahead + max_r) * (lookahead + max_r)) continue;

        /* Core circle */
        if (st->radius > 0.0f) {
            vec2 to_c = v2_sub(st->pos, pos);
            float fd = v2_dot(to_c, fwd);
            if (fd > -st->radius && fd < lookahead + st->radius) {
                vec2 perp_v = v2(-fwd.y, fwd.x);
                float lat = fabsf(v2_dot(to_c, perp_v));
                float core_clear = st->radius + ship_radius + 30.0f;
                if (lat < core_clear && fd > 0.0f) {
                    float urgency = 1.0f - clampf(fd / lookahead, 0.0f, 1.0f);
                    float scale = 1.0f - urgency;
                    if (scale < worst) worst = scale;
                }
            }
        }

        /* Module positions on rings */
        for (int mi = 0; mi < st->module_count; mi++) {
            if (st->modules[mi].scaffold) continue;
            vec2 mp = module_world_pos_ring(st, st->modules[mi].ring, st->modules[mi].slot);
            vec2 to_m = v2_sub(mp, pos);
            float fd = v2_dot(to_m, fwd);
            if (fd < -STATION_MODULE_COL_RADIUS || fd > lookahead) continue;
            vec2 perp_v = v2(-fwd.y, fwd.x);
            float lat = fabsf(v2_dot(to_m, perp_v));
            float mod_clear = STATION_MODULE_COL_RADIUS + ship_radius + 15.0f;
            if (lat < mod_clear && fd > 0.0f) {
                float urgency = 1.0f - clampf(fd / lookahead, 0.0f, 1.0f);
                float scale = 1.0f - urgency;
                if (scale < worst) worst = scale;
            }
        }

        /* Ring corridor walls — simplified check: treat each ring as a
         * circle obstacle when the probe ray crosses near a corridor arc.
         * Full ray-circle intersection is expensive; instead, check if
         * the ship is heading toward a ring band and there's no dock gap. */
        for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
            bool has_modules = false;
            for (int mi = 0; mi < st->module_count; mi++)
                if (st->modules[mi].ring == ring) { has_modules = true; break; }
            if (!has_modules) continue;

            float ring_r = STATION_RING_RADIUS[ring];
            /* Quick check: is the probe ray near this ring radius? */
            vec2 to_c = v2_sub(st->pos, pos);
            float dist_to_center = sqrtf(v2_len_sq(to_c));
            /* Ship approaching ring from outside or inside? */
            float ring_dist = fabsf(dist_to_center - ring_r);
            if (ring_dist > lookahead + ship_radius + 40.0f) continue;

            /* Check angle at closest approach to ring — is there a dock? */
            float approach_ang = atan2f(-to_c.y + fwd.y * dist_to_center,
                                        -to_c.x + fwd.x * dist_to_center);
            /* Simplified: just check if the heading points toward the ring
             * and there's no dock at that angle */
            float ship_ang = atan2f(pos.y - st->pos.y, pos.x - st->pos.x);
            bool dock_gap = false;
            for (int mi = 0; mi < st->module_count; mi++) {
                if (st->modules[mi].ring != ring) continue;
                if (st->modules[mi].type != MODULE_DOCK) continue;
                if (st->modules[mi].scaffold) continue;
                float dock_ang = module_angle_ring(st, ring, st->modules[mi].slot);
                int slots_n = STATION_RING_SLOTS[ring];
                float slot_arc = (slots_n > 0) ? (TWO_PI_F / (float)slots_n) : TWO_PI_F;
                if (fabsf(wrap_angle(ship_ang - dock_ang)) < slot_arc * 0.4f) {
                    dock_gap = true; break;
                }
            }
            (void)approach_ang;

            if (!dock_gap && ring_dist < ship_radius + 40.0f) {
                float urgency = 1.0f - clampf(ring_dist / (ship_radius + 80.0f), 0.0f, 1.0f);
                float scale = 1.0f - urgency;
                if (scale < worst) worst = scale;
            }
        }
    }

    return worst;
}
