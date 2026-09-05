#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "local_save.h"
#include "persistence_generation.h"
#include "sim_catalog.h"
#include "actor_principal_resolver.h"
#include "progress_store.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(int, local_save_browser_state, (), {
    return Module.signalLocalPersistence.state;
})
EM_JS(void, local_save_browser_flush, (), {
    Module.signalLocalPersistence.flush();
})
#else
#include "persistence_writer.h"
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#endif

struct local_save {
    char root[PERSISTENCE_GENERATION_PATH_MAX];
    char legacy[PERSISTENCE_GENERATION_PATH_MAX];
    uint8_t pubkey[32];
    persistence_generation_paths_t selected;
    float elapsed;
    float retry_wait;
    int player_slot;
    int station_count;
    int station;
    float balance;
    uint32_t ship_asset_id;
    uint64_t signed_nonce;
    uint8_t mining_level, hold_level, tractor_level;
    bool ready;
    bool failed;
    bool was_docked;
#ifndef __EMSCRIPTEN__
    persistence_writer_t *writer;
#ifdef _WIN32
    HANDLE lock;
#else
    int lock;
#endif
#endif
};

static bool local_save_lock(local_save_t *save) {
#ifdef __EMSCRIPTEN__
    (void)save;
    return local_save_browser_state() == 1;
#else
    char path[PERSISTENCE_GENERATION_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/LOCK", save->root);
    if (n <= 0 || (size_t)n >= sizeof(path)) return false;
#ifdef _WIN32
    save->lock = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return save->lock != INVALID_HANDLE_VALUE;
#else
    save->lock = open(path, O_CREAT | O_RDWR, 0600);
    return save->lock >= 0 && flock(save->lock, LOCK_EX | LOCK_NB) == 0;
#endif
#endif
}

static void local_save_release(local_save_t *save) {
    if (!save) return;
#ifndef __EMSCRIPTEN__
    persistence_writer_destroy(save->writer);
#ifdef _WIN32
    if (save->lock != INVALID_HANDLE_VALUE) CloseHandle(save->lock);
#else
    if (save->lock >= 0) close(save->lock);
#endif
#endif
    free(save);
}

local_save_t *local_save_open(const char *root, world_t *world,
                              const uint8_t pubkey[32], bool *fresh) {
    if (fresh) *fresh = false;
    uint8_t combined = 0;
    if (!root || !root[0] || !world || !pubkey || !fresh) return NULL;
    for (int i = 0; i < 32; i++) combined |= pubkey[i];
    if (!combined) return NULL;
    local_save_t *save = calloc(1, sizeof(*save));
    if (!save) return NULL;
#ifndef __EMSCRIPTEN__
#ifdef _WIN32
    save->lock = INVALID_HANDLE_VALUE;
#else
    save->lock = -1;
#endif
#endif
    int n = snprintf(save->root, sizeof(save->root), "%s", root);
    if (n <= 0 || (size_t)n >= sizeof(save->root)) goto fail;
    n = snprintf(save->legacy, sizeof(save->legacy), "%s/players", root);
    if (n <= 0 || (size_t)n >= sizeof(save->legacy)) goto fail;
    memcpy(save->pubkey, pubkey, 32);
#ifdef _WIN32
    int made = _mkdir(root);
#else
    int made = mkdir(root, 0700);
#endif
    if (made != 0 && errno != EEXIST) goto fail;
    if (!local_save_lock(save)) goto fail;
    persistence_generation_status_t status =
        persistence_generation_resolve(root, &save->selected);
    if (status == PERSISTENCE_GENERATION_INVALID) goto fail;
    *fresh = status == PERSISTENCE_GENERATION_NONE;
    if (!*fresh) {
        if (station_catalog_load_all(world->stations, MAX_STATIONS,
                                     save->selected.catalog_dir) < 0 ||
            !world_load(world, save->selected.world_path) ||
            !world_validate_station_actor_ids(world)) goto fail;
        /* Catalog records carry station identity; currency labels are
         * derived defaults, as in the dedicated server's load path. */
        static const char *labels[] = {
            "prospect vouchers", "kepler bonds", "helios credits", "freeport scrip"
        };
        for (int i = 0; i < 4 && i < MAX_STATIONS; i++) {
            if (!world->stations[i].currency_name[0])
                snprintf(world->stations[i].currency_name,
                         sizeof(world->stations[i].currency_name), "%s", labels[i]);
        }
    }
#ifndef __EMSCRIPTEN__
    save->writer = persistence_writer_create();
    if (!save->writer) goto fail;
#endif
    return save;
fail:
    fprintf(stderr, "[local-save] Recovery or exclusive storage access required: %s\n", root);
    local_save_release(save);
    return NULL;
}

bool local_save_restore_player(local_save_t *save, world_t *world, int slot) {
    if (!save || !world || slot < 0 || slot >= MAX_PLAYERS) return false;
    server_player_t *player = &world->players[slot];
    if (!server_player_can_use_pubkey_persistence(player) ||
        memcmp(player->pubkey, save->pubkey, 32) != 0) return false;
    if (save->selected.generation &&
        !player_load_by_pubkey(player, world, save->selected.player_dir,
                               save->pubkey)) return false;
    if (save->selected.generation)
        client_progress_restore_local(player->client_progress_flags);
    save->player_slot = slot;
    save->ready = true;
    save->was_docked = player->docked;
    save->station_count = world->station_count;
    save->elapsed = 30.0f;
    return true;
}

static void local_save_poll(local_save_t *save, bool wait) {
#ifdef __EMSCRIPTEN__
    (void)wait;
    if (local_save_browser_state() == -1) save->failed = true;
#else
    persistence_writer_state_t state = wait
        ? persistence_writer_wait(save->writer, &save->selected)
        : persistence_writer_poll(save->writer, &save->selected);
    if (state == PERSISTENCE_WRITER_FAILED) {
        save->failed = true;
        save->retry_wait = 5.0f;
    }
    if (state == PERSISTENCE_WRITER_SUCCEEDED) save->failed = false;
#endif
}

bool local_save_request(local_save_t *save, world_t *world, bool wait) {
    if (!save || !save->ready || !world) return false;
    const server_player_t *player = &world->players[save->player_slot];
    if (!server_player_can_use_pubkey_persistence(player) ||
        memcmp(player->pubkey, save->pubkey, 32) != 0) return false;
    local_save_poll(save, wait);
    bool slots[MAX_PLAYERS] = {false};
    slots[save->player_slot] = true;
    world->players[save->player_slot].client_progress_flags = client_progress_pack_local();
#ifdef __EMSCRIPTEN__
    if (local_save_browser_state() == 2) return false;
    persistence_generation_paths_t published = {0};
    bool ok = persistence_generation_commit(save->root, save->legacy, world, slots,
        PERSISTENCE_GENERATION_FAULT_NONE, &published);
    if (ok) {
        save->selected = published;
        local_save_browser_flush();
    }
#else
    if (persistence_writer_active(save->writer)) return false;
    bool ok = persistence_writer_start(save->writer, save->root, save->legacy, world, slots);
    if (ok && wait) {
        local_save_poll(save, true);
        ok = !save->failed;
    }
#endif
    save->failed = !ok;
    if (!ok) save->retry_wait = 5.0f;
    if (ok) {
        save->elapsed = 0.0f;
        save->was_docked = player->docked;
        save->station_count = world->station_count;
        save->station = player->current_station;
        if (save->station >= 0 && save->station < MAX_STATIONS)
            save->balance = ledger_balance_by_pubkey(
                &world->stations[save->station], save->pubkey);
        save->ship_asset_id = player->ship_asset_id;
        save->signed_nonce = player->last_signed_nonce;
        save->mining_level = player->ship->mining_level;
        save->hold_level = player->ship->hold_level;
        save->tractor_level = player->ship->tractor_level;
    }
    return ok;
}

void local_save_update(local_save_t *save, world_t *world, float dt) {
    if (!save || !save->ready || !world) return;
    local_save_poll(save, false);
    if (isfinite(dt) && dt > 0.0f) {
        save->elapsed += dt;
        save->retry_wait -= dt;
    }
    if (save->retry_wait > 0.0f) return;
    const server_player_t *player = &world->players[save->player_slot];
    int station = player->current_station;
    float balance = station >= 0 && station < MAX_STATIONS
        ? ledger_balance_by_pubkey(&world->stations[station], save->pubkey) : 0.0f;
    if (save->elapsed >= (player->docked ? 5.0f : 30.0f) ||
        player->docked != save->was_docked ||
        world->station_count != save->station_count ||
        station != save->station || balance != save->balance ||
        player->ship_asset_id != save->ship_asset_id ||
        player->last_signed_nonce != save->signed_nonce ||
        player->ship->mining_level != save->mining_level ||
        player->ship->hold_level != save->hold_level ||
        player->ship->tractor_level != save->tractor_level) {
        (void)local_save_request(save, world, false);
    }
}

void local_save_close(local_save_t *save, world_t *world) {
    if (!save) return;
    if (save->ready && world && !local_save_request(save, world, true))
        fprintf(stderr, "[local-save] Final checkpoint needs a retry: %s\n", save->root);
    local_save_release(save);
}

uint64_t local_save_generation(const local_save_t *save) {
    if (!save || save->failed) return 0;
#ifdef __EMSCRIPTEN__
    if (local_save_browser_state() != 1) return 0;
#endif
    return save->selected.generation;
}

bool local_save_failed(const local_save_t *save) {
    return save && save->failed;
}
