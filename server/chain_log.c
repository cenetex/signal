#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * chain_log.c -- Per-station signed event chain log (Layer C of #479).
 * See chain_log.h for the high-level scheme.
 */
#include "chain_log.h"

#include "game_sim.h"          /* world_t, SIM_LOG */
#include "manifest.h"          /* recipe + canonical cargo semantics */
#include "station_authority.h" /* station_sign / station_verify */
#include "sha256.h"
#include "base58.h"
#include "signal_crypto.h"
#include "persistence_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#  include <direct.h>
#  include <io.h>
#  define MKDIR(p) _mkdir(p)
#else
#  include <unistd.h>
#  define MKDIR(p) mkdir((p), 0775)
#endif

/* Compile-time guarantee that the header serializes exactly the way
 * we read/write it. If anyone reorders or repads the struct, this
 * fires before the on-disk format silently drifts. */
_Static_assert(sizeof(chain_event_header_t) == CHAIN_EVENT_HEADER_SIZE,
               "chain_event_header_t must be exactly 184 bytes — "
               "the on-disk + signed-message format depends on it");

/* The unsigned-header span signed by the station: epoch (8) + event_id
 * (8) + type (1) + pad (7) + authority (32) + payload_hash (32) +
 * prev_hash (32) = 120 bytes — i.e. everything BEFORE signature[]. */
#define CHAIN_UNSIGNED_HEADER_SIZE 120
_Static_assert(CHAIN_UNSIGNED_HEADER_SIZE ==
                   CHAIN_EVENT_HEADER_SIZE - 64,
               "unsigned-header span must equal sizeof header minus the "
               "64-byte trailing signature");

static bool chain_cargo_pub_is_zero(const uint8_t pub[32]) {
    static const uint8_t zero[32] = {0};
    return !pub || memcmp(pub, zero, sizeof(zero)) == 0;
}

static bool chain_cargo_matches_ignoring_origin(
    const cargo_unit_t *actual,
    const cargo_unit_t *expected) {
    cargo_unit_t actual_normalized;
    cargo_unit_t expected_normalized;
    uint8_t actual_wire[CARGO_UNIT_WIRE_SIZE];
    uint8_t expected_wire[CARGO_UNIT_WIRE_SIZE];

    if (!actual || !expected) return false;
    actual_normalized = *actual;
    expected_normalized = *expected;
    actual_normalized.origin_station = 0;
    expected_normalized.origin_station = 0;
    cargo_unit_wire_pack(&actual_normalized, actual_wire);
    cargo_unit_wire_pack(&expected_normalized, expected_wire);
    return memcmp(actual_wire, expected_wire,
                  sizeof(actual_wire)) == 0;
}

bool chain_payload_smelt_bind_output(
    chain_payload_smelt_t *payload,
    const uint8_t fragment_pub[32],
    uint16_t output_index,
    const cargo_unit_t *output) {
    cargo_unit_t canonical;
    if (!payload || !fragment_pub || !output ||
        chain_cargo_pub_is_zero(fragment_pub) ||
        !hash_ingot((commodity_t)output->commodity,
                    (mining_grade_t)output->grade,
                    fragment_pub, output_index, &canonical)) {
        return false;
    }
    /* mined_block is refinery context rather than hash input, but it is
     * still copied into and authenticated by the SMELT payload. */
    canonical.mined_block = output->mined_block;
    if (!chain_cargo_matches_ignoring_origin(output, &canonical))
        return false;

    memset(payload, 0, sizeof(*payload));
    memcpy(payload->fragment_pub, fragment_pub, 32);
    memcpy(payload->ingot_pub, output->pub, 32);
    payload->prefix_class = output->prefix_class;
    payload->semantics_version = CHAIN_CARGO_SEMANTICS_V1;
    payload->commodity = output->commodity;
    payload->grade = output->grade;
    payload->output_index = output_index;
    payload->mined_block = output->mined_block;
    return true;
}

bool chain_payload_craft_bind_output(
    chain_payload_craft_t *payload,
    const cargo_unit_t *inputs,
    size_t input_count,
    const cargo_unit_t *output) {
    cargo_kind_t expected_kind;
    if (!payload || !output ||
        input_count > RECIPE_INPUT_MAX ||
        (input_count > 0 && !inputs) ||
        chain_cargo_pub_is_zero(output->pub) ||
        (unsigned)output->grade >= (unsigned)MINING_GRADE_COUNT ||
        output->quantity != 1u ||
        !cargo_kind_for_commodity(
            (commodity_t)output->commodity, &expected_kind) ||
        expected_kind != (cargo_kind_t)output->kind ||
        output->recipe_id == (uint16_t)RECIPE_SMELT ||
        output->prefix_class != (uint8_t)INGOT_PREFIX_ANONYMOUS ||
        output->mined_block != 0u) {
        return false;
    }

    if (output->recipe_id == (uint16_t)RECIPE_LEGACY_MIGRATE) {
        static const uint8_t zero_parent[32] = {0};
        if (input_count != 0 ||
            memcmp(output->parent_merkle, zero_parent, 32) != 0) {
            return false;
        }
    } else {
        const recipe_def_t *recipe =
            recipe_get((recipe_id_t)output->recipe_id);
        if (!recipe ||
            input_count != recipe->input_count ||
            output->kind != (uint8_t)recipe->output_kind ||
            output->commodity != (uint8_t)recipe->output_commodity) {
            return false;
        }
        bool canonical_match = false;
        for (uint32_t output_index = 0;
             output_index < (uint32_t)recipe->output_count;
             output_index++) {
            cargo_unit_t canonical;
            if (!hash_product(
                    (recipe_id_t)output->recipe_id, inputs,
                    input_count, (uint16_t)output_index,
                    &canonical)) {
                return false;
            }
            if (chain_cargo_matches_ignoring_origin(
                    output, &canonical)) {
                canonical_match = true;
                break;
            }
        }
        if (!canonical_match) {
            return false;
        }
    }

    memset(payload, 0, sizeof(*payload));
    payload->recipe_id = output->recipe_id;
    payload->input_count = (uint8_t)input_count;
    payload->semantics_version = CHAIN_CARGO_SEMANTICS_V1;
    payload->output_kind = output->kind;
    payload->output_commodity = output->commodity;
    payload->output_grade = output->grade;
    payload->output_quantity = output->quantity;
    memcpy(payload->output_pub, output->pub, 32);
    for (size_t i = 0; i < input_count; i++)
        memcpy(payload->input_pubs[i], inputs[i].pub, 32);
    return true;
}

static void chain_cargo_transform_decode_output(
    chain_cargo_transform_t *transform) {
    if (!transform) return;
    memset(&transform->output_cargo, 0,
           sizeof(transform->output_cargo));
    transform->output_semantics_version =
        CHAIN_CARGO_SEMANTICS_UNBOUND;

    if (transform->type == CHAIN_EVT_SMELT) {
        const chain_payload_smelt_t *payload = &transform->smelt;
        static const uint8_t zero_reserved[2] = {0};
        cargo_unit_t canonical;
        if (payload->semantics_version != CHAIN_CARGO_SEMANTICS_V1 ||
            memcmp(payload->_reserved, zero_reserved,
                   sizeof(zero_reserved)) != 0 ||
            chain_cargo_pub_is_zero(payload->fragment_pub) ||
            chain_cargo_pub_is_zero(payload->ingot_pub) ||
            !hash_ingot(
                (commodity_t)payload->commodity,
                (mining_grade_t)payload->grade,
                payload->fragment_pub, payload->output_index,
                &canonical) ||
            memcmp(canonical.pub, payload->ingot_pub, 32) != 0 ||
            canonical.prefix_class != payload->prefix_class) {
            return;
        }
        canonical.mined_block = payload->mined_block;
        transform->output_cargo = canonical;
        transform->output_semantics_version =
            payload->semantics_version;
        return;
    }

    if (transform->type == CHAIN_EVT_CRAFT) {
        const chain_payload_craft_t *payload = &transform->craft;
        cargo_kind_t expected_kind;
        if (payload->semantics_version != CHAIN_CARGO_SEMANTICS_V1 ||
            payload->input_count > RECIPE_INPUT_MAX ||
            chain_cargo_pub_is_zero(payload->output_pub) ||
            (unsigned)payload->output_grade >=
                (unsigned)MINING_GRADE_COUNT ||
            payload->output_quantity != 1u ||
            !cargo_kind_for_commodity(
                (commodity_t)payload->output_commodity,
                &expected_kind) ||
            expected_kind !=
                (cargo_kind_t)payload->output_kind) {
            return;
        }
        for (size_t i = payload->input_count;
             i < RECIPE_INPUT_MAX; i++) {
            if (!chain_cargo_pub_is_zero(payload->input_pubs[i]))
                return;
        }
        if (payload->recipe_id ==
            (uint16_t)RECIPE_LEGACY_MIGRATE) {
            if (payload->input_count != 0u) return;
        } else {
            const recipe_def_t *recipe =
                recipe_get((recipe_id_t)payload->recipe_id);
            if (!recipe ||
                payload->input_count != recipe->input_count ||
                payload->output_kind !=
                    (uint8_t)recipe->output_kind ||
                payload->output_commodity !=
                    (uint8_t)recipe->output_commodity) {
                return;
            }
            bool canonical_pub = false;
            uint8_t parent_merkle[32] = {0};
            for (uint32_t output_index = 0;
                 output_index < (uint32_t)recipe->output_count;
                 output_index++) {
                uint8_t candidate_parent[32];
                uint8_t candidate_pub[32];
                if (!hash_product_identity_from_pubs(
                        (recipe_id_t)payload->recipe_id,
                        (const uint8_t (*)[32])payload->input_pubs,
                        payload->input_count,
                        (mining_grade_t)payload->output_grade,
                        (uint16_t)output_index,
                        candidate_parent, candidate_pub)) {
                    return;
                }
                if (memcmp(candidate_pub, payload->output_pub,
                           sizeof(candidate_pub)) == 0) {
                    memcpy(parent_merkle, candidate_parent,
                           sizeof(parent_merkle));
                    canonical_pub = true;
                    break;
                }
            }
            if (!canonical_pub) return;
            memcpy(transform->output_cargo.parent_merkle,
                   parent_merkle, sizeof(parent_merkle));
        }
        cargo_unit_t *output = &transform->output_cargo;
        output->kind = payload->output_kind;
        output->commodity = payload->output_commodity;
        output->grade = payload->output_grade;
        output->prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
        output->recipe_id = payload->recipe_id;
        output->quantity = payload->output_quantity;
        output->mined_block = 0u;
        memcpy(output->pub, payload->output_pub, 32);
        transform->output_semantics_version =
            payload->semantics_version;
    }
}

/* ------------------------------------------------------------------ */
/* Configurable on-disk root                                           */
/* ------------------------------------------------------------------ */

static char g_chain_dir[256] = "chain";
static bool g_chain_log_disk_enabled = true;
static uint64_t g_chain_log_configuration_generation = 1;

#define chain_log_flush_durable persistence_flush_durable

static void chain_log_bump_configuration_generation(void) {
    g_chain_log_configuration_generation++;
    if (g_chain_log_configuration_generation == 0)
        g_chain_log_configuration_generation = 1;
}

#if defined(SIGNAL_CHAIN_LOG_TESTING)
typedef struct {
    chain_log_test_fault_point_t point;
    chain_event_type_t event_type;
    uint32_t remaining;
} chain_log_test_fault_state_t;

static chain_log_test_fault_state_t g_chain_log_test_fault;

void chain_log_test_fault_clear(void) {
    memset(&g_chain_log_test_fault, 0, sizeof(g_chain_log_test_fault));
}

void chain_log_test_fault_inject(chain_log_test_fault_point_t point,
                                 chain_event_type_t event_type,
                                 uint32_t occurrence) {
    chain_log_test_fault_clear();
    if (point <= CHAIN_LOG_TEST_FAULT_NONE ||
        point > CHAIN_LOG_TEST_FAULT_PARENT_DIR_SYNC ||
        event_type < CHAIN_EVT_NONE ||
        event_type >= CHAIN_EVT_TYPE_COUNT ||
        occurrence == 0) {
        return;
    }
    g_chain_log_test_fault.point = point;
    g_chain_log_test_fault.event_type = event_type;
    g_chain_log_test_fault.remaining = occurrence;
}

static bool chain_log_test_fault_select(
    const chain_log_batch_event_t *events,
    size_t event_count,
    chain_log_test_fault_point_t *out_point,
    size_t *out_event_index) {
    if (!events || !out_point || !out_event_index ||
        g_chain_log_test_fault.point == CHAIN_LOG_TEST_FAULT_NONE ||
        g_chain_log_test_fault.remaining == 0) {
        return false;
    }
    for (size_t i = 0; i < event_count; i++) {
        if (g_chain_log_test_fault.event_type != CHAIN_EVT_NONE &&
            events[i].type != g_chain_log_test_fault.event_type) {
            continue;
        }
        g_chain_log_test_fault.remaining--;
        if (g_chain_log_test_fault.remaining != 0) continue;
        *out_point = g_chain_log_test_fault.point;
        *out_event_index = i;
        chain_log_test_fault_clear();
        return true;
    }
    return false;
}
#endif

static bool chain_log_rollback_append(const char *path, long length) {
    if (!path || length < 0) return false;
    bool ok = true;
#if defined(_WIN32)
    int fd = _open(path, _O_RDWR | _O_BINARY);
    if (fd < 0) {
        SIM_LOG("[chain] rollback open(%s) failed: %s\n",
                path, strerror(errno));
        return false;
    }
    if (_chsize_s(fd, (long long)length) != 0) {
        SIM_LOG("[chain] rollback truncate(%s) failed: %s\n",
                path, strerror(errno));
        ok = false;
    }
    if (_commit(fd) != 0) {
        SIM_LOG("[chain] rollback sync(%s) failed: %s\n",
                path, strerror(errno));
        ok = false;
    }
    if (_close(fd) != 0) {
        SIM_LOG("[chain] rollback close(%s) failed: %s\n",
                path, strerror(errno));
        ok = false;
    }
#else
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        SIM_LOG("[chain] rollback open(%s) failed: %s\n",
                path, strerror(errno));
        return false;
    }
    if (ftruncate(fd, (off_t)length) != 0) {
        SIM_LOG("[chain] rollback truncate(%s) failed: %s\n",
                path, strerror(errno));
        ok = false;
    }
    if (fsync(fd) != 0) {
        SIM_LOG("[chain] rollback sync(%s) failed: %s\n",
                path, strerror(errno));
        ok = false;
    }
    if (close(fd) != 0) {
        SIM_LOG("[chain] rollback close(%s) failed: %s\n",
                path, strerror(errno));
        ok = false;
    }
#endif
    return ok;
}

/* POSIX exposes directory fsync directly. Win32 has no portable equivalent
 * for directory handles, so _commit() on the file remains the strongest
 * supported flush there and these steps are documented no-ops. */
static bool chain_log_sync_directory(const char *path) {
#if defined(_WIN32)
    (void)path;
    return true;
#else
    if (!path || path[0] == '\0') return false;
    int flags = O_RDONLY;
#  if defined(O_DIRECTORY)
    flags |= O_DIRECTORY;
#  endif
    int fd = open(path, flags);
    if (fd < 0) {
        SIM_LOG("[chain] open directory(%s) failed: %s\n",
                path, strerror(errno));
        return false;
    }
    bool ok = true;
    if (fsync(fd) != 0) {
        SIM_LOG("[chain] sync directory(%s) failed: %s\n",
                path, strerror(errno));
        ok = false;
    }
    if (close(fd) != 0) {
        SIM_LOG("[chain] close directory(%s) failed: %s\n",
                path, strerror(errno));
        ok = false;
    }
    return ok;
#endif
}

static bool chain_log_parent_directory(
    const char *path, char *out, size_t cap) {
    if (!path || !out || cap < 2) return false;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    size_t separator = len;
    while (separator > 0 && path[separator - 1] != '/')
        separator--;
    if (separator == 0) {
        return snprintf(out, cap, ".") > 0;
    }
    if (separator == 1) {
        return snprintf(out, cap, "/") > 0;
    }
    size_t parent_len = separator - 1;
    if (parent_len + 1 > cap) return false;
    memcpy(out, path, parent_len);
    out[parent_len] = '\0';
    return true;
}

static bool chain_log_sync_chain_dir(void) {
    return chain_log_sync_directory(g_chain_dir);
}

static bool chain_log_sync_chain_dir_parent(void) {
    char parent[sizeof(g_chain_dir)];
    if (!chain_log_parent_directory(
            g_chain_dir, parent, sizeof(parent))) {
        return false;
    }
    return chain_log_sync_directory(parent);
}

static bool chain_log_remove_created_dir(void) {
#if defined(_WIN32)
    int removed = _rmdir(g_chain_dir);
#else
    int removed = rmdir(g_chain_dir);
#endif
    if (removed != 0 && errno != ENOENT) {
        SIM_LOG("[chain] rollback rmdir(%s) failed: %s\n",
                g_chain_dir, strerror(errno));
        return false;
    }
    return chain_log_sync_chain_dir_parent();
}

static bool chain_log_rollback_created_file(
    const char *path,
    bool remove_owned_chain_dir,
    bool sync_chain_dir_parent) {
    if (!path) return false;
    bool ok = true;
    if (remove(path) != 0 && errno != ENOENT) {
        SIM_LOG("[chain] rollback remove(%s) failed: %s\n",
                path, strerror(errno));
        return false;
    }
    if (!chain_log_sync_chain_dir()) ok = false;
    if (remove_owned_chain_dir) {
        if (!chain_log_remove_created_dir()) ok = false;
    } else if (sync_chain_dir_parent &&
               !chain_log_sync_chain_dir_parent()) {
        ok = false;
    }
    return ok;
}

void chain_log_set_dir(const char *dir) {
    char next[sizeof(g_chain_dir)];
    if (!dir || dir[0] == '\0') {
        snprintf(next, sizeof(next), "chain");
    } else {
        snprintf(next, sizeof(next), "%s", dir);
    }
    if (strcmp(next, g_chain_dir) != 0) {
        snprintf(g_chain_dir, sizeof(g_chain_dir), "%s", next);
        chain_log_bump_configuration_generation();
    }
}

const char *chain_log_get_dir(void) {
    return g_chain_dir;
}

void chain_log_set_disk_enabled(bool enabled) {
    if (g_chain_log_disk_enabled != enabled)
        chain_log_bump_configuration_generation();
    g_chain_log_disk_enabled = enabled;
}

bool chain_log_disk_enabled(void) {
    return g_chain_log_disk_enabled;
}

uint64_t chain_log_configuration_generation(void) {
    return g_chain_log_configuration_generation;
}

typedef struct {
    bool owned_creation;
    bool needs_parent_sync;
} chain_log_dir_prepare_t;

/* Create the configured leaf chain directory. Its parent must already exist.
 * `owned_creation` is true only when this process's mkdir succeeded, while
 * `needs_parent_sync` is also true for the EEXIST-after-stat race. That race
 * still receives the first-boot durability sync, but rollback never removes
 * a directory created by another process. */
static bool ensure_chain_dir(chain_log_dir_prepare_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    struct stat st;
    if (stat(g_chain_dir, &st) == 0) {
        if (
#if defined(_WIN32)
            (st.st_mode & _S_IFDIR) != 0
#else
            S_ISDIR(st.st_mode)
#endif
        ) {
            return true;
        }
        errno = ENOTDIR;
        SIM_LOG("[chain] configured chain path is not a directory: %s\n",
                g_chain_dir);
        return false;
    }
    if (errno != ENOENT) {
        SIM_LOG("[chain] stat(%s) failed: %s\n",
                g_chain_dir, strerror(errno));
        return false;
    }
    if (MKDIR(g_chain_dir) == 0) {
        out->owned_creation = true;
        out->needs_parent_sync = true;
        return true;
    }
    if (errno == EEXIST && stat(g_chain_dir, &st) == 0) {
#if defined(_WIN32)
        bool is_dir = (st.st_mode & _S_IFDIR) != 0;
#else
        bool is_dir = S_ISDIR(st.st_mode);
#endif
        if (is_dir) {
            /* It appeared after our failed stat. Conservatively sync its
             * parent as a newly-created entry, without claiming ownership. */
            out->needs_parent_sync = true;
            return true;
        }
    }
    SIM_LOG("[chain] mkdir(%s) failed: %s\n",
            g_chain_dir, strerror(errno));
    return false;
}

bool chain_log_path_for(const uint8_t pubkey[32], char *out, size_t cap) {
    if (!pubkey || !out || cap < 64) return false;
    char b58[64];
    size_t n = base58_encode(pubkey, 32, b58, sizeof(b58));
    if (n == 0 || n >= sizeof(b58)) return false;
    int written = snprintf(out, cap, "%s/%s.log", g_chain_dir, b58);
    return written > 0 && (size_t)written < cap;
}

/* ------------------------------------------------------------------ */
/* Header serialization helpers                                        */
/* ------------------------------------------------------------------ */

/* Pack the header into the canonical on-disk byte order. We pack
 * little-endian so the format is portable regardless of host
 * endianness. */
static void chain_event_header_pack(const chain_event_header_t *h,
                                    uint8_t out[CHAIN_EVENT_HEADER_SIZE]) {
    size_t off = 0;
    /* epoch */
    for (int i = 0; i < 8; i++) out[off + i] = (uint8_t)(h->epoch >> (i * 8));
    off += 8;
    /* event_id */
    for (int i = 0; i < 8; i++) out[off + i] = (uint8_t)(h->event_id >> (i * 8));
    off += 8;
    /* type + pad */
    out[off++] = h->type;
    memset(&out[off], 0, 7);
    off += 7;
    /* authority */
    memcpy(&out[off], h->authority, 32); off += 32;
    /* payload_hash */
    memcpy(&out[off], h->payload_hash, 32); off += 32;
    /* prev_hash */
    memcpy(&out[off], h->prev_hash, 32); off += 32;
    /* signature */
    memcpy(&out[off], h->signature, 64); off += 64;
    (void)off;
}

static bool chain_event_header_unpack(const uint8_t in[CHAIN_EVENT_HEADER_SIZE],
                                      chain_event_header_t *out) {
    size_t off = 0;
    out->epoch = 0;
    for (int i = 0; i < 8; i++)
        out->epoch |= (uint64_t)in[off + i] << (i * 8);
    off += 8;
    out->event_id = 0;
    for (int i = 0; i < 8; i++)
        out->event_id |= (uint64_t)in[off + i] << (i * 8);
    off += 8;
    out->type = in[off++];
    /* pad must be zero */
    for (int i = 0; i < 7; i++) {
        if (in[off + i] != 0) return false;
    }
    memset(out->_pad, 0, sizeof(out->_pad));
    off += 7;
    memcpy(out->authority, &in[off], 32); off += 32;
    memcpy(out->payload_hash, &in[off], 32); off += 32;
    memcpy(out->prev_hash, &in[off], 32); off += 32;
    memcpy(out->signature, &in[off], 64); off += 64;
    (void)off;
    return true;
}

void chain_event_header_hash(const chain_event_header_t *h, uint8_t out[32]) {
    uint8_t packed[CHAIN_EVENT_HEADER_SIZE];
    chain_event_header_pack(h, packed);
    sha256_bytes(packed, CHAIN_EVENT_HEADER_SIZE, out);
}

/* Pack just the unsigned-header span (the 120 bytes that get signed). */
static void chain_event_unsigned_pack(const chain_event_header_t *h,
                                      uint8_t out[CHAIN_UNSIGNED_HEADER_SIZE]) {
    uint8_t full[CHAIN_EVENT_HEADER_SIZE];
    chain_event_header_pack(h, full);
    memcpy(out, full, CHAIN_UNSIGNED_HEADER_SIZE);
}

/* ------------------------------------------------------------------ */
/* Emit                                                                */
/* ------------------------------------------------------------------ */

const char *chain_log_health_status_name(chain_health_status_t status) {
    switch (status) {
    case CHAIN_HEALTH_FRESH:    return "fresh";
    case CHAIN_HEALTH_OK:       return "ok";
    case CHAIN_HEALTH_EMPTY:    return "empty";
    case CHAIN_HEALTH_ADOPTED:  return "adopted";
    case CHAIN_HEALTH_MISMATCH: return "mismatch";
    case CHAIN_HEALTH_FAILED:   return "failed";
    case CHAIN_HEALTH_UNKNOWN:
    default:                    return "unknown";
    }
}

const char *chain_log_health_repair_hint(chain_health_status_t status,
                                         bool append_blocked) {
    switch (status) {
    case CHAIN_HEALTH_OK:
        return "No repair needed; verified chain tail matches the saved continuation pointer.";
    case CHAIN_HEALTH_EMPTY:
        return "No repair needed; the station has a verified empty chain.";
    case CHAIN_HEALTH_ADOPTED:
        return "Save the world state soon; the server adopted extra verified disk events after the last save.";
    case CHAIN_HEALTH_FRESH:
        return "No repair needed yet; this station has fresh in-memory chain state.";
    case CHAIN_HEALTH_FAILED:
        return append_blocked
            ? "Preserve the damaged log, run signal_verify on it, then restore a matching save+chain backup or quarantine the bad log before starting a new chain branch."
            : "Run signal_verify on the station log and preserve the diagnostic output.";
    case CHAIN_HEALTH_MISMATCH:
        return append_blocked
            ? "Restore the matching world.sav and chain directory, or back up both and reset/re-anchor them together; do not append from the saved head."
            : "Save and chain tails differ; compare world.sav with the chain directory before allowing appends.";
    case CHAIN_HEALTH_UNKNOWN:
    default:
        return append_blocked
            ? "Chain health is unknown and appends are blocked; verify the station log before repair."
            : "Chain health has not been verified yet; restart or run signal_verify before trusting this station.";
    }
}

void chain_log_health_set(station_t *s, chain_health_status_t status,
                          bool append_blocked,
                          uint64_t verified_event_count,
                          const uint8_t verified_last_hash[32],
                          const char *message) {
    if (!s) return;
    s->chain_health_status = (uint8_t)status;
    s->chain_append_blocked = append_blocked;
    s->chain_append_block_warned = false;
    s->chain_verified_event_count = verified_event_count;
    if (verified_last_hash) {
        memcpy(s->chain_verified_last_hash, verified_last_hash, 32);
    } else {
        memset(s->chain_verified_last_hash, 0, sizeof(s->chain_verified_last_hash));
    }
    if (message && message[0]) {
        snprintf(s->chain_health_message, sizeof(s->chain_health_message),
                 "%s", message);
    } else {
        s->chain_health_message[0] = '\0';
    }
}

const char *chain_log_append_status_name(chain_log_append_status_t status) {
    switch (status) {
    case CHAIN_LOG_APPEND_OK:                return "ok";
    case CHAIN_LOG_APPEND_BAD_ARGUMENTS:     return "bad_arguments";
    case CHAIN_LOG_APPEND_BATCH_TOO_LARGE:   return "batch_too_large";
    case CHAIN_LOG_APPEND_EVENT_ID_OVERFLOW: return "event_id_overflow";
    case CHAIN_LOG_APPEND_UNKEYED:           return "unkeyed";
    case CHAIN_LOG_APPEND_BLOCKED:           return "blocked";
    case CHAIN_LOG_APPEND_SIGNING_FAILED:    return "signing_failed";
    case CHAIN_LOG_APPEND_NO_MEMORY:         return "no_memory";
    case CHAIN_LOG_APPEND_PATH_FAILED:       return "path_failed";
    case CHAIN_LOG_APPEND_OPEN_FAILED:       return "open_failed";
    case CHAIN_LOG_APPEND_SEEK_FAILED:       return "seek_failed";
    case CHAIN_LOG_APPEND_TELL_FAILED:       return "tell_failed";
    case CHAIN_LOG_APPEND_WRITE_FAILED:      return "write_failed";
    case CHAIN_LOG_APPEND_FLUSH_FAILED:      return "flush_failed";
    case CHAIN_LOG_APPEND_CLOSE_FAILED:      return "close_failed";
    case CHAIN_LOG_APPEND_DIR_SYNC_FAILED:   return "dir_sync_failed";
    case CHAIN_LOG_APPEND_ROLLBACK_FAILED:   return "rollback_failed";
    default:                                 return "unknown";
    }
}

static chain_log_append_result_t chain_log_append_result(
    chain_log_append_status_t status) {
    chain_log_append_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = status;
    return result;
}

static void chain_log_mark_durable_failure(
    station_t *s,
    chain_log_append_status_t status,
    uint64_t first_event_id,
    uint64_t last_event_id) {
    if (!s) return;
    s->chain_append_blocked = true;
    s->chain_append_block_warned = false;
    s->chain_health_status = CHAIN_HEALTH_FAILED;
    snprintf(s->chain_health_message, sizeof(s->chain_health_message),
             "durable batch %s for events %llu..%llu",
             chain_log_append_status_name(status),
             (unsigned long long)first_event_id,
             (unsigned long long)last_event_id);
}

chain_log_append_result_t chain_log_emit_batch(
    world_t *w,
    station_t *s,
    const chain_log_batch_event_t *events,
    size_t event_count) {
    static const uint8_t zero_pub[32] = {0};
    static const uint8_t zero_secret[64] = {0};
    chain_log_append_result_t result =
        chain_log_append_result(CHAIN_LOG_APPEND_BAD_ARGUMENTS);

    if (!s || !events || event_count == 0) return result;
    if (event_count > CHAIN_LOG_BATCH_MAX_EVENTS) {
        result.status = CHAIN_LOG_APPEND_BATCH_TOO_LARGE;
        return result;
    }
    if (memcmp(s->station_pubkey, zero_pub, sizeof(zero_pub)) == 0 ||
        memcmp(s->station_secret, zero_secret, sizeof(zero_secret)) == 0) {
        result.status = CHAIN_LOG_APPEND_UNKEYED;
        return result;
    }
    if (s->chain_append_blocked) {
        if (!s->chain_append_block_warned) {
            SIM_LOG("[chain] append blocked for station %s: %s\n",
                    s->name[0] ? s->name : "(unnamed)",
                    s->chain_health_message[0]
                        ? s->chain_health_message
                        : chain_log_health_status_name(
                              (chain_health_status_t)s->chain_health_status));
            s->chain_append_block_warned = true;
        }
        result.status = CHAIN_LOG_APPEND_BLOCKED;
        return result;
    }
    if (s->chain_event_count >
        UINT64_MAX - (uint64_t)event_count) {
        result.status = CHAIN_LOG_APPEND_EVENT_ID_OVERFLOW;
        return result;
    }

    size_t serialized_size = 0;
    for (size_t i = 0; i < event_count; i++) {
        if (events[i].type <= CHAIN_EVT_NONE ||
            events[i].type >= CHAIN_EVT_TYPE_COUNT ||
            (events[i].payload_len > 0 && !events[i].payload)) {
            result.status = CHAIN_LOG_APPEND_BAD_ARGUMENTS;
            return result;
        }
        size_t entry_size = CHAIN_EVENT_HEADER_SIZE +
                            sizeof(events[i].payload_len) +
                            (size_t)events[i].payload_len;
        if (serialized_size > SIZE_MAX - entry_size) {
            result.status = CHAIN_LOG_APPEND_BATCH_TOO_LARGE;
            return result;
        }
        serialized_size += entry_size;
    }

    uint8_t *serialized = malloc(serialized_size);
    size_t *entry_offsets = NULL;
#if defined(SIGNAL_CHAIN_LOG_TESTING)
    entry_offsets =
        malloc((event_count + 1u) * sizeof(*entry_offsets));
#endif
    if (!serialized
#if defined(SIGNAL_CHAIN_LOG_TESTING)
        || !entry_offsets
#endif
    ) {
        free(entry_offsets);
        free(serialized);
        result.status = CHAIN_LOG_APPEND_NO_MEMORY;
        return result;
    }

    uint64_t first_event_id = s->chain_event_count + 1u;
    uint64_t last_event_id =
        s->chain_event_count + (uint64_t)event_count;
    uint64_t epoch_ticks = w ? (uint64_t)(w->time * 120.0) : 0;
    uint8_t staged_last_hash[32];
    memcpy(staged_last_hash, s->chain_last_hash, sizeof(staged_last_hash));

    size_t serialized_offset = 0;
    for (size_t i = 0; i < event_count; i++) {
        chain_event_header_t header;
        memset(&header, 0, sizeof(header));
        header.epoch = epoch_ticks;
        header.event_id = first_event_id + (uint64_t)i;
        header.type = (uint8_t)events[i].type;
        memcpy(header.authority, s->station_pubkey,
               sizeof(header.authority));
        sha256_bytes(events[i].payload_len > 0
                         ? events[i].payload
                         : (const void *)"",
                     events[i].payload_len, header.payload_hash);
        memcpy(header.prev_hash, staged_last_hash,
               sizeof(header.prev_hash));

        uint8_t unsigned_blob[CHAIN_UNSIGNED_HEADER_SIZE];
        chain_event_unsigned_pack(&header, unsigned_blob);
        station_sign(s, unsigned_blob, sizeof(unsigned_blob),
                     header.signature);
        if (!station_verify(s, unsigned_blob, sizeof(unsigned_blob),
                            header.signature)) {
            SIM_LOG("[chain] self-verify failed while staging batch\n");
            free(entry_offsets);
            free(serialized);
            result.status = CHAIN_LOG_APPEND_SIGNING_FAILED;
            return result;
        }

#if defined(SIGNAL_CHAIN_LOG_TESTING)
        entry_offsets[i] = serialized_offset;
#endif
        chain_event_header_pack(
            &header, &serialized[serialized_offset]);
        serialized_offset += CHAIN_EVENT_HEADER_SIZE;
        memcpy(&serialized[serialized_offset], &events[i].payload_len,
               sizeof(events[i].payload_len));
        serialized_offset += sizeof(events[i].payload_len);
        if (events[i].payload_len > 0) {
            memcpy(&serialized[serialized_offset], events[i].payload,
                   events[i].payload_len);
            serialized_offset += events[i].payload_len;
        }
        chain_event_header_hash(&header, staged_last_hash);
    }
#if defined(SIGNAL_CHAIN_LOG_TESTING)
    entry_offsets[event_count] = serialized_offset;
#endif
    assert(serialized_offset == serialized_size);

    if (!g_chain_log_disk_enabled) {
        memcpy(s->chain_last_hash, staged_last_hash,
               sizeof(s->chain_last_hash));
        s->chain_event_count = last_event_id;
        result.status = CHAIN_LOG_APPEND_OK;
        result.event_count = (uint16_t)event_count;
        result.first_event_id = first_event_id;
        result.last_event_id = last_event_id;
        memcpy(result.last_hash, staged_last_hash,
               sizeof(result.last_hash));
        free(entry_offsets);
        free(serialized);
        return result;
    }

    char path[256];
    if (!chain_log_path_for(s->station_pubkey, path, sizeof(path))) {
        free(entry_offsets);
        free(serialized);
        result.status = CHAIN_LOG_APPEND_PATH_FAILED;
        return result;
    }
#if defined(SIGNAL_CHAIN_LOG_TESTING)
    chain_log_test_fault_point_t fault_point =
        CHAIN_LOG_TEST_FAULT_NONE;
    size_t fault_event_index = 0;
    (void)chain_log_test_fault_select(
        events, event_count, &fault_point, &fault_event_index);
#endif

    chain_log_dir_prepare_t prepared_dir;
    if (!ensure_chain_dir(&prepared_dir)) {
        free(entry_offsets);
        free(serialized);
        result.status = CHAIN_LOG_APPEND_OPEN_FAILED;
        return result;
    }
    bool created_file = false;
    FILE *log = fopen(path, "r+b");
    if (!log && errno == ENOENT) {
        log = fopen(path, "w+b");
        created_file = log != NULL;
    }
    if (!log) {
        SIM_LOG("[chain] fopen(%s) failed: %s\n", path, strerror(errno));
        if (prepared_dir.owned_creation &&
            !chain_log_remove_created_dir()) {
            result.status = CHAIN_LOG_APPEND_ROLLBACK_FAILED;
        } else {
            result.status = CHAIN_LOG_APPEND_OPEN_FAILED;
        }
        chain_log_mark_durable_failure(
            s, result.status, first_event_id, last_event_id);
        free(entry_offsets);
        free(serialized);
        return result;
    }
    if (
#if defined(SIGNAL_CHAIN_LOG_TESTING)
        fault_point == CHAIN_LOG_TEST_FAULT_SEEK ||
#endif
        fseek(log, 0, SEEK_END) != 0) {
        SIM_LOG("[chain] seek(%s) failed: %s\n", path, strerror(errno));
        (void)fclose(log);
        bool rollback_ok = !created_file ||
            chain_log_rollback_created_file(
                path, prepared_dir.owned_creation,
                prepared_dir.needs_parent_sync);
        chain_log_append_status_t reported_status = rollback_ok
            ? CHAIN_LOG_APPEND_SEEK_FAILED
            : CHAIN_LOG_APPEND_ROLLBACK_FAILED;
        chain_log_mark_durable_failure(
            s, reported_status, first_event_id, last_event_id);
        free(entry_offsets);
        free(serialized);
        result.status = reported_status;
        return result;
    }
    long append_start =
#if defined(SIGNAL_CHAIN_LOG_TESTING)
        fault_point == CHAIN_LOG_TEST_FAULT_TELL
            ? -1 :
#endif
            ftell(log);
    if (append_start < 0) {
        SIM_LOG("[chain] tell(%s) failed: %s\n", path, strerror(errno));
        (void)fclose(log);
        bool rollback_ok = !created_file ||
            chain_log_rollback_created_file(
                path, prepared_dir.owned_creation,
                prepared_dir.needs_parent_sync);
        chain_log_append_status_t reported_status = rollback_ok
            ? CHAIN_LOG_APPEND_TELL_FAILED
            : CHAIN_LOG_APPEND_ROLLBACK_FAILED;
        chain_log_mark_durable_failure(
            s, reported_status, first_event_id, last_event_id);
        free(entry_offsets);
        free(serialized);
        result.status = reported_status;
        return result;
    }

    chain_log_append_status_t io_status = CHAIN_LOG_APPEND_OK;
    int failure_errno = 0;
#if defined(SIGNAL_CHAIN_LOG_TESTING)
    if (fault_point == CHAIN_LOG_TEST_FAULT_WRITE) {
        size_t prefix_len = entry_offsets[fault_event_index];
        if (prefix_len > 0 &&
            fwrite(serialized, 1, prefix_len, log) != prefix_len) {
            failure_errno = errno;
        } else {
            failure_errno = ENOSPC;
        }
        io_status = CHAIN_LOG_APPEND_WRITE_FAILED;
    } else
#endif
    if (fwrite(serialized, 1, serialized_size, log) !=
               serialized_size) {
        failure_errno = errno;
        io_status = CHAIN_LOG_APPEND_WRITE_FAILED;
    }

    if (io_status == CHAIN_LOG_APPEND_OK) {
#if defined(SIGNAL_CHAIN_LOG_TESTING)
        if (fault_point == CHAIN_LOG_TEST_FAULT_FLUSH) {
            if (fflush(log) != 0) {
                failure_errno = errno;
            } else {
                failure_errno = EIO;
            }
            io_status = CHAIN_LOG_APPEND_FLUSH_FAILED;
        } else
#endif
        if (!chain_log_flush_durable(log)) {
            failure_errno = errno;
            io_status = CHAIN_LOG_APPEND_FLUSH_FAILED;
        }
    }

    int close_result = fclose(log);
    if (io_status == CHAIN_LOG_APPEND_OK &&
        (close_result != 0
#if defined(SIGNAL_CHAIN_LOG_TESTING)
         ||
         fault_point == CHAIN_LOG_TEST_FAULT_CLOSE)) {
#else
        )) {
#endif
        failure_errno = close_result != 0 ? errno : EIO;
        io_status = CHAIN_LOG_APPEND_CLOSE_FAILED;
    }

    if (io_status == CHAIN_LOG_APPEND_OK && created_file) {
#if defined(SIGNAL_CHAIN_LOG_TESTING)
        if (fault_point == CHAIN_LOG_TEST_FAULT_DIR_SYNC) {
            failure_errno = EIO;
            io_status = CHAIN_LOG_APPEND_DIR_SYNC_FAILED;
        } else
#endif
        if (!chain_log_sync_chain_dir()) {
            failure_errno = errno;
            io_status = CHAIN_LOG_APPEND_DIR_SYNC_FAILED;
        }
    }
    if (io_status == CHAIN_LOG_APPEND_OK &&
        prepared_dir.needs_parent_sync) {
#if defined(SIGNAL_CHAIN_LOG_TESTING)
        if (fault_point ==
            CHAIN_LOG_TEST_FAULT_PARENT_DIR_SYNC) {
            failure_errno = EIO;
            io_status = CHAIN_LOG_APPEND_DIR_SYNC_FAILED;
        } else
#endif
        if (!chain_log_sync_chain_dir_parent()) {
            failure_errno = errno;
            io_status = CHAIN_LOG_APPEND_DIR_SYNC_FAILED;
        }
    }

    if (io_status != CHAIN_LOG_APPEND_OK) {
        if (failure_errno == 0) failure_errno = EIO;
        bool rollback_ok = created_file
            ? chain_log_rollback_created_file(
                  path, prepared_dir.owned_creation,
                  prepared_dir.needs_parent_sync)
            : chain_log_rollback_append(path, append_start);
        chain_log_append_status_t reported_status = rollback_ok
            ? io_status : CHAIN_LOG_APPEND_ROLLBACK_FAILED;
        chain_log_mark_durable_failure(
            s, reported_status, first_event_id, last_event_id);
        SIM_LOG("[chain] durable batch append failed for %s: %s (%s); "
                "appends blocked\n",
                path, strerror(failure_errno),
                chain_log_append_status_name(reported_status));
        free(entry_offsets);
        free(serialized);
        result.status = reported_status;
        return result;
    }

    memcpy(s->chain_last_hash, staged_last_hash,
           sizeof(s->chain_last_hash));
    s->chain_event_count = last_event_id;
    result.status = CHAIN_LOG_APPEND_OK;
    result.event_count = (uint16_t)event_count;
    result.first_event_id = first_event_id;
    result.last_event_id = last_event_id;
    memcpy(result.last_hash, staged_last_hash,
           sizeof(result.last_hash));
    free(entry_offsets);
    free(serialized);
    return result;
}

uint64_t chain_log_emit(world_t *w, station_t *s, chain_event_type_t type,
                        const void *payload, uint16_t payload_len) {
    const chain_log_batch_event_t event = {
        .type = type,
        .payload = payload,
        .payload_len = payload_len,
    };
    chain_log_append_result_t result =
        chain_log_emit_batch(w, s, &event, 1);
    return result.status == CHAIN_LOG_APPEND_OK
        ? result.last_event_id : 0;
}

/* ------------------------------------------------------------------ */
/* Verify                                                              */
/* ------------------------------------------------------------------ */

/* The core post-mortem verifier (chain_log_verify_with_pubkey) is
 * defined in server/chain_log_verify.c so the standalone signal_verify
 * tool can link it without pulling in the world_t / SIM_LOG / station
 * authority dependencies that chain_log_emit needs. */

bool chain_log_verify_identity(
    const uint8_t station_pubkey[32],
    uint64_t *out_event_count,
    uint8_t out_last_hash[32],
    chain_log_verify_report_t *out_report) {
    static const uint8_t zero_pub[32] = {0};
    if (out_event_count) *out_event_count = 0;
    if (out_last_hash) memset(out_last_hash, 0, 32);
    if (out_report) memset(out_report, 0, sizeof(*out_report));
    if (!station_pubkey ||
        memcmp(station_pubkey, zero_pub, 32) == 0) return false;
    char path[256];
    if (!chain_log_path_for(station_pubkey, path, sizeof(path))) return false;
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* No log on disk = no events authored = trivially valid. */
        return true;
    }

    chain_log_verify_report_t report;
    bool ok = chain_log_verify_with_pubkey(f, station_pubkey, &report);
    if (out_report) memcpy(out_report, &report, sizeof(report));

    /* Recompute last_hash by re-reading just the last valid header,
     * preserving the original out_last_hash semantics (caller may
     * compare against in-memory chain_last_hash). */
    if (report.valid_events > 0 && out_last_hash) {
        rewind(f);
        uint8_t prev_hash[32] = {0};
        for (uint64_t i = 0; i < report.valid_events; i++) {
            uint8_t hdr_bytes[CHAIN_EVENT_HEADER_SIZE];
            if (fread(hdr_bytes, 1, CHAIN_EVENT_HEADER_SIZE, f) != CHAIN_EVENT_HEADER_SIZE) break;
            chain_event_header_t hdr;
            if (!chain_event_header_unpack(hdr_bytes, &hdr)) break;
            uint16_t plen = 0;
            if (fread(&plen, sizeof(plen), 1, f) != 1) break;
            if (plen > 0) fseek(f, plen, SEEK_CUR);
            chain_event_header_hash(&hdr, prev_hash);
        }
        memcpy(out_last_hash, prev_hash, 32);
    }

    fclose(f);
    if (out_event_count) *out_event_count = report.tail_event_id;
    return ok;
}

bool chain_log_verify_station(const station_t *s,
                              uint64_t *out_event_count,
                              uint8_t out_last_hash[32],
                              chain_log_verify_report_t *out_report) {
    static const uint8_t zero_pub[32] = {0};
    if (out_event_count) *out_event_count = 0;
    if (out_last_hash) memset(out_last_hash, 0, 32);
    if (out_report) memset(out_report, 0, sizeof(*out_report));
    if (!s) return false;
    if (memcmp(s->station_pubkey, zero_pub, 32) == 0) {
        /* Unkeyed station — log is trivially empty. */
        return true;
    }
    return chain_log_verify_identity(s->station_pubkey,
                                     out_event_count,
                                     out_last_hash,
                                     out_report);
}

bool chain_log_verify(const station_t *s,
                      uint64_t *out_event_count,
                      uint8_t out_last_hash[32]) {
    return chain_log_verify_station(s, out_event_count, out_last_hash, NULL);
}

int chain_log_read_route_history_tail(const station_t *s,
                                      chain_route_history_tail_t *out,
                                      int cap) {
    if (!s || !out || cap <= 0) return 0;
    enum { ROUTE_HISTORY_TAIL_MAX = 64 };
    if (cap > ROUTE_HISTORY_TAIL_MAX) cap = ROUTE_HISTORY_TAIL_MAX;
    char path[256];
    if (!chain_log_path_for(s->station_pubkey, path, sizeof(path))) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    int count = 0;
    for (;;) {
        uint8_t hdr_bytes[CHAIN_EVENT_HEADER_SIZE];
        size_t got = fread(hdr_bytes, 1, sizeof(hdr_bytes), f);
        if (got == 0 && feof(f)) break;
        if (got != sizeof(hdr_bytes)) break;

        chain_event_header_t hdr;
        if (!chain_event_header_unpack(hdr_bytes, &hdr)) break;

        uint16_t plen = 0;
        if (fread(&plen, sizeof(plen), 1, f) != 1) break;

        if (hdr.type == CHAIN_EVT_ROUTE_HISTORY &&
            plen == sizeof(chain_payload_route_history_t)) {
            chain_route_history_tail_t item;
            memset(&item, 0, sizeof(item));
            item.event_id = hdr.event_id;
            item.epoch = hdr.epoch;
            if (fread(&item.payload, 1, sizeof(item.payload), f) !=
                sizeof(item.payload)) {
                break;
            }
            out[count % cap] = item;
            count++;
        } else {
            if (fseek(f, plen, SEEK_CUR) != 0) break;
        }
    }

    fclose(f);
    int n = count < cap ? count : cap;
    if (count > cap) {
        chain_route_history_tail_t ordered[ROUTE_HISTORY_TAIL_MAX] = {0};
        int start = count % cap;
        for (int i = 0; i < cap; i++)
            ordered[i] = out[(start + i) % cap];
        memcpy(out, ordered, (size_t)cap * sizeof(out[0]));
        n = cap;
    }
    return n;
}

chain_cargo_transform_find_status_t
chain_log_find_cargo_transform_for_identity_pinned(
    const uint8_t station_pubkey[32],
    const uint8_t cargo_pub[32],
    const uint8_t event_hash_pin[32],
    chain_cargo_transform_t *out) {
    static const uint8_t zero_hash[32] = {0};
    if (!station_pubkey || !cargo_pub || !out)
        return CHAIN_CARGO_TRANSFORM_READ_INVALID;
    memset(out, 0, sizeof(*out));
    bool pinned = event_hash_pin &&
        memcmp(event_hash_pin, zero_hash, sizeof(zero_hash)) != 0;

    char path[256];
    if (!chain_log_path_for(station_pubkey, path, sizeof(path)))
        return CHAIN_CARGO_TRANSFORM_READ_INVALID;
    FILE *f = fopen(path, "rb");
    if (!f) return CHAIN_CARGO_TRANSFORM_NOT_FOUND;

    bool found = false;
    bool invalid = false;
    for (;;) {
        uint8_t hdr_bytes[CHAIN_EVENT_HEADER_SIZE];
        size_t got = fread(hdr_bytes, 1, sizeof(hdr_bytes), f);
        if (got == 0 && feof(f)) break;
        if (got != sizeof(hdr_bytes)) {
            invalid = true;
            break;
        }

        chain_event_header_t hdr;
        if (!chain_event_header_unpack(hdr_bytes, &hdr)) {
            invalid = true;
            break;
        }

        uint16_t plen = 0;
        if (fread(&plen, sizeof(plen), 1, f) != 1) {
            invalid = true;
            break;
        }

        if (hdr.type == CHAIN_EVT_SMELT &&
            plen == sizeof(chain_payload_smelt_t)) {
            chain_payload_smelt_t payload;
            if (fread(&payload, 1, sizeof(payload), f) != sizeof(payload)) {
                invalid = true;
                break;
            }
            if (memcmp(payload.ingot_pub, cargo_pub, 32) == 0) {
                uint8_t header_hash[32];
                chain_event_header_hash(&hdr, header_hash);
                if (pinned &&
                    memcmp(header_hash, event_hash_pin, 32) != 0) {
                    continue;
                }
                if (found) {
                    memset(out, 0, sizeof(*out));
                    fclose(f);
                    return CHAIN_CARGO_TRANSFORM_AMBIGUOUS;
                }
                memset(out, 0, sizeof(*out));
                out->type = CHAIN_EVT_SMELT;
                out->event_id = hdr.event_id;
                out->epoch = hdr.epoch;
                memcpy(out->header_hash, header_hash, 32);
                memcpy(out->authority, hdr.authority,
                       sizeof(out->authority));
                out->smelt = payload;
                chain_cargo_transform_decode_output(out);
                found = true;
            }
        } else if (hdr.type == CHAIN_EVT_CRAFT &&
                   plen == sizeof(chain_payload_craft_t)) {
            chain_payload_craft_t payload;
            if (fread(&payload, 1, sizeof(payload), f) != sizeof(payload)) {
                invalid = true;
                break;
            }
            if (memcmp(payload.output_pub, cargo_pub, 32) == 0) {
                uint8_t header_hash[32];
                chain_event_header_hash(&hdr, header_hash);
                if (pinned &&
                    memcmp(header_hash, event_hash_pin, 32) != 0) {
                    continue;
                }
                if (found) {
                    memset(out, 0, sizeof(*out));
                    fclose(f);
                    return CHAIN_CARGO_TRANSFORM_AMBIGUOUS;
                }
                memset(out, 0, sizeof(*out));
                out->type = CHAIN_EVT_CRAFT;
                out->event_id = hdr.event_id;
                out->epoch = hdr.epoch;
                memcpy(out->header_hash, header_hash, 32);
                memcpy(out->authority, hdr.authority,
                       sizeof(out->authority));
                out->craft = payload;
                chain_cargo_transform_decode_output(out);
                found = true;
            }
        } else if (fseek(f, plen, SEEK_CUR) != 0) {
            invalid = true;
            break;
        }
    }

    if (fclose(f) != 0) invalid = true;
    if (invalid) {
        memset(out, 0, sizeof(*out));
        return CHAIN_CARGO_TRANSFORM_READ_INVALID;
    }
    return found ? CHAIN_CARGO_TRANSFORM_FOUND
                 : CHAIN_CARGO_TRANSFORM_NOT_FOUND;
}

static bool chain_log_visit_cargo_transforms_open(
    FILE *f,
    bool bounded,
    uint64_t event_limit,
    chain_cargo_transform_visitor_t visitor,
    void *user,
    size_t *out_transform_count,
    uint8_t out_last_hash[32]) {
    if (out_transform_count) *out_transform_count = 0;
    if (out_last_hash) memset(out_last_hash, 0, 32);
    if (!f || !visitor || fseek(f, 0, SEEK_SET) != 0)
        return false;

    size_t count = 0;
    uint64_t visited_events = 0;
    uint8_t last_hash[32] = {0};
    bool ok = true;
    for (;;) {
        if (bounded && visited_events == event_limit) break;
        uint8_t hdr_bytes[CHAIN_EVENT_HEADER_SIZE];
        size_t got = fread(hdr_bytes, 1, sizeof(hdr_bytes), f);
        if (got == 0 && feof(f)) break;
        if (got != sizeof(hdr_bytes)) {
            ok = false;
            break;
        }
        chain_event_header_t hdr;
        if (!chain_event_header_unpack(hdr_bytes, &hdr)) {
            ok = false;
            break;
        }
        uint16_t plen = 0;
        if (fread(&plen, sizeof(plen), 1, f) != 1) {
            ok = false;
            break;
        }

        chain_cargo_transform_t transform = {0};
        bool is_transform = false;
        if (hdr.type == CHAIN_EVT_SMELT &&
            plen == sizeof(chain_payload_smelt_t)) {
            if (fread(&transform.smelt, 1,
                      sizeof(transform.smelt), f) !=
                sizeof(transform.smelt)) {
                ok = false;
                break;
            }
            transform.type = CHAIN_EVT_SMELT;
            is_transform = true;
        } else if (hdr.type == CHAIN_EVT_CRAFT &&
                   plen == sizeof(chain_payload_craft_t)) {
            if (fread(&transform.craft, 1,
                      sizeof(transform.craft), f) !=
                sizeof(transform.craft)) {
                ok = false;
                break;
            }
            transform.type = CHAIN_EVT_CRAFT;
            is_transform = true;
        } else if (fseek(f, plen, SEEK_CUR) != 0) {
            ok = false;
            break;
        }
        visited_events++;
        chain_event_header_hash(&hdr, last_hash);
        if (!is_transform) continue;
        transform.event_id = hdr.event_id;
        transform.epoch = hdr.epoch;
        chain_event_header_hash(&hdr, transform.header_hash);
        memcpy(transform.authority, hdr.authority,
               sizeof(transform.authority));
        chain_cargo_transform_decode_output(&transform);
        if (count == SIZE_MAX) {
            ok = false;
            break;
        }
        count++;
        if (!visitor(&transform, user)) {
            ok = false;
            break;
        }
    }
    if (bounded && visited_events != event_limit) ok = false;
    if (out_transform_count) *out_transform_count = count;
    if (ok && out_last_hash)
        memcpy(out_last_hash, last_hash, sizeof(last_hash));
    return ok;
}

bool chain_log_visit_cargo_transforms_from_verified_file(
    FILE *log,
    uint64_t verified_event_count,
    chain_cargo_transform_visitor_t visitor,
    void *user,
    size_t *out_transform_count,
    uint8_t out_last_hash[32]) {
    return chain_log_visit_cargo_transforms_open(
        log, true, verified_event_count, visitor, user,
        out_transform_count, out_last_hash);
}

bool chain_log_visit_cargo_transforms_for_identity(
    const uint8_t station_pubkey[32],
    chain_cargo_transform_visitor_t visitor,
    void *user,
    size_t *out_transform_count) {
    if (out_transform_count) *out_transform_count = 0;
    if (!station_pubkey || !visitor) return false;
    char path[256];
    if (!chain_log_path_for(station_pubkey, path, sizeof(path)))
        return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = chain_log_visit_cargo_transforms_open(
        f, false, 0, visitor, user, out_transform_count, NULL);
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool chain_log_find_cargo_transform_for_identity(
    const uint8_t station_pubkey[32],
    const uint8_t cargo_pub[32],
    chain_cargo_transform_t *out) {
    return chain_log_find_cargo_transform_for_identity_pinned(
        station_pubkey, cargo_pub, NULL, out) ==
        CHAIN_CARGO_TRANSFORM_FOUND;
}

bool chain_log_find_cargo_transform(const station_t *s,
                                    const uint8_t cargo_pub[32],
                                    chain_cargo_transform_t *out) {
    if (!s) {
        if (out) memset(out, 0, sizeof(*out));
        return false;
    }
    return chain_log_find_cargo_transform_for_identity(
        s->station_pubkey, cargo_pub, out);
}

void chain_log_reset(const station_t *s) {
    static const uint8_t zero_pub[32] = {0};
    if (!s) return;
    if (memcmp(s->station_pubkey, zero_pub, 32) == 0) return;
    char path[256];
    if (!chain_log_path_for(s->station_pubkey, path, sizeof(path))) return;
    if (remove(path) == 0) (void)chain_log_sync_chain_dir();
    chain_log_bump_configuration_generation();
}
