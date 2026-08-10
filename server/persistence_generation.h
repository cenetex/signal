/*
 * Crash-consistent complete-generation persistence.
 *
 * A generation owns one world snapshot, the station catalog set, and the
 * complete player-save namespace.  Generation directories are immutable once
 * published.  CURRENT is the sole mutable commit marker and is replaced only
 * after every artifact and the generation manifest are durable.
 */
#ifndef SIGNAL_PERSISTENCE_GENERATION_H
#define SIGNAL_PERSISTENCE_GENERATION_H

#include "game_sim.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PERSISTENCE_GENERATION_PATH_MAX 512

typedef enum {
    PERSISTENCE_GENERATION_NONE = 0,
    PERSISTENCE_GENERATION_CURRENT,
    PERSISTENCE_GENERATION_PREVIOUS,
    PERSISTENCE_GENERATION_INVALID,
} persistence_generation_status_t;

typedef enum {
    PERSISTENCE_GENERATION_FAULT_NONE = 0,
    /* Artifacts are durable, but no immutable manifest exists yet. */
    PERSISTENCE_GENERATION_FAULT_AFTER_ARTIFACTS,
    /* The immutable manifest is durable, but CURRENT is unchanged. */
    PERSISTENCE_GENERATION_FAULT_AFTER_MANIFEST,
    /* CURRENT.tmp is durable, but the atomic publish rename has not run. */
    PERSISTENCE_GENERATION_FAULT_BEFORE_POINTER_PUBLISH,
    /*
     * The CURRENT rename succeeds and the parent-directory fsync is reported
     * as failed.  Commit must re-resolve the visible marker and adopt it.
     */
    PERSISTENCE_GENERATION_FAULT_POINTER_DIR_SYNC_FAILURE,
} persistence_generation_fault_t;

typedef struct {
    uint64_t generation;
    char world_path[PERSISTENCE_GENERATION_PATH_MAX];
    char catalog_dir[PERSISTENCE_GENERATION_PATH_MAX];
    char player_dir[PERSISTENCE_GENERATION_PATH_MAX];
    uint8_t manifest_sha256[32];
} persistence_generation_paths_t;

typedef enum {
    PERSISTENCE_RECOVERY_COMMIT_OK = 0,
    PERSISTENCE_RECOVERY_COMMIT_INVALID_ARGUMENT,
    PERSISTENCE_RECOVERY_COMMIT_NO_SOURCE,
    PERSISTENCE_RECOVERY_COMMIT_DESTINATION_CONFLICT,
    PERSISTENCE_RECOVERY_COMMIT_SOURCE_CHANGED,
    PERSISTENCE_RECOVERY_COMMIT_IO_FAILURE,
} persistence_recovery_commit_result_t;

/*
 * Resolve and validate the committed generation.  NONE means no CURRENT
 * marker exists and callers may use the legacy layout.  INVALID means a
 * marker exists but neither its current nor authenticated previous generation
 * validates; callers must fail closed.
 */
persistence_generation_status_t persistence_generation_resolve(
    const char *root_dir,
    persistence_generation_paths_t *out);

/*
 * Publish one complete generation.  `legacy_player_dir` is consulted only
 * before the first generation; later commits carry the complete player
 * namespace forward from the last validated generation.  `save_player_slot`
 * selects live/grace players whose copied save must be refreshed.
 *
 * Fault points are deterministic test hooks.  A false return never changes
 * the selected generation.  The post-rename sync-failure hook returns true
 * after re-validating and adopting the now-visible CURRENT generation.
 */
bool persistence_generation_commit(
    const char *root_dir,
    const char *legacy_player_dir,
    const world_t *world,
    const bool save_player_slot[MAX_PLAYERS],
    persistence_generation_fault_t fault,
    persistence_generation_paths_t *published);

/*
 * Publish the same complete generation while atomically promoting exactly one
 * canonical token-keyed player save into the proven pubkey namespace.
 *
 * The source and destination names are derived internally from the fixed-size
 * token/pubkey inputs. The source is omitted from the candidate namespace,
 * and any pre-existing destination causes a no-replace conflict before the
 * candidate can be published. `save_player_slot[player_slot]` must be true,
 * so the candidate receives the staged player's pubkey-keyed save alongside
 * the matching staged world snapshot. The manifest covers a bearer-free
 * consumption marker, and the published recovery pointer has no fallback
 * edge to its source-bearing predecessor.
 */
persistence_recovery_commit_result_t
persistence_generation_commit_recovery(
    const char *root_dir,
    const char *legacy_player_dir,
    const world_t *world,
    const bool save_player_slot[MAX_PLAYERS],
    int player_slot,
    const uint8_t session_token[8],
    const uint8_t pubkey[32],
    uint64_t source_size,
    const uint8_t source_sha256[32],
    persistence_generation_fault_t fault,
    persistence_generation_paths_t *published);

#if defined(SIGNAL_SAVE_TESTING)
typedef void (*persistence_recovery_test_hook_fn)(
    const char *source_path,
    const char *destination_path,
    void *user);
/*
 * Deterministic race seams. The source-bind hook runs after the caller has
 * decoded its snapshot but before commit re-opens it. The destination hook
 * runs after namespace copy and immediately before atomic no-replace save
 * publication.
 */
void persistence_recovery_test_set_before_source_bind_hook(
    persistence_recovery_test_hook_fn hook, void *user);
void persistence_recovery_test_set_before_destination_publish_hook(
    persistence_recovery_test_hook_fn hook, void *user);
void persistence_recovery_test_reset_hooks(void);
#endif

#endif
