#include "test_harness.h"
#include "sim_physics.h"
#include "cargo_receipt_issue.h"
#include "faction.h"
#include <stddef.h>

static bool test_issue_station_receipt(station_t *st, const uint8_t cargo_pub[32],
                                       uint64_t event_id,
                                       cargo_receipt_chain_t *out_chain) {
    uint8_t recipient[32];
    uint8_t origin_pin[32];
    for (int i = 0; i < 32; i++) {
        recipient[i] = (uint8_t)(0x30 + i);
        origin_pin[i] = (uint8_t)(0x90 + i);
    }
    memset(out_chain, 0, sizeof(*out_chain));
    if (!cargo_receipt_issue(st, 1, event_id, cargo_pub, recipient,
                             origin_pin, &out_chain->links[0])) {
        return false;
    }
    out_chain->len = 1;
    return cargo_receipt_chain_verify(out_chain->links, out_chain->len,
                                      cargo_pub) == CARGO_RECEIPT_OK;
}

static int test_find_exact_pod(const world_t *w, commodity_t c) {
    if (!w) return -1;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active || pod->kind != CARGO_POD_CARGO) continue;
        if (pod->shipment_id != 0 || pod->commodity != c) continue;
        if (pod->manifest_count == 0 || pod->manifest_count != pod->quantity)
            continue;
        bool exact = true;
        for (uint16_t u = 0; u < pod->manifest_count; u++) {
            if ((commodity_t)pod->manifest_units[u].commodity != c) {
                exact = false;
                break;
            }
        }
        if (exact) return i;
    }
    return -1;
}

static uint32_t test_crc32_update(uint32_t crc, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool test_patch_world_save_version(const char *path, uint32_t version) {
    FILE *f = fopen(path, "r+b");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long len = ftell(f);
    if (len < 16) {
        fclose(f);
        return false;
    }
    if (fseek(f, 4, SEEK_SET) != 0 ||
        fwrite(&version, sizeof(version), 1, f) != 1) {
        fclose(f);
        return false;
    }

    long data_end = len;
    uint32_t trailer_magic = 0;
    if (len >= 8 && fseek(f, len - 8, SEEK_SET) == 0 &&
        fread(&trailer_magic, sizeof(trailer_magic), 1, f) == 1 &&
        trailer_magic == 0x43524332u) {
        data_end = len - 8;
    }
    if (data_end == len) {
        fclose(f);
        return true;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    uint32_t crc = 0;
    long remaining = data_end;
    uint8_t chunk[4096];
    while (remaining > 0) {
        size_t want = remaining < (long)sizeof(chunk)
            ? (size_t)remaining
            : sizeof(chunk);
        if (fread(chunk, 1, want, f) != want) {
            fclose(f);
            return false;
        }
        crc = test_crc32_update(crc, chunk, want);
        remaining -= (long)want;
    }
    if (fseek(f, data_end + 4, SEEK_SET) != 0 ||
        fwrite(&crc, sizeof(crc), 1, f) != 1) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static int test_count_exact_frame_pod_units(const world_t *w) {
    if (!w) return 0;
    int total = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active || pod->kind != CARGO_POD_CARGO) continue;
        if (pod->shipment_id != 0 || pod->commodity != COMMODITY_FRAME)
            continue;
        if (pod->manifest_count == 0 || pod->manifest_count != pod->quantity)
            continue;
        bool exact = true;
        for (uint16_t u = 0; u < pod->manifest_count; u++) {
            if ((commodity_t)pod->manifest_units[u].commodity !=
                COMMODITY_FRAME) {
                exact = false;
                break;
            }
        }
        if (exact) total += (int)pod->manifest_count;
    }
    return total;
}

static bool test_patch_catalog_version(const char *path, uint32_t version) {
    FILE *f = fopen(path, "rb+");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 12) { fclose(f); return false; }
    if (version < 6) {
        long text_extra = (long)(sizeof(((station_t *)0)->miner_chatter) +
                                 sizeof(((station_t *)0)->hauler_chatter) +
                                 sizeof(((station_t *)0)->rati_hail_message));
        long slug_len = (long)sizeof(((station_t *)0)->station_slug);
        long crc_len = (long)sizeof(uint32_t);
        if (len > text_extra + slug_len + crc_len) {
            long remove_at = len - crc_len - slug_len - text_extra;
            long tail_len = slug_len;
            uint8_t tail[sizeof(((station_t *)0)->station_slug)];
            fseek(f, remove_at + text_extra, SEEK_SET);
            if (fread(tail, 1, (size_t)tail_len, f) != (size_t)tail_len) {
                fclose(f);
                return false;
            }
            fseek(f, remove_at, SEEK_SET);
            if (fwrite(tail, 1, (size_t)tail_len, f) != (size_t)tail_len) {
                fclose(f);
                return false;
            }
            len -= text_extra;
            fflush(f);
        }
    }
    fseek(f, 4, SEEK_SET);
    if (fwrite(&version, sizeof(version), 1, f) != 1) {
        fclose(f);
        return false;
    }
    fflush(f);

    fseek(f, 0, SEEK_SET);
    uint32_t crc = 0;
    long remaining = len - (long)sizeof(uint32_t);
    uint8_t chunk[4096];
    while (remaining > 0) {
        size_t want = remaining < (long)sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
        size_t n = fread(chunk, 1, want, f);
        if (n == 0) { fclose(f); return false; }
        crc = test_crc32_update(crc, chunk, n);
        remaining -= (long)n;
    }
    fseek(f, len - (long)sizeof(uint32_t), SEEK_SET);
    if (fwrite(&crc, sizeof(crc), 1, f) != 1) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static bool test_rewrite_crc32_trailer(const char *path) {
    FILE *f = fopen(path, "rb+");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 8) { fclose(f); return false; }
    long trailer = len - 8;
    fseek(f, 0, SEEK_SET);
    uint32_t crc = 0;
    long remaining = trailer;
    uint8_t chunk[4096];
    while (remaining > 0) {
        size_t want = remaining < (long)sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
        size_t n = fread(chunk, 1, want, f);
        if (n == 0) { fclose(f); return false; }
        crc = test_crc32_update(crc, chunk, n);
        remaining -= (long)n;
    }
    fseek(f, len - 4, SEEK_SET);
    if (fwrite(&crc, sizeof(crc), 1, f) != 1) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static bool test_patch_file_byte(const char *path, long offset, uint8_t value) {
    FILE *f = fopen(path, "rb+");
    if (!f) return false;
    if (fseek(f, offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    bool ok = fwrite(&value, 1, 1, f) == 1;
    fclose(f);
    return ok;
}

static bool test_copy_file_prefix(const char *src_path,
                                  const char *dst_path,
                                  long prefix_len) {
    if (!src_path || !dst_path || prefix_len < 0) return false;
    FILE *src = fopen(src_path, "rb");
    if (!src) return false;
    FILE *dst = fopen(dst_path, "wb");
    if (!dst) {
        fclose(src);
        return false;
    }
    bool ok = true;
    uint8_t chunk[4096];
    long remaining = prefix_len;
    while (remaining > 0) {
        size_t want = remaining < (long)sizeof(chunk)
            ? (size_t)remaining
            : sizeof(chunk);
        if (fread(chunk, 1, want, src) != want ||
            fwrite(chunk, 1, want, dst) != want) {
            ok = false;
            break;
        }
        remaining -= (long)want;
    }
    if (fclose(src) != 0) ok = false;
    if (fclose(dst) != 0) ok = false;
    return ok;
}

static long test_find_bytes_in_file(const char *path, const uint8_t *needle,
                                    size_t needle_len) {
    if (!needle || needle_len == 0) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t data[8192];
    size_t len = fread(data, 1, sizeof(data), f);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok || len < needle_len) return -1;
    for (size_t i = 0; i + needle_len <= len; i++) {
        if (memcmp(&data[i], needle, needle_len) == 0) return (long)i;
    }
    return -1;
}

static bool test_furnace_has_adjacent_ore_hopper_save(const station_t *st,
                                                      const station_module_t *furnace) {
    commodity_t ore = module_instance_input_ore(furnace);
    if (ore == COMMODITY_COUNT) return false;
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *hopper = &st->modules[i];
        if (hopper->scaffold) continue;
        if (hopper->type != MODULE_HOPPER) continue;
        if ((commodity_t)hopper->commodity != ore) continue;
        int dr = (int)hopper->ring - (int)furnace->ring;
        if (dr == 1 || dr == -1) return true;
    }
    return false;
}

TEST(test_player_save_load_roundtrip) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    player_init_ship(&w->players[0], w);
    w->players[0].connected = true;
    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS, TMP("test_cat")));
    ASSERT(world_save(w, TMP("test_player.sav")));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    station_catalog_load_all(loaded->stations, MAX_STATIONS, TMP("test_cat"));
    ASSERT(world_load(loaded, TMP("test_player.sav")));
    /* Players are cleared on load (they reconnect) */
    ASSERT(!loaded->players[0].connected);
    /* But world state (stations, etc.) survives */
    ASSERT_EQ_FLOAT(loaded->stations[0].signal_range, w->stations[0].signal_range, 0.01f);
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove(TMP("test_player.sav"));
}

TEST(test_v3_station_catalog_repairs_helios_smelter_layout) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    station_t *helios = &w->stations[2];

    int cu_seen = 0;
    for (int m = 0; m < helios->module_count; m++) {
        station_module_t *mod = &helios->modules[m];
        if (mod->type == MODULE_HOPPER) {
            commodity_t c = (commodity_t)mod->commodity;
            if (c == COMMODITY_CUPRITE_ORE) {
                mod->ring = 3; mod->slot = 0;
            } else if (c == COMMODITY_CRYSTAL_INGOT) {
                mod->ring = 2; mod->slot = 3;
            } else if (c == COMMODITY_CRYSTAL_ORE) {
                mod->ring = 3; mod->slot = 6;
            }
        } else if (mod->type == MODULE_FURNACE &&
                   (commodity_t)mod->commodity == COMMODITY_CUPRITE_INGOT) {
            if (cu_seen == 1) {
                mod->ring = 3; mod->slot = 1;
            }
            cu_seen++;
        }
    }

    for (int m = helios->module_count - 1; m >= 0; m--) {
        bool drop = helios->modules[m].type == MODULE_SHIPYARD;
        if (helios->modules[m].type == MODULE_HOPPER &&
            (commodity_t)helios->modules[m].commodity == COMMODITY_FRAME) {
            drop = true;
        }
        if (!drop) continue;
        for (int k = m + 1; k < helios->module_count; k++)
            helios->modules[k - 1] = helios->modules[k];
        helios->module_count--;
    }
    ASSERT(!station_has_module(helios, MODULE_SHIPYARD));
    ASSERT(station_find_hopper_for(helios, COMMODITY_FRAME) < 0);

    const char *dir = TMP("test_v3_helios_cat");
    ASSERT(station_catalog_save(helios, 2, dir));
    char path[256];
    snprintf(path, sizeof(path), "%s/2.cat", dir);
    ASSERT(test_patch_catalog_version(path, 3));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    ASSERT_EQ_INT(station_catalog_load_all(loaded->stations, MAX_STATIONS, dir), 1);
    ASSERT(station_has_module(&loaded->stations[2], MODULE_SHIPYARD));
    ASSERT(station_find_hopper_for(&loaded->stations[2], COMMODITY_FRAME) >= 0);
    int checked_furnaces = 0;
    for (int m = 0; m < loaded->stations[2].module_count; m++) {
        const station_module_t *mod = &loaded->stations[2].modules[m];
        if (mod->type != MODULE_FURNACE) continue;
        ASSERT(test_furnace_has_adjacent_ore_hopper_save(&loaded->stations[2], mod));
        checked_furnaces++;
    }
    ASSERT_EQ_INT(checked_furnaces, 3);
    int yard = -1;
    for (int m = 0; m < loaded->stations[2].module_count; m++) {
        if (loaded->stations[2].modules[m].type == MODULE_SHIPYARD) {
            yard = m;
            break;
        }
    }
    ASSERT(yard >= 0);
    ASSERT_EQ_INT(station_module_layout_status(&loaded->stations[2],
                                               &loaded->stations[2].modules[yard]),
                  STATION_LAYOUT_OK);
    remove(path);
}

TEST(test_station_catalog_bad_file_preserves_seeded_fallback) {
    WORLD_HEAP seeded = calloc(1, sizeof(world_t));
    ASSERT(seeded != NULL);
    world_reset(seeded);

    const char *dir = TMP("test_bad_station_catalog");
    ASSERT(station_catalog_save(&seeded->stations[3], 3, dir));

    char bad_path[256];
    snprintf(bad_path, sizeof(bad_path), "%s/0.cat", dir);
    FILE *f = fopen(bad_path, "wb");
    ASSERT(f != NULL);
    ASSERT(fwrite("bad", 1, 3, f) == 3);
    fclose(f);

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    world_reset(loaded);
    float expected_range = loaded->stations[0].signal_range;
    ASSERT(expected_range > 0.0f);

    ASSERT_EQ_INT(station_catalog_load_all(loaded->stations, MAX_STATIONS, dir), 1);
    ASSERT(station_exists(&loaded->stations[0]));
    ASSERT_EQ_FLOAT(loaded->stations[0].signal_range, expected_range, 0.01f);
    ASSERT(station_exists(&loaded->stations[3]));

    remove(bad_path);
}

TEST(test_world_save_load_preserves_stations) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    w->stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 42.0f;
    w->stations[0]._inventory_cache[COMMODITY_FRAME] = 15.0f;
    ASSERT(world_save(w, TMP("test_world.sav")));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(world_load(loaded, TMP("test_world.sav")));
    ASSERT_EQ_FLOAT(loaded->stations[0]._inventory_cache[COMMODITY_FERRITE_ORE], 42.0f, 0.01f);
    ASSERT_EQ_FLOAT(loaded->stations[0]._inventory_cache[COMMODITY_FRAME], 15.0f, 0.01f);
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove(TMP("test_world.sav"));
}

TEST(test_world_save_load_preserves_station_factions) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    w->stations[2].faction_relations[STATION_FACTION_BLACKGLASS_SYNDICATE] = -99;
    w->stations[2].faction_allegiance = STATION_FACTION_KEPLER_COMPACT;
    ASSERT(world_save(w, TMP("test_station_factions.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    ASSERT(world_load(loaded, TMP("test_station_factions.sav")));

    ASSERT_EQ_INT(loaded->stations[2].faction_id,
                  STATION_FACTION_HELIOS_CONSORTIUM);
    ASSERT_EQ_INT(loaded->stations[2].faction_allegiance,
                  STATION_FACTION_KEPLER_COMPACT);
    ASSERT_EQ_INT(loaded->stations[2].faction_ideology,
                  STATION_IDEOLOGY_EXPANSIONIST);
    ASSERT_EQ_INT(loaded->stations[2].faction_relations[
                      STATION_FACTION_BLACKGLASS_SYNDICATE], -99);
    remove(TMP("test_station_factions.sav"));
}

TEST(test_world_save_load_preserves_npcs) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int i = 0; i < 600; i++) world_sim_step(w, SIM_DT);
    ASSERT(world_save(w, TMP("test_npcs.sav")));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(world_load(loaded, TMP("test_npcs.sav")));
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        ASSERT_EQ_FLOAT(loaded->npc_ships[i].ship.pos.x, w->npc_ships[i].ship.pos.x, 0.01f);
        ASSERT_EQ_FLOAT(loaded->npc_ships[i].ship.pos.y, w->npc_ships[i].ship.pos.y, 0.01f);
    }
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove(TMP("test_npcs.sav"));
}

TEST(test_npc_ship_physics_in_sync_each_tick) {
    /* Tripwire for the Slice 13 physics flip and any future mirror
     * direction change: at the end of every sim step, every active
     * NPC's paired ship_t must agree with its npc_ship_t on hull,
     * hull_class, pos, vel, and angle. A missed write site lasts at
     * most one tick before the reverse mirror washes it out, so
     * behavioral tests don't catch it — this one does, immediately.
     * Runs for 10 sim seconds (1200 ticks @ 120 Hz) to cover spawn,
     * mine, dock, hauler-in-transit, and at least one despawn cycle. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int t = 0; t < 1200; t++) {
        world_sim_step(w, SIM_DT);
        for (int n = 0; n < MAX_NPC_SHIPS; n++) {
            const npc_ship_t *npc = &w->npc_ships[n];
            if (!npc->active) continue;
            const ship_t *s = world_npc_ship_for(w, n);
            ASSERT(s != NULL);
            ASSERT_EQ_FLOAT(s->hull, npc->hull, 0.001f);
            ASSERT(s->hull_class == npc->ship.hull_class);
            ASSERT_EQ_FLOAT(s->pos.x, npc->ship.pos.x, 0.001f);
            ASSERT_EQ_FLOAT(s->pos.y, npc->ship.pos.y, 0.001f);
            ASSERT_EQ_FLOAT(s->vel.x, npc->ship.vel.x, 0.001f);
            ASSERT_EQ_FLOAT(s->vel.y, npc->ship.vel.y, 0.001f);
            ASSERT_EQ_FLOAT(s->angle, npc->ship.angle, 0.001f);
        }
    }
}

TEST(test_world_load_rebuilds_character_pool) {
    /* world_load only restores npc_ships[]; the paired character_t /
     * ships[] pools are server-side transient and have to be rebuilt
     * via rebuild_characters_from_npcs at the end of load. Verify
     * (a) every active NPC has a paired character with the right kind
     * and ship_idx, (b) apply_npc_ship_damage hits the rebuilt ship
     * (not a phantom one), (c) the paired ship sees the damage on the
     * very next sim step's reverse-mirror back to npc.hull. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int i = 0; i < 600; i++) world_sim_step(w, SIM_DT);
    ASSERT(world_save(w, TMP("test_char_pool.sav")));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(world_load(loaded, TMP("test_char_pool.sav")));

    /* (a) paired-pool integrity */
    int active_npcs = 0;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!loaded->npc_ships[n].active) continue;
        active_npcs++;
        int char_cap = (int)(sizeof(loaded->characters) /
                             sizeof(loaded->characters[0]));
        int found_char = -1;
        for (int c = 0; c < char_cap; c++) {
            if (!loaded->characters[c].active) continue;
            if (loaded->characters[c].npc_slot == n) { found_char = c; break; }
        }
        ASSERT(found_char >= 0);
        const character_t *c = &loaded->characters[found_char];
        ASSERT(c->kind == CHARACTER_KIND_NPC_MINER ||
               c->kind == CHARACTER_KIND_NPC_HAULER ||
               c->kind == CHARACTER_KIND_NPC_TOW);
        ASSERT(c->ship_idx >= 0 && c->ship_idx < MAX_SHIPS);
    }
    ASSERT(active_npcs > 0);

    /* (b)+(c) damage flows through rebuilt ship slot */
    int target = -1;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (loaded->npc_ships[n].active) { target = n; break; }
    }
    ASSERT(target >= 0);
    float pre = loaded->npc_ships[target].hull;
    ASSERT(pre > 5.0f);
    apply_npc_ship_damage(loaded, target, 5.0f);
    /* npc.hull only updates after the next sim step's reverse mirror;
     * one tick is enough. */
    world_sim_step(loaded, SIM_DT);
    ASSERT(loaded->npc_ships[target].hull < pre);

    remove(TMP("test_char_pool.sav"));
}

TEST(test_world_save_load_preserves_fracture_children) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    asteroid_t *a;
    fracture_claim_state_t *state;

    ASSERT(w != NULL);
    ASSERT(loaded != NULL);
    world_reset(w);
    a = &w->asteroids[17];
    state = &w->fracture_claims[17];
    memset(a, 0, sizeof(*a));
    memset(state, 0, sizeof(*state));

    a->active = true;
    a->fracture_child = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_CRYSTAL_ORE;
    a->pos = v2(321.0f, -654.0f);
    a->vel = v2(7.0f, -3.5f);
    a->radius = 9.0f;
    a->hp = 4.0f;
    a->max_hp = 9.0f;
    a->ore = 6.0f;
    a->max_ore = 9.0f;
    a->rotation = 1.25f;
    a->spin = 0.4f;
    a->seed = 22.0f;
    a->age = 12.0f;
    a->smelt_progress = 0.35f;
    a->crystal_stage = CRYSTAL_STAGE_INTERMEDIATE;
    a->crystal_stage_station = 2;
    a->crystal_stage_module = 7;
    a->last_towed_by = 2;
    a->last_fractured_by = 1;
    memcpy(a->last_towed_token, "TOWTOKEN", 8);
    asteroid_mark_thrown(a, (const uint8_t *)"BALLTOKN", 4.2f);
    memcpy(a->last_fractured_token, "FRAGTOKN", 8);
    for (int i = 0; i < 32; i++) {
        a->fracture_seed[i] = (uint8_t)(0x60 + i);
        a->fragment_pub[i] = (uint8_t)(0x90 + i);
    }
    a->grade = MINING_GRADE_RATI;

    state->active = true;
    state->fracture_id = 444;
    state->deadline_ms = 123456;
    state->burst_cap = FRACTURE_CHALLENGE_BURST_CAP;
    state->best_nonce = 19;
    state->best_grade = MINING_GRADE_FINE;
    for (int i = 0; i < 32; i++)
        state->best_player_pub[i] = (uint8_t)(0xC0 + i);
    state->seen_claimant_count = 2;
    memcpy(state->seen_claimant_tokens[0], "CLAIM001", 8);
    memcpy(state->seen_claimant_tokens[1], "CLAIM002", 8);
    w->next_fracture_id = 555;

    ASSERT(world_save(w, TMP("test_fracture_children.sav")));
    ASSERT(world_load(loaded, TMP("test_fracture_children.sav")));

    ASSERT_EQ_INT(loaded->next_fracture_id, 555);
    ASSERT(loaded->asteroids[17].active);
    ASSERT(loaded->asteroids[17].fracture_child);
    ASSERT_EQ_INT(loaded->asteroids[17].tier, ASTEROID_TIER_S);
    ASSERT_EQ_INT(loaded->asteroids[17].commodity, COMMODITY_CRYSTAL_ORE);
    ASSERT_EQ_FLOAT(loaded->asteroids[17].pos.x, 321.0f, 0.01f);
    ASSERT_EQ_FLOAT(loaded->asteroids[17].pos.y, -654.0f, 0.01f);
    ASSERT_EQ_FLOAT(loaded->asteroids[17].smelt_progress, 0.35f, 0.01f);
    ASSERT_EQ_INT(loaded->asteroids[17].crystal_stage, CRYSTAL_STAGE_INTERMEDIATE);
    ASSERT_EQ_INT(loaded->asteroids[17].crystal_stage_station, 2);
    ASSERT_EQ_INT(loaded->asteroids[17].crystal_stage_module, 7);
    ASSERT_EQ_INT(loaded->asteroids[17].grade, MINING_GRADE_RATI);
    ASSERT(memcmp(loaded->asteroids[17].fracture_seed, a->fracture_seed, 32) == 0);
    ASSERT(memcmp(loaded->asteroids[17].fragment_pub, a->fragment_pub, 32) == 0);
    ASSERT(memcmp(loaded->asteroids[17].last_towed_token, a->last_towed_token, 8) == 0);
    ASSERT(memcmp(loaded->asteroids[17].thrown_by_token, a->thrown_by_token, 8) == 0);
    ASSERT_EQ_INT(loaded->asteroids[17].thrown_timer_q, a->thrown_timer_q);
    ASSERT(memcmp(loaded->asteroids[17].last_fractured_token, a->last_fractured_token, 8) == 0);
    ASSERT(loaded->fracture_claims[17].active);
    ASSERT(!loaded->fracture_claims[17].resolved);
    ASSERT(loaded->fracture_claims[17].challenge_dirty);
    ASSERT_EQ_INT(loaded->fracture_claims[17].fracture_id, 444);
    ASSERT_EQ_INT(loaded->fracture_claims[17].deadline_ms, 123456);
    ASSERT_EQ_INT(loaded->fracture_claims[17].best_nonce, 19);
    ASSERT_EQ_INT(loaded->fracture_claims[17].seen_claimant_count, 2);
    ASSERT(memcmp(loaded->fracture_claims[17].best_player_pub,
                  state->best_player_pub, 32) == 0);
    ASSERT(memcmp(loaded->fracture_claims[17].seen_claimant_tokens[0],
                  state->seen_claimant_tokens[0], 8) == 0);
    ASSERT(memcmp(loaded->fracture_claims[17].seen_claimant_tokens[1],
                  state->seen_claimant_tokens[1], 8) == 0);

    remove(TMP("test_fracture_children.sav"));
}

TEST(test_world_load_preserves_fracture_claim_dedupe_identity) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    asteroid_t *a;
    fracture_claim_state_t *state;
    uint8_t player_pub[32];
    uint32_t best_nonce = 0;
    mining_grade_t best_grade = MINING_GRADE_COMMON;

    ASSERT(w != NULL);
    ASSERT(loaded != NULL);
    world_reset(w);
    a = &w->asteroids[9];
    state = &w->fracture_claims[9];
    memset(a, 0, sizeof(*a));
    memset(state, 0, sizeof(*state));

    w->players[0].connected = true;
    w->players[0].session_ready = true;
    memcpy(w->players[0].session_token, "PERSIST01", 8);
    w->players[0].ship.pos = w->stations[0].pos;

    a->active = true;
    a->fracture_child = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 1.0f;
    a->max_ore = 1.0f;
    a->radius = 7.0f;
    a->pos = w->stations[0].pos;
    for (int i = 0; i < 32; i++) a->fracture_seed[i] = (uint8_t)(0x20 + i);

    state->active = true;
    state->fracture_id = 818;
    state->deadline_ms = 600;
    state->burst_cap = FRACTURE_CHALLENGE_BURST_CAP;

    sha256_bytes(w->players[0].session_token, 8, player_pub);
    mining_find_best_claim(a->fracture_seed, player_pub, state->burst_cap,
                           &best_nonce, &best_grade);
    ASSERT(submit_fracture_claim(w, 0, state->fracture_id, best_nonce,
                                 (uint8_t)best_grade));

    ASSERT(world_save(w, TMP("test_fracture_claim_dedupe.sav")));
    ASSERT(world_load(loaded, TMP("test_fracture_claim_dedupe.sav")));

    loaded->players[1].connected = true;
    loaded->players[1].session_ready = true;
    memcpy(loaded->players[1].session_token, w->players[0].session_token, 8);
    loaded->players[1].ship.pos = loaded->stations[0].pos;
    ASSERT(!submit_fracture_claim(loaded, 1, 818, best_nonce, (uint8_t)best_grade));

    remove(TMP("test_fracture_claim_dedupe.sav"));
}

TEST(test_world_load_missing_file) {
    WORLD_DECL;
    ASSERT(!world_load(&w, TMP("nonexistent_save_file.sav")));
}

TEST(test_player_save_load_preserves_ship) {
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, &w);
    sp.connected = true;
    sp.ship.hull = 42.0f;
    sp.ship.cargo[COMMODITY_FERRITE_ORE] = 10.0f;
    sp.ship.cargo[COMMODITY_CUPRITE_ORE] = 5.0f;
    sp.ship.mining_level = 2;
    sp.ship.hold_level = 1;
    sp.ship.tractor_level = 3;
    sp.current_station = 1;
    ASSERT(player_save(&sp, test_tmp_dir(), 99));

    SERVER_PLAYER_DECL(loaded);
    ASSERT(player_load(&loaded, &w, test_tmp_dir(), 99));
    ASSERT_EQ_FLOAT(loaded.ship.hull, 42.0f, 0.01f);
    ASSERT_EQ_FLOAT(loaded.ship.cargo[COMMODITY_FERRITE_ORE], 10.0f, 0.01f);
    ASSERT_EQ_FLOAT(loaded.ship.cargo[COMMODITY_CUPRITE_ORE], 5.0f, 0.01f);
    ASSERT_EQ_INT(loaded.ship.mining_level, 2);
    ASSERT_EQ_INT(loaded.ship.hold_level, 1);
    ASSERT_EQ_INT(loaded.ship.tractor_level, 3);
    ASSERT_EQ_INT(loaded.current_station, 1);
    ASSERT(loaded.docked);
    remove(TMP("player_99.sav"));
}

TEST(test_player_load_prefers_existing_bound_ship_asset) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->id = 0;
    sp->connected = true;
    memset(sp->session_token, 0x5A, sizeof(sp->session_token));

    ship_asset_t *miner = world_ship_asset_mint(
        &w, HULL_CLASS_MINER, SHIP_ASSET_OWNER_PLAYER_SESSION,
        -1, 0, SHIP_ASSET_PROVENANCE_SHIPYARD,
        false, 0, NULL, sp->session_token);
    ship_asset_t *hauler = world_ship_asset_mint(
        &w, HULL_CLASS_HAULER, SHIP_ASSET_OWNER_PLAYER_SESSION,
        -1, 1, SHIP_ASSET_PROVENANCE_SHIPYARD,
        false, 1, NULL, sp->session_token);
    ASSERT(miner != NULL);
    ASSERT(hauler != NULL);
    ASSERT(ship_asset_claim_for_player(&w, 0, 1));
    ASSERT_EQ_INT(sp->ship_asset_id, hauler->asset_id);
    ASSERT_EQ_INT(sp->ship.hull_class, HULL_CLASS_HAULER);
    sp->ship.hull = 88.0f;
    ASSERT(player_save(sp, test_tmp_dir(), 0));

    sp->ship.hull_class = HULL_CLASS_MINER;
    sp->ship.hull = 1.0f;
    sp->current_station = 0;
    ASSERT(player_load(sp, &w, test_tmp_dir(), 0));

    ASSERT_EQ_INT(sp->ship_asset_id, hauler->asset_id);
    ASSERT_EQ_INT(sp->ship.hull_class, HULL_CLASS_HAULER);
    ASSERT_EQ_FLOAT(sp->ship.hull, 88.0f, 0.01f);
    ASSERT_EQ_INT(hauler->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(hauler->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT_EQ_INT(hauler->operator_slot, 0);
    ASSERT_EQ_INT(miner->status, SHIP_ASSET_STATUS_STORED);

    char path[256];
    ASSERT(player_save_path(path, sizeof(path), test_tmp_dir(), sp, 0));
    remove(path);
}

TEST(test_player_load_prefers_owned_asset_over_provisional_loaner) {
    WORLD_DECL;
    world_reset(&w);
    uint8_t token[8];
    memset(token, 0x6B, sizeof(token));

    SERVER_PLAYER_DECL(saved);
    saved.id = 0;
    saved.connected = true;
    saved.session_ready = true;
    memcpy(saved.session_token, token, sizeof(token));
    ASSERT(ship_manifest_bootstrap(&saved.ship));
    saved.ship.hull_class = HULL_CLASS_HAULER;
    saved.ship.hull = 77.0f;
    saved.current_station = 1;
    ASSERT(player_save(&saved, test_tmp_dir(), 0));

    ship_asset_t *owned = world_ship_asset_mint(
        &w, HULL_CLASS_HAULER, SHIP_ASSET_OWNER_PLAYER_SESSION,
        -1, 1, SHIP_ASSET_PROVENANCE_SHIPYARD,
        false, 1, NULL, token);
    ASSERT(owned != NULL);

    server_player_t *sp = &w.players[0];
    sp->id = 0;
    sp->connected = true;
    player_init_ship(sp, &w);
    uint32_t provisional_id = sp->ship_asset_id;
    ASSERT(provisional_id != SHIP_ASSET_ID_NONE);
    ASSERT(provisional_id != owned->asset_id);
    ship_asset_t *provisional = world_ship_asset_by_id(&w, provisional_id);
    ASSERT(provisional != NULL);
    ASSERT_EQ_INT(provisional->owner_kind, SHIP_ASSET_OWNER_STATION);
    ASSERT(provisional->loaner);
    ASSERT_EQ_INT(provisional->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(provisional->operator_slot, 0);

    memcpy(sp->session_token, token, sizeof(token));
    sp->session_ready = true;
    int asset_count_before = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++)
        if (w.ship_assets[i].active) asset_count_before++;

    ASSERT(player_load_by_token(sp, &w, test_tmp_dir(), token));

    ASSERT_EQ_INT(sp->ship_asset_id, owned->asset_id);
    ASSERT_EQ_INT(sp->ship.hull_class, HULL_CLASS_HAULER);
    ASSERT_EQ_FLOAT(sp->ship.hull, 77.0f, 0.01f);
    ASSERT_EQ_INT(sp->current_station, 1);
    ASSERT_EQ_INT(owned->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(owned->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT_EQ_INT(owned->operator_slot, 0);
    ASSERT_EQ_FLOAT(owned->ship.hull, 77.0f, 0.01f);
    ASSERT_EQ_INT(provisional->status, SHIP_ASSET_STATUS_STORED);
    ASSERT_EQ_INT(provisional->operator_kind, SHIP_ASSET_OPERATOR_NONE);
    ASSERT_EQ_INT(provisional->operator_slot, -1);
    ASSERT_EQ_INT(provisional->hull_class, HULL_CLASS_MINER);
    int asset_count_after = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++)
        if (w.ship_assets[i].active) asset_count_after++;
    ASSERT_EQ_INT(asset_count_after, asset_count_before);

    char path[256];
    ASSERT(player_save_path(path, sizeof(path), test_tmp_dir(), &saved, 0));
    remove(path);
    ship_cleanup(&saved.ship);
}

TEST(test_player_load_mints_owned_asset_instead_of_reusing_loaner) {
    WORLD_DECL;
    world_reset(&w);
    uint8_t token[8];
    memset(token, 0x7C, sizeof(token));

    SERVER_PLAYER_DECL(saved);
    saved.id = 0;
    saved.connected = true;
    saved.session_ready = true;
    memcpy(saved.session_token, token, sizeof(token));
    ASSERT(ship_manifest_bootstrap(&saved.ship));
    saved.ship.hull_class = HULL_CLASS_HAULER;
    saved.ship.hull = 66.0f;
    saved.current_station = 1;
    ASSERT(player_save(&saved, test_tmp_dir(), 0));

    server_player_t *sp = &w.players[0];
    sp->id = 0;
    sp->connected = true;
    player_init_ship(sp, &w);
    uint32_t provisional_id = sp->ship_asset_id;
    ASSERT(provisional_id != SHIP_ASSET_ID_NONE);
    ship_asset_t *provisional = world_ship_asset_by_id(&w, provisional_id);
    ASSERT(provisional != NULL);
    ASSERT_EQ_INT(provisional->owner_kind, SHIP_ASSET_OWNER_STATION);
    ASSERT(provisional->loaner);
    ASSERT_EQ_INT(provisional->status, SHIP_ASSET_STATUS_ASSIGNED);
    int asset_count_before = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++)
        if (w.ship_assets[i].active) asset_count_before++;

    memcpy(sp->session_token, token, sizeof(token));
    sp->session_ready = true;
    ASSERT(player_load_by_token(sp, &w, test_tmp_dir(), token));

    ASSERT(sp->ship_asset_id != SHIP_ASSET_ID_NONE);
    ASSERT(sp->ship_asset_id != provisional_id);
    ship_asset_t *owned = world_ship_asset_by_id(&w, sp->ship_asset_id);
    ASSERT(owned != NULL);
    ASSERT_EQ_INT(owned->owner_kind, SHIP_ASSET_OWNER_PLAYER_SESSION);
    ASSERT(memcmp(owned->owner_session, token, sizeof(token)) == 0);
    ASSERT_EQ_INT(owned->provenance, SHIP_ASSET_PROVENANCE_LEGACY);
    ASSERT(!owned->loaner);
    ASSERT_EQ_INT(owned->hull_class, HULL_CLASS_HAULER);
    ASSERT_EQ_INT(owned->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(owned->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT_EQ_INT(owned->operator_slot, 0);
    ASSERT_EQ_FLOAT(owned->ship.hull, 66.0f, 0.01f);
    ASSERT_EQ_INT(provisional->owner_kind, SHIP_ASSET_OWNER_STATION);
    ASSERT(provisional->loaner);
    ASSERT_EQ_INT(provisional->status, SHIP_ASSET_STATUS_STORED);
    ASSERT_EQ_INT(provisional->operator_kind, SHIP_ASSET_OPERATOR_NONE);
    ASSERT_EQ_INT(provisional->operator_slot, -1);
    int asset_count_after = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++)
        if (w.ship_assets[i].active) asset_count_after++;
    ASSERT_EQ_INT(asset_count_after, asset_count_before + 1);

    char path[256];
    ASSERT(player_save_path(path, sizeof(path), test_tmp_dir(), &saved, 0));
    remove(path);
    ship_cleanup(&saved.ship);
}

TEST(test_world_load_stores_orphaned_player_ship_asset_for_reclaim) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    uint8_t token[8];
    memset(token, 0x8D, sizeof(token));

    server_player_t *sp = &w->players[0];
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    memcpy(sp->session_token, token, sizeof(token));
    ship_asset_t *asset = world_ship_asset_mint(
        w, HULL_CLASS_HAULER, SHIP_ASSET_OWNER_PLAYER_SESSION,
        -1, 1, SHIP_ASSET_PROVENANCE_SHIPYARD,
        false, 1, NULL, token);
    ASSERT(asset != NULL);
    ASSERT(ship_asset_claim_for_player(w, 0, 1));
    ASSERT_EQ_INT(sp->ship_asset_id, asset->asset_id);
    sp->ship.hull = 55.0f;
    ASSERT(world_ship_asset_sync_from_player(w, sp));
    ASSERT_EQ_INT(asset->custody_station, 1);

    uint32_t asset_id = asset->asset_id;
    int asset_count_before = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++)
        if (w->ship_assets[i].active) asset_count_before++;
    ASSERT(world_save(w, TMP("test_orphaned_player_asset.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(world_load(loaded, TMP("test_orphaned_player_asset.sav")));

    ship_asset_t *loaded_asset = world_ship_asset_by_id(loaded, asset_id);
    ASSERT(loaded_asset != NULL);
    ASSERT_EQ_INT(loaded_asset->status, SHIP_ASSET_STATUS_STORED);
    ASSERT_EQ_INT(loaded_asset->operator_kind, SHIP_ASSET_OPERATOR_NONE);
    ASSERT_EQ_INT(loaded_asset->operator_slot, -1);
    ASSERT_EQ_INT(loaded_asset->custody_station, 1);
    ASSERT_EQ_FLOAT(loaded_asset->ship.hull, 55.0f, 0.01f);

    server_player_t *reconnect = &loaded->players[2];
    reconnect->id = 2;
    reconnect->connected = true;
    reconnect->session_ready = true;
    memcpy(reconnect->session_token, token, sizeof(token));
    player_init_ship(reconnect, loaded);

    int asset_count_after = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++)
        if (loaded->ship_assets[i].active) asset_count_after++;
    ASSERT_EQ_INT(asset_count_after, asset_count_before);
    ASSERT_EQ_INT(reconnect->ship_asset_id, asset_id);
    ASSERT_EQ_FLOAT(reconnect->ship.hull, 55.0f, 0.01f);
    ASSERT_EQ_INT(loaded_asset->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(loaded_asset->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT_EQ_INT(loaded_asset->operator_slot, 2);
    remove(TMP("test_orphaned_player_asset.sav"));
}

TEST(test_world_load_repairs_stale_npc_ship_asset_binding) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);

    int npc_slot = -1;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (w->npc_ships[n].active &&
            w->npc_ships[n].ship_asset_id != SHIP_ASSET_ID_NONE) {
            npc_slot = n;
            break;
        }
    }
    ASSERT(npc_slot >= 0);
    npc_ship_t *npc = &w->npc_ships[npc_slot];
    ship_asset_t *asset = world_ship_asset_by_id(w, npc->ship_asset_id);
    ASSERT(asset != NULL);
    uint32_t asset_id = asset->asset_id;
    int active_assets_before = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++)
        if (w->ship_assets[i].active) active_assets_before++;

    asset->operator_slot = (int16_t)((npc_slot + 1) % MAX_NPC_SHIPS);
    ASSERT(world_save(w, TMP("test_stale_npc_asset.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(world_load(loaded, TMP("test_stale_npc_asset.sav")));

    const npc_ship_t *loaded_npc = &loaded->npc_ships[npc_slot];
    ASSERT(loaded_npc->active);
    ASSERT_EQ_INT(loaded_npc->ship_asset_id, asset_id);
    const ship_asset_t *loaded_asset =
        world_ship_asset_by_id_const(loaded, asset_id);
    ASSERT(loaded_asset != NULL);
    ASSERT_EQ_INT(loaded_asset->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(loaded_asset->operator_kind, SHIP_ASSET_OPERATOR_NPC);
    ASSERT_EQ_INT(loaded_asset->operator_slot, npc_slot);
    ASSERT_EQ_INT(loaded_asset->custody_station, loaded_npc->home_station);

    int active_assets_after = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++)
        if (loaded->ship_assets[i].active) active_assets_after++;
    ASSERT_EQ_INT(active_assets_after, active_assets_before);
    remove(TMP("test_stale_npc_asset.sav"));
}

TEST(test_player_save_uses_temp_then_atomic_rename) {
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, &w);
    sp.connected = true;
    sp.ship.hull = 42.0f;
    ASSERT(player_save(&sp, test_tmp_dir(), 91));

    char path[256], tmp_path[272];
    ASSERT(player_save_path(path, sizeof(path), test_tmp_dir(), &sp, 91));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *tmp = fopen(tmp_path, "wb");
    ASSERT(tmp != NULL);
    fputs("partial player save", tmp);
    fclose(tmp);

    SERVER_PLAYER_DECL(loaded);
    ASSERT(player_load(&loaded, &w, test_tmp_dir(), 91));
    ASSERT_EQ_FLOAT(loaded.ship.hull, 42.0f, 0.01f);

    sp.ship.hull = 77.0f;
    ASSERT(player_save(&sp, test_tmp_dir(), 91));
    FILE *leftover = fopen(tmp_path, "rb");
    ASSERT(leftover == NULL);

    SERVER_PLAYER_DECL(reloaded);
    ASSERT(player_load(&reloaded, &w, test_tmp_dir(), 91));
    ASSERT_EQ_FLOAT(reloaded.ship.hull, 77.0f, 0.01f);
    remove(path);
    remove(tmp_path);
}

TEST(test_world_save_round_trips_station_manifest) {
    /* Previously, non-empty station manifests caused world_save to fail —
     * the pre-#339 guard rejected them because manifest wasn't persisted.
     * Slice A of #339 lifted that guard and added real serialization;
     * this test asserts the round trip now works. */
    WORLD_DECL;
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    cargo_unit_t unit = {0};
    world_reset(&w);
    world_reset(loaded);
    unit.kind = (uint8_t)CARGO_KIND_INGOT;
    unit.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    unit.grade = (uint8_t)MINING_GRADE_RARE;
    unit.pub[0] = 0xA5;
    unit.pub[31] = 0x5A;
    cargo_receipt_chain_t chain = {0};
    ASSERT(test_issue_station_receipt(&w.stations[0], unit.pub, 4242, &chain));
    ASSERT(station_manifest_push_with_chain(&w.stations[0], &unit, &chain));
    ASSERT_EQ_INT(w.stations[0].manifest.count, 1);
    ASSERT(world_save(&w, TMP("test_manifest_roundtrip.sav")));
    ASSERT(world_load(loaded, TMP("test_manifest_roundtrip.sav")));
    ASSERT_EQ_INT(loaded->stations[0].manifest.count, 1);
    ASSERT(loaded->stations[0].manifest.units != NULL);
    ASSERT_EQ_INT(loaded->stations[0].manifest.units[0].kind, CARGO_KIND_INGOT);
    ASSERT_EQ_INT(loaded->stations[0].manifest.units[0].commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(loaded->stations[0].manifest.units[0].grade, MINING_GRADE_RARE);
    ASSERT(memcmp(loaded->stations[0].manifest.units[0].pub, unit.pub, 32) == 0);
    ship_receipts_t *loaded_receipts = station_get_receipts(&loaded->stations[0]);
    ASSERT(loaded_receipts != NULL);
    ASSERT_EQ_INT((int)loaded_receipts->count, 1);
    ASSERT_EQ_INT((int)loaded_receipts->chains[0].len, 1);
    ASSERT_EQ_INT((int)loaded_receipts->chains[0].links[0].event_id, 4242);
    ASSERT_EQ_INT((int)loaded_receipts->chains[0].links[0].epoch, 1);
    ASSERT(cargo_receipt_chain_verify(loaded_receipts->chains[0].links,
                                      loaded_receipts->chains[0].len,
                                      unit.pub) == CARGO_RECEIPT_OK);
    remove(TMP("test_manifest_roundtrip.sav"));
}

TEST(test_world_load_repairs_cache_only_station_finished_goods) {
    WORLD_DECL;
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    world_reset(&w);
    world_reset(loaded);

    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(helios, COMMODITY_TRACTOR_MODULE, 10));
    ASSERT_EQ_INT(station_finished_count(helios, COMMODITY_TRACTOR_MODULE), 10);

    /* Simulate an older live save where the float stock survived but some
     * cargo_unit_t rows were never minted. This exact shape makes haulers
     * look paused: UI sees inventory, routing sees no manifest-backed cargo. */
    ASSERT_EQ_INT(station_manifest_consume_by_commodity(
                      helios, COMMODITY_TRACTOR_MODULE, 6), 6);
    ASSERT_EQ_INT(station_finished_count(helios, COMMODITY_TRACTOR_MODULE), 4);
    ASSERT_EQ_FLOAT(helios->_inventory_cache[COMMODITY_TRACTOR_MODULE],
                    10.0f, 0.001f);

    ASSERT(world_save(&w, TMP("test_manifest_repair.sav")));
    ASSERT(world_load(loaded, TMP("test_manifest_repair.sav")));

    ASSERT_EQ_INT(station_finished_count(&loaded->stations[2],
                                         COMMODITY_TRACTOR_MODULE), 10);
    ASSERT_EQ_FLOAT(loaded->stations[2]._inventory_cache[COMMODITY_TRACTOR_MODULE],
                    10.0f, 0.001f);
    remove(TMP("test_manifest_repair.sav"));
}

TEST(test_player_load_clamps_negative_credits) {
    /* Credits are now in station ledgers, not ship_t. PLY3 format has no
     * credits field. This test just confirms save/load round-trip works. */
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, &w);
    sp.connected = true;
    ASSERT(player_save(&sp, test_tmp_dir(), 98));

    SERVER_PLAYER_DECL(loaded);
    ASSERT(player_load(&loaded, &w, test_tmp_dir(), 98));
    /* No credits field to clamp — ledger balances are always >= 0 */
    remove(TMP("player_98.sav"));
}

TEST(test_player_save_round_trips_ship_manifest) {
    /* Pre-#339/A.2 the ship manifest was guarded empty on save (PLY4
     * format had no tail). Slice A.2 moved to PLY5 which appends the
     * manifest after the fixed ship blob. Verify round trip preserves
     * kind, commodity, grade, and pub of each entry. */
    WORLD_DECL;
    SERVER_PLAYER_DECL(sp);
    SERVER_PLAYER_DECL(loaded);
    cargo_unit_t unit = {0};
    world_reset(&w);
    player_init_ship(&sp, &w);
    sp.connected = true;
    unit.kind = (uint8_t)CARGO_KIND_INGOT;
    unit.commodity = (uint8_t)COMMODITY_CUPRITE_INGOT;
    unit.grade = (uint8_t)MINING_GRADE_FINE;
    unit.pub[0] = 0x5A;
    unit.pub[7] = 0xA5;
    ASSERT(manifest_push(&sp.ship.manifest, &unit));
    ASSERT(sp.ship.manifest.count == 1);
    ASSERT(player_save(&sp, test_tmp_dir(), 92));
    ASSERT(player_load(&loaded, &w, test_tmp_dir(), 92));
    ASSERT_EQ_INT(loaded.ship.manifest.count, 1);
    ASSERT(loaded.ship.manifest.units != NULL);
    ASSERT_EQ_INT(loaded.ship.manifest.units[0].kind, CARGO_KIND_INGOT);
    ASSERT_EQ_INT(loaded.ship.manifest.units[0].commodity, COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(loaded.ship.manifest.units[0].grade, MINING_GRADE_FINE);
    ASSERT(memcmp(loaded.ship.manifest.units[0].pub, unit.pub, 32) == 0);
    remove(TMP("player_92.sav"));
}

TEST(test_player_load_bad_crc_rejects_without_mutating_live_player) {
    WORLD_DECL;
    SERVER_PLAYER_DECL(sp);
    SERVER_PLAYER_DECL(loaded);
    char path[256];

    world_reset(&w);
    player_init_ship(&sp, &w);
    sp.connected = true;
    sp.id = 89;
    sp.ship.cargo[COMMODITY_FERRITE_ORE] = 12.0f;
    ASSERT(player_save(&sp, test_tmp_dir(), 89));
    ASSERT(player_save_path(path, sizeof(path), test_tmp_dir(), &sp, 89));

    ASSERT(test_patch_file_byte(path, 16, 0xA5));
    player_init_ship(&loaded, &w);
    loaded.ship.cargo[COMMODITY_FERRITE_ORE] = 77.0f;
    loaded.current_station = 2;
    loaded.docked = false;
    ASSERT(!player_load(&loaded, &w, test_tmp_dir(), 89));
    ASSERT_EQ_FLOAT(loaded.ship.cargo[COMMODITY_FERRITE_ORE], 77.0f, 0.001f);
    ASSERT_EQ_INT(loaded.current_station, 2);
    ASSERT(!loaded.docked);
    remove(path);
}

TEST(test_player_load_bad_receipt_count_rejects_without_mutating_live_player) {
    WORLD_DECL;
    SERVER_PLAYER_DECL(sp);
    SERVER_PLAYER_DECL(loaded);
    cargo_unit_t unit = {0};
    char path[256];

    world_reset(&w);
    player_init_ship(&sp, &w);
    sp.connected = true;
    sp.id = 88;
    unit.kind = (uint8_t)CARGO_KIND_INGOT;
    unit.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    unit.grade = (uint8_t)MINING_GRADE_RARE;
    for (int i = 0; i < 32; i++) unit.pub[i] = (uint8_t)(0xC0 + i);
    ASSERT(manifest_push(&sp.ship.manifest, &unit));
    ASSERT(player_save(&sp, test_tmp_dir(), 88));
    ASSERT(player_save_path(path, sizeof(path), test_tmp_dir(), &sp, 88));

    long pub_at = test_find_bytes_in_file(path, unit.pub, sizeof(unit.pub));
    ASSERT(pub_at >= 0);
    long unit_at = pub_at - (long)offsetof(cargo_unit_t, pub);
    long receipt_len_at = unit_at + (long)sizeof(cargo_unit_t) + (long)sizeof(uint64_t);
    ASSERT(test_patch_file_byte(path, receipt_len_at,
                                (uint8_t)(CARGO_RECEIPT_CHAIN_MAX_LEN + 1)));
    ASSERT(test_rewrite_crc32_trailer(path));

    player_init_ship(&loaded, &w);
    loaded.ship.cargo[COMMODITY_CUPRITE_ORE] = 55.0f;
    loaded.current_station = 1;
    ASSERT(!player_load(&loaded, &w, test_tmp_dir(), 88));
    ASSERT_EQ_FLOAT(loaded.ship.cargo[COMMODITY_CUPRITE_ORE], 55.0f, 0.001f);
    ASSERT_EQ_INT(loaded.ship.manifest.count, 0);
    ASSERT_EQ_INT(loaded.current_station, 1);
    remove(path);
}

TEST(test_player_load_clamps_negative_cargo) {
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, &w);
    sp.connected = true;
    sp.ship.cargo[COMMODITY_FERRITE_ORE] = -50.0f;
    ASSERT(player_save(&sp, test_tmp_dir(), 97));

    SERVER_PLAYER_DECL(loaded);
    ASSERT(player_load(&loaded, &w, test_tmp_dir(), 97));
    ASSERT(loaded.ship.cargo[COMMODITY_FERRITE_ORE] >= 0.0f);
    remove(TMP("player_97.sav"));
}

TEST(test_player_load_clamps_hull_hp) {
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, &w);
    sp.connected = true;
    sp.ship.hull = 99999.0f;  /* way above max */
    ASSERT(player_save(&sp, test_tmp_dir(), 96));

    SERVER_PLAYER_DECL(loaded);
    ASSERT(player_load(&loaded, &w, test_tmp_dir(), 96));
    ASSERT(loaded.ship.hull <= ship_max_hull(&loaded.ship));
    remove(TMP("player_96.sav"));
}

TEST(test_player_load_clamps_upgrade_levels) {
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, &w);
    sp.connected = true;
    sp.ship.mining_level = 100;
    sp.ship.hold_level = -5;
    ASSERT(player_save(&sp, test_tmp_dir(), 95));

    SERVER_PLAYER_DECL(loaded);
    ASSERT(player_load(&loaded, &w, test_tmp_dir(), 95));
    ASSERT(loaded.ship.mining_level >= 0 && loaded.ship.mining_level <= SHIP_UPGRADE_MAX_LEVEL);
    ASSERT(loaded.ship.hold_level >= 0 && loaded.ship.hold_level <= SHIP_UPGRADE_MAX_LEVEL);
    remove(TMP("player_95.sav"));
}

TEST(test_player_load_invalid_station_falls_back) {
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, &w);
    sp.connected = true;
    sp.current_station = 99;  /* out of range */
    ASSERT(player_save(&sp, test_tmp_dir(), 94));

    SERVER_PLAYER_DECL(loaded);
    ASSERT(player_load(&loaded, &w, test_tmp_dir(), 94));
    ASSERT(loaded.current_station >= 0 && loaded.current_station < MAX_STATIONS);
    remove(TMP("player_94.sav"));
}

TEST(test_player_load_repairs_degenerate_dock_berth) {
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(saved);
    player_init_ship(&saved, &w);
    saved.id = 0;
    saved.connected = true;
    saved.current_station = 0;
    saved.docked = true;
    saved.ship.pos = w.stations[0].pos;
    ASSERT(player_save(&saved, test_tmp_dir(), 87));

    station_t *prospect = &w.stations[0];
    bool corrupted = false;
    for (int i = 0; i < prospect->module_count; i++) {
        station_module_t *mod = &prospect->modules[i];
        if (mod->type != MODULE_DOCK || mod->scaffold) continue;
        mod->ring = 0;
        mod->slot = 0;
        corrupted = true;
        break;
    }
    ASSERT(corrupted);

    server_player_t *loaded = &w.players[0];
    loaded->id = 0;
    loaded->connected = true;
    loaded->session_ready = true;
    memset(loaded->session_token, 0x87, sizeof(loaded->session_token));
    ASSERT(player_load(loaded, &w, test_tmp_dir(), 87));
    ASSERT(loaded->docked);
    vec2 berth_pos = loaded->ship.pos;
    ASSERT(v2_len(v2_sub(berth_pos, prospect->pos)) > prospect->radius + 1.0f);

    world_sim_step(&w, SIM_DT);
    ASSERT(loaded->docked);
    ASSERT(v2_len(v2_sub(loaded->ship.pos, prospect->pos)) > prospect->radius + 1.0f);
    ASSERT(v2_len(v2_sub(loaded->ship.pos, berth_pos)) < 0.01f);

    loaded->input.launch = true;
    world_sim_step(&w, SIM_DT);
    ASSERT(!loaded->docked);
    ASSERT(v2_len(v2_sub(loaded->ship.pos, berth_pos)) < 2.0f);
    ASSERT(v2_len(loaded->ship.vel) > 50.0f);

    char path[256];
    if (player_save_path(path, sizeof(path), test_tmp_dir(), &saved, 87))
        remove(path);
}

TEST(test_player_load_bad_magic_fails) {
    /* Write garbage with wrong magic */
    FILE *f = fopen(TMP("player_93.sav"), "wb");
    ASSERT(f != NULL);
    uint32_t bad_magic = 0xDEADBEEF;
    fwrite(&bad_magic, sizeof(bad_magic), 1, f);
    fclose(f);

    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(loaded);
    ASSERT(!player_load(&loaded, &w, test_tmp_dir(), 93));
    remove(TMP("player_93.sav"));
}

TEST(test_world_load_rejects_stale_version) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    ASSERT(world_save(w, TMP("test_stale.sav")));
    /* Overwrite version (bytes 4-7) with old version 11 */
    FILE *f = fopen(TMP("test_stale.sav"), "r+b");
    ASSERT(f != NULL);
    fseek(f, 4, SEEK_SET);
    uint32_t old_version = 11;
    fwrite(&old_version, sizeof(old_version), 1, f);
    fclose(f);
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(!world_load(loaded, TMP("test_stale.sav")));
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove(TMP("test_stale.sav"));
}

TEST(test_world_save_load_preserves_module_ring_slot) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    /* Prospect's furnace at ring 1 slot 2 and ferrite-ore intake hopper at
     * ring 2 slot 4. Folded frame pods are tractored by the furnace directly;
     * no dedicated frame-shell hopper is seeded here. */
    ASSERT_EQ_INT((int)w->stations[0].module_count, 4);
    station_module_t orig = w->stations[0].modules[2]; /* furnace at ring 1 slot 2 */
    ASSERT(orig.type == MODULE_FURNACE);
    ASSERT_EQ_INT((int)orig.ring, 1);
    ASSERT_EQ_INT((int)orig.slot, 2);
    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS, TMP("test_modcat")));
    ASSERT(world_save(w, TMP("test_modules.sav")));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    station_catalog_load_all(loaded->stations, MAX_STATIONS, TMP("test_modcat"));
    ASSERT(world_load(loaded, TMP("test_modules.sav")));
    station_module_t restored = loaded->stations[0].modules[2];
    ASSERT_EQ_INT((int)restored.type, (int)orig.type);
    ASSERT_EQ_INT((int)restored.ring, (int)orig.ring);
    ASSERT_EQ_INT((int)restored.slot, (int)orig.slot);
    ASSERT_EQ_INT((int)restored.scaffold, (int)orig.scaffold);
    ASSERT_EQ_FLOAT(restored.build_progress, orig.build_progress, 0.001f);
    /* modules[3] = ferrite-ore intake hopper at ring 2 slot 4. */
    station_module_t intake = loaded->stations[0].modules[3];
    ASSERT(intake.type == MODULE_HOPPER);
    ASSERT_EQ_INT((int)intake.ring, 2);
    ASSERT_EQ_INT((int)intake.slot, 4);
    ASSERT_EQ_INT((int)intake.commodity, (int)COMMODITY_FERRITE_ORE);
    ASSERT_EQ_INT((int)loaded->stations[0].module_count, 4);
    remove(TMP("test_modules.sav"));
}

TEST(test_v51_migration_tags_untagged_furnaces_and_fills_hoppers) {
    /* Simulate a v50 save: world_reset gives us correctly-tagged
     * furnaces and full hoppers, then we manually break Helios to look
     * pre-Slice-1 (untagged furnaces, no LASER_MODULE / TRACTOR_MODULE
     * output hoppers). Running the migration must restore the seeded
     * invariant: every Helios producer has a matching tagged hopper
     * for its output, and furnaces are tagged by the legacy migration
     * heuristic. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    station_t *helios = &w->stations[2];

    /* Untag every Helios furnace (pre-Slice-1 state). */
    int n_furnaces = 0;
    for (int m = 0; m < helios->module_count; m++) {
        if (helios->modules[m].type == MODULE_FURNACE) {
            helios->modules[m].commodity = (uint8_t)COMMODITY_COUNT;
            n_furnaces++;
        }
    }
    ASSERT_EQ_INT(n_furnaces, 3);

    /* Drop Helios's LASER_MODULE and TRACTOR_MODULE output hoppers.
     * Walk in reverse so removing entries doesn't shift indices we
     * still need to check. */
    for (int m = helios->module_count - 1; m >= 0; m--) {
        if (helios->modules[m].type != MODULE_HOPPER) continue;
        commodity_t c = (commodity_t)helios->modules[m].commodity;
        if (c == COMMODITY_LASER_MODULE || c == COMMODITY_TRACTOR_MODULE) {
            for (int k = m + 1; k < helios->module_count; k++) {
                helios->modules[k - 1] = helios->modules[k];
            }
            helios->module_count--;
        }
    }
    ASSERT(station_find_hopper_for(helios, COMMODITY_LASER_MODULE)   < 0);
    ASSERT(station_find_hopper_for(helios, COMMODITY_TRACTOR_MODULE) < 0);

    /* Run the migration. */
    world_apply_cargo_schema_migration(w);

    /* All 3 furnaces tagged with valid ingot commodities (3-furnace
     * legacy heuristic → 2×CR + 1×CU). */
    int cu = 0, cr = 0;
    for (int m = 0; m < helios->module_count; m++) {
        if (helios->modules[m].type != MODULE_FURNACE) continue;
        commodity_t tag = (commodity_t)helios->modules[m].commodity;
        if      (tag == COMMODITY_CUPRITE_INGOT) cu++;
        else if (tag == COMMODITY_CRYSTAL_INGOT) cr++;
        else ASSERT(false /* unexpected tag */);
    }
    ASSERT_EQ_INT(cu, 1);
    ASSERT_EQ_INT(cr, 2);

    /* Missing output hoppers were auto-spawned. */
    ASSERT(station_find_hopper_for(helios, COMMODITY_LASER_MODULE)   >= 0);
    ASSERT(station_find_hopper_for(helios, COMMODITY_TRACTOR_MODULE) >= 0);

    /* Idempotent: running again is a no-op. */
    int count_after_first = helios->module_count;
    world_apply_cargo_schema_migration(w);
    ASSERT_EQ_INT(helios->module_count, count_after_first);
}

TEST(test_v51_migration_furnace_count_heuristic) {
    /* Synthetic stations covering 1/2/3-furnace legacy layouts, all
     * furnaces untagged. Migration tags them per the legacy
     * furnace-count heuristic, updated so 3+ furnace stations get the
     * two crystal beams needed for the staged crystal process.
     * Use stations[3+] to avoid clobbering seeded state (which the
     * heap WORLD_DECL initializes to zero already). */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    /* 1-furnace station: should tag FERRITE. */
    station_t *st1 = &w->stations[3];
    st1->signal_range = 1.0f;
    add_module_at(st1, MODULE_FURNACE, 1, 0);
    /* 2-furnace station: should tag FERRITE + CUPRITE. */
    station_t *st2 = &w->stations[4];
    st2->signal_range = 1.0f;
    add_module_at(st2, MODULE_FURNACE, 1, 0);
    add_module_at(st2, MODULE_FURNACE, 1, 1);
    /* 3-furnace station: should tag CRYSTAL + CRYSTAL + CUPRITE. */
    station_t *st3 = &w->stations[5];
    st3->signal_range = 1.0f;
    add_module_at(st3, MODULE_FURNACE, 1, 0);
    add_module_at(st3, MODULE_FURNACE, 1, 1);
    add_module_at(st3, MODULE_FURNACE, 1, 2);

    for (int s = 3; s <= 5; s++) {
        for (int m = 0; m < w->stations[s].module_count; m++) {
            if (w->stations[s].modules[m].type == MODULE_FURNACE)
                w->stations[s].modules[m].commodity = (uint8_t)COMMODITY_COUNT;
        }
    }

    world_apply_cargo_schema_migration(w);

    ASSERT_EQ_INT((int)st1->modules[0].commodity, (int)COMMODITY_FERRITE_INGOT);

    ASSERT_EQ_INT((int)st2->modules[0].commodity, (int)COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT((int)st2->modules[1].commodity, (int)COMMODITY_CUPRITE_INGOT);

    ASSERT_EQ_INT((int)st3->modules[0].commodity, (int)COMMODITY_CRYSTAL_INGOT);
    ASSERT_EQ_INT((int)st3->modules[1].commodity, (int)COMMODITY_CRYSTAL_INGOT);
    ASSERT_EQ_INT((int)st3->modules[2].commodity, (int)COMMODITY_CUPRITE_INGOT);

    /* Output hoppers spawned for every furnace's tagged output. */
    ASSERT(station_find_hopper_for(st1, COMMODITY_FERRITE_INGOT) >= 0);
    ASSERT(station_find_hopper_for(st2, COMMODITY_FERRITE_INGOT) >= 0);
    ASSERT(station_find_hopper_for(st2, COMMODITY_CUPRITE_INGOT) >= 0);
    ASSERT(station_find_hopper_for(st3, COMMODITY_CUPRITE_INGOT) >= 0);
    ASSERT(station_find_hopper_for(st3, COMMODITY_CRYSTAL_INGOT) >= 0);
}

TEST(test_world_save_load_preserves_smelted_ingot_pod) {
    /* world_t is ~600KB — use heap to avoid stack overflow on CI. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        w->stations[0].arm_speed[arm] = 0.0f;
        w->stations[0].arm_rotation[arm] = 0.0f;
    }

    int furnace_idx = -1, silo_idx = -1;
    for (int m = 0; m < w->stations[0].module_count; m++) {
        station_module_t *mod = &w->stations[0].modules[m];
        if (mod->type == MODULE_FURNACE &&
            module_instance_input_ore(mod) == COMMODITY_FERRITE_ORE)
            furnace_idx = m;
        if (mod->type == MODULE_HOPPER &&
            mod->commodity == (uint8_t)COMMODITY_FERRITE_ORE)
            silo_idx = m;
    }
    ASSERT(furnace_idx >= 0 && silo_idx >= 0);
    vec2 furnace_pos = module_world_pos_ring(&w->stations[0],
        w->stations[0].modules[furnace_idx].ring,
        w->stations[0].modules[furnace_idx].slot);
    vec2 silo_pos = module_world_pos_ring(&w->stations[0],
        w->stations[0].modules[silo_idx].ring,
        w->stations[0].modules[silo_idx].slot);

    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) { frag = i; break; }
    }
    ASSERT(frag >= 0);
    asteroid_t *a = &w->asteroids[frag];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 20.0f;
    a->max_ore = 20.0f;
    a->radius = 6.0f;
    a->fracture_child = true;
    a->grade = (uint8_t)MINING_GRADE_COMMON;
    for (int b = 0; b < 32; b++) a->fracture_seed[b] = (uint8_t)(0xA0 + b);
    a->pos = v2_scale(v2_add(furnace_pos, silo_pos), 0.5f);
    a->vel = v2(0.0f, 0.0f);

    ASSERT(station_finished_mint(&w->stations[0], COMMODITY_FRAME, 1, NULL) == 1);
    for (int i = 0; i < (int)(10.0f / SIM_DT) && w->asteroids[frag].active; i++)
        world_sim_step(w, SIM_DT);
    ASSERT(!w->asteroids[frag].active);
    int pod_idx = test_find_exact_pod(w, COMMODITY_FERRITE_INGOT);
    ASSERT(pod_idx >= 0);
    cargo_pod_t expected = w->cargo_pods[pod_idx];
    ASSERT_EQ_INT(expected.manifest_count, 20);
    ASSERT_EQ_INT(expected.quantity, 20);
    ASSERT_EQ_INT((int)w->hopper_smelt_events, 0);
    ASSERT(world_save(w, TMP("test_ingots.sav")));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    ASSERT(world_load(loaded, TMP("test_ingots.sav")));
    int loaded_pod_idx = test_find_exact_pod(loaded, COMMODITY_FERRITE_INGOT);
    ASSERT(loaded_pod_idx >= 0);
    const cargo_pod_t *loaded_pod = &loaded->cargo_pods[loaded_pod_idx];
    ASSERT_EQ_INT(loaded_pod->manifest_count, expected.manifest_count);
    ASSERT_EQ_INT(loaded_pod->quantity, expected.quantity);
    ASSERT_EQ_INT(loaded_pod->tractor_station, expected.tractor_station);
    ASSERT_EQ_INT(loaded_pod->tractor_module, expected.tractor_module);
    ASSERT(memcmp(loaded_pod->manifest_units[0].pub,
                  expected.manifest_units[0].pub, 32) == 0);
    ASSERT(memcmp(loaded_pod->manifest_units[expected.manifest_count - 1].pub,
                  expected.manifest_units[expected.manifest_count - 1].pub,
                  32) == 0);
    remove(TMP("test_ingots.sav"));
    /* loaded + w auto-freed by WORLD_HEAP cleanup */
}

TEST(test_world_save_load_preserves_hauler_manifest_cargo) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    int seeded_hauler = spawn_npc(w, 0, NPC_ROLE_HAULER);
    ASSERT(seeded_hauler >= 0);

    int hauler_slot = -1;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        if (w->npc_ships[n].role != NPC_ROLE_HAULER) continue;
        if (w->npc_ships[n].home_station != 0) continue;
        hauler_slot = n;
        break;
    }
    ASSERT(hauler_slot >= 0);
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (n != hauler_slot) w->npc_ships[n].active = false;
    }

    npc_ship_t *hauler = &w->npc_ships[hauler_slot];
    ship_t *hauler_ship = world_npc_ship_for(w, hauler_slot);
    ASSERT(hauler_ship != NULL);
    ASSERT(ship_manifest_bootstrap(hauler_ship));
    manifest_clear(&hauler_ship->manifest);
    ship_receipts_t *hauler_receipts = ship_get_receipts(hauler_ship);
    ASSERT(hauler_receipts != NULL);
    ship_receipts_clear(hauler_receipts);
    memset(hauler->cargo, 0, sizeof(hauler->cargo));

    station_t *home = &w->stations[0];
    station_t *dest = &w->stations[1];
    ASSERT(station_manifest_bootstrap(home));
    ASSERT(station_manifest_bootstrap(dest));
    manifest_clear(&home->manifest);
    manifest_clear(&dest->manifest);
    memset(home->_inventory_cache, 0, sizeof(home->_inventory_cache));
    memset(dest->_inventory_cache, 0, sizeof(dest->_inventory_cache));
    dest->scaffold = false;

    enum { EXPECTED_MOVED = 2 };
    int stock_units = (int)HAULER_RESERVE + EXPECTED_MOVED;
    cargo_unit_t units[16] = {{0}};
    cargo_receipt_chain_t chains[16] = {0};
    ASSERT(stock_units <= (int)(sizeof(units) / sizeof(units[0])));
    for (int i = 0; i < stock_units; i++) {
        uint8_t fragment_pub[32] = {0};
        fragment_pub[31] = (uint8_t)(0x60 + i);
        ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
                          fragment_pub, (uint16_t)i, &units[i]));
        ASSERT(test_issue_station_receipt(home, units[i].pub,
                                          (uint64_t)(900 + i), &chains[i]));
        ASSERT(station_manifest_push_with_chain(home, &units[i], &chains[i]));
    }
    home->_inventory_cache[COMMODITY_FERRITE_INGOT] = (float)stock_units;

    memset(w->contracts, 0, sizeof(w->contracts));
    w->contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 25.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    hauler->state = NPC_STATE_DOCKED;
    hauler->state_timer = 0.0f;
    hauler->home_station = 0;
    hauler->dest_station = 0;
    /* Seed gossip — see comment in test_hauler_preserves_cargo_identity_in_transit. */
    hauler->known_contract_count = 0;
    for (int k = 0; k < MAX_CONTRACTS && hauler->known_contract_count < SHIP_KNOWN_CONTRACT_CAP; k++) {
        if (!w->contracts[k].active) continue;
        hauler->known_contracts[hauler->known_contract_count++] = (contract_summary_t){
            .active = true,
            .action = (uint8_t)w->contracts[k].action,
            .station_index = w->contracts[k].station_index,
            .commodity = (uint8_t)w->contracts[k].commodity,
            .quantity_needed = w->contracts[k].quantity_needed,
            .base_price = w->contracts[k].base_price,
            .age_at_copy = w->contracts[k].age,
        };
    }

    step_npc_ships(w, SIM_DT);

    ASSERT_EQ_INT(hauler->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(hauler_ship->manifest.count, EXPECTED_MOVED);
    ASSERT(manifest_find(&hauler_ship->manifest, units[0].pub) >= 0);
    ASSERT(manifest_find(&hauler_ship->manifest, units[1].pub) >= 0);
    ASSERT_EQ_INT((int)hauler_receipts->count, EXPECTED_MOVED);
    ASSERT_EQ_INT((int)hauler_receipts->chains[0].len, 2);
    ASSERT_EQ_INT((int)hauler_receipts->chains[0].links[0].event_id, 900);
    ASSERT(hauler_receipts->chains[0].links[1].event_id != 0);
    ASSERT(memcmp(hauler_receipts->chains[0].links[1].authoring_station,
                  home->station_pubkey, 32) == 0);

    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS,
                                    TMP("test_hauler_manifest_cat")));
    ASSERT(world_save(w, TMP("test_hauler_manifest.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    station_catalog_load_all(loaded->stations, MAX_STATIONS,
                             TMP("test_hauler_manifest_cat"));
    ASSERT(world_load(loaded, TMP("test_hauler_manifest.sav")));

    npc_ship_t *loaded_hauler = &loaded->npc_ships[hauler_slot];
    ship_t *loaded_ship = world_npc_ship_for(loaded, hauler_slot);
    ASSERT(loaded_ship != NULL);
    ship_receipts_t *loaded_receipts = ship_get_receipts(loaded_ship);
    ASSERT(loaded_receipts != NULL);
    ASSERT_EQ_INT(loaded_ship->manifest.count, EXPECTED_MOVED);
    ASSERT_EQ_INT((int)loaded_receipts->count, EXPECTED_MOVED);
    ASSERT_EQ_INT((int)loaded_receipts->chains[0].len, 2);
    ASSERT_EQ_INT((int)loaded_receipts->chains[0].links[0].event_id, 900);
    ASSERT(loaded_receipts->chains[0].links[1].event_id != 0);
    ASSERT(memcmp(loaded_receipts->chains[0].links[1].authoring_station,
                  loaded->stations[0].station_pubkey, 32) == 0);
    ASSERT(manifest_find(&loaded_ship->manifest, units[0].pub) >= 0);
    ASSERT(manifest_find(&loaded_ship->manifest, units[1].pub) >= 0);
    ASSERT_EQ_FLOAT(loaded_hauler->cargo[COMMODITY_FERRITE_INGOT],
                    (float)EXPECTED_MOVED, 0.001f);

    loaded_hauler->state = NPC_STATE_UNLOADING;
    loaded_hauler->state_timer = 0.0f;
    loaded_hauler->role = NPC_ROLE_HAULER;
    loaded_hauler->brain_mode = SERVER_BRAIN_MODE_NEURAL_FLIGHT;
    loaded_hauler->ship.hull_class = HULL_CLASS_HAULER;
    loaded_hauler->dest_station = 1;
    loaded_hauler->pickup_station = -1;
    loaded_hauler->pickup_commodity = COMMODITY_COUNT;
    loaded_hauler->pickup_action = (uint8_t)CONTRACT_TRACTOR;
    loaded_hauler->towed_fragment = -1;
    loaded_hauler->towed_scaffold = -1;
    loaded_hauler->ship.pos = station_approach_target(&loaded->stations[1],
                                                       loaded_hauler->ship.pos);
    loaded_ship->hull_class = HULL_CLASS_HAULER;
    loaded_ship->pos = loaded_hauler->ship.pos;
    loaded_ship->vel = v2(0.0f, 0.0f);
    loaded_ship->hull = 100.0f;
    loaded_hauler->ship.vel = v2(0.0f, 0.0f);
    loaded_hauler->hull = loaded_ship->hull;

    step_npc_ships(loaded, SIM_DT);

    loaded_ship = world_npc_ship_for(loaded, hauler_slot);
    ASSERT(loaded_ship != NULL);
    loaded_receipts = ship_get_receipts(loaded_ship);
    ASSERT(loaded_receipts != NULL);

    ASSERT_EQ_INT(loaded_ship->manifest.count, 0);
    ASSERT_EQ_INT((int)loaded_receipts->count, 0);
    ASSERT(manifest_find(&loaded->stations[1].manifest, units[0].pub) >= 0);
    ASSERT(manifest_find(&loaded->stations[1].manifest, units[1].pub) >= 0);
    ship_receipts_t *dest_receipts = station_get_receipts(&loaded->stations[1]);
    ASSERT(dest_receipts != NULL);
    int d0 = manifest_find(&loaded->stations[1].manifest, units[0].pub);
    int d1 = manifest_find(&loaded->stations[1].manifest, units[1].pub);
    ASSERT(d0 >= 0);
    ASSERT(d1 >= 0);
    ASSERT((int)dest_receipts->chains[d0].len >= 2);
    ASSERT_EQ_INT((int)dest_receipts->chains[d0].links[0].event_id, 900);
    ASSERT(memcmp(dest_receipts->chains[d0].links[1].authoring_station,
                  loaded->stations[0].station_pubkey, 32) == 0);
    if (dest_receipts->chains[d0].len >= 3) {
        ASSERT(memcmp(dest_receipts->chains[d0].links[2].authoring_station,
                      loaded->stations[1].station_pubkey, 32) == 0);
    }
    ASSERT((int)dest_receipts->chains[d1].len >= 2);
    ASSERT_EQ_INT((int)dest_receipts->chains[d1].links[0].event_id, 901);
    ASSERT(memcmp(dest_receipts->chains[d1].links[1].authoring_station,
                  loaded->stations[0].station_pubkey, 32) == 0);
    if (dest_receipts->chains[d1].len >= 3) {
        ASSERT(memcmp(dest_receipts->chains[d1].links[2].authoring_station,
                      loaded->stations[1].station_pubkey, 32) == 0);
    }
    for (uint16_t i = 0; i < loaded->stations[1].manifest.count; i++) {
        ASSERT(loaded->stations[1].manifest.units[i].recipe_id !=
               RECIPE_LEGACY_MIGRATE);
    }
    remove(TMP("test_hauler_manifest.sav"));
    /* loaded + w auto-freed by WORLD_HEAP cleanup */
}

TEST(test_world_save_load_preserves_delivery_shipments) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    w->next_delivery_shipment_id = 12;
    w->delivery_shipments[0] = (delivery_shipment_t){
        .active = true,
        .shipment_id = 11,
        .origin_station = 0,
        .destination_station = 2,
        .contract_index = 3,
        .debtor_player = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .quantity_total = 2,
        .quantity_bound = 2,
        .quantity_delivered = 1,
        .quantity_black_market_sold = 0,
        .debt_principal = 40.0f,
        .destination_payout = 100.0f,
        .origin_completion_credit = 4.0f,
        .due_tick = 1234,
        .status = DELIVERY_SHIPMENT_DELIVERED,
    };
    memset(w->delivery_shipments[0].cargo_pub[0], 0xa1, 32);
    memset(w->delivery_shipments[0].cargo_pub[1], 0xb2, 32);
    w->delivery_shipments[0].cargo_units[0] = (cargo_unit_t){
        .kind = CARGO_KIND_INGOT,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity = 1,
    };
    w->delivery_shipments[0].cargo_units[1] =
        w->delivery_shipments[0].cargo_units[0];
    memcpy(w->delivery_shipments[0].cargo_units[0].pub,
           w->delivery_shipments[0].cargo_pub[0], 32);
    memcpy(w->delivery_shipments[0].cargo_units[1].pub,
           w->delivery_shipments[0].cargo_pub[1], 32);
    w->delivery_shipments[0].cargo_chains[0].len = 1;
    memset(&w->delivery_shipments[0].cargo_chains[0].links[0],
           0xc3, sizeof(w->delivery_shipments[0].cargo_chains[0].links[0]));
    memset(&w->cargo_pods[7], 0, sizeof(w->cargo_pods[7]));
    w->cargo_pods[7] = (cargo_pod_t){
        .active = true,
        .kind = CARGO_POD_CARGO,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity = 1,
        .shipment_id = 11,
        .pos = { 44.0f, -12.0f },
        .vel = { 1.0f, 2.0f },
        .radius = 18.0f,
        .rotation = 0.7f,
        .spin = 0.25f,
        .age = 3.0f,
        .towed_by = 0,
    };
    w->cargo_pods[7].has_shell_frame = true;
    cargo_pod_set_station_custody(&w->cargo_pods[7], 2);
    ASSERT(hash_legacy_migrate_unit((const uint8_t *)"SAVESHEL",
                                    COMMODITY_FRAME, 0,
                                    &w->cargo_pods[7].shell_frame));

    ASSERT(world_save(w, TMP("test_delivery_shipments.sav")));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(world_load(loaded, TMP("test_delivery_shipments.sav")));
    ASSERT_EQ_INT(loaded->next_delivery_shipment_id, 12);
    const delivery_shipment_t *shipment = &loaded->delivery_shipments[0];
    ASSERT(shipment->active);
    ASSERT_EQ_INT(shipment->shipment_id, 11);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_DELIVERED);
    ASSERT_EQ_INT(shipment->origin_station, 0);
    ASSERT_EQ_INT(shipment->destination_station, 2);
    ASSERT_EQ_INT(shipment->quantity_delivered, 1);
    ASSERT_EQ_FLOAT(shipment->debt_principal, 40.0f, 0.001f);
    ASSERT(memcmp(shipment->cargo_pub[0], w->delivery_shipments[0].cargo_pub[0], 32) == 0);
    ASSERT(memcmp(shipment->cargo_pub[1], w->delivery_shipments[0].cargo_pub[1], 32) == 0);
    ASSERT(memcmp(shipment->cargo_units[0].pub, w->delivery_shipments[0].cargo_pub[0], 32) == 0);
    ASSERT(memcmp(shipment->cargo_units[1].pub, w->delivery_shipments[0].cargo_pub[1], 32) == 0);
    ASSERT_EQ_INT(shipment->cargo_chains[0].len, 1);
    ASSERT(memcmp(&shipment->cargo_chains[0].links[0],
                  &w->delivery_shipments[0].cargo_chains[0].links[0],
                  sizeof(shipment->cargo_chains[0].links[0])) == 0);
    const cargo_pod_t *pod = &loaded->cargo_pods[7];
    ASSERT(pod->active);
    ASSERT_EQ_INT(pod->kind, CARGO_POD_CARGO);
    ASSERT_EQ_INT(pod->commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(pod->quantity, 1);
    ASSERT_EQ_INT(pod->shipment_id, 11);
    ASSERT_EQ_INT(pod->towed_by, 0);
    ASSERT_EQ_INT(cargo_pod_custody_station(pod), 2);
    ASSERT(pod->has_shell_frame);
    ASSERT_EQ_INT(pod->shell_frame.commodity, COMMODITY_FRAME);
    ASSERT(memcmp(pod->shell_frame.pub,
                  w->cargo_pods[7].shell_frame.pub, 32) == 0);
    ASSERT_EQ_FLOAT(pod->pos.x, 44.0f, 0.001f);
    ASSERT_EQ_FLOAT(pod->pos.y, -12.0f, 0.001f);
    remove(TMP("test_delivery_shipments.sav"));
}

TEST(test_world_load_v70_backfills_missing_starter_frame_pods) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    memset(w->cargo_pods, 0, sizeof(w->cargo_pods));
    ASSERT_EQ_INT(test_count_exact_frame_pod_units(w), 0);

    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS,
                                    TMP("test_v70_missing_starter_frames_cat")));
    ASSERT(world_save(w, TMP("test_v70_missing_starter_frames.sav")));
    ASSERT(test_patch_world_save_version(
        TMP("test_v70_missing_starter_frames.sav"), 70));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    world_reset(loaded);
    ASSERT(station_catalog_load_all(
        loaded->stations, MAX_STATIONS,
        TMP("test_v70_missing_starter_frames_cat")) > 0);
    ASSERT(world_load(loaded, TMP("test_v70_missing_starter_frames.sav")));
    ASSERT_EQ_INT(test_count_exact_frame_pod_units(loaded), 32);

    remove(TMP("test_v70_missing_starter_frames.sav"));
}

TEST(test_world_load_current_does_not_duplicate_starter_frame_pods) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    int starter_frames = test_count_exact_frame_pod_units(w);
    ASSERT_EQ_INT(starter_frames, 32);

    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS,
                                    TMP("test_existing_starter_frames_cat")));
    ASSERT(world_save(w, TMP("test_existing_starter_frames.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    world_reset(loaded);
    ASSERT(station_catalog_load_all(
        loaded->stations, MAX_STATIONS,
        TMP("test_existing_starter_frames_cat")) > 0);
    ASSERT(world_load(loaded, TMP("test_existing_starter_frames.sav")));
    ASSERT_EQ_INT(test_count_exact_frame_pod_units(loaded), starter_frames);

    remove(TMP("test_existing_starter_frames.sav"));
}

TEST(test_world_load_v71_backfills_missing_starter_laser_modules) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    memset(w->cargo_pods, 0, sizeof(w->cargo_pods));
    ASSERT_EQ_INT(station_finished_drain(&w->stations[1],
                                         COMMODITY_LASER_MODULE, 8), 8);
    ASSERT_EQ_INT(station_finished_count(&w->stations[1],
                                         COMMODITY_LASER_MODULE), 0);

    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS,
                                    TMP("test_v71_missing_starter_lasers_cat")));
    ASSERT(world_save(w, TMP("test_v71_missing_starter_lasers.sav")));
    ASSERT(test_patch_world_save_version(
        TMP("test_v71_missing_starter_lasers.sav"), 71));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    world_reset(loaded);
    ASSERT(station_catalog_load_all(
        loaded->stations, MAX_STATIONS,
        TMP("test_v71_missing_starter_lasers_cat")) > 0);
    ASSERT(world_load(loaded, TMP("test_v71_missing_starter_lasers.sav")));
    ASSERT_EQ_INT(station_finished_count(&loaded->stations[1],
                                         COMMODITY_LASER_MODULE), 8);

    remove(TMP("test_v71_missing_starter_lasers.sav"));
}

TEST(test_world_load_v71_does_not_duplicate_starter_laser_modules) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    memset(w->cargo_pods, 0, sizeof(w->cargo_pods));
    ASSERT_EQ_INT(station_finished_count(&w->stations[1],
                                         COMMODITY_LASER_MODULE), 8);

    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS,
                                    TMP("test_v71_existing_starter_lasers_cat")));
    ASSERT(world_save(w, TMP("test_v71_existing_starter_lasers.sav")));
    ASSERT(test_patch_world_save_version(
        TMP("test_v71_existing_starter_lasers.sav"), 71));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    world_reset(loaded);
    ASSERT(station_catalog_load_all(
        loaded->stations, MAX_STATIONS,
        TMP("test_v71_existing_starter_lasers_cat")) > 0);
    ASSERT(world_load(loaded, TMP("test_v71_existing_starter_lasers.sav")));
    ASSERT_EQ_INT(station_finished_count(&loaded->stations[1],
                                         COMMODITY_LASER_MODULE), 8);

    remove(TMP("test_v71_existing_starter_lasers.sav"));
}

TEST(test_player_load_restores_towed_cargo_pods_from_world) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    memset(w->cargo_pods, 0, sizeof(w->cargo_pods));

    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0x72, sizeof(sp->session_token));
    sp->current_station = 0;
    sp->ship.pos = v2(120.0f, -30.0f);
    sp->ship.angle = 0.5f;
    cargo_unit_t frame_units[8] = {{0}};
    const uint8_t origin[8] = { 'S','A','V','E','P','O','D','1' };
    for (uint16_t i = 0; i < 8; i++) {
        ASSERT(hash_legacy_migrate_unit(origin, COMMODITY_FRAME, i,
                                        &frame_units[i]));
    }
    int pod_idx = spawn_cargo_pod_with_manifest(w, sp->ship.pos,
                                                v2(0.0f, 0.0f),
                                                COMMODITY_FRAME,
                                                frame_units, 8,
                                                CARGO_POD_CARGO);
    ASSERT(pod_idx >= 0);
    w->cargo_pods[pod_idx].towed_by = 0;
    sp->ship.towed_pods[0] = (int16_t)pod_idx;
    sp->ship.towed_pod_count = 1;

    ASSERT(player_save(sp, test_tmp_dir(), 0));
    ASSERT(world_save(w, TMP("test_towed_pods_world.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    world_reset(loaded);
    ASSERT(world_load(loaded, TMP("test_towed_pods_world.sav")));
    server_player_t *lp = &loaded->players[0];
    lp->id = 0;
    lp->session_ready = true;
    memset(lp->session_token, 0x72, sizeof(lp->session_token));
    ASSERT(player_load_by_token(lp, loaded, test_tmp_dir(), sp->session_token));

    ASSERT_EQ_INT(lp->ship.towed_pod_count, 1);
    int restored_idx = lp->ship.towed_pods[0];
    ASSERT_EQ_INT(restored_idx, pod_idx);
    ASSERT(loaded->cargo_pods[restored_idx].active);
    ASSERT_EQ_INT(loaded->cargo_pods[restored_idx].towed_by, 0);
    ASSERT_EQ_INT(loaded->cargo_pods[restored_idx].quantity, 8);
    ASSERT_EQ_INT(loaded->cargo_pods[restored_idx].manifest_count, 8);
    ASSERT(memcmp(loaded->cargo_pods[restored_idx].manifest_units[0].pub,
                  frame_units[0].pub, 32) == 0);
    ASSERT(memcmp(loaded->cargo_pods[restored_idx].manifest_units[7].pub,
                  frame_units[7].pub, 32) == 0);
    ASSERT(v2_dist_sq(loaded->cargo_pods[restored_idx].pos,
                      lp->ship.pos) < 120.0f * 120.0f);

    remove(TMP("test_towed_pods_world.sav"));
}

/*
 * EXPECTED_SAVE_SIZE is the exact byte count of a world.sav written by the
 * current SAVE_VERSION. If a field is added to write_station / write_asteroid /
 * write_npc / write_contract / the scaffolds array, or the header, this
 * number changes and the test fails. That failure is the reminder to:
 *   1. Bump SAVE_VERSION
 *   2. Add a migration block in world_load()
 *   3. Update this constant to the new size
 */
/* v23: station credit pool added (#312) — +4 bytes per station (8×4=32). */
/* v29: +2 bytes per station (uint16 manifest count) = +128 bytes for all
 * MAX_STATIONS=64 slots. Empty stations carry only the count; no units. */
/* v30: +1 byte per contract (required_grade) = +24 for MAX_CONTRACTS=24. */
/* v35: dropped station.named_ingots[] (4B count + 64 × 56B record =
 * 3588B per station, × MAX_STATIONS=64 = 229,632 bytes saved). The
 * 56-byte per-slot disk size includes natural alignment padding — the
 * 52-byte wire record packed tighter, but the field-by-field WRITE
 * preserved the in-memory layout. */
/* v36: pubkey registry tail (#479 A.2) — 4-byte count + N×40 entries.
 * On a fresh world with no clients connected the count is zero, so
 * only the 4-byte header lands on disk.
 * v37: +4B belt_seed + 2B destroyed_rocks count prefix (#285 slice 1).
 * Fresh world has no destroyed rocks, so only the 6-byte header
 * lands on disk.
 * v38 unchanged from v37 on disk for a fresh world (no destroyed rocks
 * means no per-entry tail bytes, just the 4B belt_seed + 2B count).
 * v39: Layer A.3 of #479 added last_signed_nonce to the per-player
 * save (PLY6); world.sav format is unchanged so the size constant
 * stays the same. */
/* v40: Layer B of #479 — per-station Ed25519 pubkey (32B) + outpost
 * provenance (founder_pubkey 32B + planted_tick 8B) + station name
 * (32B, also written here so outpost rederivation stays self-
 * contained when the catalog isn't loaded alongside) = +104B per
 * station × MAX_STATIONS=64 = +6656 bytes. station_secret is
 * deliberately NOT persisted.
 * v41: Layer C of #479 — chain log continuation pointers per station
 * (chain_last_hash 32B + chain_event_count 8B) = +40B per station ×
 * MAX_STATIONS=64 = +2560 bytes. The chain event records themselves
 * live in side files under chain/<pubkey>.log, NOT in world.sav. */
/* v43: credit_pool field dropped (-4 bytes × 64 stations = -256). */
/* v46: ledger entry expanded for #257 station-player relationship.
 * Per-entry was 16B (8B player_token + 4B balance + 4B lifetime_supply);
 * now 76B = 32B player_pubkey + 4B balance + 4B lifetime_supply +
 * 8B first_dock_tick + 8B last_dock_tick + 4B total_docks +
 * 4B lifetime_ore_units + 4B lifetime_credits_in + 4B lifetime_credits_out +
 * 1B top_commodity + 3B _pad. Diff: +60B per entry × 16 entries × 64
 * stations = +61440 bytes.
 * v47: cross-ring pair-rule reseed adds modules to Kepler (+5) and
 * Helios (+2). Module placements live in the catalog file, not
 * world.sav, so EXPECTED_SAVE_SIZE doesn't shift.
 * v48: spoke + drag ring dynamics adds arm_omega[MAX_ARMS] = 4
 * floats × MAX_STATIONS=64 = +1024 bytes.
 * v52: NPC paired ship manifest tail writes uint16 count per
 * MAX_NPC_SHIPS slot on a fresh world. Active haulers with cargo add
 * variable cargo_unit_t + receipt-chain payloads.
 * v53: station manifest entries gained inline receipt-chain payloads.
 * Fresh world station manifests are empty, so this adds no bytes to
 * EXPECTED_SAVE_SIZE until a station is holding cargo.
 * v54: +4B world_seq added immediately after belt_seed in the world tail.
 * world.sav has zero fracture children so EXPECTED_SAVE_SIZE is unchanged.
 * v56: +36B per contract for heritage provenance requirements
 * (proof_flags + prefix + recipe + parent hash), × MAX_CONTRACTS=24.
 * v57: +8B per contract for forbidden origin masks.
 * v58: station session section expanded from 64 to 128 slots.
 * v59: +2B next_delivery_shipment_id + fixed delivery shipment sidecar table.
	 * v60: active fracture-child sidecars add thrown_by_token + thrown_timer_q;
	 * fresh world.sav has zero fracture children, so EXPECTED_SAVE_SIZE is unchanged.
	 * v61: +32B per contract for stable target_pub identity.
	 * v62: station ledger table expands from 16 to STATION_LEDGER_MAX=64 entries.
	 * v46+ ledger entries are 76B, so +48 entries × 76B × MAX_STATIONS=128.
	 * v63: station session persists pending ship-build queue:
	 * count int + 4 × 12B records per station, × MAX_STATIONS=128.
	 * v64: appends contract-origin ship asset registry tail.
	 * v65: pending ship-build records expand from 12B to 52B so each
	 * player commission captures the owner's pubkey/session identity.
	 * v66: +8B per station for faction id/allegiance/ideology and
	 * compact diplomacy relations.
		 * v67: active delivery shipments add variable exact cargo payloads;
		 * fresh world.sav has no active shipment payload, so unchanged.
		 * v68: active cargo pod tail persists the starter Kepler frame pod
		 * in fresh worlds.
             * v69: cargo pods can persist exact manifest payloads; the fresh
             * Kepler frame pod now saves its 16 frame units.
             * Prospect also starts with a 16-frame shell pod for the starter
             * furnace loop.
             * v70: active cargo pods persist their folded shell-frame flag.
             * v71: one-time starter frame pod backfill, no layout change.
             * v72: Kepler starter Laser Module reserve adds eight
             * manifest-backed cargo_unit_t rows.
             * v73: active cargo pods persist custody_station; fresh worlds
             * have two starter pods, so +2 bytes.
             * v74: each active pod also persists tractor_station and
             * tractor_module; two starter pods add four bytes. */
			#define EXPECTED_SAVE_SIZE 767648

TEST(test_save_file_size_stable) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    ASSERT(world_save(w, TMP("test_size.sav")));
    /* w auto-freed by WORLD_HEAP cleanup */
    FILE *f = fopen(TMP("test_size.sav"), "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    /* If this fails you changed the binary save format.
     * Bump SAVE_VERSION, add a migration, and update EXPECTED_SAVE_SIZE. */
    ASSERT_EQ_INT((int)size, EXPECTED_SAVE_SIZE);
    remove(TMP("test_size.sav"));
}

TEST(test_save_header_golden_bytes) {
    WORLD_DECL;
    w.rng = 2037u;  /* default seed */
    world_reset(&w);
    w.time = 0.0f;
    w.field_spawn_timer = 0.0f;
    ASSERT(world_save(&w, TMP("test_header.sav")));
    FILE *f = fopen(TMP("test_header.sav"), "rb");
    ASSERT(f != NULL);
    uint32_t magic, version, rng;
    float time_val, spawn_timer;
    ASSERT_EQ_INT((int)fread(&magic,       4, 1, f), 1);
    ASSERT_EQ_INT((int)fread(&version,     4, 1, f), 1);
    ASSERT_EQ_INT((int)fread(&rng,         4, 1, f), 1);
    ASSERT_EQ_INT((int)fread(&time_val,    4, 1, f), 1);
    ASSERT_EQ_INT((int)fread(&spawn_timer, 4, 1, f), 1);
    fclose(f);
    ASSERT_EQ_INT((int)magic, (int)0x5349474E);    /* "SIGN" */
    ASSERT_EQ_INT((int)version, 74);
    ASSERT(rng != 0);  /* seed is set */
    ASSERT_EQ_FLOAT(time_val, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(spawn_timer, 0.0f, 0.001f);
    remove(TMP("test_header.sav"));
}

TEST(test_contract_target_pub_roundtrips) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    w->contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_FRACTURE,
        .station_index = 0,
        .target_index = 12,
        .target_pos = v2(120.0f, -40.0f),
        .base_price = 30.0f,
        .age = 4.0f,
        .claimed_by = -1,
    };
    for (int i = 0; i < 32; i++)
        w->contracts[0].target_pub[i] = (uint8_t)(0xA0u + (uint8_t)i);

    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS,
                                    TMP("test_contract_pub_cat")));
    ASSERT(world_save(w, TMP("test_contract_pub.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    ASSERT(station_catalog_load_all(loaded->stations, MAX_STATIONS,
                                    TMP("test_contract_pub_cat")));
    ASSERT(world_load(loaded, TMP("test_contract_pub.sav")));
    ASSERT(loaded->contracts[0].active);
    ASSERT_EQ_INT(loaded->contracts[0].action, CONTRACT_FRACTURE);
    ASSERT(memcmp(loaded->contracts[0].target_pub,
                  w->contracts[0].target_pub, 32) == 0);

    remove(TMP("test_contract_pub.sav"));
}

TEST(test_save_load_preserves_player_outpost) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    player_init_ship(&w->players[0], w);
    w->players[0].connected = true;
    w->players[0].docked = false;  /* must be undocked to place */
    /* Place outside Prospect's core coverage (signal < 0.80 at placement). */
    vec2 pos = v2(6000.0f, -2400.0f);
    int slot = test_place_outpost_via_tow(w, &w->players[0], pos);
    ASSERT(slot >= 0);
    ASSERT(station_exists(&w->stations[slot]));
    ASSERT(w->stations[slot].scaffold);
    /* Deliver some frames to advance progress */
    w->stations[slot]._inventory_cache[COMMODITY_FRAME] = 30.0f;
    for (int i = 0; i < 600; i++) world_sim_step(w, SIM_DT);
    float progress = w->stations[slot].scaffold_progress;
    int mod_count = w->stations[slot].module_count;
    char name_buf[32];
    memcpy(name_buf, w->stations[slot].name, 32);
    /* Save and reload (world + catalog) */
    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS, TMP("test_outcat")));
    ASSERT(world_save(w, TMP("test_outpost.sav")));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    station_catalog_load_all(loaded->stations, MAX_STATIONS, TMP("test_outcat"));
    ASSERT(world_load(loaded, TMP("test_outpost.sav")));
    /* Outpost must survive */
    ASSERT(station_exists(&loaded->stations[slot]));
    ASSERT(loaded->stations[slot].scaffold);
    ASSERT_EQ_FLOAT(loaded->stations[slot].pos.x, 6000.0f, 1.0f);
    ASSERT_EQ_FLOAT(loaded->stations[slot].pos.y, -2400.0f, 1.0f);
    ASSERT_EQ_FLOAT(loaded->stations[slot].scaffold_progress, progress, 0.01f);
    ASSERT_EQ_INT(loaded->stations[slot].module_count, mod_count);
    ASSERT_STR_EQ(loaded->stations[slot].name, name_buf);
    /* Signal chain rebuilt — outpost may or may not be connected depending on
     * scaffold state, but the station slot must still exist */
    ASSERT(loaded->stations[slot].signal_range > 0.0f);
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove(TMP("test_outpost.sav"));
}

TEST(test_save_backward_compat_version_accepted) {
    /* v24 save format roundtrips correctly with inventory data.
     * Station identity comes from catalog, session data from world save. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    w->stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 77.0f;
    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS, TMP("test_compatcat")));
    ASSERT(world_save(w, TMP("test_compat.sav")));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    station_catalog_load_all(loaded->stations, MAX_STATIONS, TMP("test_compatcat"));
    ASSERT(world_load(loaded, TMP("test_compat.sav")));
    ASSERT_EQ_FLOAT(loaded->stations[0]._inventory_cache[COMMODITY_FERRITE_ORE], 77.0f, 0.01f);
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove(TMP("test_compat.sav"));
}

TEST(test_save_v21_module_remap) {
    /* SKIPPED: This test patches a v23 save to look like v21, but the v23
     * format has credit_pool fields woven into each station record that
     * v21 doesn't have. The loader can't distinguish real v21 from
     * patched v23, so the file is unreadable. Skipping until a proper
     * v21 binary fixture is created. (#312)
     *
     * The original disabled body remapped module types 0/5/6/11/12/15 from
     * the v21 enum space to v22 outcomes (DOCK/dropped/REPAIR_BAY/dropped/
     * ORE_SILO/SHIPYARD). See git blame on this test for the full body. */
}

TEST(test_save_future_version_rejected) {
    /* A save with version > SAVE_VERSION must be rejected (can't load future formats) */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    ASSERT(world_save(w, TMP("test_future.sav")));
    FILE *f = fopen(TMP("test_future.sav"), "r+b");
    ASSERT(f != NULL);
    fseek(f, 4, SEEK_SET);
    uint32_t future = 9999;
    fwrite(&future, sizeof(future), 1, f);
    fclose(f);
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(!world_load(loaded, TMP("test_future.sav")));
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove(TMP("test_future.sav"));
}

TEST(test_world_load_bad_crc_rejects_before_mutating_destination) {
    WORLD_HEAP saved = calloc(1, sizeof(world_t));
    ASSERT(saved != NULL);
    saved->rng = 424242u;
    world_reset(saved);
    ASSERT(world_save(saved, TMP("test_world_bad_crc.sav")));

    FILE *f = fopen(TMP("test_world_bad_crc.sav"), "r+b");
    ASSERT(f != NULL);
    ASSERT(fseek(f, 12, SEEK_SET) == 0);
    int byte = fgetc(f);
    ASSERT(byte != EOF);
    ASSERT(fseek(f, 12, SEEK_SET) == 0);
    ASSERT(fputc(byte ^ 0x5a, f) != EOF);
    ASSERT(fclose(f) == 0);

    WORLD_HEAP destination = calloc(1, sizeof(world_t));
    ASSERT(destination != NULL);
    destination->rng = 0xdeadbeefu;
    destination->world_seq = 0xa5a5a5a5u;
    ASSERT(!world_load(destination, TMP("test_world_bad_crc.sav")));
    ASSERT(destination->rng == 0xdeadbeefu);
    ASSERT(destination->world_seq == 0xa5a5a5a5u);

    remove(TMP("test_world_bad_crc.sav"));
}

TEST(test_world_load_missing_crc_rejects_before_mutating_destination) {
    WORLD_HEAP saved = calloc(1, sizeof(world_t));
    ASSERT(saved != NULL);
    saved->rng = 424242u;
    world_reset(saved);
    ASSERT(world_save(saved, TMP("test_world_with_crc.sav")));

    FILE *f = fopen(TMP("test_world_with_crc.sav"), "rb");
    ASSERT(f != NULL);
    ASSERT(fseek(f, 0, SEEK_END) == 0);
    long len = ftell(f);
    ASSERT(len > 8);
    ASSERT(fclose(f) == 0);
    ASSERT(test_copy_file_prefix(TMP("test_world_with_crc.sav"),
                                 TMP("test_world_without_crc.sav"),
                                 len - 8));

    WORLD_HEAP destination = calloc(1, sizeof(world_t));
    ASSERT(destination != NULL);
    destination->rng = 0xdeadbeefu;
    destination->world_seq = 0xa5a5a5a5u;
    ASSERT(!world_load(destination, TMP("test_world_without_crc.sav")));
    ASSERT(destination->rng == 0xdeadbeefu);
    ASSERT(destination->world_seq == 0xa5a5a5a5u);

    remove(TMP("test_world_with_crc.sav"));
    remove(TMP("test_world_without_crc.sav"));
}

void register_save_persistence_tests(void) {
    TEST_SECTION("\nPersistence tests:\n");
    RUN(test_player_save_load_roundtrip);
    RUN(test_world_save_load_preserves_stations);
    RUN(test_world_save_load_preserves_station_factions);
    RUN(test_world_save_load_preserves_npcs);
    RUN(test_npc_ship_physics_in_sync_each_tick);
    RUN(test_world_load_rebuilds_character_pool);
    RUN(test_world_save_load_preserves_fracture_children);
    RUN(test_world_load_preserves_fracture_claim_dedupe_identity);
    RUN(test_world_load_missing_file);
    RUN(test_player_save_load_preserves_ship);
    RUN(test_player_load_prefers_existing_bound_ship_asset);
    RUN(test_player_load_prefers_owned_asset_over_provisional_loaner);
    RUN(test_player_load_mints_owned_asset_instead_of_reusing_loaner);
    RUN(test_world_load_stores_orphaned_player_ship_asset_for_reclaim);
    RUN(test_world_load_repairs_stale_npc_ship_asset_binding);
    RUN(test_player_save_uses_temp_then_atomic_rename);
    RUN(test_world_save_round_trips_station_manifest);
    RUN(test_world_load_repairs_cache_only_station_finished_goods);
    RUN(test_player_load_clamps_negative_credits);
    RUN(test_player_save_round_trips_ship_manifest);
    RUN(test_player_load_bad_crc_rejects_without_mutating_live_player);
    RUN(test_player_load_bad_receipt_count_rejects_without_mutating_live_player);
    RUN(test_player_load_clamps_negative_cargo);
    RUN(test_player_load_clamps_hull_hp);
    RUN(test_player_load_clamps_upgrade_levels);
    RUN(test_player_load_invalid_station_falls_back);
    RUN(test_player_load_repairs_degenerate_dock_berth);
    RUN(test_world_load_bad_crc_rejects_before_mutating_destination);
    RUN(test_world_load_missing_crc_rejects_before_mutating_destination);
    RUN(test_player_load_bad_magic_fails);
    RUN(test_world_load_rejects_stale_version);
    RUN(test_world_save_load_preserves_module_ring_slot);
    RUN(test_v3_station_catalog_repairs_helios_smelter_layout);
    RUN(test_station_catalog_bad_file_preserves_seeded_fallback);
    RUN(test_v51_migration_tags_untagged_furnaces_and_fills_hoppers);
    RUN(test_v51_migration_furnace_count_heuristic);
    RUN(test_world_save_load_preserves_smelted_ingot_pod);
    RUN(test_world_save_load_preserves_hauler_manifest_cargo);
    RUN(test_world_save_load_preserves_delivery_shipments);
    RUN(test_world_load_v70_backfills_missing_starter_frame_pods);
    RUN(test_world_load_current_does_not_duplicate_starter_frame_pods);
    RUN(test_world_load_v71_backfills_missing_starter_laser_modules);
    RUN(test_world_load_v71_does_not_duplicate_starter_laser_modules);
    RUN(test_player_load_restores_towed_cargo_pods_from_world);
    RUN(test_contract_target_pub_roundtrips);
}

void register_save_format_tests(void) {
    TEST_SECTION("\nSave format stability:\n");
    RUN(test_save_file_size_stable);
    RUN(test_save_header_golden_bytes);
    RUN(test_save_load_preserves_player_outpost);
    RUN(test_save_backward_compat_version_accepted);
    RUN(test_save_v21_module_remap);
    RUN(test_save_future_version_rejected);
}
