/*
 * world_draw.c -- World-space rendering: camera/frustum, VFX, ships,
 * asteroids, stations, and networked players.
 * Split from main.c for Phase 3 refactoring.
 */
#include "client.h"
#include "world_draw.h"
#include "render.h"
#include "npc.h"
#include "net.h"
#include "net_sync.h"
#include "contract_objective.h"
#include "station_voice.h"
#include "signal_model.h"
#include "manifest.h"
#include "palette.h"
#include "station_palette.h"
#include "sim_mining.h"
#include "sim_ship.h"
#include "tractor.h"
#include "npc_radio.h"
#include <stddef.h>  /* ptrdiff_t for station index */
#include <stdlib.h>

#define sgl_c4f render_color4f

#define HAIL_PING_DURATION   1.50f   /* ring sweep - gentle, not a shockwave */
#define HAIL_PING_LIFECYCLE  8.00f   /* widen + very long drift back */
#define HAIL_PING_PEAK_ZOOM  1.18f   /* half-extent multiplier - subtle */
#define HAIL_PING_IN_END     0.10f   /* lifecycle frac where widen finishes (~0.8s) */
#define HAIL_PING_HOLD_END   0.20f   /* lifecycle frac where slow zoom-back starts */
#define HAIL_SCAN_ASTEROID_TAG_LIMIT 32
#define HAIL_SCAN_REVEAL_SOFTNESS 120.0f
#define HAIL_CONVERSATION_LINE_DURATION 3.4f
#define ASTEROID_FRACTURE_DRIFT_SEC 0.62f
#define ASTEROID_FAULT_START_RATIO 0.58f
#define ASTEROID_LIGHT_LEAK_RATIO 0.20f
#define THROW_PREVIEW_MAX_LEN 132.0f
#define THROW_PREVIEW_HOT_RANGE 280.0f
#define THROW_LOCK_MAX_RANGE 900.0f
#define THROW_LOCK_MIN_HOTNESS 0.08f

/* Mirror server/game_sim.c's release floor so the throw preview shows
 * the actual slingshot release velocity instead of a generic aim line. */
#define ROCK_THROW_BASE_SPEED 40.0f

/* --- Frustum culling: skip objects entirely off-screen --- */
static float g_cam_left, g_cam_right, g_cam_top, g_cam_bottom;
static float g_cam_half_w; /* cached for LOD calculations */

static float ping_ease_out(float t);

void set_camera_bounds(vec2 camera, float half_w, float half_h) {
    g_cam_left   = camera.x - half_w;
    g_cam_right  = camera.x + half_w;
    g_cam_top    = camera.y - half_h;
    g_cam_bottom = camera.y + half_h;
    g_cam_half_w = half_w;
}

bool on_screen(float x, float y, float radius) {
    return x + radius > g_cam_left  && x - radius < g_cam_right &&
           y + radius > g_cam_top   && y - radius < g_cam_bottom;
}

float cam_left(void)   { return g_cam_left; }
float cam_right(void)  { return g_cam_right; }
float cam_top(void)    { return g_cam_top; }
float cam_bottom(void) { return g_cam_bottom; }

/* --- LOD: reduce asteroid segments when small on screen --- */
int lod_segments(int base_segments, float radius) {
    float screen_ratio = radius / g_cam_half_w;
    if (screen_ratio < 0.005f) return 6;
    if (screen_ratio < 0.015f) return base_segments / 2;
    if (screen_ratio < 0.03f)  return (base_segments * 3) / 4;
    return base_segments;
}

/* Float-RGB wrapper of the canonical mining_grade_rgb_f palette (defined
 * alongside the grade enum in shared/mining.h) for sokol_gl callers. */
void grade_tint(uint8_t grade, float *r, float *g, float *b) {
    mining_grade_rgb_f((mining_grade_t)grade, r, g, b);
}

static bool hail_scan_active(void) {
    return g.hail_ping_timer > 0.0f &&
           g.hail_ping_timer <= HAIL_PING_LIFECYCLE;
}

static float hail_scan_range(void) {
    return (g.hail_ping_range > 0.0f) ? g.hail_ping_range : 1500.0f;
}

static float hail_scan_wave_radius(void) {
    if (!hail_scan_active()) return 0.0f;
    float t = clampf(g.hail_ping_timer / HAIL_PING_DURATION, 0.0f, 1.0f);
    return hail_scan_range() * ping_ease_out(t);
}

static float hail_scan_reveal_alpha(vec2 pos) {
    if (!hail_scan_active()) return 0.0f;

    float range = hail_scan_range();
    float dist_sq = v2_dist_sq(pos, g.hail_ping_origin);
    if (dist_sq > range * range) return 0.0f;

    float dist = sqrtf(dist_sq);
    float wave = hail_scan_wave_radius();
    if (dist > wave) return 0.0f;

    float reveal = clampf((wave - dist) / HAIL_SCAN_REVEAL_SOFTNESS,
                          0.0f, 1.0f);
    float life_left = HAIL_PING_LIFECYCLE - g.hail_ping_timer;
    float fade = clampf(life_left / 0.45f, 0.0f, 1.0f);
    return reveal * fade;
}

float world_signal_visual_saturation_at(vec2 pos, void *user) {
    (void)user;
    float base = g.signal_visual_saturation_initialized
               ? g.signal_visual_saturation : 1.0f;
    float reveal = hail_scan_reveal_alpha(pos);
    return clampf(base + (1.0f - base) * reveal, 0.0f, 1.0f);
}

float world_signal_visual_base_saturation(void) {
    float base = g.signal_visual_saturation_initialized
               ? g.signal_visual_saturation : 1.0f;
    if (!hail_scan_active()) return base;

    float n = clampf(g.hail_ping_timer / HAIL_PING_LIFECYCLE, 0.0f, 1.0f);
    float pulse = (1.0f - n) * 0.22f;
    return clampf(base + (1.0f - base) * pulse, 0.0f, 1.0f);
}

float world_signal_visual_cue_saturation(void) {
    return signal_visual_cue_saturation(world_signal_visual_base_saturation());
}

float world_signal_visual_player_saturation(void) {
    return signal_visual_player_saturation(world_signal_visual_base_saturation());
}

static float world_signal_visual_enter_cue(void) {
    float prev = render_min_saturation();
    float cue = world_signal_visual_cue_saturation();
    if (cue > prev) render_set_min_saturation(cue);
    return prev;
}

static float world_signal_visual_enter_player_ship(void) {
    float prev = render_min_saturation();
    float player = world_signal_visual_player_saturation();
    if (player > prev) render_set_min_saturation(player);
    return prev;
}

static void world_signal_visual_leave_cue(float prev) {
    render_set_min_saturation(prev);
}

static bool world_hash32_is_zero(const uint8_t hash[32]) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) return false;
    }
    return true;
}

static void world_hash_short_label(const uint8_t hash[32], char out[8]) {
    if (!hash || world_hash32_is_zero(hash)) {
        out[0] = '\0';
        return;
    }
    mining_callsign_from_pubkey(hash, out);
}

static const uint8_t *hail_asteroid_identity_hash(const asteroid_t *a) {
    if (!a) return NULL;
    if (!world_hash32_is_zero(a->fragment_pub)) return a->fragment_pub;
    if (!world_hash32_is_zero(a->rock_pub)) return a->rock_pub;
    return NULL;
}

static void hail_asteroid_identity_label(const asteroid_t *a, char out[8]) {
    const uint8_t *hash = hail_asteroid_identity_hash(a);
    if (hash) {
        world_hash_short_label(hash, out);
        return;
    }
    if (a && a->fracture_child && !world_hash32_is_zero(a->fracture_seed)) {
        snprintf(out, 8, "pending");
        return;
    }
    out[0] = '\0';
}

static const char *world_npc_role_label(npc_role_t role) {
    switch (role) {
    case NPC_ROLE_MINER:  return "MINER";
    case NPC_ROLE_HAULER: return "HAULER";
    case NPC_ROLE_TOW:    return "TOW";
    default:              return "NPC";
    }
}

static void world_npc_scan_label(const npc_ship_t *npc, int idx,
                                 char out[32]) {
    if (!npc) {
        snprintf(out, 32, "NPC --");
        return;
    }
    if (npc->session_token[0] == 'N' && npc->session_token[1] == 'P' &&
        npc->session_token[2] == 'C') {
        snprintf(out, 32, "%s N%02u", world_npc_role_label(npc->role),
                 (unsigned)npc->session_token[5]);
    } else {
        snprintf(out, 32, "%s %02d", world_npc_role_label(npc->role), idx);
    }
}

typedef struct {
    int s_tier[MAX_ASTEROIDS];
    int s_tier_count;
    int smelting[MAX_ASTEROIDS];
    int smelting_count;
} asteroid_render_lists_t;

#define ASTEROID_RENDER_MAX_VERTS 29
#define ASTEROID_RENDER_MAX_CRYSTAL_SPIKES 5

typedef struct {
    int index;
    int segments;
    int crystal_spikes;
    bool crystal;
    bool target;
    bool ineffective;
    vec2 center;
    float fracture_birth_t;
    float fault_t;
    float light_leak_t;
    float progress_ratio;
    float body_r, body_g, body_b;
    float rim_r, rim_g, rim_b, rim_a;
    vec2 outline[ASTEROID_RENDER_MAX_VERTS];
    vec2 crystal_corners[ASTEROID_RENDER_MAX_CRYSTAL_SPIKES][4];
} asteroid_draw_item_t;

typedef struct {
    bool valid;
    asteroid_draw_item_t items[MAX_ASTEROIDS];
    int count;
} asteroid_draw_frame_t;

static asteroid_render_lists_t g_asteroid_render_lists;
static bool g_asteroid_render_lists_valid = false;
static asteroid_draw_frame_t g_asteroid_draw_frame;

void world_draw_begin_frame(void) {
    g_asteroid_render_lists_valid = false;
    g_asteroid_draw_frame.valid = false;
}

static const asteroid_render_lists_t *asteroid_render_lists(void) {
    if (g_asteroid_render_lists_valid) return &g_asteroid_render_lists;

    g_asteroid_render_lists.s_tier_count = 0;
    g_asteroid_render_lists.smelting_count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &g.world.asteroids[i];
        if (!a->active) continue;
        if (a->tier == ASTEROID_TIER_S) {
            g_asteroid_render_lists.s_tier[g_asteroid_render_lists.s_tier_count++] = i;
        }
        if (a->smelt_progress >= 0.05f) {
            g_asteroid_render_lists.smelting[g_asteroid_render_lists.smelting_count++] = i;
        }
    }
    g_asteroid_render_lists_valid = true;
    return &g_asteroid_render_lists;
}

float asteroid_profile(const asteroid_t* asteroid, float angle) {
    /* Polar-profile silhouettes for ferrite (lumpy round) and cuprite
     * (six-sided hex crystal). Crystal-ore asteroids do NOT use this
     * path — see draw_crystal_asteroid_*; they're built from explicit
     * rotated rectangles because real straight crystal edges can't
     * survive a per-angle radius sample. */
    float profile;
    switch (asteroid->commodity) {
    case COMMODITY_CUPRITE_ORE: {
        /* Hex crystal — six dominant lobes, light high-freq texture. */
        float hex     = sinf(6.0f * angle + asteroid->seed);
        float texture = sinf(angle * 11.0f + asteroid->seed * 1.31f) * 0.025f;
        profile = 1.0f + hex * 0.12f + texture;
        break;
    }
    case COMMODITY_FERRITE_ORE:
    default: {
        /* Lumpy round — original profile, retained for ferrite + any
         * non-ore (debris, fragments without a commodity tag). */
        float bump1 = sinf(angle * 3.0f + asteroid->seed);
        float bump2 = sinf(angle * 7.0f + asteroid->seed * 1.71f);
        float bump3 = cosf(angle * 5.0f + asteroid->seed * 0.63f);
        profile = 1.0f + (bump1 * 0.08f) + (bump2 * 0.06f) + (bump3 * 0.04f);
        break;
    }
    }
    return asteroid->radius * profile;
}

/* ------------------------------------------------------------------ */
/* Crystal asteroids — built from explicit rectangles                  */
/* ------------------------------------------------------------------ */

int crystal_spike_count(const asteroid_t *a) {
    /* Larger rocks read as 5-spike druzes, smaller fragments as 3.
     * S-tier fragments use 3 too — a single bar would look pasted-on
     * next to its 3-spike parents. */
    switch (a->tier) {
    case ASTEROID_TIER_XXL:
    case ASTEROID_TIER_XL:
    case ASTEROID_TIER_L:
        return 5;
    case ASTEROID_TIER_M:
    case ASTEROID_TIER_S:
    default:
        return 3;
    }
}

static float ease_out_cubic(float t) {
    t = clampf(t, 0.0f, 1.0f);
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

static vec2 asteroid_fault_axis(const asteroid_t *a) {
    float angle = a->rotation + a->seed * 0.37f;
    return v2_from_angle(angle);
}

static vec2 asteroid_fracture_axis(const asteroid_t *a) {
    vec2 dir = v2_norm(a->vel);
    if (v2_len_sq(a->vel) < 4.0f) {
        dir = asteroid_fault_axis(a);
    }
    return dir;
}

static vec2 asteroid_fracture_render_center(const asteroid_t *a,
                                            float *out_birth_t) {
    *out_birth_t = 1.0f;
    if (!a->fracture_child || a->age >= ASTEROID_FRACTURE_DRIFT_SEC) {
        return a->pos;
    }

    float t = clampf(a->age / ASTEROID_FRACTURE_DRIFT_SEC, 0.0f, 1.0f);
    float eased = ease_out_cubic(t);
    float birth_offset = a->radius * 2.20f + 18.0f;
    vec2 dir = asteroid_fracture_axis(a);
    *out_birth_t = eased;
    return v2_sub(a->pos, v2_scale(dir, birth_offset * (1.0f - eased)));
}

static float asteroid_fault_progress(float ratio) {
    if (ratio >= ASTEROID_FAULT_START_RATIO) return 0.0f;
    return clampf((ASTEROID_FAULT_START_RATIO - ratio) /
                  ASTEROID_FAULT_START_RATIO, 0.0f, 1.0f);
}

static float asteroid_light_leak_progress(float ratio) {
    if (ratio >= ASTEROID_LIGHT_LEAK_RATIO) return 0.0f;
    return clampf((ASTEROID_LIGHT_LEAK_RATIO - ratio) /
                  ASTEROID_LIGHT_LEAK_RATIO, 0.0f, 1.0f);
}

/* Build the four world-space corners of one crystal spike (a rotated
 * rectangle anchored at the asteroid center, extending outward by
 * `length` along `dir`, `width` thick). Out-corners are CCW from the
 * inner-left so the caller can fan-triangulate as (0,1,2)+(0,2,3). */
static void crystal_spike_corners(const asteroid_t *a, vec2 center, int i, int n,
                                   float out_x[4], float out_y[4])
{
    /* Spread spikes around the full circle but perturb each one by a
     * seed-derived offset so they're never perfectly symmetric. */
    float spacing = TWO_PI_F / (float)n;
    float dir = a->rotation + (float)i * spacing
              + a->seed * 0.5f
              + sinf(a->seed * 1.7f + (float)i * 2.13f) * 0.18f;

    /* Per-spike length + width jitter — keeps individual spikes
     * looking like distinct broken pieces of one larger crystal.
     * Width is set wide enough that adjacent spikes overlap near
     * the asteroid core, so the silhouette reads as a tightly
     * packed cluster (no big furrows between bars) instead of a
     * thin starburst. */
    float length = a->radius * (1.00f + 0.15f * sinf(a->seed + (float)i * 1.71f));
    float width  = a->radius * (0.42f + 0.10f * cosf(a->seed * 1.3f + (float)i * 1.31f));

    float c = cosf(dir), s = sinf(dir);
    /* Local frame: x along the spike axis (0..length), y perpendicular
     * (-width..+width). Order: inner-left, tip-left, tip-right,
     * inner-right. CCW. */
    float lx[4] = { 0.0f,    length, length,  0.0f   };
    float ly[4] = { -width,  -width, +width,  +width };
    for (int k = 0; k < 4; k++) {
        out_x[k] = center.x + lx[k] * c - ly[k] * s;
        out_y[k] = center.y + lx[k] * s + ly[k] * c;
    }
}

static int asteroid_render_base_segments(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XXL: return 28;
        case ASTEROID_TIER_XL:  return 22;
        case ASTEROID_TIER_L:   return 18;
        case ASTEROID_TIER_M:   return 15;
        case ASTEROID_TIER_S:   return 12;
        default:                return 18;
    }
}

static void asteroid_draw_item_build_boundary(asteroid_draw_item_t *item,
                                              const asteroid_t *a) {
    if (item->crystal) {
        item->crystal_spikes = crystal_spike_count(a);
        if (item->crystal_spikes > ASTEROID_RENDER_MAX_CRYSTAL_SPIKES) {
            item->crystal_spikes = ASTEROID_RENDER_MAX_CRYSTAL_SPIKES;
        }
        for (int i = 0; i < item->crystal_spikes; i++) {
            float wx[4], wy[4];
            crystal_spike_corners(a, item->center, i, item->crystal_spikes, wx, wy);
            for (int k = 0; k < 4; k++) {
                item->crystal_corners[i][k] = v2(wx[k], wy[k]);
            }
        }
        return;
    }

    int segments = lod_segments(asteroid_render_base_segments(a->tier), a->radius);
    if (segments > ASTEROID_RENDER_MAX_VERTS - 1) segments = ASTEROID_RENDER_MAX_VERTS - 1;
    if (segments < 3) segments = 3;
    item->segments = segments;

    float step = TWO_PI_F / (float)segments;
    for (int j = 0; j <= segments; j++) {
        float angle = a->rotation + (float)j * step;
        float radius = asteroid_profile(a, angle);
        item->outline[j] = v2(item->center.x + cosf(angle) * radius,
                              item->center.y + sinf(angle) * radius);
    }
}

static const asteroid_draw_frame_t *asteroid_draw_frame(void) {
    if (g_asteroid_draw_frame.valid) return &g_asteroid_draw_frame;

    g_asteroid_draw_frame.count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &g.world.asteroids[i];
        if (!a->active) continue;
        if (g_asteroid_draw_frame.count >= MAX_ASTEROIDS) break;

        asteroid_draw_item_t *item =
            &g_asteroid_draw_frame.items[g_asteroid_draw_frame.count++];
        item->index = i;
        item->segments = 0;
        item->crystal_spikes = 0;
        item->crystal = a->commodity == COMMODITY_CRYSTAL_ORE;
        item->target = (i == LOCAL_PLAYER.hover_asteroid);
        item->ineffective = item->target && LOCAL_PLAYER.beam_ineffective;
        item->center = asteroid_fracture_render_center(a, &item->fracture_birth_t);
        if (!on_screen(item->center.x, item->center.y, a->radius + 32.0f)) {
            g_asteroid_draw_frame.count--;
            continue;
        }
        item->progress_ratio = asteroid_progress_ratio(a);
        item->fault_t = (a->tier != ASTEROID_TIER_S && !a->fracture_child)
            ? asteroid_fault_progress(item->progress_ratio)
            : 0.0f;
        item->light_leak_t = (a->tier != ASTEROID_TIER_S && !a->fracture_child)
            ? asteroid_light_leak_progress(item->progress_ratio)
            : 0.0f;

        asteroid_body_color(a->tier, a->commodity, item->progress_ratio,
                            &item->body_r, &item->body_g, &item->body_b);
        if (a->smelt_progress > 0.01f) {
            float sp = a->smelt_progress;
            item->body_r = item->body_r + (1.0f - item->body_r) * sp * 0.8f;
            item->body_g = item->body_g + (0.6f - item->body_g) * sp * 0.6f;
            item->body_b = item->body_b + (0.2f - item->body_b) * sp * 0.3f;
        }
        if (a->phase == ASTEROID_PHASE_GAS_RICH && a->tier != ASTEROID_TIER_S) {
            item->body_r = lerpf(item->body_r, 0.18f, 0.22f);
            item->body_g = lerpf(item->body_g, 0.68f, 0.28f);
            item->body_b = lerpf(item->body_b, 0.72f, 0.28f);
        }

        float base_r, base_g, base_b;
        asteroid_body_color(a->tier, a->commodity, item->progress_ratio,
                            &base_r, &base_g, &base_b);
        item->rim_r = item->target ? (item->ineffective ? 1.0f : 0.45f) : (base_r * 0.85f);
        item->rim_g = item->target ? (item->ineffective ? 0.15f : 0.94f) : (base_g * 0.95f);
        item->rim_b = item->target ? (item->ineffective ? 0.10f : 1.0f) : fminf(1.0f, base_b * 1.2f);
        item->rim_a = item->target ? 1.0f : 0.8f;
        asteroid_draw_item_build_boundary(item, a);
    }

    g_asteroid_draw_frame.valid = true;
    return &g_asteroid_draw_frame;
}

static void draw_asteroid_veins(const asteroid_t *a,
                                const asteroid_draw_item_t *item) {
    float vr, vg, vb;
    commodity_material_tint(a->commodity, &vr, &vg, &vb);
    vr = lerpf(item->body_r, vr, 0.72f);
    vg = lerpf(item->body_g, vg, 0.72f);
    vb = lerpf(item->body_b, vb, 0.72f);

    sgl_c4f(vr, vg, vb, item->target ? 0.48f : 0.30f);
    sgl_begin_lines();
    if (item->crystal) {
        for (int si = 0; si < item->crystal_spikes; si++) {
            const vec2 *c = item->crystal_corners[si];
            vec2 root = v2_scale(v2_add(c[0], c[3]), 0.5f);
            vec2 tip = v2_scale(v2_add(c[1], c[2]), 0.5f);
            vec2 p0 = v2_add(root, v2_scale(v2_sub(tip, root), 0.18f));
            vec2 p1 = v2_add(root, v2_scale(v2_sub(tip, root), 0.82f));
            sgl_v2f(p0.x, p0.y);
            sgl_v2f(p1.x, p1.y);
        }
    } else {
        int veins = (a->tier == ASTEROID_TIER_S) ? 2 : 3;
        for (int k = 0; k < veins; k++) {
            float phase = a->seed * (0.47f + 0.13f * (float)k);
            float angle = a->rotation + phase + (float)k * 2.17f;
            float bend = 0.24f * sinf(a->seed * 1.9f + (float)k);
            float outer = asteroid_profile(a, angle) * 0.78f;
            float inner = a->radius * (0.16f + 0.06f * (float)k);
            vec2 p0 = v2(item->center.x + cosf(angle + bend) * inner,
                         item->center.y + sinf(angle + bend) * inner);
            vec2 p1 = v2(item->center.x + cosf(angle - bend * 0.5f) * outer,
                         item->center.y + sinf(angle - bend * 0.5f) * outer);
            sgl_v2f(p0.x, p0.y);
            sgl_v2f(p1.x, p1.y);
        }
    }
    sgl_end();
}

static void draw_asteroid_faults(const asteroid_t *a,
                                 const asteroid_draw_item_t *item) {
    if (item->fault_t <= 0.001f) return;

    float fault_t = ease_out_cubic(item->fault_t);
    float leak_t = ease_out_cubic(item->light_leak_t);
    vec2 axis = asteroid_fault_axis(a);
    vec2 perp = v2_perp(axis);
    float half = a->radius * lerpf(0.42f, 0.86f, fault_t);
    float spread = lerpf(1.0f, 4.0f, fault_t);
    float alpha = lerpf(0.18f, 0.72f, fault_t);
    float wr, wg, wb;
    commodity_material_tint(a->commodity, &wr, &wg, &wb);

    sgl_begin_lines();
    for (int k = -1; k <= 1; k++) {
        float offs = (float)k * spread;
        vec2 shift = v2_scale(perp, offs);
        vec2 p0 = v2_add(v2_sub(item->center, v2_scale(axis, half)), shift);
        vec2 p1 = v2_add(v2_add(item->center, v2_scale(axis, half)), shift);
        sgl_c4f(lerpf(item->body_r * 0.55f, wr * 1.3f, leak_t),
                lerpf(item->body_g * 0.55f, wg * 1.3f, leak_t),
                lerpf(item->body_b * 0.55f, wb * 1.3f, leak_t),
                alpha * (k == 0 ? 1.0f : 0.45f));
        sgl_v2f(p0.x, p0.y);
        sgl_v2f(p1.x, p1.y);
    }

    for (int b = 0; b < 2; b++) {
        float side = (b == 0) ? -1.0f : 1.0f;
        float branch_phase = a->seed * (0.21f + 0.11f * (float)b);
        vec2 root = v2_add(item->center,
                           v2_scale(axis, side * half * (0.16f + 0.12f * sinf(branch_phase))));
        vec2 branch_dir = v2_norm(v2_add(v2_scale(axis, side * 0.55f),
                                         v2_scale(perp, (b == 0) ? 0.82f : -0.82f)));
        vec2 tip = v2_add(root, v2_scale(branch_dir, half * lerpf(0.22f, 0.48f, fault_t)));
        sgl_c4f(wr * 0.86f, wg * 0.86f, wb * 0.86f, alpha * 0.55f);
        sgl_v2f(root.x, root.y);
        sgl_v2f(tip.x, tip.y);
    }
    sgl_end();

    if (leak_t > 0.001f) {
        float pulse = 0.82f + 0.18f * sinf(g.world.time * 16.0f + a->seed);
        draw_circle_filled(item->center, a->radius * lerpf(0.08f, 0.18f, leak_t),
                           10, 1.0f, 0.82f, 0.34f, 0.22f * leak_t * pulse);
    }
}

static void draw_asteroid_fracture_wake(const asteroid_t *a,
                                        const asteroid_draw_item_t *item) {
    if (!a->fracture_child || item->fracture_birth_t >= 0.999f) return;

    float t = item->fracture_birth_t;
    float fade = 1.0f - t;
    vec2 axis = asteroid_fracture_axis(a);
    vec2 perp = v2_perp(axis);
    float half = a->radius * lerpf(0.32f, 0.92f, t);
    vec2 p0 = v2_sub(item->center, v2_scale(perp, half));
    vec2 p1 = v2_add(item->center, v2_scale(perp, half));
    draw_segment(p0, p1, 1.0f, 0.82f, 0.34f, 0.58f * fade);
    draw_segment(v2_sub(p0, v2_scale(axis, a->radius * 0.22f)),
                 v2_add(p1, v2_scale(axis, a->radius * 0.22f)),
                 0.22f, 0.86f, 0.78f, 0.25f * fade);
    draw_circle_filled(item->center, a->radius * lerpf(0.10f, 0.24f, fade),
                       10, 1.0f, 0.76f, 0.28f, 0.14f * fade);
}

void draw_asteroids(void) {
    const asteroid_draw_frame_t *frame = asteroid_draw_frame();

    sgl_begin_triangles();
    for (int i = 0; i < frame->count; i++) {
        const asteroid_draw_item_t *item = &frame->items[i];

        sgl_c4f(item->body_r, item->body_g, item->body_b, 1.0f);
        if (item->crystal) {
            for (int si = 0; si < item->crystal_spikes; si++) {
                const vec2 *c = item->crystal_corners[si];
                sgl_v2f(c[0].x, c[0].y); sgl_v2f(c[1].x, c[1].y); sgl_v2f(c[2].x, c[2].y);
                sgl_v2f(c[0].x, c[0].y); sgl_v2f(c[2].x, c[2].y); sgl_v2f(c[3].x, c[3].y);
            }
            continue;
        }

        for (int j = 1; j <= item->segments; j++) {
            sgl_v2f(item->center.x, item->center.y);
            sgl_v2f(item->outline[j - 1].x, item->outline[j - 1].y);
            sgl_v2f(item->outline[j].x, item->outline[j].y);
        }
    }
    sgl_end();

    for (int i = 0; i < frame->count; i++) {
        const asteroid_draw_item_t *item = &frame->items[i];
        const asteroid_t *a = &g.world.asteroids[item->index];

        if (a->phase == ASTEROID_PHASE_GAS_RICH && a->tier != ASTEROID_TIER_S) {
            float pulse = 1.0f + 0.06f * sinf(g.world.time * 2.0f + a->seed);
            draw_circle_filled(item->center, a->radius * 1.55f * pulse, 24,
                               0.10f, 0.52f, 0.58f, 0.09f);
        }

        if (item->crystal) {
            sgl_c4f(item->rim_r, item->rim_g, item->rim_b, item->rim_a);
            sgl_begin_lines();
            for (int si = 0; si < item->crystal_spikes; si++) {
                const vec2 *c = item->crystal_corners[si];
                for (int k = 0; k < 4; k++) {
                    const vec2 p0 = c[k];
                    const vec2 p1 = c[(k + 1) % 4];
                    sgl_v2f(p0.x, p0.y);
                    sgl_v2f(p1.x, p1.y);
                }
            }
            sgl_end();
        } else {
            sgl_c4f(item->rim_r, item->rim_g, item->rim_b, item->rim_a);
            sgl_begin_line_strip();
            for (int j = 0; j <= item->segments; j++) {
                sgl_v2f(item->outline[j].x, item->outline[j].y);
            }
            sgl_end();
        }
        draw_asteroid_veins(a, item);
        draw_asteroid_faults(a, item);
        draw_asteroid_fracture_wake(a, item);

        /* Glow core (the "dot"). Common and unscanned fragments keep the
         * muted commodity tint. H-scanned fine+ fragments use the original
         * grade bloom/halo treatment. M-tier always uses commodity tint
         * (no payable ore). */
        if (a->tier == ASTEROID_TIER_S) {
            uint8_t grade = (a->grade < (uint8_t)MINING_GRADE_COUNT)
                ? a->grade
                : (uint8_t)MINING_GRADE_COMMON;
            float scan_reveal = hail_scan_reveal_alpha(item->center);
            bool reveal_grade = scan_reveal > 0.01f &&
                grade > (uint8_t)MINING_GRADE_COMMON;
            if (!reveal_grade) {
                float cr, cg, cb;
                commodity_material_tint(a->commodity, &cr, &cg, &cb);
                draw_circle_filled(item->center, a->radius * lerpf(0.14f, 0.24f, item->progress_ratio), 10,
                    lerpf(0.48f, cr * 1.6f, 0.5f), lerpf(0.96f, cg * 1.6f, 0.5f),
                    lerpf(0.78f, cb * 1.6f, 0.5f), lerpf(0.35f, 0.8f, item->progress_ratio));
            } else {
                float cr, cg, cb;
                grade_tint(grade, &cr, &cg, &cb);
                float bloom = 1.10f + 0.18f * (float)(grade - 1);
                float pulse = (grade >= (uint8_t)MINING_GRADE_RATI)
                    ? (1.0f + 0.18f * sinf(g.world.time * 6.0f))
                    : 1.0f;
                float base_r = a->radius * lerpf(0.18f, 0.30f, item->progress_ratio) * bloom * pulse;
                draw_circle_filled(item->center, base_r, 12,
                    cr, cg, cb, lerpf(0.65f, 0.95f, item->progress_ratio) * scan_reveal);
                if (grade >= (uint8_t)MINING_GRADE_RARE) {
                    draw_circle_outline(item->center, base_r * 1.9f, 18,
                        cr, cg, cb, 0.45f * pulse * scan_reveal);
                }
            }
        } else if (a->tier == ASTEROID_TIER_M) {
            float cr, cg, cb;
            commodity_material_tint(a->commodity, &cr, &cg, &cb);
            draw_circle_filled(item->center, a->radius * 0.16f, 8,
                lerpf(0.36f, cr * 1.4f, 0.4f), lerpf(0.78f, cg * 1.4f, 0.4f),
                lerpf(0.98f, cb * 1.4f, 0.4f), 0.4f);
        }

        if (item->target && item->ineffective) {
            draw_circle_outline(item->center, a->radius + 12.0f, 24, 1.0f, 0.2f, 0.15f, 0.75f);
        } else if (item->target) {
            draw_circle_outline(item->center, a->radius + 12.0f, 24, 0.35f, 1.0f, 0.92f, 0.75f);
        }
    }
}

static float void_noise01(int x, int y) {
    uint32_t h = ((uint32_t)x * 0x8da6b343u) ^
                 ((uint32_t)y * 0xd8163841u) ^
                 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return (float)(h & 0xffu) / 255.0f;
}

void draw_background(vec2 camera) {
    const float grain_cell = 42.0f;
    int gx0 = (int)floorf(g_cam_left / grain_cell) - 1;
    int gx1 = (int)floorf(g_cam_right / grain_cell) + 1;
    int gy0 = (int)floorf(g_cam_top / grain_cell) - 1;
    int gy1 = (int)floorf(g_cam_bottom / grain_cell) + 1;

    sgl_begin_quads();
    for (int gy = gy0; gy <= gy1; gy++) {
        for (int gx = gx0; gx <= gx1; gx++) {
            float n = void_noise01(gx, gy);
            if (n < 0.62f) continue;
            float ox = void_noise01(gx + 193, gy - 71);
            float oy = void_noise01(gx - 47, gy + 211);
            float x0 = (float)gx * grain_cell + ox * grain_cell;
            float y0 = (float)gy * grain_cell + oy * grain_cell;
            float sz = 0.9f + 1.0f * void_noise01(gx + 17, gy + 29);
            sgl_c4f(0.030f + n * 0.020f,
                    0.032f + n * 0.018f,
                    0.052f + n * 0.030f,
                    0.22f);
            sgl_v2f(x0, y0);
            sgl_v2f(x0 + sz, y0);
            sgl_v2f(x0 + sz, y0 + sz);
            sgl_v2f(x0, y0 + sz);
        }
    }
    sgl_end();

    sgl_begin_quads();
    for (int i = 0; i < MAX_STARS; i++) {
        const star_t* star = &g.stars[i];
        vec2 parallax_pos = v2_add(star->pos, v2_scale(camera, 1.0f - star->depth));
        if (!on_screen(parallax_pos.x, parallax_pos.y, star->size * 2.0f)) continue;
        float sr, sg, sb;
        if ((i & 3) == 0) {
            PAL_UNPACK3(PAL_F_STAR_AMBER, sr, sg, sb);
        } else if ((i & 1) == 0) {
            PAL_UNPACK3(PAL_F_STAR_STEEL, sr, sg, sb);
        } else {
            PAL_UNPACK3(PAL_STAR_BASE, sr, sg, sb);
        }
        float tint = star->brightness * 0.58f;
        sgl_c4f(sr * tint, sg * tint, sb * tint, 0.46f + tint * 0.34f);
        sgl_v2f(parallax_pos.x - star->size, parallax_pos.y - star->size);
        sgl_v2f(parallax_pos.x + star->size, parallax_pos.y - star->size);
        sgl_v2f(parallax_pos.x + star->size, parallax_pos.y + star->size);
        sgl_v2f(parallax_pos.x - star->size, parallax_pos.y + star->size);
    }
    sgl_end();
}

/* ------------------------------------------------------------------ */
/* Signal border rendering — union-of-circles arc clipping            */
/* ------------------------------------------------------------------ */

typedef struct {
    float threshold;
    float r, g, b, a;
    float width;
} signal_border_band_t;

static const signal_border_band_t SIGNAL_BORDER_BANDS[] = {
    { SIGNAL_BAND_OPERATIONAL, 1.00f, 0.72f, 0.22f, 0.30f, 3.4f },
    { SIGNAL_BAND_FRINGE,     0.22f, 0.86f, 0.78f, 0.22f, 2.7f },
    { SIGNAL_BAND_FRONTIER,   0.90f, 0.95f, 1.00f, 0.18f, 2.2f },
};

enum {
    SIGNAL_BORDER_BAND_COUNT = (int)(sizeof(SIGNAL_BORDER_BANDS) / sizeof(SIGNAL_BORDER_BANDS[0])),
    SIGNAL_BORDER_SEGS = 180,
    SIGNAL_BORDER_MAX_STRIPS = SIGNAL_BORDER_BAND_COUNT * MAX_STATIONS * (SIGNAL_BORDER_SEGS + 1),
    SIGNAL_BORDER_MAX_VERTS = SIGNAL_BORDER_MAX_STRIPS * 2,
};

typedef struct {
    bool provides;
    float x, y, range;
} signal_border_source_t;

typedef struct {
    uint32_t first;
    uint32_t count;
    uint8_t band;
    float x, y, radius;
} signal_border_strip_t;

static struct {
    bool valid;
    bool trig_ready;
    signal_border_source_t sources[MAX_STATIONS];
    float sin_lut[SIGNAL_BORDER_SEGS + 1];
    float cos_lut[SIGNAL_BORDER_SEGS + 1];
    vec2 verts[SIGNAL_BORDER_MAX_VERTS];
    signal_border_strip_t strips[SIGNAL_BORDER_MAX_STRIPS];
    uint32_t vert_count;
    uint32_t strip_count;
} g_signal_border_cache;

static void signal_border_ensure_trig(void) {
    if (g_signal_border_cache.trig_ready) return;
    float step = TWO_PI_F / (float)SIGNAL_BORDER_SEGS;
    for (int i = 0; i <= SIGNAL_BORDER_SEGS; i++) {
        float a = (float)(i % SIGNAL_BORDER_SEGS) * step;
        g_signal_border_cache.sin_lut[i] = sinf(a);
        g_signal_border_cache.cos_lut[i] = cosf(a);
    }
    g_signal_border_cache.trig_ready = true;
}

static bool signal_border_sources_changed(signal_border_source_t current[MAX_STATIONS]) {
    bool changed = !g_signal_border_cache.valid;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        current[s].provides = station_provides_signal(st);
        current[s].x = current[s].provides ? st->pos.x : 0.0f;
        current[s].y = current[s].provides ? st->pos.y : 0.0f;
        current[s].range = current[s].provides ? st->signal_range : 0.0f;
        if (!g_signal_border_cache.valid) continue;
        const signal_border_source_t *prev = &g_signal_border_cache.sources[s];
        if (prev->provides != current[s].provides ||
            prev->x != current[s].x ||
            prev->y != current[s].y ||
            prev->range != current[s].range) {
            changed = true;
        }
    }
    return changed;
}

static void signal_border_finish_strip(uint8_t band, float x, float y, float radius,
                                       uint32_t first, uint32_t count) {
    if (count < 4 || g_signal_border_cache.strip_count >= SIGNAL_BORDER_MAX_STRIPS)
        return;
    signal_border_strip_t *strip =
        &g_signal_border_cache.strips[g_signal_border_cache.strip_count++];
    strip->first = first;
    strip->count = count;
    strip->band = band;
    strip->x = x;
    strip->y = y;
    strip->radius = radius;
}

static void signal_border_rebuild_cache(const signal_border_source_t sources[MAX_STATIONS]) {
    signal_border_ensure_trig();
    g_signal_border_cache.vert_count = 0;
    g_signal_border_cache.strip_count = 0;

    memcpy(g_signal_border_cache.sources, sources, sizeof(g_signal_border_cache.sources));

    /* Overlap boost for the drawn contour. Matches the server's signal
     * strength rule (server/game_sim.c:signal_strength_raw): at points
     * where N stations' ranges overlap, effective_strength = best * N
     * (capped at 3). Threshold contour extends accordingly:
     *     r_threshold = R * (1 - threshold/boost)
     *
     * Per-station boost is `1 + (other stations whose signal circles
     * intersect this one's range)`, capped at 3 — same max the sim uses.
     * This is a coarser approximation than per-angle boost but matches the
     * common case (starter triangle where all three stations mutually
     * overlap) and doesn't require solving a piecewise contour equation. */
    int station_overlap[MAX_STATIONS];
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_overlap[s] = 1;
        if (!sources[s].provides) continue;
        float r_s = sources[s].range;
        for (int o = 0; o < MAX_STATIONS; o++) {
            if (o == s) continue;
            if (!sources[o].provides) continue;
            float r_o = sources[o].range;
            float dx = sources[s].x - sources[o].x;
            float dy = sources[s].y - sources[o].y;
            float reach = r_s + r_o;
            if (dx * dx + dy * dy < reach * reach) station_overlap[s]++;
        }
        if (station_overlap[s] > 3) station_overlap[s] = 3;
    }

    for (int band = 0; band < SIGNAL_BORDER_BAND_COUNT; band++) {
        float thr = SIGNAL_BORDER_BANDS[band].threshold;
        float hw = SIGNAL_BORDER_BANDS[band].width;

        float radii[MAX_STATIONS];
        float radii_sq[MAX_STATIONS];
        for (int s = 0; s < MAX_STATIONS; s++) {
            if (!sources[s].provides) {
                radii[s] = 0.0f;
                radii_sq[s] = 0.0f;
                continue;
            }
            float boost = (float)station_overlap[s];
            float effective_thr = thr / boost;    /* boost reduces the per-station needed strength */
            if (effective_thr > 1.0f) effective_thr = 1.0f;
            radii[s] = sources[s].range * (1.0f - effective_thr);
            radii_sq[s] = radii[s] * radii[s];
        }

        for (int s = 0; s < MAX_STATIONS; s++) {
            if (radii[s] <= 0.0f) continue;
            float r = radii[s];

            bool active = false;
            uint32_t first = 0;

            for (int i = 0; i <= SIGNAL_BORDER_SEGS; i++) {
                float ca = g_signal_border_cache.cos_lut[i];
                float sa = g_signal_border_cache.sin_lut[i];
                float px = sources[s].x + ca * r;
                float py = sources[s].y + sa * r;

                /* Clip: skip if inside another station's circle */
                bool clipped = false;
                for (int o = 0; o < MAX_STATIONS; o++) {
                    if (o == s || radii[o] <= 0.0f) continue;
                    float dx = px - sources[o].x;
                    float dy = py - sources[o].y;
                    if (dx*dx + dy*dy < radii_sq[o]) { clipped = true; break; }
                }

                if (!clipped && (i % 4 < 3)) { /* 3 on, 1 off = dashed */
                    if (g_signal_border_cache.vert_count + 2 > SIGNAL_BORDER_MAX_VERTS) {
                        if (active) {
                            signal_border_finish_strip((uint8_t)band, sources[s].x, sources[s].y,
                                                       r, first,
                                                       g_signal_border_cache.vert_count - first);
                        }
                        active = false;
                        break;
                    }
                    if (!active) {
                        active = true;
                        first = g_signal_border_cache.vert_count;
                    }
                    g_signal_border_cache.verts[g_signal_border_cache.vert_count++] =
                        v2(px - ca * hw, py - sa * hw);
                    g_signal_border_cache.verts[g_signal_border_cache.vert_count++] =
                        v2(px + ca * hw, py + sa * hw);
                    active = true;
                } else if (active) {
                    signal_border_finish_strip((uint8_t)band, sources[s].x, sources[s].y,
                                               r, first,
                                               g_signal_border_cache.vert_count - first);
                    active = false;
                }
            }
            if (active) {
                signal_border_finish_strip((uint8_t)band, sources[s].x, sources[s].y,
                                           r, first,
                                           g_signal_border_cache.vert_count - first);
            }
        }
    }
    g_signal_border_cache.valid = true;
}

void draw_signal_borders(void) {
    /* S(p) >= t iff p is inside any circle of radius R_i*(1-t).
     * The contour is the boundary of the union of those circles.
     * For each station, draw only the arc NOT inside another circle.
     * Exact geometry is cached until station signal sources change. */
    signal_border_source_t sources[MAX_STATIONS];
    if (signal_border_sources_changed(sources)) {
        signal_border_rebuild_cache(sources);
    }

    float cue_prev = world_signal_visual_enter_cue();
    for (uint32_t si = 0; si < g_signal_border_cache.strip_count; si++) {
        const signal_border_strip_t *strip = &g_signal_border_cache.strips[si];
        if (!on_screen(strip->x, strip->y, strip->radius)) continue;
        const signal_border_band_t *band = &SIGNAL_BORDER_BANDS[strip->band];
        sgl_begin_triangle_strip();
        sgl_c4f(band->r, band->g, band->b, band->a);
        for (uint32_t vi = 0; vi < strip->count; vi++) {
            vec2 p = g_signal_border_cache.verts[strip->first + vi];
            sgl_v2f(p.x, p.y);
        }
        sgl_end();
    }
    world_signal_visual_leave_cue(cue_prev);
}

/* ------------------------------------------------------------------ */
/* Module type color palette                                          */
/* ------------------------------------------------------------------ */

static void module_color(module_type_t type, float *r, float *g, float *b) {
    switch (type) {
    case MODULE_FURNACE:      PAL_UNPACK3(PAL_MODULE_FURNACE,      *r, *g, *b); return;
    case MODULE_HOPPER:       PAL_UNPACK3(PAL_MODULE_HOPPER,       *r, *g, *b); return;
    case MODULE_FRAME_PRESS:  PAL_UNPACK3(PAL_MODULE_FRAME_PRESS,  *r, *g, *b); return;
    case MODULE_LASER_FAB:    PAL_UNPACK3(PAL_MODULE_LASER_FAB,    *r, *g, *b); return;
    case MODULE_TRACTOR_FAB:  PAL_UNPACK3(PAL_MODULE_TRACTOR_FAB,  *r, *g, *b); return;
    case MODULE_SIGNAL_RELAY: PAL_UNPACK3(PAL_MODULE_SIGNAL_RELAY,  *r, *g, *b); return;
    case MODULE_REPAIR_BAY:   PAL_UNPACK3(PAL_MODULE_REPAIR_BAY,    *r, *g, *b); return;
    case MODULE_SHIPYARD:     PAL_UNPACK3(PAL_MODULE_SHIPYARD,      *r, *g, *b); return;
    default:                  PAL_UNPACK3(PAL_MODULE_GENERIC,        *r, *g, *b); return;
    }
}

void module_color_fn(module_type_t type, float *r, float *g, float *b) {
    module_color(type, r, g, b);
}

/* Per-commodity color — used to tint hoppers (which buffer one
 * commodity each) and the cross-ring spokes that flow that
 * commodity. Derived from the asteroid/ore palette; see palette.h
 * for the chain ore → ingot → product. */
void commodity_color(commodity_t c, float *r, float *g, float *b) {
    switch (c) {
    case COMMODITY_FERRITE_ORE:    PAL_UNPACK3(PAL_COMMODITY_FERRITE_ORE,    *r, *g, *b); return;
    case COMMODITY_CUPRITE_ORE:    PAL_UNPACK3(PAL_COMMODITY_CUPRITE_ORE,    *r, *g, *b); return;
    case COMMODITY_CRYSTAL_ORE:    PAL_UNPACK3(PAL_COMMODITY_CRYSTAL_ORE,    *r, *g, *b); return;
    case COMMODITY_FERRITE_INGOT:  PAL_UNPACK3(PAL_COMMODITY_FERRITE_INGOT,  *r, *g, *b); return;
    case COMMODITY_CUPRITE_INGOT:  PAL_UNPACK3(PAL_COMMODITY_CUPRITE_INGOT,  *r, *g, *b); return;
    case COMMODITY_CRYSTAL_INGOT:  PAL_UNPACK3(PAL_COMMODITY_CRYSTAL_INGOT,  *r, *g, *b); return;
    case COMMODITY_FRAME:          PAL_UNPACK3(PAL_COMMODITY_FRAME,          *r, *g, *b); return;
    case COMMODITY_LASER_MODULE:   PAL_UNPACK3(PAL_COMMODITY_LASER_MODULE,   *r, *g, *b); return;
    case COMMODITY_TRACTOR_MODULE: PAL_UNPACK3(PAL_COMMODITY_TRACTOR_MODULE, *r, *g, *b); return;
    case COMMODITY_REPAIR_KIT:     PAL_UNPACK3(PAL_COMMODITY_REPAIR_KIT,     *r, *g, *b); return;
    default:                       PAL_UNPACK3(PAL_MODULE_GENERIC,           *r, *g, *b); return;
    }
}

static bool commodity_is_ore(commodity_t c) {
    return c == COMMODITY_FERRITE_ORE ||
           c == COMMODITY_CUPRITE_ORE ||
           c == COMMODITY_CRYSTAL_ORE;
}

static bool commodity_is_ingot(commodity_t c) {
    return c == COMMODITY_FERRITE_INGOT ||
           c == COMMODITY_CUPRITE_INGOT ||
           c == COMMODITY_CRYSTAL_INGOT;
}

static void commodity_resource_color(commodity_t c, float *r, float *g, float *b) {
    switch (commodity_ore_form(c)) {
    case COMMODITY_FERRITE_ORE: PAL_UNPACK3(PAL_COMMODITY_FERRITE_ORE, *r, *g, *b); return;
    case COMMODITY_CUPRITE_ORE: PAL_UNPACK3(PAL_COMMODITY_CUPRITE_ORE, *r, *g, *b); return;
    case COMMODITY_CRYSTAL_ORE: PAL_UNPACK3(PAL_COMMODITY_CRYSTAL_ORE, *r, *g, *b); return;
    default:                    PAL_UNPACK3(PAL_COMMODITY_FERRITE_ORE, *r, *g, *b); return;
    }
}

static void commodity_hopper_palette(commodity_t c,
                                     float *base_r, float *base_g, float *base_b,
                                     float *accent_r, float *accent_g, float *accent_b) {
    float rr = 0.0f, rg = 0.0f, rb = 0.0f;
    float mr = 0.0f, mg = 0.0f, mb = 0.0f;
    commodity_resource_color(c, &rr, &rg, &rb);
    PAL_UNPACK3(PAL_COMMODITY_METAL_ACCENT, mr, mg, mb);

    if (commodity_is_ore(c)) {
        *base_r = rr; *base_g = rg; *base_b = rb;
        *accent_r = rr; *accent_g = rg; *accent_b = rb;
    } else if (commodity_is_ingot(c)) {
        *base_r = rr; *base_g = rg; *base_b = rb;
        *accent_r = mr; *accent_g = mg; *accent_b = mb;
    } else {
        *base_r = mr; *base_g = mg; *base_b = mb;
        *accent_r = rr; *accent_g = rg; *accent_b = rb;
        if (c == COMMODITY_REPAIR_KIT) {
            *accent_r = 0.90f; *accent_g = 0.20f; *accent_b = 0.20f;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Solid module block + corridor to core                              */
/* ------------------------------------------------------------------ */

/* Helper: filled quad (two triangles) in local coords */
static void fill_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {
    sgl_begin_triangles();
    sgl_v2f(x0,y0); sgl_v2f(x1,y1); sgl_v2f(x2,y2);
    sgl_v2f(x0,y0); sgl_v2f(x2,y2); sgl_v2f(x3,y3);
    sgl_end();
}

/* Helper: filled circle in local coords */
static void fill_circle_local(float cx, float cy, float r, int segs, float cr, float cg, float cb, float ca) {
    sgl_c4f(cr, cg, cb, ca);
    sgl_begin_triangles();
    for (int i = 0; i < segs; i++) {
        float a0 = TWO_PI_F * (float)i / (float)segs;
        float a1 = TWO_PI_F * (float)(i+1) / (float)segs;
        sgl_v2f(cx, cy);
        sgl_v2f(cx + cosf(a0)*r, cy + sinf(a0)*r);
        sgl_v2f(cx + cosf(a1)*r, cy + sinf(a1)*r);
    }
    sgl_end();
}

/* 6-shape module system: shape=function, hull color=faction, accent=product/ore.
 *   Dock(+repair) = U-shape    Intake(hopper+silo) = Triangle
 *   Furnace       = Circle     Fabricator(all)     = Pentagon
 *   Relay         = Diamond    Shipyard            = Open frame
 * Drawn in local rotated space, ~64x64 bounding. */

/* Helper: outline a regular polygon */
static void outline_ngon(int n, float r, float cr, float cg, float cb, float ca) {
    sgl_c4f(cr, cg, cb, ca);
    sgl_begin_lines();
    for (int i = 0; i < n; i++) {
        float a0 = TWO_PI_F * (float)i / (float)n - PI_F * 0.5f;
        float a1 = TWO_PI_F * (float)(i+1) / (float)n - PI_F * 0.5f;
        sgl_v2f(cosf(a0)*r, sinf(a0)*r);
        sgl_v2f(cosf(a1)*r, sinf(a1)*r);
    }
    sgl_end();
}

/* Helper: fill a regular polygon */
static void fill_ngon(int n, float r, float cr, float cg, float cb, float ca) {
    sgl_c4f(cr, cg, cb, ca);
    sgl_begin_triangles();
    for (int i = 0; i < n; i++) {
        float a0 = TWO_PI_F * (float)i / (float)n - PI_F * 0.5f;
        float a1 = TWO_PI_F * (float)(i+1) / (float)n - PI_F * 0.5f;
        sgl_v2f(0, 0);
        sgl_v2f(cosf(a0)*r, sinf(a0)*r);
        sgl_v2f(cosf(a1)*r, sinf(a1)*r);
    }
    sgl_end();
}

static void draw_module_shape(module_type_t type, float mr, float mg, float mb, float alpha) {
    switch (type) {

    /* ---- DOCK (+repair): U-shape ---- */
    case MODULE_DOCK:
    case MODULE_REPAIR_BAY: {
        /* Solid backplate covers corridor end underneath */
        sgl_c4f(mr*0.12f, mg*0.12f, mb*0.12f, alpha);
        fill_quad(-30, -26, 30, -26, 30, 26, -30, 26);
        /* U-shape fill: back plate + two arms */
        sgl_c4f(mr*0.30f, mg*0.30f, mb*0.30f, alpha);
        fill_quad(-28, 24, 28, 24, 28, 12, -28, 12);   /* back plate */
        fill_quad(-28, -24, -14, -24, -14, 24, -28, 24); /* left arm */
        fill_quad(14, -24, 28, -24, 28, 24, 14, 24);     /* right arm */
        /* Clamp teeth */
        sgl_c4f(mr*0.55f, mg*0.55f, mb*0.55f, alpha);
        fill_quad(-14, -24, -6, -24, -6, -18, -14, -18);
        fill_quad(6, -24, 14, -24, 14, -18, 6, -18);
        /* Guide lights */
        fill_circle_local(-8, -4, 4, 6, mr*0.9f, mg*0.9f, mb*0.9f, alpha*0.7f);
        fill_circle_local( 8, -4, 4, 6, mr*0.9f, mg*0.9f, mb*0.9f, alpha*0.7f);
        /* Repair wrench hint (small, bottom-right) */
        if (type == MODULE_REPAIR_BAY) {
            sgl_c4f(mr*0.6f, mg*0.6f, mb*0.6f, alpha*0.5f);
            sgl_begin_lines();
            sgl_v2f(4, 20); sgl_v2f(14, 10);
            sgl_end();
            fill_circle_local(3, 21, 3, 6, mr*0.4f, mg*0.4f, mb*0.4f, alpha*0.4f);
        }
        /* Bold outline */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        sgl_begin_lines();
        sgl_v2f(-28, 24); sgl_v2f(-28, -24);
        sgl_v2f(-28, -24); sgl_v2f(-6, -24);
        sgl_v2f(6, -24); sgl_v2f(28, -24);
        sgl_v2f(28, -24); sgl_v2f(28, 24);
        sgl_v2f(28, 24); sgl_v2f(-28, 24);
        sgl_end();
        break;
    }

    /* ---- INTAKE (hopper): Triangle ---- */
    case MODULE_HOPPER: {
        /* Triangle pointing outward (-Y) = funnel mouth */
        sgl_c4f(mr*0.30f, mg*0.30f, mb*0.30f, alpha);
        sgl_begin_triangles();
        sgl_v2f(-32, -20); sgl_v2f(32, -20); sgl_v2f(0, 28);
        sgl_end();
        /* Mouth rim highlight */
        sgl_c4f(mr*0.8f, mg*0.8f, mb*0.8f, alpha);
        fill_quad(-32, -22, 32, -22, 32, -18, -32, -18);
        /* Ore accent glow in center */
        fill_circle_local(0, 0, 7, 8, mr*0.5f, mg*0.5f, mb*0.5f, alpha*0.2f);
        fill_circle_local(0, 0, 3, 6, mr*0.7f, mg*0.7f, mb*0.7f, alpha*0.35f);
        /* Bold outline */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        sgl_begin_lines();
        sgl_v2f(-32, -20); sgl_v2f(32, -20);
        sgl_v2f(32, -20); sgl_v2f(0, 28);
        sgl_v2f(0, 28); sgl_v2f(-32, -20);
        sgl_end();
        break;
    }

    /* ---- FURNACE: Circle ---- */
    case MODULE_FURNACE: {
        /* Filled circle hull */
        fill_circle_local(0, 0, 28, 20, mr*0.42f, mg*0.42f, mb*0.42f, alpha);
        /* Ore-type accent glow. Keep this commodity colored, not generic
         * orange, so furnace role matches the hopper resource at a glance. */
        fill_circle_local(0, 0, 24, 16, mr*0.20f, mg*0.20f, mb*0.20f, alpha*0.45f);
        fill_circle_local(0, 0, 18, 14, mr*0.36f, mg*0.30f, mb*0.26f, alpha*0.55f);
        fill_circle_local(0, 0, 12, 12, mr*0.62f, mg*0.48f, mb*0.34f, alpha*0.65f);
        fill_circle_local(0, 0, 7,  10, mr*0.90f, mg*0.66f, mb*0.42f, alpha*0.80f);
        fill_circle_local(0, 0, 3,  8,  mr*1.0f,  mg*0.82f, mb*0.56f, alpha*0.95f);
        /* Bright hot core dot */
        fill_circle_local(0, 0, 1.5f, 6, 1.0f, 0.95f, 0.7f, alpha*0.8f);
        /* Bold outline */
        outline_ngon(20, 29, mr*0.95f, mg*0.95f, mb*0.95f, alpha);
        break;
    }

    /* ---- FABRICATOR (press, laser, tractor): Pentagon ---- */
    case MODULE_FRAME_PRESS:
    case MODULE_LASER_FAB:
    case MODULE_TRACTOR_FAB: {
        /* Filled pentagon hull */
        fill_ngon(5, 28, mr*0.30f, mg*0.30f, mb*0.30f, alpha);
        /* Inner pentagon (product chamber) */
        fill_ngon(5, 14, mr*0.15f, mg*0.15f, mb*0.15f, alpha*0.8f);
        /* Product accent dot */
        fill_circle_local(0, 2, 4, 6, mr*0.8f, mg*0.8f, mb*0.8f, alpha*0.4f);
        /* Crosshair for press type */
        if (type == MODULE_FRAME_PRESS) {
            sgl_c4f(mr*0.8f, mg*0.8f, mb*0.8f, alpha*0.5f);
            sgl_begin_lines();
            sgl_v2f(-8, 2); sgl_v2f(8, 2);
            sgl_v2f(0, -6); sgl_v2f(0, 10);
            sgl_end();
        }
        /* Bold outline */
        outline_ngon(5, 28, mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        break;
    }

    /* ---- RELAY: Diamond ---- */
    case MODULE_SIGNAL_RELAY: {
        /* Filled diamond (rotated square) */
        sgl_c4f(mr*0.30f, mg*0.30f, mb*0.30f, alpha);
        sgl_begin_triangles();
        sgl_v2f(0, -32); sgl_v2f(26, 0); sgl_v2f(0, 32);
        sgl_v2f(0, -32); sgl_v2f(0, 32); sgl_v2f(-26, 0);
        sgl_end();
        /* Beacon center */
        fill_circle_local(0, 0, 7, 8, mr*0.4f, mg*0.4f, mb*0.4f, alpha*0.8f);
        fill_circle_local(0, 0, 3, 6, mr*1.0f, mg*1.0f, mb*1.0f, alpha*0.8f);
        /* Signal ripple diamond (inner echo) */
        sgl_c4f(mr*0.6f, mg*0.6f, mb*0.6f, alpha*0.15f);
        sgl_begin_lines();
        sgl_v2f(0, -18); sgl_v2f(14, 0);
        sgl_v2f(14, 0); sgl_v2f(0, 18);
        sgl_v2f(0, 18); sgl_v2f(-14, 0);
        sgl_v2f(-14, 0); sgl_v2f(0, -18);
        sgl_end();
        /* Bold outline */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        sgl_begin_lines();
        sgl_v2f(0, -32); sgl_v2f(26, 0);
        sgl_v2f(26, 0); sgl_v2f(0, 32);
        sgl_v2f(0, 32); sgl_v2f(-26, 0);
        sgl_v2f(-26, 0); sgl_v2f(0, -32);
        sgl_end();
        break;
    }

    /* ---- SHIPYARD: Open square frame ---- */
    case MODULE_SHIPYARD: {
        /* Solid backplate covers corridor end underneath */
        sgl_c4f(mr*0.12f, mg*0.12f, mb*0.12f, alpha);
        fill_quad(-30, -30, 30, -30, 30, 30, -30, 30);
        /* Corner blocks (filled) */
        sgl_c4f(mr*0.30f, mg*0.30f, mb*0.30f, alpha);
        fill_quad(-28, -28, -16, -28, -16, -16, -28, -16); /* TL */
        fill_quad( 16, -28,  28, -28,  28, -16,  16, -16); /* TR */
        fill_quad(-28,  16, -16,  16, -16,  28, -28,  28); /* BL */
        fill_quad( 16,  16,  28,  16,  28,  28,  16,  28); /* BR */
        /* Cross-bracing */
        sgl_c4f(mr*0.5f, mg*0.5f, mb*0.5f, alpha*0.4f);
        sgl_begin_lines();
        sgl_v2f(-16, -16); sgl_v2f(16, 16);
        sgl_v2f(16, -16); sgl_v2f(-16, 16);
        sgl_end();
        /* Work lights */
        fill_circle_local(-22, 0, 2.5f, 4, mr*0.9f, mg*0.7f, mb*0.2f, alpha*0.35f);
        fill_circle_local( 22, 0, 2.5f, 4, mr*0.9f, mg*0.7f, mb*0.2f, alpha*0.35f);
        /* Bold frame outline */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        sgl_begin_lines();
        sgl_v2f(-28, -28); sgl_v2f(28, -28);
        sgl_v2f(28, -28); sgl_v2f(28, 28);
        sgl_v2f(28, 28); sgl_v2f(-28, 28);
        sgl_v2f(-28, 28); sgl_v2f(-28, -28);
        sgl_end();
        break;
    }

    default: {
        /* Generic chamfered square fallback */
        float ch = 6.0f;
        sgl_c4f(mr*0.35f, mg*0.35f, mb*0.35f, alpha);
        fill_quad(-24+ch, -24, 24-ch, -24, 24-ch, 24, -24+ch, 24);
        fill_quad(-24, -24+ch, -24+ch, -24, -24+ch, 24, -24, 24-ch);
        fill_quad(24-ch, -24, 24, -24+ch, 24, 24-ch, 24-ch, 24);
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        sgl_begin_lines();
        sgl_v2f(-24+ch, -24); sgl_v2f(24-ch, -24);
        sgl_v2f(24-ch, -24); sgl_v2f(24, -24+ch);
        sgl_v2f(24, -24+ch); sgl_v2f(24, 24-ch);
        sgl_v2f(24, 24-ch); sgl_v2f(24-ch, 24);
        sgl_v2f(24-ch, 24); sgl_v2f(-24+ch, 24);
        sgl_v2f(-24+ch, 24); sgl_v2f(-24, 24-ch);
        sgl_v2f(-24, 24-ch); sgl_v2f(-24, -24+ch);
        sgl_v2f(-24, -24+ch); sgl_v2f(-24+ch, -24);
        sgl_end();
        break;
    }
    }
}

static void draw_hopper_shape(float br, float bg, float bb,
                              float ar, float ag, float ab,
                              float alpha) {
    /* Triangle pointing outward (-Y) = funnel mouth. The body carries the
     * resource-family base; the rim/core carries the accent. */
    sgl_c4f(br * 0.46f, bg * 0.46f, bb * 0.46f, alpha);
    sgl_begin_triangles();
    sgl_v2f(-32, -20); sgl_v2f(32, -20); sgl_v2f(0, 28);
    sgl_end();

    /* Large inset keeps ore hoppers visibly "full resource" instead of
     * reading as a dark generic triangle at station scale. */
    sgl_c4f(br * 0.82f, bg * 0.82f, bb * 0.82f, alpha * 0.72f);
    sgl_begin_triangles();
    sgl_v2f(-22, -14); sgl_v2f(22, -14); sgl_v2f(0, 18);
    sgl_end();

    sgl_c4f(ar * 0.95f, ag * 0.95f, ab * 0.95f, alpha);
    fill_quad(-32, -22, 32, -22, 32, -18, -32, -18);

    fill_circle_local(0, 0, 8, 8, ar * 0.7f, ag * 0.7f, ab * 0.7f, alpha * 0.26f);
    fill_circle_local(0, 0, 4, 6, ar * 1.0f, ag * 1.0f, ab * 1.0f, alpha * 0.42f);

    sgl_c4f(ar * 0.55f + br * 0.25f,
            ag * 0.55f + bg * 0.25f,
            ab * 0.55f + bb * 0.25f,
            alpha);
    sgl_begin_lines();
    sgl_v2f(-32, -20); sgl_v2f(32, -20);
    sgl_v2f(32, -20); sgl_v2f(0, 28);
    sgl_v2f(0, 28); sgl_v2f(-32, -20);
    sgl_end();
}

static void draw_layout_warning_outline(module_type_t type,
                                        station_layout_status_t status,
                                        float pulse) {
    if (status == STATION_LAYOUT_OK) return;

    float wr = 1.0f, wg = 0.25f, wb = 0.10f;
    if (status == STATION_LAYOUT_MISSING_OUTPUT_HOPPER) {
        wr = 1.0f; wg = 0.72f; wb = 0.18f;
    }
    float a = 0.60f + 0.25f * pulse;

    if (type == MODULE_FURNACE) {
        outline_ngon(20, 35.0f, wr, wg, wb, a);
    } else if (type == MODULE_FRAME_PRESS ||
               type == MODULE_LASER_FAB ||
               type == MODULE_TRACTOR_FAB) {
        outline_ngon(5, 35.0f, wr, wg, wb, a);
    } else {
        sgl_c4f(wr, wg, wb, a);
        sgl_begin_lines();
        sgl_v2f(-34, -34); sgl_v2f( 34, -34);
        sgl_v2f( 34, -34); sgl_v2f( 34,  34);
        sgl_v2f( 34,  34); sgl_v2f(-34,  34);
        sgl_v2f(-34,  34); sgl_v2f(-34, -34);
        sgl_end();
    }

    /* Broken-connection ticks make the warning legible even when the
     * module shape color is already warm. */
    sgl_c4f(wr, wg, wb, a);
    sgl_begin_lines();
    sgl_v2f(-38, -30); sgl_v2f(-24, -38);
    sgl_v2f( 24, -38); sgl_v2f( 38, -30);
    sgl_v2f(-38,  30); sgl_v2f(-24,  38);
    sgl_v2f( 24,  38); sgl_v2f( 38,  30);
    sgl_end();
}

static void draw_module_at(vec2 pos, float angle, module_type_t type, bool scaffold, float progress, vec2 station_center,
                           const station_t *station, commodity_t hopper_commodity,
                           const station_module_t *module,
                           station_layout_status_t layout_status) {
    float mr, mg, mb;
    float hr = 0.0f, hg = 0.0f, hb = 0.0f;
    bool custom_hopper = false;
    /* Hoppers tint by their commodity tag (each hopper buffers ONE
     * commodity). Non-hoppers fall back to the static module-type
     * palette. */
    if (type == MODULE_HOPPER && hopper_commodity != COMMODITY_COUNT) {
        commodity_hopper_palette(hopper_commodity, &mr, &mg, &mb, &hr, &hg, &hb);
        custom_hopper = true;
    } else {
        module_color(type, &mr, &mg, &mb);
    }
    (void)station_center;
    /* Furnaces tint from their instance commodity tag when present, with
     * legacy ring fallback for old/untagged stations. See station_palette.h. */
    if (type == MODULE_FURNACE && station != NULL) {
        station_palette_furnace_module_color(station, module, &mr, &mg, &mb);
    }

    sgl_push_matrix();
    sgl_translate(pos.x, pos.y, 0.0f);
    sgl_rotate(angle + PI_F * 0.5f, 0.0f, 0.0f, 1.0f);
    sgl_scale(1.4f, 1.4f, 1.0f);

    if (scaffold) {
        /* Wireframe outline circle — construction amber (#FFD977) */
        float amb_r = 1.0f, amb_g = 0.85f, amb_b = 0.47f;
        float pulse = 0.3f + 0.15f * sinf((float)(pos.x + pos.y) * 0.1f + progress * 10.0f);

        /* Progress fill: partial circle from bottom */
        float fill = fminf(progress, 1.0f);
        if (fill > 0.01f) {
            int segs = (int)(16.0f * fill);
            if (segs < 2) segs = 2;
            sgl_begin_triangles();
            sgl_c4f(amb_r * 0.3f, amb_g * 0.3f, amb_b * 0.3f, pulse * 0.6f);
            float fill_angle = fill * TWO_PI_F;
            float start = PI_F * 0.5f; /* bottom */
            for (int i = 0; i < segs; i++) {
                float a0 = start + fill_angle * (float)i / (float)segs;
                float a1 = start + fill_angle * (float)(i + 1) / (float)segs;
                sgl_v2f(0, 0);
                sgl_v2f(cosf(a0) * 22.0f, sinf(a0) * 22.0f);
                sgl_v2f(cosf(a1) * 22.0f, sinf(a1) * 22.0f);
            }
            sgl_end();
        }

        /* Wireframe circle outline */
        sgl_begin_lines();
        sgl_c4f(amb_r, amb_g, amb_b, pulse + 0.3f);
        int wire_segs = 16;
        for (int i = 0; i < wire_segs; i++) {
            float a0 = TWO_PI_F * (float)i / (float)wire_segs;
            float a1 = TWO_PI_F * (float)(i + 1) / (float)wire_segs;
            sgl_v2f(cosf(a0) * 22.0f, sinf(a0) * 22.0f);
            sgl_v2f(cosf(a1) * 22.0f, sinf(a1) * 22.0f);
        }
        /* Cross-hatch for scaffolding feel */
        sgl_v2f(-16, -16); sgl_v2f(16, 16);
        sgl_v2f(-16, 16); sgl_v2f(16, -16);
        sgl_end();

        /* Progress bar below */
        if (fill > 0.01f) {
            float bar_w = 48.0f * fill;
            sgl_c4f(amb_r * 0.8f, amb_g * 0.8f, amb_b * 0.4f, 0.7f);
            fill_quad(-24, 30, -24 + bar_w, 30, -24 + bar_w, 34, -24, 34);
        }
    } else {
        if (custom_hopper) {
            draw_hopper_shape(mr, mg, mb, hr, hg, hb, 0.92f);
        } else {
            draw_module_shape(type, mr, mg, mb, 0.92f);
        }
        draw_layout_warning_outline(type, layout_status,
                                    0.5f + 0.5f * sinf(g.world.time * 5.0f + (float)(pos.x + pos.y) * 0.01f));

    }

    sgl_pop_matrix();
}

/* ------------------------------------------------------------------ */
/* Main station draw                                                  */
/* ------------------------------------------------------------------ */

/* Draw station core and dock range (below ships in render order). */
void draw_station(const station_t* station, bool is_current, bool is_nearby) {
    if (!station_exists(station) && !station->scaffold) return;
    (void)is_nearby;

    float role_r = 0.45f, role_g = 0.85f, role_b = 1.0f;
    station_role_color(station, &role_r, &role_g, &role_b);

    /* Scaffold rendering */
    if (station->scaffold) {
        float alpha = 0.3f + 0.2f * sinf(g.world.time * 1.5f);
        float prog = station->scaffold_progress;
        int dash_segs = 24;
        float step = TWO_PI_F / (float)dash_segs;
        for (int i = 0; i < dash_segs; i += 2) {
            float a0 = (float)i * step;
            float a1 = (float)(i + 1) * step;
            vec2 p0 = v2_add(station->pos, v2(cosf(a0) * station->dock_radius, sinf(a0) * station->dock_radius));
            vec2 p1 = v2_add(station->pos, v2(cosf(a1) * station->dock_radius, sinf(a1) * station->dock_radius));
            draw_segment(p0, p1, role_r * 0.5f, role_g * 0.5f, role_b * 0.5f, alpha);
        }
        draw_circle_outline(station->pos, station->radius, 18, role_r * 0.6f, role_g * 0.6f, role_b * 0.6f, alpha + 0.15f);
        if (prog > 0.01f) {
            int filled = (int)(prog * 24.0f);
            float fs = TWO_PI_F / 24.0f;
            for (int i = 0; i < filled && i < 24; i++) {
                vec2 p0 = v2_add(station->pos, v2(cosf(i*fs) * (station->radius+12.0f), sinf(i*fs) * (station->radius+12.0f)));
                vec2 p1 = v2_add(station->pos, v2(cosf((i+1)*fs) * (station->radius+12.0f), sinf((i+1)*fs) * (station->radius+12.0f)));
                draw_segment(p0, p1, role_r, role_g, role_b, 0.8f);
            }
        }
        return;
    }

    (void)is_current;

    /* Station center is empty space — the construction yard.
     * Just a faint marker so the player can locate the geometric center. */
    float pulse = 0.15f + 0.08f * sinf(g.world.time * 2.0f);
    draw_circle_outline(station->pos, 4.0f, 8, role_r * 0.4f, role_g * 0.4f, role_b * 0.4f, pulse);

    /* Radial spokes from core to ring 1 modules */
    for (int i = 0; i < station->module_count; i++) {
        if (station->modules[i].ring != 1) continue;
        vec2 mod_pos = module_world_pos_ring(station, 1, station->modules[i].slot);
        sgl_c4f(role_r * 0.2f, role_g * 0.2f, role_b * 0.2f, 0.25f);
        sgl_begin_lines();
        sgl_v2f(station->pos.x, station->pos.y);
        sgl_v2f(mod_pos.x, mod_pos.y);
        sgl_end();
    }

    /* Faint ring orbit guides */
    for (int r = 1; r <= STATION_NUM_RINGS; r++) {
        bool has_modules = false;
        for (int i = 0; i < station->module_count; i++)
            if (station->modules[i].ring == r) { has_modules = true; break; }
        if (!has_modules) continue;
        draw_circle_outline(station->pos, STATION_RING_RADIUS[r], 48, role_r * 0.08f, role_g * 0.08f, role_b * 0.08f, 0.08f);
    }

    /* Chain-event heartbeat — gentle expanding green halo when this
     * station's inventory or credit_pool moved this tick. Same muted
     * green used for the scan ring; pulse expands as it fades, so a
     * settled value (heartbeat = 0) is invisible and a fresh delta
     * (heartbeat = 1) starts as a tight ring at the station core. */
    ptrdiff_t s_idx = station - g.world.stations;
    if (s_idx >= 0 && s_idx < MAX_STATIONS) {
        float hb = g.station_heartbeat[s_idx];
        if (hb > 0.001f) {
            float t = 1.0f - hb;                    /* 0 at fire, 1 at decay */
            float radius = 12.0f + t * 18.0f;        /* tight 12u → 30u */
            float alpha = hb * 0.8f;
            draw_circle_outline(station->pos, radius, 24,
                                0.20f, 0.95f, 0.45f, alpha);
            draw_circle_outline(station->pos, radius * 0.55f, 16,
                                0.20f, 0.95f, 0.45f, alpha * 0.6f);
        }
    }
}

/* Solid corridor tube between adjacent modules on the same ring. */
/* Draw a curved corridor that arcs along the ring radius between two module positions. */
#define CORRIDOR_ARC_SEGMENTS 8

static void draw_corridor_arc(vec2 center, float ring_radius, float angle_a, float arc_delta,
                               float cr, float cg, float cb, float alpha) {
    /* Corridor visual band — slightly wider than STATION_CORRIDOR_HW to account
     * for the angular margin expansion in collision (ship radius ~12-15 units). */
    float hw = STATION_CORRIDOR_HW + 4.0f;
    float r_inner = ring_radius - hw;
    float r_outer = ring_radius + hw;

    /* arc_delta comes from the geom emitter and is already the
     * canonical forward span — no normalization. */
    float da = arc_delta;

    /* Solid fill — triangle strip as quads */
    sgl_c4f(cr * 0.15f, cg * 0.15f, cb * 0.15f, alpha * 0.6f);
    sgl_begin_triangles();
    for (int i = 0; i < CORRIDOR_ARC_SEGMENTS; i++) {
        float t0 = (float)i / (float)CORRIDOR_ARC_SEGMENTS;
        float t1 = (float)(i + 1) / (float)CORRIDOR_ARC_SEGMENTS;
        float a0 = angle_a + da * t0;
        float a1 = angle_a + da * t1;
        vec2 i0 = v2_add(center, v2(cosf(a0) * r_inner, sinf(a0) * r_inner));
        vec2 o0 = v2_add(center, v2(cosf(a0) * r_outer, sinf(a0) * r_outer));
        vec2 i1 = v2_add(center, v2(cosf(a1) * r_inner, sinf(a1) * r_inner));
        vec2 o1 = v2_add(center, v2(cosf(a1) * r_outer, sinf(a1) * r_outer));
        sgl_v2f(i0.x,i0.y); sgl_v2f(o0.x,o0.y); sgl_v2f(o1.x,o1.y);
        sgl_v2f(i0.x,i0.y); sgl_v2f(o1.x,o1.y); sgl_v2f(i1.x,i1.y);
    }
    sgl_end();

    /* Edge lines (inner and outer arcs) — brighter than fill */
    sgl_c4f(cr * 0.55f, cg * 0.55f, cb * 0.55f, alpha * 0.7f);
    sgl_begin_line_strip();
    for (int i = 0; i <= CORRIDOR_ARC_SEGMENTS; i++) {
        float t = (float)i / (float)CORRIDOR_ARC_SEGMENTS;
        float a = angle_a + da * t;
        sgl_v2f(center.x + cosf(a) * r_inner, center.y + sinf(a) * r_inner);
    }
    sgl_end();
    sgl_begin_line_strip();
    for (int i = 0; i <= CORRIDOR_ARC_SEGMENTS; i++) {
        float t = (float)i / (float)CORRIDOR_ARC_SEGMENTS;
        float a = angle_a + da * t;
        sgl_v2f(center.x + cosf(a) * r_outer, center.y + sinf(a) * r_outer);
    }
    sgl_end();
}

/* Draw module rings (above ships in render order). */
void draw_station_rings(const station_t* station, bool is_current, bool is_nearby) {
    if (!station_exists(station) || station->scaffold) return;

    float role_r = 0.45f, role_g = 0.85f, role_b = 1.0f;
    station_role_color(station, &role_r, &role_g, &role_b);
    float base_alpha = is_current ? 0.9f : (is_nearby ? 0.7f : 0.5f);

    vec2 module_pos[MAX_MODULES_PER_STATION];
    float module_angle_cache[MAX_MODULES_PER_STATION];
    for (int i = 0; i < station->module_count; i++) {
        module_pos[i] = module_world_pos_ring(station, station->modules[i].ring,
                                              station->modules[i].slot);
        module_angle_cache[i] = module_angle_ring(station, station->modules[i].ring,
                                                  station->modules[i].slot);
    }

    /* Find outermost populated ring */
    int max_ring = 0;
    for (int i = 0; i < station->module_count; i++)
        if (station->modules[i].ring >= 1 && station->modules[i].ring <= STATION_NUM_RINGS)
            if (station->modules[i].ring > max_ring) max_ring = station->modules[i].ring;

    (void)max_ring;

    /* Per-ring dominant color for corridors */
    float ring_cr[STATION_NUM_RINGS + 1], ring_cg[STATION_NUM_RINGS + 1], ring_cb[STATION_NUM_RINGS + 1];
    for (int r = 0; r <= STATION_NUM_RINGS; r++) {
        ring_cr[r] = role_r; ring_cg[r] = role_g; ring_cb[r] = role_b;
    }
    {
        /* Most saturated module sets the base, others tint it.
         * 80% base + 20% influence from the rest — enough to
         * shift the hue without muddying it. */
        for (int r = 1; r <= STATION_NUM_RINGS; r++) {
            float colors[MAX_MODULES_PER_STATION][3];
            float sats[MAX_MODULES_PER_STATION];
            int count = 0;
            int best = 0;
            float best_sat = -1.0f;
            for (int i = 0; i < station->module_count; i++) {
                if (station->modules[i].ring != r) continue;
                if (station->modules[i].type == MODULE_DOCK) continue;
                /* Furnaces use the dynamic per-ring tint (ferrite dull red /
                 * cuprite green-copper / crystal violet / chunks steel) - same
                 * source the furnace glow + body uses, so the corridor
                 * matches the furnace it borders. Without this every
                 * furnace contributed static PAL_MODULE_FURNACE amber
                 * and every ring with a furnace turned amber. */
                if (station->modules[i].type == MODULE_FURNACE) {
                    station_palette_furnace_module_color(station, &station->modules[i],
                        &colors[count][0], &colors[count][1], &colors[count][2]);
                } else if (station->modules[i].type == MODULE_HOPPER) {
                    /* Hopper color = its commodity's color. Each
                     * hopper buffers exactly one commodity (auto-
                     * solved at placement); the tint reads the
                     * commodity directly so the silhouette of a
                     * station tells you what flows there. */
                    commodity_t hc = (commodity_t)station->modules[i].commodity;
                    commodity_color(hc,
                        &colors[count][0], &colors[count][1], &colors[count][2]);
                } else {
                    module_color(station->modules[i].type,
                        &colors[count][0], &colors[count][1], &colors[count][2]);
                }
                float cmax = fmaxf(colors[count][0], fmaxf(colors[count][1], colors[count][2]));
                float cmin = fminf(colors[count][0], fminf(colors[count][1], colors[count][2]));
                sats[count] = (cmax > 0.001f) ? (cmax - cmin) / cmax : 0.0f;
                if (sats[count] > best_sat) { best_sat = sats[count]; best = count; }
                count++;
            }
            if (count == 0) continue;
            /* Start with the dominant color */
            ring_cr[r] = colors[best][0];
            ring_cg[r] = colors[best][1];
            ring_cb[r] = colors[best][2];
            if (count > 1) {
                /* Tint: lerp 20% toward the average of the others */
                float tr = 0, tg = 0, tb = 0;
                for (int c = 0; c < count; c++) {
                    if (c == best) continue;
                    tr += colors[c][0]; tg += colors[c][1]; tb += colors[c][2];
                }
                float n = (float)(count - 1);
                float blend = 0.2f;
                ring_cr[r] = ring_cr[r] * (1.0f - blend) + (tr / n) * blend;
                ring_cg[r] = ring_cg[r] * (1.0f - blend) + (tg / n) * blend;
                ring_cb[r] = ring_cb[r] * (1.0f - blend) + (tb / n) * blend;
            }
        }
    }

    /* Draw all corridors from the geometry emitter, colored per ring */
    station_geom_t geom;
    station_build_geom(station, &geom);
    for (int ci = 0; ci < geom.corridor_count; ci++) {
        int r = geom.corridors[ci].ring;
        draw_corridor_arc(station->pos, geom.corridors[ci].ring_radius,
            geom.corridors[ci].angle_a, geom.corridors[ci].arc_delta,
            ring_cr[r], ring_cg[r], ring_cb[r], base_alpha * 0.7f);
    }

    /* Spokes — tractor beams from each producer to each of its
     * input-commodity hoppers. Color = the commodity flowing
     * through the spoke (rust for ferrite, blue for cuprite,
     * green for crystal, gold for frame, etc). Alpha fades with
     * the producer's activity pulse: full bright when actively
     * consuming, gone within RING_PULSE_LINGER_SEC of idleness.
     * Render-only — no collision impact. */
    sgl_begin_lines();
    for (int si = 0; si < geom.spoke_count; si++) {
        float pulse = geom.spokes[si].pulse;
        if (pulse <= 0.01f) continue;
        float cr_, cg_, cb_;
        commodity_color((commodity_t)geom.spokes[si].commodity, &cr_, &cg_, &cb_);
        float intensity = 0.55f + 0.45f * pulse;
        sgl_c4f(cr_ * intensity, cg_ * intensity, cb_ * intensity,
                base_alpha * 0.65f * pulse);
        sgl_v2f(geom.spokes[si].a.x, geom.spokes[si].a.y);
        sgl_v2f(geom.spokes[si].b.x, geom.spokes[si].b.y);
    }
    sgl_end();

    /* Per-ring: tethers + modules (each ring rotates independently) */
    for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
        int mod_idx[MAX_MODULES_PER_STATION];
        int mod_count = 0;
        for (int i = 0; i < station->module_count; i++) {
            if (station->modules[i].ring == ring)
                mod_idx[mod_count++] = i;
        }
        if (mod_count == 0) continue;

        /* Sort modules by slot (insertion sort, small N) */
        for (int i = 1; i < mod_count; i++) {
            int key = mod_idx[i];
            int j = i - 1;
            while (j >= 0 && station->modules[mod_idx[j]].slot > station->modules[key].slot) {
                mod_idx[j + 1] = mod_idx[j]; j--;
            }
            mod_idx[j + 1] = key;
        }

        vec2 positions[MAX_MODULES_PER_STATION];
        for (int i = 0; i < mod_count; i++) {
            positions[i] = module_pos[mod_idx[i]];
        }

        /* Modules + dock indicators + furnace glow */
        for (int i = 0; i < mod_count; i++) {
            const station_module_t *m = &station->modules[mod_idx[i]];
            float angle = module_angle_cache[mod_idx[i]];
            station_layout_status_t layout_status = station_module_layout_status(station, m);
            draw_module_at(positions[i], angle, m->type, m->scaffold, m->build_progress, station->pos, station,
                           (commodity_t)m->commodity, m, layout_status);

            /* Furnace: glow + red laser beam to target module when smelting */
            if (!m->scaffold && m->type == MODULE_FURNACE) {
                /* Glow inherits the per-ring furnace tint (red ferrite,
                 * green-copper cuprite, violet crystal, steel chunks-feeder, plus
                 * the dynamic cuprite/crystal tint for last-smelted-ore on the
                 * middle ring). Without this, the glow stays the static
                 * PAL_MODULE_FURNACE amber and dominates the per-ring
                 * body color. */
                float fr, fg, fb;
                station_palette_furnace_module_color(station, m, &fr, &fg, &fb);
                float pulse = 0.3f + 0.15f * sinf(g.world.time * 3.0f + (float)m->slot);

                /* Always: warm glow at furnace */
                draw_circle_filled(positions[i], 44.0f, 12, fr * 0.6f, fg * 0.3f, fb * 0.15f, pulse * 0.3f);
                draw_circle_filled(positions[i], 28.0f, 10, fr * 0.9f, fg * 0.5f, fb * 0.2f, pulse * 0.4f);

                /* Find nearest matching ore hopper on an adjacent ring.
                 * This mirrors step_furnace_smelting; otherwise the idle
                 * line can imply a furnace is connected when the sim would
                 * never fire. */
                vec2 target = positions[i];
                bool has_target = false;
                {
                    float best_d = 1e18f;
                    commodity_t ore = module_instance_input_ore(m);
                    int adj_rings[] = { ring + 1, ring - 1 };
                    for (int ri = 0; ri < 2; ri++) {
                        int adj = adj_rings[ri];
                        if (adj < 1 || adj > STATION_NUM_RINGS) continue;
                        for (int mi2 = 0; mi2 < station->module_count; mi2++) {
                            if (station->modules[mi2].ring != adj) continue;
                            if (station->modules[mi2].scaffold) continue;
                            if (station->modules[mi2].type != MODULE_HOPPER) continue;
                            if ((commodity_t)station->modules[mi2].commodity != ore) continue;
                            vec2 mp2 = module_pos[mi2];
                            float dd = v2_dist_sq(positions[i], mp2);
                            if (dd < best_d) { best_d = dd; target = mp2; has_target = true; }
                        }
                    }
                }

                /* Check if any fragment is smelting near this furnace */
                bool has_smelting = false;
                const asteroid_render_lists_t *lists = asteroid_render_lists();
                for (int li = 0; li < lists->smelting_count; li++) {
                    const asteroid_t *fa = &g.world.asteroids[lists->smelting[li]];
                    if (v2_dist_sq(fa->pos, positions[i]) < 300.0f * 300.0f) {
                        has_smelting = true; break;
                    }
                }

                if (has_smelting && has_target) {
                    /* RED LASER between furnace and target — zappy flicker */
                    float flicker = 0.7f + 0.3f * sinf(g.world.time * 47.0f);
                    float zap1 = sinf(g.world.time * 31.0f) * 0.5f + 0.5f;
                    float zap2 = sinf(g.world.time * 53.0f) * 0.5f + 0.5f;
                    vec2 bdir = v2_sub(target, positions[i]);
                    float blen = sqrtf(v2_len_sq(bdir));
                    if (blen > 1.0f) {
                        vec2 nd = v2_scale(bdir, 1.0f / blen);
                        vec2 perp = v2(-nd.y, nd.x);
                        vec2 mid = v2_scale(v2_add(positions[i], target), 0.5f);
                        vec2 j1 = v2_add(mid, v2_scale(perp, 5.0f * zap1));
                        vec2 j2 = v2_add(mid, v2_scale(perp, -5.0f * zap2));
                        /* Main red beam */
                        draw_segment(positions[i], target, 1.0f, 0.2f, 0.1f, 0.8f * flicker);
                        /* Jittering side beams */
                        draw_segment(positions[i], j1, 1.0f, 0.35f, 0.1f, 0.5f * flicker);
                        draw_segment(j1, target, 1.0f, 0.35f, 0.1f, 0.5f * flicker);
                        draw_segment(positions[i], j2, 1.0f, 0.15f, 0.05f, 0.4f * flicker);
                        draw_segment(j2, target, 1.0f, 0.15f, 0.05f, 0.4f * flicker);
                        /* Hot white core */
                        draw_segment(positions[i], target, 1.0f, 0.9f, 0.7f, 0.25f * flicker);
                    }
                    /* Glow at both ends */
                    draw_circle_filled(positions[i], 36.0f, 10, 1.0f, 0.3f, 0.1f, 0.3f * flicker);
                    draw_circle_filled(target, 28.0f, 8, 1.0f, 0.2f, 0.05f, 0.2f * flicker);
                } else if (has_target) {
                    /* Idle: faint connection line to target */
                    draw_segment(positions[i], target, fr, fg, fb, pulse * 0.15f);
                }
            }

            /* Fabricator: beam to nearest supplier when input buffer has material */
            if (!m->scaffold && (m->type == MODULE_FRAME_PRESS ||
                                  m->type == MODULE_LASER_FAB ||
                                  m->type == MODULE_TRACTOR_FAB)) {
                float fr, fg, fb;
                module_color(m->type, &fr, &fg, &fb);
                bool producing = station->module_input[mod_idx[i]] > 0.1f;

                /* Find nearest module that could supply this fab (furnace or storage) */
                vec2 supplier = positions[i];
                {
                    float best_d = 1e18f;
                    for (int mi2 = 0; mi2 < station->module_count; mi2++) {
                        if (mi2 == mod_idx[i]) continue;
                        if (station->modules[mi2].scaffold) continue;
                        module_type_t st = station->modules[mi2].type;
                        /* Suppliers: furnaces or hoppers (the latter
                         * absorbed the legacy ORE_SILO/CARGO_BAY storage
                         * roles). */
                        bool is_supplier = (st == MODULE_FURNACE ||
                                           st == MODULE_HOPPER);
                        if (!is_supplier) continue;
                        vec2 sp = module_pos[mi2];
                        float dd = v2_dist_sq(positions[i], sp);
                        if (dd < best_d) { best_d = dd; supplier = sp; }
                    }
                }

                if (producing) {
                    /* Active: colored beam from supplier to fab */
                    float flicker = 0.7f + 0.3f * sinf(g.world.time * 37.0f + (float)m->slot * 2.0f);
                    float zap = sinf(g.world.time * 29.0f) * 0.5f + 0.5f;
                    vec2 bdir = v2_sub(supplier, positions[i]);
                    float blen = sqrtf(v2_len_sq(bdir));
                    if (blen > 1.0f) {
                        vec2 nd = v2_scale(bdir, 1.0f / blen);
                        vec2 perp = v2(-nd.y, nd.x);
                        vec2 mid = v2_scale(v2_add(positions[i], supplier), 0.5f);
                        vec2 j1 = v2_add(mid, v2_scale(perp, 4.0f * zap));
                        /* Main colored beam */
                        draw_segment(positions[i], supplier, fr, fg, fb, 0.6f * flicker);
                        /* Jitter beam */
                        draw_segment(positions[i], j1, fr * 0.7f, fg * 0.7f, fb * 0.7f, 0.35f * flicker);
                        draw_segment(j1, supplier, fr * 0.7f, fg * 0.7f, fb * 0.7f, 0.35f * flicker);
                        /* White core */
                        draw_segment(positions[i], supplier, 1.0f, 0.95f, 0.9f, 0.15f * flicker);
                    }
                    /* Glow at fab */
                    draw_circle_filled(positions[i], 30.0f, 8, fr * 0.8f, fg * 0.8f, fb * 0.8f, 0.2f * flicker);
                } else {
                    /* Idle: faint connection line */
                    float pulse = 0.3f + 0.15f * sinf(g.world.time * 2.0f + (float)m->slot);
                    draw_segment(positions[i], supplier, fr, fg, fb, pulse * 0.1f);
                }
            }

            /* Dock berth indicator: show assigned berth when docking,
             * or all unoccupied berths dimly when in range */
            if (m->type == MODULE_DOCK && is_nearby && !m->scaffold) {
                vec2 outward = v2_sub(positions[i], station->pos);
                float od = sqrtf(v2_len_sq(outward));
                if (od > 0.001f) outward = v2_scale(outward, 1.0f / od);
                vec2 tang = v2(-outward.y, outward.x);
                int dock_slots = STATION_RING_SLOTS[ring];
                float next_ang = module_angle_ring(station, ring, (m->slot + 1) % dock_slots);
                float dock_ang = module_angle_ring(station, ring, m->slot);
                float ang_to_next = wrap_angle(next_ang - dock_ang);
                float gap_dir = (ang_to_next > 0.0f) ? 1.0f : -1.0f;
                vec2 berths[3];
                berths[0] = v2_add(positions[i], v2_scale(outward, 55.0f));
                berths[1] = v2_add(positions[i], v2_scale(outward, -55.0f));
                berths[2] = v2_add(positions[i], v2_scale(tang, gap_dir * 55.0f));
                (void)dock_slots;

                /* Which dock module is this? Compute berth index offset. */
                int dock_idx = 0;
                for (int di = 0; di < station->module_count; di++) {
                    if (station->modules[di].type != MODULE_DOCK) continue;
                    if (di == mod_idx[i]) break;
                    dock_idx++;
                }
                int berth_base = dock_idx * 3;  /* BERTHS_PER_DOCK = 3 */

                int station_idx = (int)(station - g.world.stations);
                bool approaching = LOCAL_PLAYER.docking_approach &&
                    LOCAL_PLAYER.nearby_station == station_idx;
                float dp = 0.5f + 0.4f * sinf(g.world.time * 4.0f);

                for (int b = 0; b < 3; b++) {
                    int global_berth = berth_base + b;
                    bool is_assigned = approaching &&
                        LOCAL_PLAYER.dock_berth == global_berth;

                    /* When approaching: only show the assigned berth */
                    /* When just nearby: show all dimly */
                    float alpha;
                    float cr, cg, cb;
                    if (approaching) {
                        if (!is_assigned) continue;  /* hide non-assigned */
                        cr = 0.2f; cg = 1.0f; cb = 0.6f;
                        alpha = dp;
                    } else {
                        cr = 0.15f; cg = 0.5f; cb = 0.4f;
                        alpha = 0.15f;
                    }

                    vec2 bdir = (b < 2) ? outward : tang;
                    vec2 bperp = (b < 2) ? tang : outward;
                    float bw = 14.0f, bh = 8.0f;
                    vec2 c0 = v2_add(berths[b], v2_add(v2_scale(bdir, -bh), v2_scale(bperp, -bw)));
                    vec2 c1 = v2_add(berths[b], v2_add(v2_scale(bdir,  bh), v2_scale(bperp, -bw)));
                    vec2 c2 = v2_add(berths[b], v2_add(v2_scale(bdir,  bh), v2_scale(bperp,  bw)));
                    vec2 c3 = v2_add(berths[b], v2_add(v2_scale(bdir, -bh), v2_scale(bperp,  bw)));
                    sgl_c4f(cr, cg, cb, alpha);
                    sgl_begin_lines();
                    sgl_v2f(c0.x, c0.y); sgl_v2f(c1.x, c1.y);
                    sgl_v2f(c1.x, c1.y); sgl_v2f(c2.x, c2.y);
                    sgl_v2f(c2.x, c2.y); sgl_v2f(c3.x, c3.y);
                    sgl_v2f(c3.x, c3.y); sgl_v2f(c0.x, c0.y);
                    sgl_end();
                }
            }
        }
    }
}

void draw_ship_tractor_field(void) {
    if (g.death_cinematic.active) return;
    float tr = ship_tractor_range(&LOCAL_PLAYER.ship);

    float now = g.world.time;
    float field_dt = 0.0f;
    if (g.tractor_field_last_time > 0.0f && now >= g.tractor_field_last_time)
        field_dt = now - g.tractor_field_last_time;
    if (field_dt > 0.05f) field_dt = 0.05f;
    g.tractor_field_last_time = now;
    if (LOCAL_PLAYER.ship.tractor_active) {
        g.tractor_field_expand = clampf(g.tractor_field_expand +
                                        field_dt / 0.30f, 0.0f, 1.0f);
    } else {
        g.tractor_field_expand = clampf(g.tractor_field_expand -
                                        field_dt / 0.12f, 0.0f, 1.0f);
    }

    float cue_prev = world_signal_visual_enter_cue();
    if (LOCAL_PLAYER.ship.tractor_active) {
        float expand = g.tractor_field_expand;
        float ease = expand * expand * (3.0f - 2.0f * expand);
        float radius = 20.0f + (tr - 20.0f) * ease;
        float alpha = (0.6f - 0.25f * ease) * (expand < 1.0f ? 1.0f : 0.5f);
        draw_circle_outline(LOCAL_PLAYER.ship.pos, radius, 40, PAL_F_SIGNAL_MINT, alpha);
        float shimmer = 0.5f + 0.5f * sinf(g.world.time * 13.0f);
        draw_circle_outline(LOCAL_PLAYER.ship.pos, radius * (0.94f + 0.04f * shimmer), 40,
                            PAL_F_SIGNAL_OPERATIONAL, alpha * 0.26f);
    } else if (LOCAL_PLAYER.ship.towed_count > 0) {
        /* LEASHED: beam lines to fragments. Towed fragments are already in
         * custody, so showing rarity here is intentional; brightness still
         * ramps with leash stretch so taut reads as urgent. */
        float slack = tr * 0.5f;
        float band = tr - slack;
        for (int t = 0; t < LOCAL_PLAYER.ship.towed_count; t++) {
            int idx = LOCAL_PLAYER.ship.towed_fragments[t];
            if (idx < 0 || idx >= MAX_ASTEROIDS || !g.world.asteroids[idx].active) continue;
            const asteroid_t *a = &g.world.asteroids[idx];
            vec2 fpos = a->pos;
            float dist = sqrtf(v2_dist_sq(LOCAL_PLAYER.ship.pos, fpos));
            float stretch = clampf((dist - slack) / band, 0.0f, 1.0f);
            float gr, gg, gb;
            grade_tint(a->grade, &gr, &gg, &gb);
            float boost = 1.0f + 0.5f * stretch;
            float beam_r = fminf(1.0f, gr * boost);
            float beam_g = fminf(1.0f, gg * boost);
            float beam_b = fminf(1.0f, gb * boost);
            float beam_a = 0.20f + 0.55f * stretch;
            draw_segment(LOCAL_PLAYER.ship.pos, fpos, beam_r, beam_g, beam_b, beam_a);
        }
    }
    world_signal_visual_leave_cue(cue_prev);
}

static float throw_preview_size_mult(const asteroid_t *a) {
    float size_mult = a ? a->radius / 30.0f : 1.0f;
    if (size_mult < 0.5f) size_mult = 0.5f;
    if (size_mult > 2.5f) size_mult = 2.5f;
    return size_mult;
}

static float throw_preview_damage_threshold(const asteroid_t *a) {
    return SHIP_COLLISION_DAMAGE_THRESHOLD * throw_preview_size_mult(a);
}

static bool throw_preview_for_fragment(const asteroid_t *a,
                                       vec2 *out_start,
                                       vec2 *out_dir,
                                       float *out_speed,
                                       float *out_hotness)
{
    if (!a || !out_start || !out_dir || !out_speed || !out_hotness)
        return false;
    vec2 to_ship = v2_sub(LOCAL_PLAYER.ship.pos, a->pos);
    float dist = v2_len(to_ship);
    vec2 release_dir = dist > 0.01f
        ? v2_scale(to_ship, 1.0f / dist)
        : v2_from_angle(LOCAL_PLAYER.ship.angle);
    float stretch = dist - SHIP_TOW_BAND_REST_LEN;
    if (stretch < 0.0f) stretch = 0.0f;
    float fling = ROCK_THROW_BASE_SPEED +
        sqrtf(SHIP_TOW_BAND_SPRING_K) * stretch;
    vec2 predicted_vel = v2_add(LOCAL_PLAYER.ship.vel,
                                v2_scale(release_dir, fling));
    float speed = v2_len(predicted_vel);
    vec2 dir = speed > 0.01f ? v2_scale(predicted_vel, 1.0f / speed)
                             : release_dir;
    float threshold = throw_preview_damage_threshold(a);
    float hotness = clampf((speed - threshold) / THROW_PREVIEW_HOT_RANGE,
                           0.0f, 1.0f);
    *out_start = a->pos;
    *out_dir = dir;
    *out_speed = speed;
    *out_hotness = hotness;
    return true;
}

static void draw_throw_arrow(vec2 start, vec2 dir, float speed, float hotness) {
    float len = 18.0f + clampf(speed / 4.0f, 0.0f, THROW_PREVIEW_MAX_LEN);
    vec2 end = v2_add(start, v2_scale(dir, len));
    vec2 side = v2(-dir.y, dir.x);
    float cold = 1.0f - hotness;
    float r = 0.48f * cold + 1.00f * hotness;
    float g = 0.52f * cold + 0.24f * hotness;
    float b = 0.54f * cold + 0.08f * hotness;
    float a = 0.22f + 0.72f * hotness;

    draw_segment(start, end, r, g, b, a);
    if (hotness > 0.0f)
        draw_segment(start, end, r, g * 0.78f, b * 0.65f, a * 0.45f);

    float head = 8.0f + 8.0f * hotness;
    vec2 back = v2_sub(end, v2_scale(dir, head));
    draw_segment(end, v2_add(back, v2_scale(side, head * 0.55f)),
                 r, g, b, a);
    draw_segment(end, v2_sub(back, v2_scale(side, head * 0.55f)),
                 r, g, b, a);
}

static void draw_throw_lock_bracket(vec2 pos, float radius, float hotness) {
    float half = radius + 10.0f + 4.0f * hotness;
    float arm = fminf(half * 0.45f, 18.0f);
    float pulse = 0.65f + 0.35f * sinf(g.world.time * 10.0f);
    float r = 1.0f;
    float g0 = 0.32f + 0.34f * (1.0f - hotness);
    float b = 0.10f;
    float a = (0.48f + 0.35f * hotness) * pulse;

    vec2 tl = v2(pos.x - half, pos.y - half);
    vec2 tr = v2(pos.x + half, pos.y - half);
    vec2 bl = v2(pos.x - half, pos.y + half);
    vec2 br = v2(pos.x + half, pos.y + half);
    draw_segment(tl, v2(tl.x + arm, tl.y), r, g0, b, a);
    draw_segment(tl, v2(tl.x, tl.y + arm), r, g0, b, a);
    draw_segment(tr, v2(tr.x - arm, tr.y), r, g0, b, a);
    draw_segment(tr, v2(tr.x, tr.y + arm), r, g0, b, a);
    draw_segment(bl, v2(bl.x + arm, bl.y), r, g0, b, a);
    draw_segment(bl, v2(bl.x, bl.y - arm), r, g0, b, a);
    draw_segment(br, v2(br.x - arm, br.y), r, g0, b, a);
    draw_segment(br, v2(br.x, br.y - arm), r, g0, b, a);
}

static void draw_throw_practice_target(vec2 start, vec2 dir, float speed,
                                       float hotness) {
    if (g.onboarding.threw || !g.onboarding.tractored) return;
    float dist = 180.0f + clampf(speed * 0.55f, 0.0f, 260.0f);
    vec2 pos = v2_add(start, v2_scale(dir, dist));
    float pulse = 0.65f + 0.35f * sinf(g.world.time * 7.0f);
    float a = 0.28f + 0.42f * pulse;
    float r = 0.36f + 0.44f * hotness;
    float g0 = 0.90f;
    float b = 0.84f - 0.30f * hotness;
    draw_circle_outline(pos, 32.0f, 28, r, g0, b, a);
    draw_circle_outline(pos, 12.0f, 20, r, g0, b, a * 0.80f);
    draw_segment(v2(pos.x - 48.0f, pos.y), v2(pos.x - 20.0f, pos.y),
                 r, g0, b, a);
    draw_segment(v2(pos.x + 20.0f, pos.y), v2(pos.x + 48.0f, pos.y),
                 r, g0, b, a);
    draw_segment(v2(pos.x, pos.y - 48.0f), v2(pos.x, pos.y - 20.0f),
                 r, g0, b, a);
    draw_segment(v2(pos.x, pos.y + 20.0f), v2(pos.x, pos.y + 48.0f),
                 r, g0, b, a);
}

static bool throw_preview_hits_target(vec2 start, vec2 dir, vec2 target,
                                      float radius, float max_dist)
{
    vec2 rel = v2_sub(target, start);
    float along = v2_dot(rel, dir);
    if (along <= 0.0f || along > max_dist) return false;
    vec2 closest = v2_add(start, v2_scale(dir, along));
    float miss_sq = v2_dist_sq(target, closest);
    float lock_r = radius + 22.0f;
    return miss_sq <= lock_r * lock_r;
}

static void draw_throw_locks(vec2 start, vec2 dir, float speed, float hotness) {
    if (hotness < THROW_LOCK_MIN_HOTNESS) return;
    float max_dist = fminf(THROW_LOCK_MAX_RANGE,
                           260.0f + speed * 1.45f);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == g.local_player_slot) continue;
        const server_player_t *sp = &g.world.players[i];
        if (!sp->connected || sp->docked) continue;
        float radius = ship_hull_def(&sp->ship)->ship_radius;
        if (throw_preview_hits_target(start, dir, sp->ship.pos,
                                      radius, max_dist))
            draw_throw_lock_bracket(sp->ship.pos, radius, hotness);
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *npc = &g.world.npc_ships[i];
        if (!npc->active || npc->state == NPC_STATE_DOCKED) continue;
        float radius = npc_hull_def(npc)->ship_radius;
        if (throw_preview_hits_target(start, dir, npc->ship.pos,
                                      radius, max_dist))
            draw_throw_lock_bracket(npc->ship.pos, radius, hotness);
    }
}

static void draw_throw_preview(void) {
    if (LOCAL_PLAYER.docked || LOCAL_PLAYER.ship.towed_count <= 0) return;

    float best_hotness = -1.0f;
    vec2 best_start = v2(0.0f, 0.0f);
    vec2 best_dir = v2(1.0f, 0.0f);
    float best_speed = 0.0f;

    for (int t = 0; t < LOCAL_PLAYER.ship.towed_count; t++) {
        int idx = LOCAL_PLAYER.ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
        const asteroid_t *a = &g.world.asteroids[idx];
        if (!a->active) continue;
        vec2 start, dir;
        float speed, hotness;
        if (!throw_preview_for_fragment(a, &start, &dir, &speed, &hotness))
            continue;
        draw_throw_arrow(start, dir, speed, hotness);
        if (hotness > best_hotness ||
            (hotness == best_hotness && speed > best_speed)) {
            best_hotness = hotness;
            best_start = start;
            best_dir = dir;
            best_speed = speed;
        }
    }

    if (best_hotness >= 0.0f) {
        draw_throw_practice_target(best_start, best_dir, best_speed,
                                   best_hotness);
        draw_throw_locks(best_start, best_dir, best_speed, best_hotness);
    }
}

void draw_ship(void) {
    /* While the death cinematic is rolling, the player ship is hidden —
     * we draw the wreckage at the death position via draw_death_wreckage. */
    if (g.death_cinematic.active) return;
    draw_throw_preview();
    float ship_sat_prev = world_signal_visual_enter_player_ship();
    sgl_push_matrix();
    sgl_translate(LOCAL_PLAYER.ship.pos.x, LOCAL_PLAYER.ship.pos.y, 0.0f);
    sgl_rotate(LOCAL_PLAYER.ship.angle, 0.0f, 0.0f, 1.0f);

    if (g.thrusting) {
        float flicker = 10.0f + sinf(g.world.time * 42.0f) * 3.0f;
        /* Flame color reads the ship's current situation:
         *   boost held      -> blue (exhaust is hotter, burning hull)
         *   frontier signal -> static white
         *   fringe signal   -> teal degradation
         *   core            -> warm gold/orange default
         * Boost wins - you can always see it even in a desert. */
        bool boost_on = g.input.key_down[SAPP_KEYCODE_LEFT_SHIFT]
                        || g.input.key_down[SAPP_KEYCODE_RIGHT_SHIFT];
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
        float fr, fg, fb;
        if (boost_on && !LOCAL_PLAYER.docked) {
            fr = 0.35f; fg = 0.80f; fb = 1.00f;
        } else if (sig < SIGNAL_BAND_FRONTIER) {
            fr = 0.82f; fg = 0.92f; fb = 1.00f;
        } else if (sig < SIGNAL_BAND_FRINGE) {
            fr = 0.18f; fg = 0.78f; fb = 0.72f;
        } else {
            fr = 1.00f; fg = 0.74f; fb = 0.24f;
        }
        float cue_prev = world_signal_visual_enter_cue();
        sgl_c4f(fr, fg, fb, 0.95f);
        sgl_begin_triangles();
        sgl_v2f(-12.0f, 0.0f);
        sgl_v2f(-26.0f - flicker, 6.0f);
        sgl_v2f(-26.0f - flicker, -6.0f);
        sgl_end();
        world_signal_visual_leave_cue(cue_prev);
    }

    /* Ship body tint: signal-blue player livery when empty, blending toward
     * the manifest's grade-weighted color as cargo fills. Same helper drives
     * NPC hulls and future inspect chips so rarity reads consistently. */
    const float hull_base_r = 0.30f, hull_base_g = 0.56f, hull_base_b = 0.64f;
    float tr = hull_base_r, tg = hull_base_g, tb = hull_base_b;
    {
        const ship_t *s = &LOCAL_PLAYER.ship;
        float cap   = ship_cargo_capacity(s);
        float total = ship_total_cargo(s);
        if (cap > 0.0f && total > 0.001f) {
            float fill = total / cap;
            (void)manifest_rarity_tint(&s->manifest, fill,
                                       hull_base_r, hull_base_g, hull_base_b,
                                       &tr, &tg, &tb);
        }
    }
    render_color4f_at(LOCAL_PLAYER.ship.pos, tr, tg, tb, 1.0f);
    sgl_begin_triangles();
    sgl_v2f(22.0f, 0.0f);
    sgl_v2f(-14.0f, 12.0f);
    sgl_v2f(-14.0f, -12.0f);
    sgl_end();

    render_color4f_at(LOCAL_PLAYER.ship.pos, 0.04f, 0.16f, 0.18f, 1.0f);
    sgl_begin_triangles();
    sgl_v2f(8.0f, 0.0f);
    sgl_v2f(-5.0f, 5.5f);
    sgl_v2f(-5.0f, -5.5f);
    sgl_end();

    draw_segment(v2(-9.0f, 8.0f), v2(-15.0f, 17.0f), 0.20f, 0.72f, 0.82f, 0.90f);
    draw_segment(v2(-9.0f, -8.0f), v2(-15.0f, -17.0f), 0.20f, 0.72f, 0.82f, 0.90f);

    sgl_pop_matrix();
    world_signal_visual_leave_cue(ship_sat_prev);
}

/* Death wreckage — drawn at the cinematic position when the player has
 * died. Burnt wreckage + 8 shards drifting outward, plus an ember
 * burst right after the impact. */
void draw_death_wreckage(void) {
    if (!g.death_cinematic.active) return;
    float age = g.death_cinematic.age;
    vec2 wp = g.death_cinematic.pos;
    float impact_speed = sqrtf(v2_len_sq(g.death_cinematic.vel));
    float severity = clampf(impact_speed / 260.0f, 0.7f, 2.2f);

    /* --- Initial explosion flare: long enough to read before fade/menu. --- */
    if (age < 1.45f) {
        float t = age / 1.45f;
        float flare = 1.0f - t;
        float r1 = 45.0f + severity * 18.0f + t * (120.0f + severity * 34.0f);
        draw_circle_filled(wp, r1 * 0.24f, 18, 1.0f, 0.38f, 0.12f, flare * 0.26f);
        draw_circle_outline(wp, r1, 32, 1.0f, 0.28f, 0.16f, flare * 0.90f);
        draw_circle_outline(wp, r1 * 0.62f, 24, 1.0f, 0.72f, 0.24f, flare * 0.65f);
        draw_circle_outline(wp, r1 * 0.32f, 18, 1.0f, 0.95f, 0.70f, flare * 0.45f);
        draw_spark_burst(wp, 1.3f + severity, true, 51.0f + severity * 3.1f);
    }

    /* Hot spiral trails make the impact readable even after the first flash. */
    float trail_alpha = clampf(1.0f - age / DEATH_CINEMATIC_WORLD_PHASE_SEC, 0.0f, 1.0f);
    if (trail_alpha > 0.01f) {
        for (int arm = 0; arm < 3; arm++) {
            float base = g.death_cinematic.angle + (float)arm * (2.0f * PI_F / 3.0f);
            float twist = -g.death_cinematic.spin * (0.18f + 0.05f * (float)arm);
            float a0 = base + twist;
            float a1 = base + twist * 2.1f + 0.9f;
            float len0 = 16.0f + severity * 5.0f;
            float len1 = 54.0f + severity * 18.0f;
            vec2 p0 = v2_add(wp, v2(cosf(a0) * len0, sinf(a0) * len0));
            vec2 p1 = v2_add(wp, v2(cosf(a1) * len1, sinf(a1) * len1));
            draw_segment(p0, p1, 1.0f, 0.22f, 0.08f, 0.40f * trail_alpha);
        }
    }

    /* --- Major hull breakup. Three larger pieces separate immediately,
     * so death does not read like an intact ship bouncing off a rock. */
    float chunk_alpha = 0.96f * clampf(1.0f - age / DEATH_CINEMATIC_FADE_TO_BLACK_SEC,
                                      0.18f, 1.0f);
    for (int piece = 0; piece < 3; piece++) {
        float *f = g.death_cinematic.fragments[piece];
        float sep = 0.36f + 0.08f * severity;
        vec2 cp = v2_add(wp, v2(f[0] * sep, f[1] * sep));
        float angle = g.death_cinematic.angle + f[4] * 0.45f +
                      ((piece == 1) ? 0.55f : (piece == 2) ? -0.55f : 0.0f);
        sgl_push_matrix();
        sgl_translate(cp.x, cp.y, 0.0f);
        sgl_rotate(angle, 0.0f, 0.0f, 1.0f);
        if (piece == 0) {
            sgl_c4f(0.18f, 0.11f, 0.07f, chunk_alpha);
            sgl_begin_triangles();
            sgl_v2f(16.0f, 0.0f);
            sgl_v2f(-2.0f, 7.0f);
            sgl_v2f(-5.0f, -6.0f);
            sgl_end();
            sgl_c4f(0.72f, 0.24f, 0.08f, chunk_alpha * 0.78f);
            sgl_begin_lines();
            sgl_v2f(16.0f, 0.0f); sgl_v2f(-2.0f, 7.0f);
            sgl_v2f(-2.0f, 7.0f); sgl_v2f(-5.0f, -6.0f);
            sgl_v2f(-5.0f, -6.0f); sgl_v2f(16.0f, 0.0f);
            sgl_end();
        } else {
            float side = (piece == 1) ? 1.0f : -1.0f;
            sgl_c4f(0.11f, 0.08f, 0.055f, chunk_alpha);
            sgl_begin_triangles();
            sgl_v2f(-1.0f, side * 4.0f);
            sgl_v2f(-20.0f, side * 19.0f);
            sgl_v2f(-13.0f, side * 3.0f);
            sgl_end();
            sgl_c4f(0.62f, 0.20f, 0.07f, chunk_alpha * 0.72f);
            sgl_begin_lines();
            sgl_v2f(-1.0f, side * 4.0f); sgl_v2f(-20.0f, side * 19.0f);
            sgl_v2f(-20.0f, side * 19.0f); sgl_v2f(-13.0f, side * 3.0f);
            sgl_end();
        }
        sgl_pop_matrix();
    }

    /* --- Shards drifting outward --- */
    for (int i = 0; i < 8; i++) {
        float *f = g.death_cinematic.fragments[i];
        float fx = wp.x + f[0];
        float fy = wp.y + f[1];
        sgl_push_matrix();
        sgl_translate(fx, fy, 0.0f);
        sgl_rotate(f[4], 0.0f, 0.0f, 1.0f);
        /* Burnt shards cool from ember-brown toward charcoal. */
        float fade = expf(-age * 0.22f);
        sgl_c4f(0.30f * fade, 0.16f * fade, 0.08f * fade, 0.92f);
        sgl_begin_triangles();
        float sz = 6.5f + severity * 1.4f + (float)(i % 3) * 1.1f;
        sgl_v2f(sz, 0.0f);
        sgl_v2f(-sz * 0.6f, sz * 0.7f);
        sgl_v2f(-sz * 0.6f, -sz * 0.7f);
        sgl_end();
        /* Faint trailing line */
        sgl_c4f(0.50f * fade, 0.18f * fade, 0.06f * fade, 0.42f);
        sgl_begin_lines();
        sgl_v2f(-sz * 0.6f, 0.0f);
        sgl_v2f(-sz * 2.0f, 0.0f);
        sgl_end();
        sgl_pop_matrix();
    }

    /* --- Smoldering embers near the wreckage --- */
    for (int i = 0; i < 5; i++) {
        float t = g.world.time;
        float seed = (float)i * 1.7f;
        float ang = t * 0.6f + seed;
        float r = 12.0f + 4.0f * sinf(t * 1.3f + seed);
        float ex = wp.x + cosf(ang) * r;
        float ey = wp.y + sinf(ang) * r - age * 6.0f; /* embers drift up */
        float a = 0.4f + 0.3f * sinf(t * 5.0f + seed * 3.0f);
        if (a < 0.0f) a = 0.0f;
        sgl_begin_triangles();
        sgl_c4f(1.0f, 0.55f, 0.15f, a);
        sgl_v2f(ex - 1.0f, ey - 1.0f);
        sgl_v2f(ex + 1.0f, ey - 1.0f);
        sgl_v2f(ex,        ey + 2.0f);
        sgl_end();
    }
}

void draw_npc_ship(const npc_ship_t* npc) {
    const hull_def_t* hull = npc_hull_def(npc);
    bool is_hauler = npc->ship.hull_class == HULL_CLASS_HAULER;
    float scale = hull->render_scale;
    /* NPC hulls keep cleaner, colder lines than the player ship while
     * still absorbing manifest rarity/cargo tint from the server. */
    float hull_r = lerpf(0.50f, npc->tint_r, 0.38f);
    float hull_g = lerpf(0.55f, npc->tint_g, 0.38f);
    float hull_b = lerpf(0.60f, npc->tint_b, 0.38f);

    (void)is_hauler;

    sgl_push_matrix();
    sgl_translate(npc->ship.pos.x, npc->ship.pos.y, 0.0f);
    sgl_rotate(npc->ship.angle, 0.0f, 0.0f, 1.0f);
    sgl_scale(scale, scale, 1.0f);

    if (npc->thrusting) {
        float flicker = 8.0f + sinf(g.world.time * 38.0f + npc->ship.pos.x) * 2.5f;
        sgl_c4f(1.0f, 0.6f, 0.15f, 0.9f);
        sgl_begin_triangles();
        sgl_v2f(-12.0f, 0.0f);
        sgl_v2f(-26.0f - flicker, 6.0f);
        sgl_v2f(-26.0f - flicker, -6.0f);
        sgl_end();
    }

    sgl_c4f(hull_r, hull_g, hull_b, 1.0f);
    sgl_begin_triangles();
    sgl_v2f(22.0f, 0.0f);
    sgl_v2f(-14.0f, 12.0f);
    sgl_v2f(-14.0f, -12.0f);
    sgl_end();

    sgl_c4f(hull_r * 0.3f, hull_g * 0.3f, hull_b * 0.3f, 1.0f);
    sgl_begin_triangles();
    sgl_v2f(8.0f, 0.0f);
    sgl_v2f(-5.0f, 5.5f);
    sgl_v2f(-5.0f, -5.5f);
    sgl_end();

    draw_segment(v2(-9.0f, 8.0f), v2(-15.0f, 17.0f), hull_r * 0.9f, hull_g * 0.8f, hull_b * 0.3f, 0.85f);
    draw_segment(v2(-9.0f, -8.0f), v2(-15.0f, -17.0f), hull_r * 0.9f, hull_g * 0.8f, hull_b * 0.3f, 0.85f);

    sgl_pop_matrix();
}

void draw_npc_mining_beam(const npc_ship_t* npc) {
    if (npc->state != NPC_STATE_MINING) return;
    if (npc->target_asteroid < 0) return;
    if (npc->target_asteroid >= MAX_ASTEROIDS) return;
    const asteroid_t* asteroid = &g.world.asteroids[npc->target_asteroid];
    if (!asteroid->active) return;

    vec2 forward = v2_from_angle(npc->ship.angle);
    vec2 muzzle = v2_add(npc->ship.pos, v2_scale(forward, npc_hull_def(npc)->ship_radius + 5.0f));
    vec2 hit;
    if (!sim_mining_target_hit(muzzle, forward, asteroid, &hit, NULL)) return;

    draw_segment(muzzle, hit, 0.92f, 0.68f, 0.28f, 0.85f);
    draw_segment(muzzle, hit, 0.45f, 0.30f, 0.10f, 0.35f);
}

void draw_npc_ships(void) {
    /* Scan-target ring: while the inspect snapshot is alive (active scan
     * or post-release linger) and aimed at an NPC, mark that NPC with a
     * pulsing green outline. Lets the player keep visual track of the
     * ship the snapshot pane is describing — especially during the
     * linger window when scan_active has gone false. */
    int scan_npc = -1;
    if (g.inspect_snapshot_timer > 0.0f
        && g.inspect_snapshot.target_type == INSPECT_TARGET_NPC
        && g.inspect_snapshot.target_index != 0xFFu
        && !g.death_cinematic.active) {
        scan_npc = (int)g.inspect_snapshot.target_index;
    }

    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!g.world.npc_ships[i].active) continue;
        if (!on_screen(g.world.npc_ships[i].ship.pos.x, g.world.npc_ships[i].ship.pos.y, 50.0f)) continue;
        draw_npc_ship(&g.world.npc_ships[i]);
        draw_npc_mining_beam(&g.world.npc_ships[i]);
        /* NPC tow tether */
        const npc_ship_t *tnpc = &g.world.npc_ships[i];
        int towed_fragment = npc_towed_fragment_index(tnpc);
        if (tnpc->role == NPC_ROLE_MINER && towed_fragment >= 0) {
            const asteroid_t *ta = &g.world.asteroids[towed_fragment];
            float tr = ship_tractor_range(&tnpc->ship);
            float d = ta->active ? v2_len(v2_sub(ta->pos, tnpc->ship.pos)) : 0.0f;
            if (ta->active && tr > 0.0f && d <= tr * 1.5f) {
                float tp = 0.4f + 0.15f * sinf(g.world.time * 3.0f + (float)i * 1.5f);
                draw_segment(tnpc->ship.pos, ta->pos, 0.7f, 0.5f, 0.2f, tp);
            }
        }
        if (i == scan_npc) {
            /* Hug the visible ship: ship_radius is the collision radius
             * and the rendered triangle sits within it, so a tight
             * fraction reads as "highlighting this ship" rather than
             * "drawing a halo around general space near it". Pulse is
             * subtle (±1 unit) so the radius stays visually stable. */
            float pulse = 0.5f + 0.5f * sinf(g.world.time * 6.0f);
            float ship_r = npc_hull_def(tnpc)->ship_radius;
            float r = ship_r * 0.7f + 2.0f + 1.0f * pulse;
            float a = 0.65f + 0.20f * pulse;
            /* Fade with the linger timer so the ring decays in sync
             * with the panel rather than vanishing abruptly. */
            float decay = g.inspect_snapshot_timer < 1.0f
                          ? g.inspect_snapshot_timer : 1.0f;
            draw_circle_outline(tnpc->ship.pos, r, 28,
                                0.20f, 0.95f, 0.45f, a * decay);
        }
    }
}

static void draw_cargo_pod_module_tractor_beam(vec2 anchor,
                                               vec2 pod_pos,
                                               const cargo_pod_t *pod,
                                               commodity_t commodity,
                                               float intensity,
                                               int seed) {
    float radius = (pod && pod->radius > 0.0f) ? pod->radius : 18.0f;
    if (!on_screen(pod_pos.x, pod_pos.y, radius + 80.0f) &&
        !on_screen(anchor.x, anchor.y, 80.0f)) {
        return;
    }

    float cr = 0.78f, cg = 0.60f, cb = 0.30f;
    if (commodity < COMMODITY_COUNT)
        commodity_color(commodity, &cr, &cg, &cb);

    float t = clampf(intensity, 0.0f, 1.0f);
    if (t <= 0.0f) return;

    vec2 beam_vec = v2_sub(pod_pos, anchor);
    float beam_len = sqrtf(v2_len_sq(beam_vec));
    vec2 dir = beam_len > 0.001f
        ? v2_scale(beam_vec, 1.0f / beam_len)
        : v2(1.0f, 0.0f);
    vec2 tug_pos = anchor;
    if (beam_len > 12.0f) {
        vec2 perp = v2(-dir.y, dir.x);
        float tug_standoff = fminf(radius + 36.0f, beam_len * 0.46f);
        float bob = sinf(g.world.time * 4.6f + (float)seed * 0.73f) * 5.0f;
        tug_pos = v2_add(pod_pos,
                         v2_add(v2_scale(dir, -tug_standoff),
                                v2_scale(perp, bob)));
    }

    float tug_alpha = 0.54f + 0.28f * t;
    float tug_angle = atan2f(dir.y, dir.x);
    sgl_push_matrix();
    sgl_translate(tug_pos.x, tug_pos.y, 0.0f);
    sgl_rotate(tug_angle, 0.0f, 0.0f, 1.0f);
    sgl_c4f(cr * 0.42f + 0.22f, cg * 0.42f + 0.24f,
            cb * 0.42f + 0.26f, tug_alpha);
    sgl_begin_triangles();
    sgl_v2f(12.0f, 0.0f);
    sgl_v2f(-8.0f, 6.5f);
    sgl_v2f(-8.0f, -6.5f);
    sgl_end();
    sgl_c4f(fminf(1.0f, cr * 1.35f),
            fminf(1.0f, cg * 1.25f),
            fminf(1.0f, cb * 1.25f), tug_alpha * 0.82f);
    sgl_begin_lines();
    sgl_v2f(12.0f, 0.0f); sgl_v2f(-8.0f, 6.5f);
    sgl_v2f(-8.0f, 6.5f); sgl_v2f(-8.0f, -6.5f);
    sgl_v2f(-8.0f, -6.5f); sgl_v2f(12.0f, 0.0f);
    sgl_end();
    sgl_pop_matrix();

    float pulse = 0.50f + 0.24f *
        sinf(g.world.time * 7.0f + (float)seed * 1.37f);
    float zap = sinf(g.world.time * 41.0f + (float)seed * 5.9f);
    vec2 mid = v2_scale(v2_add(tug_pos, pod_pos), 0.5f);
    vec2 perp = v2(-(pod_pos.y - tug_pos.y), pod_pos.x - tug_pos.x);
    float plen = sqrtf(v2_len_sq(perp));
    if (plen > 0.1f)
        perp = v2_scale(perp, 3.0f * zap / plen);
    mid = v2_add(mid, perp);
    float alpha = (0.24f + 0.42f * t) * pulse;
    draw_segment(tug_pos, mid, cr, cg, cb, alpha);
    draw_segment(mid, pod_pos, cr, cg, cb, alpha);
    draw_segment(tug_pos, pod_pos,
                 fminf(1.0f, cr * 1.4f),
                 fminf(1.0f, cg * 1.3f),
                 fminf(1.0f, cb * 1.3f),
                 alpha * 0.42f);
}

static bool draw_published_cargo_pod_module_tractors(void) {
    bool drew = false;
    for (int i = 0; i < g.world.interactions.count; i++) {
        const sim_interaction_t *it = &g.world.interactions.items[i];
        if (it->type != SIM_INTERACTION_TRACTOR_BEAM ||
            it->visual != SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR ||
            it->target.type != SIM_INTERACTION_ENTITY_CARGO_POD) {
            continue;
        }

        int pod_idx = it->target.index;
        if (pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) continue;
        const cargo_pod_t *pod = &g.world.cargo_pods[pod_idx];
        if (!pod->active) continue;
        if (it->intensity <= 0.0f) continue;

        commodity_t commodity = it->commodity < COMMODITY_COUNT
            ? (commodity_t)it->commodity
            : pod->commodity;
        draw_cargo_pod_module_tractor_beam(
            it->source_pos, pod->pos, pod, commodity,
            it->intensity, pod_idx);
        drew = true;
    }
    return drew;
}

/* Draw furnace tractor beams: orange tendrils to nearby S-tier fragments,
 * plus cargo-pod tractor beams from typed hoppers/producers. */
void draw_hopper_tractors(void) {
    float pull_range = 300.0f;
    float pull_sq = pull_range * pull_range;
    const asteroid_render_lists_t *lists = asteroid_render_lists();
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st) || st->scaffold) continue;
        float station_pull_radius =
            fmaxf(st->dock_radius, STATION_RING_RADIUS[STATION_NUM_RINGS]) + pull_range + 80.0f;
        if (!on_screen(st->pos.x, st->pos.y, station_pull_radius)) continue;
        for (int m = 0; m < st->module_count; m++) {
            if (st->modules[m].scaffold) continue;
            module_type_t mt = st->modules[m].type;
            if (mt != MODULE_FURNACE && mt != MODULE_HOPPER) continue;
            vec2 mp = module_world_pos_ring(st, st->modules[m].ring, st->modules[m].slot);
            if (!on_screen(mp.x, mp.y, pull_range + 50.0f)) continue;

            float fr, fg, fb;
            module_color(mt, &fr, &fg, &fb);

            /* Draw orange tractor tendrils to all S-tier fragments in range */
            for (int li = 0; li < lists->s_tier_count; li++) {
                int i = lists->s_tier[li];
                const asteroid_t *a = &g.world.asteroids[i];
                float d_sq = v2_dist_sq(a->pos, mp);
                if (d_sq > pull_sq) continue;
                float d = sqrtf(d_sq);
                float t = 1.0f - d / pull_range;
                float pulse = 0.5f + 0.3f * sinf(g.world.time * 6.0f + (float)i * 1.7f);

                /* Zappy tractor tendril — jittery, electrical feel */
                float brightness = (a->smelt_progress > 0.01f) ? (0.6f + a->smelt_progress * 0.4f) : 0.3f;
                float zap = sinf(g.world.time * 37.0f + (float)i * 5.3f);
                float jitter = 4.0f * zap;
                vec2 mid = v2_scale(v2_add(mp, a->pos), 0.5f);
                vec2 perp = v2(-((a->pos.y - mp.y)), (a->pos.x - mp.x));
                float plen = sqrtf(v2_len_sq(perp));
                if (plen > 0.1f) perp = v2_scale(perp, jitter / plen);
                vec2 mid_jitter = v2_add(mid, perp);
                /* Two-segment zap line through jittered midpoint */
                draw_segment(mp, mid_jitter, fr, fg, fb, t * pulse * brightness);
                draw_segment(mid_jitter, a->pos, fr, fg, fb, t * pulse * brightness);
                /* Hot core line — straighter, brighter */
                draw_segment(mp, a->pos, 1.0f, 0.85f, 0.4f, t * pulse * brightness * 0.3f);

                /* Sparks on smelting fragments — more intense */
                if (a->smelt_progress > 0.1f) {
                    float spark1 = sinf(g.world.time * 31.0f + (float)i * 3.1f);
                    float spark2 = sinf(g.world.time * 43.0f + (float)i * 7.3f);
                    float spark3 = sinf(g.world.time * 19.0f + (float)i * 2.7f);
                    float sr = a->radius * 1.2f;
                    float sp = a->smelt_progress;
                    if (spark1 > 0.0f) {
                        vec2 s1 = v2_add(a->pos, v2(sr * sinf(g.world.time * 11.0f), sr * cosf(g.world.time * 13.0f)));
                        draw_segment(a->pos, s1, 1.0f, 0.9f, 0.3f, spark1 * sp * 0.7f);
                    }
                    if (spark2 > 0.0f) {
                        vec2 s2 = v2_add(a->pos, v2(-sr * cosf(g.world.time * 9.0f), sr * sinf(g.world.time * 7.0f)));
                        draw_segment(a->pos, s2, 1.0f, 0.7f, 0.15f, spark2 * sp * 0.5f);
                    }
                    if (spark3 > 0.0f) {
                        vec2 s3 = v2_add(a->pos, v2(sr * cosf(g.world.time * 17.0f), -sr * sinf(g.world.time * 23.0f)));
                        draw_segment(a->pos, s3, 0.9f, 0.5f, 0.1f, spark3 * sp * 0.4f);
                    }
                }
            }
        }
    }

    (void)draw_published_cargo_pod_module_tractors();
}

/* ------------------------------------------------------------------ */
/* Spark burst — short, jittery streaks at a contact point.            */
/* Used by the mining laser impact and ship collisions. The "seed"     */
/* parameter de-correlates per-call patterns so two simultaneous       */
/* bursts (e.g. beam + crash) don't pulse in lockstep.                 */
/* ------------------------------------------------------------------ */

static float hash11(float x) {
    /* Cheap deterministic noise in [-1, 1]: take the fractional part of
     * a chaotic sine, remap [0,1) -> [-1, 1). */
    float s = sinf(x * 127.1f + 311.7f) * 43758.5453f;
    float f = s - floorf(s);          /* [0, 1) */
    return f * 2.0f - 1.0f;           /* [-1, 1) */
}

void draw_spark_burst(vec2 pos, float intensity, bool red, float seed) {
    if (intensity <= 0.01f) return;
    /* Intensity > 1 grows the burst (used for damaging-velocity hits). */
    if (intensity > 2.5f) intensity = 2.5f;
    float scale = intensity > 1.0f ? intensity : 1.0f;
    float t = g.world.time;
    float bucket = floorf(t * 32.0f) + seed * 71.3f;

    /* Hot core cross — 3 very short rays */
    float core_r = red ? 1.0f : 1.0f;
    float core_g = red ? 0.42f : 0.95f;
    float core_b = red ? 0.18f : 0.78f;
    for (int k = 0; k < 3; k++) {
        float ang = hash11(bucket + (float)k * 3.7f) * PI_F;
        float len = (1.5f + 1.5f * fabsf(hash11(bucket * 1.3f + (float)k * 5.1f))) * scale;
        vec2 tip = v2_add(pos, v2(cosf(ang) * len, sinf(ang) * len));
        draw_segment(pos, tip, core_r, core_g, core_b, 0.85f * fminf(intensity, 1.0f));
    }

    /* Main spark plume — 6 streaks (8 when overdriven), tight radius */
    int streaks = (intensity > 1.0f) ? 8 : 6;
    for (int k = 0; k < streaks; k++) {
        float kseed = bucket + (float)k * 2.71f + seed;
        float gate = hash11(kseed * 0.91f);
        if (gate < 0.0f) continue; /* drop ~50% of streaks each frame */
        float ang = hash11(kseed) * PI_F;
        float len = (2.0f + 4.0f * fabsf(hash11(kseed * 1.7f))) * scale;
        vec2 tip = v2_add(pos, v2(cosf(ang) * len, sinf(ang) * len));
        float r = red ? 1.0f : 1.0f;
        float g = red ? (0.45f + 0.2f * fabsf(hash11(kseed * 0.5f))) : 0.85f;
        float b = red ? 0.15f : (0.25f + 0.3f * fabsf(hash11(kseed * 0.7f)));
        float a = (0.55f + 0.35f * gate) * fminf(intensity, 1.0f);
        draw_segment(pos, tip, r, g, b, a);
    }
}

static void local_player_beam_render_line(vec2 *beam_start, vec2 *beam_end) {
    *beam_start = LOCAL_PLAYER.beam_start;
    *beam_end = LOCAL_PLAYER.beam_end;

    if (!g.net_authority_enabled) {
        return;
    }

    /* The local network-authoritative ship can be drawn with a visual-only
     * reconciliation offset in render_frame(). Anchor the beam to that
     * same render pose while leaving authoritative hit endpoints alone. */
    *beam_start = ship_muzzle(LOCAL_PLAYER.ship.pos,
                              LOCAL_PLAYER.ship.angle,
                              &LOCAL_PLAYER.ship);
    if (!LOCAL_PLAYER.beam_hit) {
        float beam_len = v2_len(v2_sub(LOCAL_PLAYER.beam_end,
                                       LOCAL_PLAYER.beam_start));
        if (beam_len < 1.0f) beam_len = MINING_RANGE;
        vec2 forward = v2_from_angle(LOCAL_PLAYER.ship.angle);
        *beam_end = v2_add(*beam_start, v2_scale(forward, beam_len));
    }
}

void draw_beam(void) {
    if (g.death_cinematic.active) return;
    if (!LOCAL_PLAYER.beam_active) {
        return;
    }

    vec2 beam_start;
    vec2 beam_end;
    local_player_beam_render_line(&beam_start, &beam_end);

    float cue_prev = world_signal_visual_enter_cue();
    if (LOCAL_PLAYER.scan_active) {
        /* Scan beam: cyan/blue — information, not damage */
        draw_segment(beam_start, beam_end, 0.30f, 0.70f, 1.0f, 0.90f);
        draw_segment(beam_start, beam_end, 0.15f, 0.50f, 0.90f, 0.35f);
    } else if (LOCAL_PLAYER.beam_hit && LOCAL_PLAYER.beam_ineffective) {
        /* Red beam: hitting a rock too tough for current laser */
        draw_segment(beam_start, beam_end, 1.0f, 0.2f, 0.15f, 0.85f);
        draw_segment(beam_start, beam_end, 0.8f, 0.1f, 0.05f, 0.30f);
    } else if (LOCAL_PLAYER.beam_hit) {
        /* Normal mining beam: teal */
        draw_segment(beam_start, beam_end, 0.45f, 1.0f, 0.92f, 0.95f);
        draw_segment(beam_start, beam_end, 0.12f, 0.78f, 1.0f, 0.35f);
    } else {
        /* Beam into empty space */
        draw_segment(beam_start, beam_end, 0.9f, 0.75f, 0.30f, 0.55f);
    }

    /* Impact sparks at the beam contact point. */
    if (LOCAL_PLAYER.beam_hit) {
        bool is_station = LOCAL_PLAYER.scan_active &&
            LOCAL_PLAYER.scan_target_type == 1;
        bool is_asteroid = !LOCAL_PLAYER.scan_active;
        if (is_asteroid) {
            draw_spark_burst(beam_end,
                             LOCAL_PLAYER.beam_ineffective ? 0.7f : 1.0f,
                             LOCAL_PLAYER.beam_ineffective,
                             3.14f);
        } else if (is_station) {
            /* Lasering a station/module — hot orange metal sparks. */
            draw_spark_burst(beam_end, 0.9f, true, 9.7f);
        }
    }
    world_signal_visual_leave_cue(cue_prev);
}

/* ------------------------------------------------------------------ */
/* Collision sparks — emit a burst at any point where the local ship  */
/* hull is currently in contact with an asteroid or station body.     */
/* Pure visual; the server is authoritative for damage.               */
/* ------------------------------------------------------------------ */

void draw_collision_sparks(void) {
    if (LOCAL_PLAYER.docked) return;
    float cue_prev = world_signal_visual_enter_cue();
    vec2 sp = LOCAL_PLAYER.ship.pos;
    vec2 sv = LOCAL_PLAYER.ship.vel;
    float ship_r = ship_hull_def(&LOCAL_PLAYER.ship)->ship_radius;
    /* Only spark on actual hull contact (no slack). */
    const float pad = 0.0f;

    /* Pick the single deepest asteroid contact this frame and the deepest
     * station contact — avoids the screen-filling cluster effect when
     * threading through a tight rock field. */
    int best_a = -1;
    float best_a_overlap = 0.0f;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &g.world.asteroids[i];
        if (!a->active) continue;
        float reach = ship_r + a->radius + pad;
        vec2 d = v2_sub(a->pos, sp);
        float d_sq = v2_len_sq(d);
        if (d_sq >= reach * reach) continue;
        float overlap = reach - sqrtf(d_sq);
        if (overlap > best_a_overlap) { best_a_overlap = overlap; best_a = i; }
    }
    if (best_a >= 0) {
        const asteroid_t *a = &g.world.asteroids[best_a];
        vec2 d = v2_sub(a->pos, sp);
        float dist = sqrtf(v2_len_sq(d));
        if (dist > 0.01f) {
            vec2 normal = v2_scale(d, 1.0f / dist);
            vec2 rel = v2_sub(sv, a->vel);
            float closing = v2_dot(rel, normal);
            /* SHIP_COLLISION_DAMAGE_THRESHOLD = 115. Below = scrape, above
             * = damaging hit, where the burst grows past 1.0 intensity. */
            float intensity;
            if (closing < 115.0f)
                intensity = 0.30f + fmaxf(0.0f, closing) * (0.7f / 115.0f);
            else
                intensity = 1.0f + fminf((closing - 115.0f) / 100.0f, 1.5f);
            vec2 contact = v2_add(sp, v2_scale(normal, ship_r - 1.0f));
            draw_spark_burst(contact, intensity, false, (float)best_a * 0.37f);
        }
    }

    int best_s = -1;
    float best_s_overlap = 0.0f;
    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t *st = &g.world.stations[i];
        if (!station_exists(st)) continue;
        if (st->planned) continue;
        float reach = ship_r + st->radius + pad;
        vec2 d = v2_sub(st->pos, sp);
        float d_sq = v2_len_sq(d);
        if (d_sq >= reach * reach) continue;
        float overlap = reach - sqrtf(d_sq);
        if (overlap > best_s_overlap) { best_s_overlap = overlap; best_s = i; }
    }
    if (best_s >= 0) {
        const station_t *st = &g.world.stations[best_s];
        vec2 d = v2_sub(st->pos, sp);
        float dist = sqrtf(v2_len_sq(d));
        if (dist > 0.01f) {
            vec2 normal = v2_scale(d, 1.0f / dist);
            float closing = v2_dot(sv, normal);
            float intensity;
            if (closing < 115.0f)
                intensity = 0.40f + fmaxf(0.0f, closing) * (0.6f / 115.0f);
            else
                intensity = 1.0f + fminf((closing - 115.0f) / 100.0f, 1.5f);
            vec2 contact = v2_add(sp, v2_scale(normal, ship_r - 1.0f));
            draw_spark_burst(contact, intensity, true, (float)best_s * 1.13f + 17.0f);
        }
    }
    world_signal_visual_leave_cue(cue_prev);
}

/* Draw autopilot path preview: dotted line from ship through next waypoints.
 * Only draws one screen-width worth (~1200u) so it doesn't clutter. */
void draw_autopilot_path(void) {
    if (!LOCAL_PLAYER.autopilot_mode) return;

    /* Under network authority, the server syncs its actual A* path waypoints via
     * PLAYER_SHIP message. g.autopilot_path is already populated by
     * apply_remote_player_ship in net_sync.c. No client computation. */

    if (g.autopilot_path_count == 0) return;
    float cue_prev = world_signal_visual_enter_cue();
    vec2 prev = LOCAL_PLAYER.ship.pos;
    float total_drawn = 0.0f;
    const float MAX_DRAW_DIST = 1200.0f;
    const float DASH_LEN = 20.0f;
    const float GAP_LEN = 15.0f;
    for (int i = g.autopilot_path_current; i < g.autopilot_path_count; i++) {
        vec2 wp = g.autopilot_path[i];
        vec2 delta = v2_sub(wp, prev);
        float seg_len = v2_len(delta);
        if (seg_len < 1.0f) { prev = wp; continue; }
        float remaining = MAX_DRAW_DIST - total_drawn;
        if (remaining <= 0.0f) break;
        if (seg_len > remaining) seg_len = remaining;
        vec2 dir = v2_scale(delta, 1.0f / v2_len(delta));
        /* Draw dashed line along this segment */
        float t = 0.0f;
        float pulse = 0.35f + 0.15f * sinf(g.world.time * 2.0f);
        sgl_begin_lines();
        sgl_c4f(0.3f, 0.85f, 1.0f, pulse);
        while (t < seg_len) {
            float dash_end = t + DASH_LEN;
            if (dash_end > seg_len) dash_end = seg_len;
            vec2 a = v2_add(prev, v2_scale(dir, t));
            vec2 b = v2_add(prev, v2_scale(dir, dash_end));
            sgl_v2f(a.x, a.y);
            sgl_v2f(b.x, b.y);
            t = dash_end + GAP_LEN;
        }
        sgl_end();
        /* Small dot at waypoint */
        draw_circle_filled(wp, 3.0f, 6, 0.3f, 0.85f, 1.0f, pulse * 1.2f);
        total_drawn += seg_len;
        prev = wp;
    }
    world_signal_visual_leave_cue(cue_prev);
}

/* Draw tractor tether lines from ship to towed fragments. Tethers show
 * rarity because the fragment has already been collected/towed. */
void draw_towed_tethers(void) {
    if (g.death_cinematic.active) return;
    if (LOCAL_PLAYER.ship.towed_count == 0) return;
    float cue_prev = world_signal_visual_enter_cue();
    for (int t = 0; t < LOCAL_PLAYER.ship.towed_count; t++) {
        int idx = LOCAL_PLAYER.ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
        const asteroid_t *a = &g.world.asteroids[idx];
        if (!a->active) continue;
        float r, gg, b;
        grade_tint(a->grade, &r, &gg, &b);
        float pulse = 0.4f + 0.15f * sinf(g.world.time * 3.0f + (float)t * 1.5f);
        if (a->grade >= (uint8_t)MINING_GRADE_RARE)
            pulse += 0.12f * sinf(g.world.time * 7.0f + (float)t);
        draw_segment(LOCAL_PLAYER.ship.pos, a->pos, r, gg, b, pulse);
    }
    world_signal_visual_leave_cue(cue_prev);
}

/* --- Compass ring: navigation pips around the player ship --- */
/* Resolve the world-space target the player should go to next for the
 * currently tracked contract. The objective module owns source/destination
 * choice so SIGNAL text, compass pips, and the world ring cannot drift. */
static bool resolve_tracked_contract_target(vec2 *out_pos, float *out_radius,
                                            contract_objective_target_kind_t *out_kind) {
    contract_objective_t objective;
    if (!contract_objective_for_tracked(&objective)) return false;
    if (!objective.has_world_target) return false;
    *out_pos = objective.world_target;
    *out_radius = objective.world_radius;
    if (out_kind) *out_kind = objective.target_kind;
    return true;
}

static void contract_target_color(contract_objective_target_kind_t kind,
                                  float *r, float *g0, float *b) {
    (void)kind;
    *r = 1.00f;
    *g0 = 0.87f;
    *b = 0.20f;
}

/* In-world pulsing ring at the tracked contract's current next objective. */
void draw_tracked_contract_highlight(void) {
    vec2 target; float radius;
    contract_objective_target_kind_t kind = CONTRACT_OBJECTIVE_TARGET_NONE;
    if (!resolve_tracked_contract_target(&target, &radius, &kind)) return;
    if (!on_screen(target.x, target.y, radius + 40.0f)) return;
    float t = g.world.time;
    float pulse = 0.5f + 0.5f * sinf(t * 2.4f);
    float cr, cg, cb;
    contract_target_color(kind, &cr, &cg, &cb);
    float cue_prev = world_signal_visual_enter_cue();
    float a = 0.70f + 0.22f * pulse;
    float reticle_r = 8.0f + 1.5f * pulse;
    draw_circle_outline(target, reticle_r, 14, cr, cg, cb, a);
    draw_circle_filled(target, 2.0f, 8, cr, cg, cb, a * 0.75f);
    sgl_begin_lines();
    sgl_c4f(cr, cg, cb, a * 0.82f);
    float inner = 14.0f;
    float outer = 22.0f;
    sgl_v2f(target.x - outer, target.y); sgl_v2f(target.x - inner, target.y);
    sgl_v2f(target.x + inner, target.y); sgl_v2f(target.x + outer, target.y);
    sgl_v2f(target.x, target.y - outer); sgl_v2f(target.x, target.y - inner);
    sgl_v2f(target.x, target.y + inner); sgl_v2f(target.x, target.y + outer);
    sgl_end();
    world_signal_visual_leave_cue(cue_prev);
}

void draw_compass_ring(void) {
    if (LOCAL_PLAYER.docked) return;
    float cue_prev = world_signal_visual_enter_cue();
    vec2 ship = LOCAL_PLAYER.ship.pos;
    float ring_r = 120.0f;
    float pip_size = 8.0f;

    /* Faint ring outline */
    draw_circle_outline(ship, ring_r, 32, 0.25f, 0.27f, 0.30f, 0.07f);

    /* Local callsign rendered with sdtx — see draw_callsigns() pass below. */

    /* Helper: draw a chevron pip at position on the ring */
    #define COMPASS_PIP(target, pr, pg, pb) do { \
        vec2 _to = v2_sub(target, ship); \
        float _dsq = v2_len_sq(_to); \
        if (_dsq > 2500.0f) { \
            float _a = atan2f(_to.y, _to.x); \
            float _px = ship.x + cosf(_a) * ring_r; \
            float _py = ship.y + sinf(_a) * ring_r; \
            float _ca = cosf(_a), _sa = sinf(_a); \
            float _pulse = 0.6f + 0.3f * sinf(g.world.time * 3.0f); \
            sgl_begin_lines(); sgl_c4f(pr, pg, pb, _pulse); \
            sgl_v2f(_px+(-_ca*pip_size-_sa*pip_size*0.6f), _py+(-_sa*pip_size+_ca*pip_size*0.6f)); sgl_v2f(_px, _py); \
            sgl_v2f(_px, _py); sgl_v2f(_px+(-_ca*pip_size+_sa*pip_size*0.6f), _py+(-_sa*pip_size-_ca*pip_size*0.6f)); \
            sgl_end(); \
        } \
    } while(0)

    /* Nearest station pip (green) */
    {
        const station_t *nav = navigation_station_ptr();
        if (nav) COMPASS_PIP(nav->pos, 0.34f, 0.96f, 0.76f);
    }

    /* Nav pip (yellow, blueprint placement) */
    if (g.nav_pip_active && g.nav_pip_is_blueprint)
        COMPASS_PIP(g.nav_pip_pos, 1.0f, 0.87f, 0.20f);

    /* Nearest minable asteroid pip (red) — filtered by mining level
     * so the pip matches what the autopilot would target. */
    {
        float best_d = 1e18f;
        vec2 best_pos = ship;
        bool found = false;
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            const asteroid_t *a = &g.world.asteroids[i];
            if (!mining_level_can_fracture_asteroid(LOCAL_PLAYER.ship.mining_level, a))
                continue;
            float d = v2_dist_sq(a->pos, ship);
            if (d < best_d) { best_d = d; best_pos = a->pos; found = true; }
        }
        if (found) COMPASS_PIP(best_pos, 0.9f, 0.25f, 0.2f);
    }

    /* Tracked contract pip. Uses the same resolver/color policy as the
     * in-world ring so quota work cannot masquerade as a specific asteroid
     * target. */
    {
        vec2 target;
        float radius;
        contract_objective_target_kind_t kind = CONTRACT_OBJECTIVE_TARGET_NONE;
        if (resolve_tracked_contract_target(&target, &radius, &kind)) {
            float cr, cg, cb;
            contract_target_color(kind, &cr, &cg, &cb);
            COMPASS_PIP(target, cr, cg, cb);
        }
    }

    /* Nearest 3 remote players (colored pips) */
    if (g.net_authority_enabled) {
        const NetPlayerState *rp = net_get_interpolated_players();
        int nearest[3] = {-1, -1, -1};
        float nearest_d[3] = {1e18f, 1e18f, 1e18f};
        for (int i = 0; i < NET_MAX_PLAYERS; i++) {
            if (!rp[i].active || i == (int)net_local_id()) continue;
            float d = v2_dist_sq(v2(rp[i].x, rp[i].y), ship);
            for (int s = 0; s < 3; s++) {
                if (d < nearest_d[s]) {
                    for (int j = 2; j > s; j--) { nearest[j] = nearest[j-1]; nearest_d[j] = nearest_d[j-1]; }
                    nearest[s] = i; nearest_d[s] = d; break;
                }
            }
        }
        static const float pcols[][3] = {
            {1.0f, 0.45f, 0.25f}, {0.25f, 1.0f, 0.55f}, {0.55f, 0.35f, 1.0f},
            {1.0f, 0.85f, 0.15f}, {0.15f, 0.85f, 1.0f}, {1.0f, 0.35f, 0.75f},
        };
        for (int s = 0; s < 3; s++) {
            int pi = nearest[s];
            if (pi < 0) continue;
            int ci = pi % 6;
            bool scanned = net_remote_player_scanned(pi);
            COMPASS_PIP(v2(rp[pi].x, rp[pi].y),
                        scanned ? pcols[ci][0] : 0.45f,
                        scanned ? pcols[ci][1] : 0.50f,
                        scanned ? pcols[ci][2] : 0.56f);
        }
    }

    #undef COMPASS_PIP
    world_signal_visual_leave_cue(cue_prev);
}

/* --- Multiplayer: draw remote players as colored triangles --- */
void draw_remote_players(void) {
    if (!g.net_authority_enabled) return;
    const NetPlayerState* players = net_get_interpolated_players();
    net_update_remote_player_scans(players);
    static const float colors[][3] = {
        {1.0f, 0.45f, 0.25f},
        {0.25f, 1.0f, 0.55f},
        {0.55f, 0.35f, 1.0f},
        {1.0f, 0.85f, 0.15f},
        {0.15f, 0.85f, 1.0f},
        {1.0f, 0.35f, 0.75f},
    };
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (!players[i].active) continue;
        /* Skip the local player — belt-and-suspenders: index match OR
         * slot match OR colocated with the actual ship pos. The last
         * guard catches the window between connect and JOIN arrival
         * where neither id is populated yet and we'd otherwise draw
         * a red/orange ghost ship on top of the local one. */
        if (i == (int)net_local_id()) continue;
        if (i == g.local_player_slot) continue;
        {
            float dx = players[i].x - LOCAL_PLAYER.ship.pos.x;
            float dy = players[i].y - LOCAL_PLAYER.ship.pos.y;
            if (dx * dx + dy * dy < 4.0f) continue; /* within 2u = us */
        }
        bool scanned = net_remote_player_scanned(i);
        int ci = i % 6;
        float cr = colors[ci][0], cg = colors[ci][1], cb = colors[ci][2];
        if (!scanned) {
            cr = 0.35f;
            cg = 0.41f;
            cb = 0.47f;
        }
        bool thrusting = (players[i].flags & 1) != 0;
        bool mining = (players[i].flags & 2) != 0;
        bool tractor_on = (players[i].flags & 16) != 0;
        /* Compute tractor range from level (mirrors ship_tractor_range).
         * Remote snapshots don't carry hull class yet; networked player
         * ships are currently the default miner hull. */
        float base_range = 150.0f;
        float tr = base_range + (float)players[i].tractor_level * SHIP_TRACTOR_UPGRADE_STEP;
        float render_radius = tractor_on ? tr : 50.0f;
        if (players[i].towed_count > 0 && render_radius < tr * 1.5f)
            render_radius = tr * 1.5f;
        if (!on_screen(players[i].x, players[i].y, render_radius)) continue;

        sgl_push_matrix();
        sgl_translate(players[i].x, players[i].y, 0.0f);
        sgl_rotate(players[i].angle, 0.0f, 0.0f, 1.0f);

        /* Thrust flame */
        if (thrusting) {
            float flicker = 10.0f + sinf(g.world.time * 42.0f + (float)i * 7.0f) * 3.0f;
            sgl_c4f(1.0f, 0.74f, 0.24f, 0.9f);
            sgl_begin_triangles();
            sgl_v2f(-12.0f, 0.0f);
            sgl_v2f(-26.0f - flicker, 6.0f);
            sgl_v2f(-26.0f - flicker, -6.0f);
            sgl_end();
        }

        /* Hull */
        sgl_c4f(cr, cg, cb, 0.9f);
        sgl_begin_triangles();
        sgl_v2f(22.0f, 0.0f);
        sgl_v2f(-14.0f, 12.0f);
        sgl_v2f(-14.0f, -12.0f);
        sgl_end();

        /* Cockpit */
        sgl_c4f(cr * 0.3f, cg * 0.3f, cb * 0.3f, 1.0f);
        sgl_begin_triangles();
        sgl_v2f(8.0f, 0.0f);
        sgl_v2f(-5.0f, 5.5f);
        sgl_v2f(-5.0f, -5.5f);
        sgl_end();

        /* Wing struts */
        draw_segment(v2(-9.0f, 8.0f), v2(-15.0f, 17.0f), cr * 0.7f, cg * 0.7f, cb * 0.7f, 0.85f);
        draw_segment(v2(-9.0f, -8.0f), v2(-15.0f, -17.0f), cr * 0.7f, cg * 0.7f, cb * 0.7f, 0.85f);

        sgl_pop_matrix();

        /* Callsign label above ship */
        /* Callsign rendered with sdtx (real font) — see callsign pass below. */
        if (scanned) {
            vec2 panel = v2(players[i].x + 64.0f, players[i].y + 45.0f);
            draw_rect_centered(panel, 58.0f, 18.0f,
                               0.018f, 0.024f, 0.032f, 0.54f);
            draw_rect_outline(panel, 58.0f, 18.0f,
                              cr * 0.75f, cg * 0.9f, cb, 0.34f);
        }

        /* Mining or scan beam — server-authoritative endpoints. */
        if (mining) {
            bool scanning = (players[i].flags & 8) != 0;
            vec2 muzzle  = v2(players[i].beam_start_x, players[i].beam_start_y);
            vec2 beam_end = v2(players[i].beam_end_x, players[i].beam_end_y);
            if (scanning) {
                draw_segment(muzzle, beam_end, 0.30f, 0.70f, 1.0f, 0.6f);
            } else {
                draw_segment(muzzle, beam_end, cr, cg, cb, 0.6f);
            }
        }

        /* Tractor field circle + towed tethers. Mirror local rendering:
         * active tractor shows the field even before pickup, and leashed
         * fragments keep visible tethers after the player releases R. */
        if (tractor_on || players[i].towed_count > 0) {
            vec2 pos = v2(players[i].x, players[i].y);
            if (tractor_on) {
                float pulse = 0.28f + (sinf(g.world.time * 7.0f + (float)i * 2.0f) * 0.08f);
                draw_circle_outline(pos, tr, 40, cr * 0.4f, cg * 0.8f, cb * 0.9f, pulse);
            }

            /* Tether lines to towed fragments */
            for (int t = 0; t < players[i].towed_count && t < 10; t++) {
                uint16_t raw = players[i].towed_fragments[t];
                if (raw == 0xFFFFu || raw >= MAX_ASTEROIDS) continue;
                const asteroid_t *a = &g.world.asteroids[raw];
                if (!a->active) continue;
                float rr, rg, rb;
                grade_tint(a->grade, &rr, &rg, &rb);
                float tp = 0.4f + 0.15f * sinf(g.world.time * 3.0f + (float)t * 1.5f);
                if (a->grade >= (uint8_t)MINING_GRADE_RARE)
                    tp += 0.12f * sinf(g.world.time * 7.0f + (float)t);
                draw_segment(pos, a->pos, rr, rg, rb, tp);
            }
        }
    }
}

/* ================================================================== */
/* Callsigns — readable sdtx labels above all visible ships           */
/* ================================================================== */

/* Convert a world position to a sdtx canvas cell coordinate, accounting
 * for the world Y-up vs canvas Y-down mismatch.
 *
 * sgl_ortho is set with (l, r, b=cam_top, t=cam_bottom) and cam_top <
 * cam_bottom, so bigger world Y maps to NDC +1 (top of screen). But
 * sokol_debugtext's vertex shader maps canvas y=0 → NDC +1 and canvas
 * y=1 → NDC -1, so canvas Y is screen-down. To anchor text at a world
 * point we have to flip Y at the call site: canvas_y = cam_bottom -
 * world_y. X needs no flip — both sgl and sdtx have x=cam_left at NDC
 * -1 (left edge).
 *
 * After this conversion, "above the ship on screen" means LARGER
 * canvas_y subtracted from world_y, which numerically reads as
 * `cam_bottom() - (world_y + screen_above_offset)` — i.e. the offset
 * is added to world_y like Y-up math even though we're computing a
 * Y-down canvas coord. The two flips cancel. */
static void sdtx_world_pos(float world_x, float world_y, float cell) {
    sdtx_pos((world_x - cam_left()) / cell,
             (cam_bottom() - world_y) / cell);
}

void draw_callsigns(void) {
    /* World-aligned sdtx: canvas dimensions match the sgl world view, so
     * 1 canvas pixel = 1 world unit. Origin stays at (0,0); the
     * sdtx_world_pos helper above does the world→canvas conversion
     * (including the Y flip needed because sdtx canvas is Y-down). */
    float view_w = cam_right() - cam_left();
    float view_h = cam_bottom() - cam_top();
    const float cell = 8.0f;
    sdtx_canvas(view_w, view_h);
    sdtx_origin(0, 0);

    /* Remote player callsigns */
    if (g.net_authority_enabled) {
        const NetPlayerState *players = net_get_interpolated_players();
        net_update_remote_player_scans(players);
        int local_id = (int)net_local_id();
        for (int i = 0; i < NET_MAX_PLAYERS; i++) {
            if (!players[i].active) continue;
            if (i == local_id) continue;
            if (!net_remote_player_scanned(i)) continue;
            if (!on_screen(players[i].x, players[i].y, 60.0f)) continue;
            sdtx_color3b(PAL_WORLD_STATION_CYAN);
            char label[16];
            if (players[i].callsign[0]) {
                snprintf(label, sizeof(label), "%s", players[i].callsign);
            } else {
                snprintf(label, sizeof(label), "PILOT %02d", i);
            }
            sdtx_world_pos(players[i].x + 16.0f, players[i].y + 53.0f, cell);
            sdtx_puts(label);

            char info[24];
            if (players[i].towed_count > 0) {
                snprintf(info, sizeof(info), "MINER  TOW %u",
                         (unsigned)players[i].towed_count);
            } else if ((players[i].flags & 16) != 0) {
                snprintf(info, sizeof(info), "MINER  TRACTOR");
            } else if ((players[i].flags & 8) != 0) {
                snprintf(info, sizeof(info), "MINER  SCANNING");
            } else {
                snprintf(info, sizeof(info), "MINER  CLEAR");
            }
            sdtx_color3b(PAL_TEXT_MUTED);
            sdtx_world_pos(players[i].x + 16.0f, players[i].y + 41.0f, cell);
            sdtx_puts(info);
        }
    }
}

static const hail_conversation_entry_t *hail_conversation_entry_for_npc(int npc_index) {
    int count = g.hail_conversation_count;
    if (count > HAIL_CONVERSATION_NPC_LIMIT) count = HAIL_CONVERSATION_NPC_LIMIT;
    for (int i = 0; i < count; i++) {
        if (g.hail_conversation[i].npc_index == npc_index)
            return &g.hail_conversation[i];
    }
    return NULL;
}

static bool hail_conversation_age_active(float start, float *out_alpha) {
    float age = g.hail_ping_timer - start;
    if (age < 0.0f || age > HAIL_CONVERSATION_LINE_DURATION) return false;

    float fade_in = clampf(age / 0.25f, 0.0f, 1.0f);
    float fade_out = clampf((HAIL_CONVERSATION_LINE_DURATION - age) / 0.55f,
                            0.0f, 1.0f);
    if (out_alpha) *out_alpha = fade_in < fade_out ? fade_in : fade_out;
    return true;
}

static bool hail_conversation_line_active(const hail_conversation_entry_t *entry,
                                          float *out_alpha) {
    if (!entry) return false;
    return hail_conversation_age_active(entry->at_s, out_alpha);
}

void draw_npc_chatter(void) {
    if (g.hail_ping_timer <= 0.0f || g.hail_ping_timer > HAIL_PING_LIFECYCLE) return;
    float hail_range = hail_scan_range();
    float hail_range_sq = hail_range * hail_range;
    float view_w = cam_right() - cam_left();
    float view_h = cam_bottom() - cam_top();
    const float cell = 8.0f;
    sdtx_canvas(view_w, view_h);
    sdtx_origin(0, 0);

    typedef struct {
        int index;
        float dist_sq;
        float reveal;
    } hail_asteroid_tag_t;

    hail_asteroid_tag_t tags[HAIL_SCAN_ASTEROID_TAG_LIMIT];
    int tag_count = 0;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &g.world.asteroids[i];
        if (!a->active) continue;
        if (!on_screen(a->pos.x, a->pos.y, a->radius + 80.0f)) continue;

        float dist_sq = v2_dist_sq(a->pos, g.hail_ping_origin);
        if (dist_sq > hail_range_sq) continue;
        float reveal = hail_scan_reveal_alpha(a->pos);
        if (reveal <= 0.01f) continue;

        if (tag_count < HAIL_SCAN_ASTEROID_TAG_LIMIT) {
            tags[tag_count++] = (hail_asteroid_tag_t){ i, dist_sq, reveal };
        } else {
            int worst = 0;
            for (int j = 1; j < HAIL_SCAN_ASTEROID_TAG_LIMIT; j++) {
                if (tags[j].dist_sq > tags[worst].dist_sq) worst = j;
            }
            if (dist_sq < tags[worst].dist_sq)
                tags[worst] = (hail_asteroid_tag_t){ i, dist_sq, reveal };
        }
    }

    for (int t = 0; t < tag_count; t++) {
        const asteroid_t *a = &g.world.asteroids[tags[t].index];
        char label[64];
        char id[8];
        hail_asteroid_identity_label(a, id);
        if (a->tier == ASTEROID_TIER_S) {
            const char *code = commodity_code((commodity_t)a->commodity);
            snprintf(label, sizeof(label), "%s", code);
        } else {
            if (id[0]) {
                snprintf(label, sizeof(label), "%s %s %s",
                         commodity_code((commodity_t)a->commodity),
                         asteroid_tier_name((asteroid_tier_t)a->tier),
                         id);
            } else {
                snprintf(label, sizeof(label), "%s %s",
                         commodity_code((commodity_t)a->commodity),
                         asteroid_tier_name((asteroid_tier_t)a->tier));
            }
        }

        uint8_t r, gg, b;
        uint8_t grade = (a->grade < (uint8_t)MINING_GRADE_COUNT)
            ? a->grade
            : (uint8_t)MINING_GRADE_COMMON;
        mining_grade_rgb((mining_grade_t)grade, &r, &gg, &b);
        uint8_t alpha = (uint8_t)(220.0f * tags[t].reveal);
        sdtx_color4b(r, gg, b, alpha);
        int len = (int)strlen(label);
        sdtx_world_pos(a->pos.x - len * cell * 0.5f,
                       a->pos.y + a->radius + 18.0f, cell);
        sdtx_puts(label);
    }

    if (g.hail_player_line[0] && on_screen(LOCAL_PLAYER.ship.pos.x,
                                           LOCAL_PLAYER.ship.pos.y, 50.0f)) {
        float player_alpha = 0.0f;
        if (hail_conversation_age_active(0.0f, &player_alpha)) {
            int len = (int)strlen(g.hail_player_line);
            uint8_t line_alpha = (uint8_t)(230.0f * player_alpha);
            sdtx_color4b(PAL_WORLD_STATION_CYAN, line_alpha);
            sdtx_world_pos(LOCAL_PLAYER.ship.pos.x - len * cell * 0.5f,
                           LOCAL_PLAYER.ship.pos.y - 34.0f, cell);
            sdtx_puts(g.hail_player_line);
        }
    }

    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *npc = &g.world.npc_ships[i];
        if (!npc->active) continue;
        if (!on_screen(npc->ship.pos.x, npc->ship.pos.y, 50.0f)) continue;
        if (v2_dist_sq(npc->ship.pos, g.hail_ping_origin) > hail_range_sq) continue;
        float reveal = hail_scan_reveal_alpha(npc->ship.pos);
        if (reveal <= 0.01f) continue;

        const hail_conversation_entry_t *entry =
            hail_conversation_entry_for_npc(i);
        float conversation_alpha = 0.0f;
        bool speak_now = hail_conversation_line_active(entry,
                                                       &conversation_alpha);

        /* Rotate fallback line every 8 seconds, offset by NPC index.
         * Station-authored lines still backstop workers without situated
         * memory, but the visible hail response is gated by the async
         * proximity-ordered conversation window. */
        const char *line;
        char memory_line[96];
        bool has_memory_line = npc_radio_line(g.world.stations, npc, i,
                                              memory_line, sizeof(memory_line));
        if (npc->role == NPC_ROLE_MINER) {
            int idx = (i + (int)(g.world.time / 8.0f)) % NPC_CHATTER_MINER_COUNT;
            line = NPC_CHATTER_MINER[idx];
            if (npc->home_station >= 0 && npc->home_station < MAX_STATIONS) {
                const char *station_line =
                    g.world.stations[npc->home_station].miner_chatter[idx % STATION_IDENTITY_CHATTER_LINES];
                if (station_line[0]) line = station_line;
            }
        } else if (npc->role == NPC_ROLE_HAULER) {
            int idx = (i + (int)(g.world.time / 8.0f)) % NPC_CHATTER_HAULER_COUNT;
            line = NPC_CHATTER_HAULER[idx];
            if (npc->home_station >= 0 && npc->home_station < MAX_STATIONS) {
                const char *station_line =
                    g.world.stations[npc->home_station].hauler_chatter[idx % STATION_IDENTITY_CHATTER_LINES];
                if (station_line[0]) line = station_line;
            }
        } else {
            line = "worker";
        }
        if (has_memory_line && memory_line[0]) line = memory_line;
        if (entry && entry->line[0]) line = entry->line;

        char ident[32];
        world_npc_scan_label(npc, i, ident);
        int ident_len = (int)strlen(ident);
        uint8_t ident_alpha = (uint8_t)(220.0f * reveal);
        sdtx_color4b(PAL_WORLD_STATION_CYAN, ident_alpha);
        sdtx_world_pos(npc->ship.pos.x - ident_len * cell * 0.5f,
                       npc->ship.pos.y + 34.0f, cell);
        sdtx_puts(ident);

        if (!speak_now) continue;

        int len = (int)strlen(line);
        uint8_t nr = (uint8_t)(clampf(npc->tint_r, 0.0f, 1.0f) * 255.0f);
        uint8_t ng = (uint8_t)(clampf(npc->tint_g, 0.0f, 1.0f) * 255.0f);
        uint8_t nb = (uint8_t)(clampf(npc->tint_b, 0.0f, 1.0f) * 255.0f);
        uint8_t line_alpha = (uint8_t)(230.0f * reveal * conversation_alpha);
        sdtx_color4b(nr, ng, nb, line_alpha);
        /* Sit chatter just below the NPC sprite. World Y-up: smaller
         * world_y is below on screen. */
        sdtx_world_pos(npc->ship.pos.x - len * cell * 0.5f,
                       npc->ship.pos.y - 24.0f, cell);
        sdtx_puts(line);
    }
}

/* ================================================================== */
/* Sell FX — floating "+N" popups on SIM_EVENT_SELL                   */
/* ================================================================== */

void spawn_sell_fx(const vec2 *origin, int amount, mining_grade_t grade, bool by_contract) {
    if (amount <= 0 || !origin) return;
    /* Pick the oldest available slot. */
    int slot = -1;
    float oldest_age = -1.0f;
    for (int i = 0; i < (int)(sizeof(g.sell_fx) / sizeof(g.sell_fx[0])); i++) {
        if (g.sell_fx[i].life <= 0.0f) { slot = i; break; }
        if (g.sell_fx[i].age > oldest_age) { oldest_age = g.sell_fx[i].age; slot = i; }
    }
    if (slot < 0) return;

    /* Small horizontal jitter so stacked popups don't exactly overlap. */
    static uint32_t seed = 0xC0FFEEu;
    seed = seed * 1664525u + 1013904223u;
    float jitter_x = ((int)((seed >> 8) & 0x1F) - 16) * 2.0f;
    seed = seed * 1664525u + 1013904223u;
    float jitter_y = ((int)((seed >> 8) & 0x1F) - 16) * 1.5f;

    /* Spawn just above the station (world Y-up: +40 = up on screen). */
    g.sell_fx[slot].pos = v2(origin->x + jitter_x, origin->y + 40.0f + jitter_y);
    g.sell_fx[slot].age = 0.0f;
    g.sell_fx[slot].life = 1.5f;
    if (by_contract) {
        /* Gold/yellow regardless of grade — reads as "contract payout". */
        g.sell_fx[slot].r = 255;
        g.sell_fx[slot].g = 210;
        g.sell_fx[slot].b = 60;
    } else {
        mining_grade_rgb(grade, &g.sell_fx[slot].r, &g.sell_fx[slot].g, &g.sell_fx[slot].b);
    }
    snprintf(g.sell_fx[slot].text, sizeof(g.sell_fx[slot].text), "+%d", amount);
}

void update_sell_fx(float dt) {
    for (int i = 0; i < (int)(sizeof(g.sell_fx) / sizeof(g.sell_fx[0])); i++) {
        if (g.sell_fx[i].life <= 0.0f) continue;
        g.sell_fx[i].age += dt;
        if (g.sell_fx[i].age >= g.sell_fx[i].life) {
            g.sell_fx[i].life = 0.0f;
        }
    }
}

void draw_sell_fx(void) {
    /* World-aligned sdtx: see sdtx_world_pos (above draw_callsigns)
     * for the world→canvas conversion. */
    float view_w = cam_right() - cam_left();
    float view_h = cam_bottom() - cam_top();
    const float cell = 8.0f;
    sdtx_canvas(view_w, view_h);
    sdtx_origin(0, 0);

    for (int i = 0; i < (int)(sizeof(g.sell_fx) / sizeof(g.sell_fx[0])); i++) {
        if (g.sell_fx[i].life <= 0.0f) continue;
        float t = g.sell_fx[i].age / g.sell_fx[i].life;  /* 0..1 */
        if (t > 1.0f) continue;
        /* Rise +28 world units over lifetime (Y-up = up on screen). Fade
         * out in the last third. */
        float rise_y = 28.0f * t;
        float alpha = (t < 0.67f) ? 1.0f : (1.0f - (t - 0.67f) / 0.33f);
        if (alpha < 0.0f) alpha = 0.0f;
        uint8_t a8 = (uint8_t)(alpha * 255.0f);
        float x = g.sell_fx[i].pos.x;
        float y = g.sell_fx[i].pos.y + rise_y;
        if (!on_screen(x, y, 32.0f)) continue;

        int len = (int)strlen(g.sell_fx[i].text);
        sdtx_color4b(g.sell_fx[i].r, g.sell_fx[i].g, g.sell_fx[i].b, a8);
        sdtx_world_pos(x - len * cell * 0.5f, y, cell);
        sdtx_puts(g.sell_fx[i].text);
    }
}

/* ================================================================== */
/* Damage FX — floating "-N" + red vignette on SIM_EVENT_DAMAGE       */
/* ================================================================== */

void spawn_damage_fx(const vec2 *origin, int amount) {
    if (amount <= 0 || !origin) return;
    int slot = -1;
    float oldest_age = -1.0f;
    int pool = (int)(sizeof(g.damage_fx) / sizeof(g.damage_fx[0]));
    for (int i = 0; i < pool; i++) {
        if (g.damage_fx[i].life <= 0.0f) { slot = i; break; }
        if (g.damage_fx[i].age > oldest_age) { oldest_age = g.damage_fx[i].age; slot = i; }
    }
    if (slot < 0) return;
    /* Small jitter so back-to-back hits don't render on top of each other. */
    static uint32_t seed = 0x600DBADu;
    seed = seed * 1664525u + 1013904223u;
    float jitter_x = ((int)((seed >> 8) & 0x1F) - 16) * 1.5f;
    seed = seed * 1664525u + 1013904223u;
    float jitter_y = ((int)((seed >> 8) & 0x1F) - 16) * 1.0f;
    g.damage_fx[slot].pos = v2(origin->x + jitter_x, origin->y + 24.0f + jitter_y);
    g.damage_fx[slot].age = 0.0f;
    g.damage_fx[slot].life = 1.0f;
    snprintf(g.damage_fx[slot].text, sizeof(g.damage_fx[slot].text), "-%d", amount);
}

void update_damage_fx(float dt) {
    int pool = (int)(sizeof(g.damage_fx) / sizeof(g.damage_fx[0]));
    for (int i = 0; i < pool; i++) {
        if (g.damage_fx[i].life <= 0.0f) continue;
        g.damage_fx[i].age += dt;
        if (g.damage_fx[i].age >= g.damage_fx[i].life) g.damage_fx[i].life = 0.0f;
    }
    if (g.damage_flash_timer > 0.0f) {
        g.damage_flash_timer -= dt;
        if (g.damage_flash_timer < 0.0f) g.damage_flash_timer = 0.0f;
    }
}

void draw_damage_fx(void) {
    float view_w = cam_right() - cam_left();
    float view_h = cam_bottom() - cam_top();
    const float cell = 8.0f;
    sdtx_canvas(view_w, view_h);
    sdtx_origin(cam_left() / cell, cam_top() / cell);
    int pool = (int)(sizeof(g.damage_fx) / sizeof(g.damage_fx[0]));
    for (int i = 0; i < pool; i++) {
        if (g.damage_fx[i].life <= 0.0f) continue;
        float t = g.damage_fx[i].age / g.damage_fx[i].life;
        if (t > 1.0f) continue;
        /* Rise ~22 px and fade in the last quarter. */
        float rise_y = -22.0f * t;
        float alpha = (t < 0.75f) ? 1.0f : (1.0f - (t - 0.75f) / 0.25f);
        if (alpha < 0.0f) alpha = 0.0f;
        uint8_t a8 = (uint8_t)(alpha * 255.0f);
        float x = g.damage_fx[i].pos.x;
        float y = g.damage_fx[i].pos.y + rise_y;
        if (!on_screen(x, y, 32.0f)) continue;
        int len = (int)strlen(g.damage_fx[i].text);
        sdtx_color4b(255, 70, 70, a8);
        sdtx_pos((x - len * cell * 0.5f) / cell, y / cell);
        sdtx_puts(g.damage_fx[i].text);
    }
}

/* Damage flash deliberately a no-op for now. Earlier attempts (solid
 * red border, then a per-vertex-alpha vignette) both looked bad: the
 * codebase's sokol_gl pipeline doesn't reliably interpolate per-
 * vertex color, and any flat-alpha fill at a noticeable opacity reads
 * as a hard square. The "-N" popup, the existing screen shake, and
 * the damage SFX already telegraph the hit clearly without an
 * additional full-screen overlay. Keep the symbol so the call site
 * doesn't need surgery, and the timer in g.damage_flash_timer keeps
 * decaying — left in place for a future re-enable once we have a
 * working shader path for radial fades. */
void draw_damage_flash(float screen_w, float screen_h) {
    (void)screen_w;
    (void)screen_h;
}

/* ================================================================== */
/* Scaffold world objects                                             */
/* ================================================================== */

static uint8_t cargo_pod_visual_best_grade(const cargo_pod_t *pod) {
    uint8_t best = (uint8_t)MINING_GRADE_COMMON;
    if (!pod || !pod->active) return best;
    bool exact_local = pod->manifest_count > 0 &&
                       pod->manifest_count == pod->quantity;
    if (exact_local) {
        bool all_match = true;
        for (uint16_t i = 0; i < pod->manifest_count; i++) {
            const cargo_unit_t *unit = &pod->manifest_units[i];
            if ((commodity_t)unit->commodity != pod->commodity) {
                all_match = false;
                break;
            }
            if (unit->grade < (uint8_t)MINING_GRADE_COUNT &&
                unit->grade > best) {
                best = unit->grade;
            }
        }
        if (all_match) return best;
    }
    if (pod->summary_grade < (uint8_t)MINING_GRADE_COUNT) {
        return pod->summary_grade;
    }
    return best;
}

static void cargo_pod_content_color(const cargo_pod_t *pod,
                                    float *r, float *g0, float *b) {
    if (!pod || pod->kind == CARGO_POD_GAS) {
        *r = 0.20f; *g0 = 0.86f; *b = 0.78f;
        return;
    }
    if (pod->commodity < COMMODITY_COUNT) {
        commodity_color(pod->commodity, r, g0, b);
        return;
    }
    *r = 0.78f; *g0 = 0.60f; *b = 0.30f;
}

void draw_cargo_pods(void) {
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &g.world.cargo_pods[i];
        if (!pod->active) continue;
        if (!on_screen(pod->pos.x, pod->pos.y, pod->radius + 24.0f)) continue;

        float r, g0, b;
        cargo_pod_content_color(pod, &r, &g0, &b);
        float pulse = 1.0f + 0.08f * sinf(g.world.time * 4.0f + pod->rotation);

        if (pod->kind == CARGO_POD_GAS) {
            draw_circle_filled(pod->pos, pod->radius * 1.8f * pulse, 18,
                               0.12f, 0.72f, 0.68f, 0.16f);
        }

        sgl_push_matrix();
        sgl_translate(pod->pos.x, pod->pos.y, 0.0f);
        sgl_rotate(pod->rotation, 0.0f, 0.0f, 1.0f);

        float half = pod->radius * 0.72f;
        sgl_begin_quads();
        sgl_c4f(r * 0.58f, g0 * 0.58f, b * 0.58f, 0.86f);
        sgl_v2f(-half, -half);
        sgl_v2f( half, -half);
        sgl_v2f( half,  half);
        sgl_v2f(-half,  half);
        sgl_end();

        sgl_begin_lines();
        sgl_c4f(fminf(1.0f, r * 1.62f), fminf(1.0f, g0 * 1.48f),
                fminf(1.0f, b * 1.48f), 0.96f);
        sgl_v2f(-half, -half); sgl_v2f( half, -half);
        sgl_v2f( half, -half); sgl_v2f( half,  half);
        sgl_v2f( half,  half); sgl_v2f(-half,  half);
        sgl_v2f(-half,  half); sgl_v2f(-half, -half);
        sgl_v2f(-half, 0.0f);  sgl_v2f( half, 0.0f);
        sgl_v2f(0.0f, -half);  sgl_v2f(0.0f,  half);
        sgl_end();

        sgl_pop_matrix();

        bool scan_reveal =
            LOCAL_PLAYER.scan_active &&
            LOCAL_PLAYER.scan_target_type == INSPECT_TARGET_CARGO_POD &&
            LOCAL_PLAYER.scan_target_index == i;
        uint8_t grade = cargo_pod_visual_best_grade(pod);
        if (scan_reveal && grade > (uint8_t)MINING_GRADE_COMMON) {
            float gr, gg, gb;
            grade_tint(grade, &gr, &gg, &gb);
            draw_circle_outline(pod->pos, pod->radius * 1.18f * pulse, 18,
                                gr, gg, gb, 0.58f);
            draw_circle_filled(pod->pos, pod->radius * 0.16f, 10,
                               gr, gg, gb, 0.82f);
        }

        if (pod->towed_by >= 0 && pod->towed_by < MAX_PLAYERS) {
            const ship_t *ship = &g.world.players[pod->towed_by].ship;
            draw_segment(ship->pos, pod->pos, r, g0, b, 0.42f);
        }
    }
}

void draw_scaffolds(void) {
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &g.world.scaffolds[i];
        if (!sc->active) continue;
        if (!on_screen(sc->pos.x, sc->pos.y, sc->radius + 20.0f)) continue;

        float amb_r = 1.0f, amb_g = 0.85f, amb_b = 0.47f; /* construction amber */
        float pulse = 0.5f + 0.2f * sinf(g.world.time * 2.5f + sc->age * 3.0f);

        /* Module-type tint blended with amber */
        float mr, mg, mb;
        module_color_fn(sc->module_type, &mr, &mg, &mb);
        amb_r = lerpf(amb_r, mr, 0.3f);
        amb_g = lerpf(amb_g, mg, 0.3f);
        amb_b = lerpf(amb_b, mb, 0.3f);

        /* Nascent build progress (0..1) — drives visual fill */
        float build_frac = 0.0f;
        if (sc->state == SCAFFOLD_NASCENT) {
            float total = module_build_cost_lookup(sc->module_type);
            if (total > 0.0f) build_frac = sc->build_amount / total;
            if (build_frac > 1.0f) build_frac = 1.0f;
        }

        sgl_push_matrix();
        sgl_translate(sc->pos.x, sc->pos.y, 0.0f);
        sgl_rotate(sc->rotation, 0.0f, 0.0f, 1.0f);

        float r = sc->radius;
        /* Nascent scaffolds grow visually as build progress advances */
        if (sc->state == SCAFFOLD_NASCENT) {
            r = sc->radius * (0.4f + 0.6f * build_frac);
        }

        /* Wireframe octagon */
        sgl_begin_lines();
        int segs = 8;
        float alpha = (sc->state == SCAFFOLD_SNAPPING) ? pulse + 0.3f : pulse;
        sgl_c4f(amb_r, amb_g, amb_b, alpha);
        for (int s = 0; s < segs; s++) {
            float a0 = TWO_PI_F * (float)s / (float)segs;
            float a1 = TWO_PI_F * (float)(s + 1) / (float)segs;
            sgl_v2f(cosf(a0) * r, sinf(a0) * r);
            sgl_v2f(cosf(a1) * r, sinf(a1) * r);
        }
        /* Internal cross-brace — scaffolding structure */
        float inner = r * 0.6f;
        sgl_c4f(amb_r * 0.7f, amb_g * 0.7f, amb_b * 0.7f, alpha * 0.6f);
        sgl_v2f(-inner, -inner); sgl_v2f(inner, inner);
        sgl_v2f(-inner, inner); sgl_v2f(inner, -inner);
        sgl_v2f(-inner, 0); sgl_v2f(inner, 0);
        sgl_v2f(0, -inner); sgl_v2f(0, inner);
        sgl_end();

        /* Module type indicator: small filled circle at center */
        sgl_begin_triangles();
        sgl_c4f(mr * 0.8f, mg * 0.8f, mb * 0.8f, pulse * 0.5f);
        int csegs = 8;
        float cr2 = 6.0f;
        for (int s = 0; s < csegs; s++) {
            float a0 = TWO_PI_F * (float)s / (float)csegs;
            float a1 = TWO_PI_F * (float)(s + 1) / (float)csegs;
            sgl_v2f(0, 0);
            sgl_v2f(cosf(a0) * cr2, sinf(a0) * cr2);
            sgl_v2f(cosf(a1) * cr2, sinf(a1) * cr2);
        }
        sgl_end();

        sgl_pop_matrix();

        /* SNAPPING state: draw tendrils from station to scaffold */
        if (sc->state == SCAFFOLD_SNAPPING && sc->placed_station >= 0) {
            const station_t *st = &g.world.stations[sc->placed_station];
            vec2 target = module_world_pos_ring(st, sc->placed_ring, sc->placed_slot);
            float t_pulse = 0.4f + 0.3f * sinf(g.world.time * 4.0f);

            /* Main tendril: station slot → scaffold */
            draw_segment(target, sc->pos, amb_r * 0.6f, amb_g * 0.6f, amb_b * 0.4f, t_pulse);

            /* Secondary tendrils from station center */
            draw_segment(st->pos, sc->pos, amb_r * 0.3f, amb_g * 0.3f, amb_b * 0.2f, t_pulse * 0.4f);

            /* Target slot indicator: pulsing ring at the destination */
            draw_circle_outline(target, sc->radius + 4.0f, 12,
                amb_r * 0.5f, amb_g * 0.5f, amb_b * 0.3f, t_pulse * 0.6f);
        }
    }
}

void draw_scaffold_tether(void) {
    if (g.death_cinematic.active) return;
    /* Tether line from player ship to towed scaffold */
    int idx = LOCAL_PLAYER.ship.towed_scaffold;
    if (idx < 0 || idx >= MAX_SCAFFOLDS) return;
    const scaffold_t *sc = &g.world.scaffolds[idx];
    if (!sc->active) return;

    float pulse = 0.5f + 0.2f * sinf(g.world.time * 3.0f);
    float cue_prev = world_signal_visual_enter_cue();
    draw_segment(LOCAL_PLAYER.ship.pos, sc->pos, 0.5f, 0.85f, 0.75f, pulse);
    world_signal_visual_leave_cue(cue_prev);
}

/* Draw beams from producer modules to active shipyard intakes.
 * Shipyards with a pending order get a pulsing line to the nearest
 * same-ring producer of the required commodity. */
static module_type_t producer_for_commodity_client(commodity_t c) {
    switch (c) {
        case COMMODITY_FRAME:         return MODULE_FRAME_PRESS;
        /* All three ingots come from commodity-tagged furnace instances. */
        case COMMODITY_FERRITE_INGOT:
        case COMMODITY_CUPRITE_INGOT:
        case COMMODITY_CRYSTAL_INGOT: return MODULE_FURNACE;
        default:                      return MODULE_COUNT;
    }
}

/* Compute max unlocked ring on a station. */
static int station_unlocked_rings_client(const station_t *st) {
    int counts[STATION_NUM_RINGS + 1] = {0};
    for (int m = 0; m < st->module_count; m++) {
        int r = st->modules[m].ring;
        if (r >= 1 && r <= STATION_NUM_RINGS) counts[r]++;
    }
    for (int p = 0; p < st->placement_plan_count; p++) {
        int r = st->placement_plans[p].ring;
        if (r >= 1 && r <= STATION_NUM_RINGS) counts[r]++;
    }
    int unlocked = 1;
    if (counts[1] >= 2) unlocked = 2;
    if (counts[2] >= 4) unlocked = 3;
    return unlocked;
}

/* Draw planned stations (server-side ghost outposts) as wireframe rings.
 * Visible to all players. Materialized when a scaffold is towed near. */
static void draw_planned_stations(void) {
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!st->planned) continue;
        vec2 c = st->pos;
        float pulse = 0.4f + 0.3f * sinf(g.world.time * 2.5f);
        int max_ring = station_unlocked_rings_client(st);
        bool abandoned = station_planned_site_abandoned(st);
        float ghost_r = abandoned ? 0.95f : 0.40f;
        float ghost_g = abandoned ? 0.42f : 0.85f;
        float ghost_b = abandoned ? 0.28f : 1.00f;

        /* Wireframe rings — dashed cyan, only unlocked */
        for (int r = 1; r <= max_ring; r++) {
            float radius = STATION_RING_RADIUS[r];
            int dashes = 32;
            sgl_begin_lines();
            sgl_c4f(ghost_r, ghost_g, ghost_b, pulse * 0.6f);
            for (int i = 0; i < dashes; i += 2) {
                float a0 = TWO_PI_F * (float)i / (float)dashes;
                float a1 = TWO_PI_F * (float)(i + 1) / (float)dashes;
                sgl_v2f(c.x + cosf(a0) * radius, c.y + sinf(a0) * radius);
                sgl_v2f(c.x + cosf(a1) * radius, c.y + sinf(a1) * radius);
            }
            sgl_end();
        }
        /* Dashed dock-radius perimeter */
        {
            int dashes = 48;
            sgl_begin_lines();
            sgl_c4f(ghost_r, ghost_g, ghost_b, pulse * 0.4f);
            float radius = OUTPOST_DOCK_RADIUS;
            for (int i = 0; i < dashes; i += 2) {
                float a0 = TWO_PI_F * (float)i / (float)dashes;
                float a1 = TWO_PI_F * (float)(i + 1) / (float)dashes;
                sgl_v2f(c.x + cosf(a0) * radius, c.y + sinf(a0) * radius);
                sgl_v2f(c.x + cosf(a1) * radius, c.y + sinf(a1) * radius);
            }
            sgl_end();
        }
        /* Center marker */
        draw_circle_outline(c, 6.0f, 12, ghost_r, ghost_g, ghost_b, pulse);
        if (abandoned) {
            sgl_begin_lines();
            sgl_c4f(ghost_r, ghost_g, ghost_b, pulse * 0.8f);
            sgl_v2f(c.x - 22.0f, c.y - 22.0f);
            sgl_v2f(c.x + 22.0f, c.y + 22.0f);
            sgl_v2f(c.x - 22.0f, c.y + 22.0f);
            sgl_v2f(c.x + 22.0f, c.y - 22.0f);
            sgl_end();
        }

        /* Planned slot ghosts (already drawn by draw_placement_plans below) */
    }
}

/* Draw existing placement plans as faint colored ghosts at their slots. */
static void draw_placement_plans(void) {
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st) || st->scaffold) continue;
        if (st->placement_plan_count == 0) continue;
        for (int p = 0; p < st->placement_plan_count; p++) {
            int ring = st->placement_plans[p].ring;
            int slot = st->placement_plans[p].slot;
            module_type_t type = st->placement_plans[p].type;
            vec2 pos = module_world_pos_ring(st, ring, slot);
            float mr, mg, mb;
            module_color_fn(type, &mr, &mg, &mb);
            float pulse = 0.25f + 0.15f * sinf(g.world.time * 1.5f + (float)p * 0.7f);
            /* Faint dashed outline + filled core in module color */
            draw_circle_outline(pos, 22.0f, 16, mr, mg, mb, pulse);
            draw_circle_filled(pos, 4.0f, 8, mr, mg, mb, pulse * 1.5f);
        }
    }
}

static bool station_yard_blocked_for_overlay(int station_idx,
                                             int *out_blocker_idx,
                                             module_type_t *out_pending_type) {
    if (station_idx < 0 || station_idx >= MAX_STATIONS) return false;
    const station_t *st = &g.world.stations[station_idx];
    if (!station_is_active(st)) return false;
    if (st->pending_scaffold_count <= 0) return false;
    if (!station_has_module(st, MODULE_SHIPYARD)) return false;
    if (station_nascent_scaffold_index(g.world.scaffolds, MAX_SCAFFOLDS,
                                       station_idx) >= 0) {
        return false;
    }

    int blocker = station_construction_blocker_index(st, g.world.scaffolds,
                                                     MAX_SCAFFOLDS);
    if (blocker < 0) return false;

    if (out_blocker_idx) *out_blocker_idx = blocker;
    if (out_pending_type) *out_pending_type = st->pending_scaffolds[0].type;
    return true;
}

static void draw_blocked_construction_yards(void) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        int blocker_idx = -1;
        module_type_t pending_type = MODULE_COUNT;
        if (!station_yard_blocked_for_overlay(s, &blocker_idx, &pending_type))
            continue;

        const station_t *st = &g.world.stations[s];
        if (!on_screen(st->pos.x, st->pos.y,
                       STATION_RING_RADIUS[1] + 80.0f)) {
            continue;
        }

        const scaffold_t *blocker = &g.world.scaffolds[blocker_idx];
        float pulse = 0.55f + 0.30f * sinf(g.world.time * 5.0f);
        float dim = 0.35f + 0.15f * sinf(g.world.time * 3.0f);
        const float br = 1.0f;
        const float bg = 0.34f;
        const float bb = 0.14f;
        float clear_r = STATION_RING_RADIUS[1] * 0.6f;

        draw_circle_outline(st->pos, clear_r, 36, br, bg, bb, 0.20f + pulse * 0.45f);
        draw_circle_outline(st->pos, clear_r + 8.0f, 36, br, bg, bb, dim * 0.45f);
        draw_segment(st->pos, blocker->pos, br, bg, bb, 0.18f + pulse * 0.25f);
        draw_circle_outline(blocker->pos, blocker->radius + 18.0f, 18,
                            br, bg, bb, 0.22f + pulse * 0.45f);

        float mr = br, mg = bg, mb = bb;
        if (pending_type != MODULE_COUNT) module_color_fn(pending_type, &mr, &mg, &mb);
        draw_circle_filled(st->pos, 5.0f, 8, mr, mg, mb, 0.45f + pulse * 0.25f);

        sgl_begin_lines();
        sgl_c4f(br, bg, bb, 0.45f + pulse * 0.35f);
        float cross = 26.0f;
        sgl_v2f(st->pos.x - cross, st->pos.y - cross);
        sgl_v2f(st->pos.x + cross, st->pos.y + cross);
        sgl_v2f(st->pos.x - cross, st->pos.y + cross);
        sgl_v2f(st->pos.x + cross, st->pos.y - cross);
        float tag = blocker->radius + 10.0f;
        sgl_v2f(blocker->pos.x - tag, blocker->pos.y - tag);
        sgl_v2f(blocker->pos.x + tag, blocker->pos.y + tag);
        sgl_v2f(blocker->pos.x - tag, blocker->pos.y + tag);
        sgl_v2f(blocker->pos.x + tag, blocker->pos.y - tag);
        sgl_end();
    }
}

void draw_placement_reticle(void) {
    {
        float cue_prev = world_signal_visual_enter_cue();
        /* Always draw planned stations (server-side ghosts) */
        draw_planned_stations();
        /* Always draw existing plans on stations (active or planned) */
        draw_placement_plans();
        /* Blocked shipyards are construction state, so surface them with the
         * same always-on overlay pass as planned and reserved slots. */
        draw_blocked_construction_yards();
        world_signal_visual_leave_cue(cue_prev);
    }

    /* Ghost preview: local-only, uncommitted. Keep it amber/washed-out so
     * it does not read as the committed cyan planned-station blueprint. */
    if (g.plan_mode_active && g.plan_target_station == -1) {
        float cue_prev = world_signal_visual_enter_cue();
        vec2 c = LOCAL_PLAYER.ship.pos;
        float pulse = 0.4f + 0.3f * sinf(g.world.time * 2.5f);
        const float ghost_r = 0.78f;
        const float ghost_g = 0.70f;
        const float ghost_b = 0.48f;
        /* Ring 1 wireframe: long amber ghost dashes. */
        float radius = STATION_RING_RADIUS[1];
        int dashes = 40;
        sgl_begin_lines();
        sgl_c4f(ghost_r, ghost_g, ghost_b, pulse * 0.55f);
        for (int i = 0; i < dashes; i += 4) {
            float a0 = TWO_PI_F * (float)i / (float)dashes;
            float a1 = TWO_PI_F * (float)(i + 2) / (float)dashes;
            sgl_v2f(c.x + cosf(a0) * radius, c.y + sinf(a0) * radius);
            sgl_v2f(c.x + cosf(a1) * radius, c.y + sinf(a1) * radius);
        }
        sgl_end();
        /* Center marker with a faint cancel-cross: this is preview state,
         * not physical station state. */
        draw_circle_outline(c, 8.0f, 12, ghost_r, ghost_g, ghost_b, pulse * 0.8f);
        sgl_begin_lines();
        sgl_c4f(ghost_r, ghost_g, ghost_b, pulse * 0.45f);
        sgl_v2f(c.x - 16.0f, c.y - 16.0f); sgl_v2f(c.x + 16.0f, c.y + 16.0f);
        sgl_v2f(c.x - 16.0f, c.y + 16.0f); sgl_v2f(c.x + 16.0f, c.y - 16.0f);
        sgl_end();
        /* Slot dots around ring 1 — all slots shown as small circles */
        int slots_n = STATION_RING_SLOTS[1];
        for (int slot = 0; slot < slots_n; slot++) {
            float angle = TWO_PI_F * (float)slot / (float)slots_n;
            vec2 sp = v2_add(c, v2(cosf(angle) * radius, sinf(angle) * radius));
            bool active = (slot == g.placement_target_slot && g.placement_target_ring == 1);
            if (active) {
                float mr, mg, mb;
                module_color_fn((module_type_t)g.plan_type, &mr, &mg, &mb);
                float ap = 0.5f + 0.4f * sinf(g.world.time * 5.0f);
                draw_circle_outline(sp, 32.0f, 24, mr, mg, mb, ap);
                draw_circle_outline(sp, 26.0f, 24, mr, mg, mb, ap * 0.7f);
                draw_circle_filled(sp, 6.0f, 8, mr, mg, mb, ap);
            } else {
                /* Muted amber dots = preview slots only, not reserved slots. */
                draw_circle_filled(sp, 4.0f, 8, ghost_r, ghost_g, ghost_b, pulse * 0.55f);
            }
        }
        world_signal_visual_leave_cue(cue_prev);
    }

    /* Plan mode on real station: draw the cycling-type ghost at the
     * current target slot. */
    if (g.plan_mode_active && g.placement_target_station >= 0) {
        const station_t *st = &g.world.stations[g.placement_target_station];
        if (station_exists(st)) {
            float cue_prev = world_signal_visual_enter_cue();
            vec2 target = module_world_pos_ring(st, g.placement_target_ring, g.placement_target_slot);
            float mr, mg, mb;
            module_color_fn((module_type_t)g.plan_type, &mr, &mg, &mb);
            float pulse = 0.5f + 0.4f * sinf(g.world.time * 5.0f);
            /* Bright module-tinted ring */
            draw_circle_outline(target, 32.0f, 24, mr, mg, mb, pulse);
            draw_circle_outline(target, 26.0f, 24, mr, mg, mb, pulse * 0.7f);
            draw_circle_filled(target, 6.0f, 8, mr, mg, mb, pulse);
            /* Crosshair tick marks */
            sgl_begin_lines();
            sgl_c4f(mr, mg, mb, pulse);
            float tick = 10.0f;
            sgl_v2f(target.x - 40.0f, target.y); sgl_v2f(target.x - 40.0f + tick, target.y);
            sgl_v2f(target.x + 40.0f, target.y); sgl_v2f(target.x + 40.0f - tick, target.y);
            sgl_v2f(target.x, target.y - 40.0f); sgl_v2f(target.x, target.y - 40.0f + tick);
            sgl_v2f(target.x, target.y + 40.0f); sgl_v2f(target.x, target.y + 40.0f - tick);
            sgl_end();
            /* Tether line from ship to target */
            draw_segment(LOCAL_PLAYER.ship.pos, target, mr, mg, mb, pulse * 0.5f);
            world_signal_visual_leave_cue(cue_prev);
        }
    }

    /* Outpost lock effect: expanding ring flash at lock position. */
    if (g.outpost_lock_timer > 0.0f) {
        float cue_prev = world_signal_visual_enter_cue();
        float t = 1.0f - (g.outpost_lock_timer / 1.5f); /* 0→1 over lifetime */
        float expand_r = STATION_RING_RADIUS[1] * (0.8f + 0.5f * t);
        float alpha = (1.0f - t) * 1.2f;
        if (alpha > 1.0f) alpha = 1.0f;
        draw_circle_outline(g.outpost_lock_pos, expand_r, 48, 0.4f, 1.0f, 0.8f, alpha);
        draw_circle_outline(g.outpost_lock_pos, expand_r * 0.6f, 32, 0.6f, 1.0f, 1.0f, alpha * 0.6f);
        world_signal_visual_leave_cue(cue_prev);
    }

    if (!g.placement_reticle_active) return;

    vec2 target;
    bool slot_mode = (g.placement_target_station >= 0);
    bool valid = true;

    if (slot_mode) {
        int s = g.placement_target_station;
        if (s < 0 || s >= MAX_STATIONS) return;
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st)) return;
        target = module_world_pos_ring(st, g.placement_target_ring, g.placement_target_slot);
    } else {
        /* Found-new-outpost preview: show reticle at the scaffold's position.
         * Color it red if signal is too weak / placement is invalid. */
        int idx = LOCAL_PLAYER.ship.towed_scaffold;
        if (idx < 0 || idx >= MAX_SCAFFOLDS) return;
        const scaffold_t *sc = &g.world.scaffolds[idx];
        if (!sc->active) return;
        target = sc->pos;
        /* Validity: has signal, not deep in core coverage, and not too close */
        float gsig = signal_strength_at(&g.world, target);
        valid = gsig > 0.0f && gsig < OUTPOST_MAX_SIGNAL;
        if (valid) {
            for (int s = 0; s < MAX_STATIONS; s++) {
                const station_t *st = &g.world.stations[s];
                if (!station_exists(st)) continue;
                if (v2_dist_sq(st->pos, target) < OUTPOST_MIN_DISTANCE * OUTPOST_MIN_DISTANCE) {
                    valid = false; break;
                }
            }
        }
    }

    float pulse = 0.5f + 0.4f * sinf(g.world.time * 5.0f);
    float r = valid ? 0.4f : 1.0f;
    float g0 = valid ? 1.0f : 0.3f;
    float b = valid ? 1.0f : 0.3f;

    float cue_prev = world_signal_visual_enter_cue();
    if (slot_mode) {
        /* Slot reticle: small precise crosshair */
        draw_circle_outline(target, 30.0f, 24, r, g0, b, pulse);
        draw_circle_outline(target, 24.0f, 24, r, g0, b, pulse * 0.6f);
        sgl_begin_lines();
        sgl_c4f(r, g0, b, pulse);
        float tick = 8.0f;
        sgl_v2f(target.x - 36.0f, target.y); sgl_v2f(target.x - 36.0f + tick, target.y);
        sgl_v2f(target.x + 36.0f, target.y); sgl_v2f(target.x + 36.0f - tick, target.y);
        sgl_v2f(target.x, target.y - 36.0f); sgl_v2f(target.x, target.y - 36.0f + tick);
        sgl_v2f(target.x, target.y + 36.0f); sgl_v2f(target.x, target.y + 36.0f - tick);
        sgl_end();
        draw_segment(LOCAL_PLAYER.ship.pos, target, r, g0, b, pulse * 0.5f);
    } else {
        /* Outpost-founding reticle: larger, dashed circle showing the
         * approximate dock radius of the outpost-to-be. */
        draw_circle_outline(target, OUTPOST_DOCK_RADIUS, 32, r, g0, b, pulse * 0.7f);
        draw_circle_outline(target, OUTPOST_RADIUS, 18, r, g0, b, pulse);
        /* Compass tick marks */
        sgl_begin_lines();
        sgl_c4f(r, g0, b, pulse);
        for (int i = 0; i < 4; i++) {
            float a = (float)i * (TWO_PI_F / 4.0f);
            float r1 = OUTPOST_DOCK_RADIUS - 6.0f;
            float r2 = OUTPOST_DOCK_RADIUS + 6.0f;
            sgl_v2f(target.x + cosf(a) * r1, target.y + sinf(a) * r1);
            sgl_v2f(target.x + cosf(a) * r2, target.y + sinf(a) * r2);
        }
        sgl_end();
    }
    world_signal_visual_leave_cue(cue_prev);
}

void draw_shipyard_intake_beams(void) {
    /* Find each nascent scaffold and draw beams from contributing modules
     * (producer modules of the required commodity, plus the shipyard itself)
     * converging on the scaffold at the station center. */
    float cue_prev = world_signal_visual_enter_cue();
    for (int si = 0; si < MAX_SCAFFOLDS; si++) {
        const scaffold_t *sc = &g.world.scaffolds[si];
        if (!sc->active || sc->state != SCAFFOLD_NASCENT) continue;
        int s = sc->built_at_station;
        if (s < 0 || s >= MAX_STATIONS) continue;
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st)) continue;

        commodity_t mat = module_build_material_lookup(sc->module_type);
        module_type_t prod_type = producer_for_commodity_client(mat);

        vec2 target = sc->pos;
        float t = g.world.time * 4.0f;

        /* Beam from each contributing module */
        for (int i = 0; i < st->module_count; i++) {
            if (st->modules[i].scaffold) continue;
            bool is_yard = (st->modules[i].type == MODULE_SHIPYARD);
            bool is_prod = (st->modules[i].type == prod_type);
            if (!is_yard && !is_prod) continue;

            vec2 mod_pos = module_world_pos_ring(st, st->modules[i].ring, st->modules[i].slot);
            float pulse = 0.4f + 0.3f * sinf(t + (float)i * 0.7f);
            if (is_yard) pulse *= 0.7f; /* shipyard line is steadier */

            /* Different color for shipyard vs producer */
            float r = is_yard ? 0.5f : 1.0f;
            float gc = is_yard ? 0.75f : 0.85f;
            float b = is_yard ? 1.0f : 0.47f;

            draw_segment(mod_pos, target, r, gc, b, pulse);

            /* Flow dots along the beam */
            int dots = 4;
            for (int d = 0; d < dots; d++) {
                float frac = fmodf((t * 0.18f) + (float)d / (float)dots, 1.0f);
                vec2 p = v2_add(mod_pos, v2_scale(v2_sub(target, mod_pos), frac));
                draw_circle_filled(p, 2.5f, 6, r, gc, b, pulse + 0.15f);
            }
        }
    }
    world_signal_visual_leave_cue(cue_prev);
}


/* ================================================================== */
/* Hail ping — expanding yellow ring from ship on H-press             */
/* ================================================================== */

/* Zoom-out is relatively quick (~0.5s) so the player feels the camera
 * react to the ping. Zoom-BACK is deliberately long (~5s, ~88% of the
 * lifecycle) so the drift home is almost imperceptible — you notice
 * the world opening up, you don't notice it closing. */
static float ping_ease_out(float t) {
    float u = 1.0f - t;
    return 1.0f - (u * u * u);
}

static float ping_smoothstep(float edge0, float edge1, float x) {
    float t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0f) t = 0.0f;
    else if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* Camera zoom envelope: quick smoothstep out, brief hold, very slow
 * smoothstep back. The return is stretched across ~5s so it drifts
 * in almost imperceptibly while the player is back in normal flight. */
float hail_ping_camera_zoom(void) {
    if (g.hail_ping_timer <= 0.0f || g.hail_ping_timer > HAIL_PING_LIFECYCLE) return 1.0f;
    float n = g.hail_ping_timer / HAIL_PING_LIFECYCLE;
    float ramp_in  = ping_smoothstep(0.00f, HAIL_PING_IN_END, n);
    float ramp_out = 1.0f - ping_smoothstep(HAIL_PING_HOLD_END, 1.00f, n);
    float envelope = ramp_in < ramp_out ? ramp_in : ramp_out;
    return 1.0f + (HAIL_PING_PEAK_ZOOM - 1.0f) * envelope;
}

void draw_hail_ping(void) {
    if (g.hail_ping_timer <= 0.0f) return;
    float t = g.hail_ping_timer / HAIL_PING_DURATION;
    if (t >= 1.0f) return;
    float cue_prev = world_signal_visual_enter_cue();
    float e = ping_ease_out(t);
    /* Scale the visual ring to the camera view so the sweep is always
     * on-screen regardless of comm_range vs. window size. Treats the
     * ring as a radar-pulse indicator rather than a literal radius
     * (the hail overlay is what tells you which station responded). */
    float cam_half = (g_cam_right - g_cam_left) * 0.5f;
    float cam_v    = (g_cam_bottom - g_cam_top) * 0.5f;
    if (cam_v < cam_half) cam_half = cam_v;
    float visual_max = cam_half * 0.88f;
    /* Cap by the local scan/tag range so the pulse matches the objects
     * that can reveal temporary hail labels. */
    if (visual_max > g.hail_ping_range) visual_max = g.hail_ping_range;
    float r = visual_max * e;
    /* Softer: lower alpha, thinner pad, drop the inner afterglow. */
    float alpha = (1.0f - t) * 0.45f;
    float soft  = (1.0f - t) * 0.18f;
    const float pad = 2.0f;
    draw_circle_outline(g.hail_ping_origin, r,        96, 1.0f, 0.88f, 0.32f, alpha);
    draw_circle_outline(g.hail_ping_origin, r + pad,  96, 1.0f, 0.88f, 0.32f, soft);
    draw_circle_outline(g.hail_ping_origin, r - pad,  96, 1.0f, 0.80f, 0.22f, soft);
    world_signal_visual_leave_cue(cue_prev);
}
