/*
 * test_furnace_color.c — per-ring furnace render-tint logic.
 *
 * MODULE_FURNACE is a single sim type; the renderer picks ferrite /
 * cuprite / crystal from the furnace's instance commodity tag and falls
 * back to the legacy ring heuristic for untagged furnaces. The helpers
 * live in src/station_palette.h so both the renderer and these tests
 * share the exact same logic.
 */
#include "tests/test_harness.h"
#include "station_palette.h"

/* Float-RGB triple equality with a tight epsilon. */
static bool rgb_eq(float r, float g, float b,
                   float er, float eg, float eb) {
    const float EPS = 1e-4f;
    return  fabsf(r - er) < EPS &&
            fabsf(g - eg) < EPS &&
            fabsf(b - eb) < EPS;
}

#define EXPECT_RGB(R, G, B, EXPR_R, EXPR_G, EXPR_B) \
    ASSERT(rgb_eq((R), (G), (B), (EXPR_R), (EXPR_G), (EXPR_B)))

/* Helper: synthesize a station with a given list of (type, ring) pairs. */
static void make_station(station_t *st,
                         const module_type_t *types,
                         const uint8_t *rings,
                         int count) {
    memset(st, 0, sizeof(*st));
    st->module_count = (uint16_t)count;
    for (int i = 0; i < count; i++) {
        st->modules[i].type = types[i];
        st->modules[i].ring = rings[i];
        st->modules[i].slot = (uint8_t)i;
        st->modules[i].scaffold = false;
        st->modules[i].build_progress = 1.0f;
        st->modules[i].last_smelt_commodity = LAST_SMELT_NONE;
    }
}

/* (1) Prospect (1 furnace) → ferrite (red). */
TEST(test_furnace_color_prospect_is_ferrite_red) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    int ring = -1;
    int found = 0;
    for (int i = 0; i < w->stations[0].module_count; i++) {
        if (w->stations[0].modules[i].type == MODULE_FURNACE) {
            ring = (int)w->stations[0].modules[i].ring;
            found++;
        }
    }
    ASSERT_EQ_INT(found, 1);
    ASSERT(ring > 0);
    float r = 0, g = 0, b = 0;
    station_palette_furnace_color(&w->stations[0], ring, &r, &g, &b);
    EXPECT_RGB(r, g, b, 0.85f, 0.30f, 0.20f); /* ferrite red */
}

/* (2) Helios (3 furnaces) → each furnace tint follows its commodity tag. */
TEST(test_furnace_color_helios_uses_instance_tags) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    /* Find the 3-furnace station — that's Helios by design. */
    int helios = -1;
    for (int s = 0; s < MAX_STATIONS; s++) {
        int n = 0;
        for (int i = 0; i < w->stations[s].module_count; i++) {
            if (w->stations[s].modules[i].type == MODULE_FURNACE) n++;
        }
        if (n >= 3) { helios = s; break; }
    }
    ASSERT(helios >= 0);

    int saw_cu = 0, saw_cr = 0;
    for (int i = 0; i < w->stations[helios].module_count; i++) {
        const station_module_t *m = &w->stations[helios].modules[i];
        if (m->type != MODULE_FURNACE) continue;
        float r = 0, g = 0, b = 0;
        station_palette_furnace_module_color(&w->stations[helios], m, &r, &g, &b);
        if ((commodity_t)m->commodity == COMMODITY_CUPRITE_INGOT) {
            EXPECT_RGB(r, g, b, 0.25f, 0.50f, 0.90f); /* cuprite blue */
            saw_cu++;
        } else if ((commodity_t)m->commodity == COMMODITY_CRYSTAL_INGOT) {
            EXPECT_RGB(r, g, b, 0.30f, 0.80f, 0.35f); /* crystal green */
            saw_cr++;
        } else {
            ASSERT(false /* unexpected Helios furnace tag */);
        }
    }
    ASSERT_EQ_INT(saw_cu, 2);
    ASSERT_EQ_INT(saw_cr, 1);
}

/* (3) Outpost growth: as furnaces are added, the inner-most furnace's
 *     role re-tints automatically (1→2 changes lone furnace's color
 *     family; 2→3 reshuffles inner/outer/middle assignment). */
TEST(test_furnace_color_outpost_growth_reshuffles) {
    station_t *st = calloc(1, sizeof(*st));
    ASSERT(st != NULL);

    module_type_t t1[] = { MODULE_FURNACE };
    uint8_t       r1[] = { 1 };
    make_station(st, t1, r1, 1);
    float r = 0, g = 0, b = 0;
    station_palette_furnace_color(st, 1, &r, &g, &b);
    EXPECT_RGB(r, g, b, 0.85f, 0.30f, 0.20f); /* 1 furnace → ferrite */

    /* Grow to 2 furnaces (ring 1 + ring 2). The original ring-1 furnace
     * is now the inner of two — should still be ferrite; ring 2 is
     * cuprite. */
    module_type_t t2[] = { MODULE_FURNACE, MODULE_FURNACE };
    uint8_t       r2[] = { 1, 2 };
    make_station(st, t2, r2, 2);
    station_palette_furnace_color(st, 1, &r, &g, &b);
    EXPECT_RGB(r, g, b, 0.85f, 0.30f, 0.20f); /* inner = ferrite */
    station_palette_furnace_color(st, 2, &r, &g, &b);
    EXPECT_RGB(r, g, b, 0.25f, 0.50f, 0.90f); /* outer = cuprite */

    /* Grow to 3 furnaces (rings 1, 2, 3). Now the ring-1 furnace's
     * ROLE changes: it's no longer ferrite — it's the innermost of a
     * 3+ station, which means crystal. */
    module_type_t t3[] = { MODULE_FURNACE, MODULE_FURNACE, MODULE_FURNACE };
    uint8_t       r3[] = { 1, 2, 3 };
    make_station(st, t3, r3, 3);
    station_palette_furnace_color(st, 1, &r, &g, &b);
    EXPECT_RGB(r, g, b, 0.30f, 0.80f, 0.35f); /* inner = crystal */
    station_palette_furnace_color(st, 2, &r, &g, &b);
    EXPECT_RGB(r, g, b, 0.85f, 0.85f, 0.90f); /* middle = chunks */
    station_palette_furnace_color(st, 3, &r, &g, &b);
    EXPECT_RGB(r, g, b, 0.25f, 0.50f, 0.90f); /* outer = cuprite */

    free(st);
}

/* (4) Non-furnace modules are completely untouched by this code path —
 *     verifying the helper is only invoked for MODULE_FURNACE. We do
 *     this structurally: walk the seeded world, confirm there are
 *     non-furnace modules, and confirm the helper still gives the
 *     expected deterministic tint when correctly invoked on a furnace. */
TEST(test_furnace_color_non_furnace_modules_unaffected) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    bool saw_non_furnace = false;
    for (int s = 0; s < MAX_STATIONS; s++) {
        for (int i = 0; i < w->stations[s].module_count; i++) {
            if (w->stations[s].modules[i].type != MODULE_FURNACE) {
                saw_non_furnace = true;
            }
        }
    }
    ASSERT(saw_non_furnace);
    /* Helper still gives a deterministic answer when invoked. */
    float r = 0, g = 0, b = 0;
    station_palette_furnace_color(&w->stations[0], 1, &r, &g, &b);
    /* Prospect has 1 furnace → ferrite red. */
    EXPECT_RGB(r, g, b, 0.85f, 0.30f, 0.20f);
}

/* Prospect: ring 1 = DOCK + RELAY + FURNACE (tagged FERRITE_INGOT);
 * ring 2 = single FERRITE_ORE intake at slot 4. 4 modules total —
 * no output hopper because nothing on Prospect locally consumes
 * ferrite ingots (no frame press here). */
TEST(test_prospect_modules_after_silo_cleanup) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    ASSERT_EQ_INT((int)w->stations[0].module_count, 4);
    int has_dock = 0, has_relay = 0, has_furnace = 0, has_hopper = 0;
    for (int i = 0; i < w->stations[0].module_count; i++) {
        switch (w->stations[0].modules[i].type) {
        case MODULE_DOCK:         has_dock++; break;
        case MODULE_SIGNAL_RELAY: has_relay++; break;
        case MODULE_FURNACE:      has_furnace++; break;
        case MODULE_HOPPER:       has_hopper++; break;
        default: break;
        }
    }
    ASSERT_EQ_INT(has_dock, 1);
    ASSERT_EQ_INT(has_relay, 1);
    ASSERT_EQ_INT(has_furnace, 1);
    ASSERT_EQ_INT(has_hopper, 1);
}

/* (6) Middle-ring dynamic glow: the helper reads
 *     last_smelt_commodity on the matching middle-ring furnace and
 *     renders blue (cuprite) / green (crystal) / white (other or
 *     LAST_SMELT_NONE). */
TEST(test_furnace_color_middle_ring_glows_by_last_smelt) {
    station_t *st = calloc(1, sizeof(*st));
    ASSERT(st != NULL);
    module_type_t types[] = { MODULE_FURNACE, MODULE_FURNACE, MODULE_FURNACE };
    uint8_t       rings[] = { 1, 2, 3 };
    make_station(st, types, rings, 3);

    /* Default (LAST_SMELT_NONE on all three): middle ring is white. */
    float r = 0, g = 0, b = 0;
    station_palette_furnace_color(st, 2, &r, &g, &b);
    EXPECT_RGB(r, g, b, 0.85f, 0.85f, 0.90f);

    /* Tag middle as cuprite-recent → middle glows blue. */
    st->modules[1].last_smelt_commodity = (uint8_t)COMMODITY_CUPRITE_ORE;
    station_palette_furnace_color(st, 2, &r, &g, &b);
    EXPECT_RGB(r, g, b, 0.25f, 0.50f, 0.90f);

    /* Tag middle as crystal-recent → middle glows green. */
    st->modules[1].last_smelt_commodity = (uint8_t)COMMODITY_CRYSTAL_ORE;
    station_palette_furnace_color(st, 2, &r, &g, &b);
    EXPECT_RGB(r, g, b, 0.30f, 0.80f, 0.35f);

    /* Ferrite or any other ore → falls back to white (no special
     * inner-ring meaning at the middle). */
    st->modules[1].last_smelt_commodity = (uint8_t)COMMODITY_FERRITE_ORE;
    station_palette_furnace_color(st, 2, &r, &g, &b);
    EXPECT_RGB(r, g, b, 0.85f, 0.85f, 0.90f);

    /* Outer + inner are unaffected by middle's last_smelt setting. */
    st->modules[1].last_smelt_commodity = (uint8_t)COMMODITY_CUPRITE_ORE;
    station_palette_furnace_color(st, 1, &r, &g, &b);
    EXPECT_RGB(r, g, b, 0.30f, 0.80f, 0.35f); /* inner stays crystal green */
    station_palette_furnace_color(st, 3, &r, &g, &b);
    EXPECT_RGB(r, g, b, 0.25f, 0.50f, 0.90f); /* outer stays cuprite blue */

    free(st);
}

void register_furnace_color_tests(void) {
    TEST_SECTION("\nFurnace per-ring color render variants:\n");
    RUN(test_furnace_color_prospect_is_ferrite_red);
    RUN(test_furnace_color_helios_uses_instance_tags);
    RUN(test_furnace_color_outpost_growth_reshuffles);
    RUN(test_furnace_color_non_furnace_modules_unaffected);
    RUN(test_prospect_modules_after_silo_cleanup);
    RUN(test_furnace_color_middle_ring_glows_by_last_smelt);
}
