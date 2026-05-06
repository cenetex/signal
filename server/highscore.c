#include "highscore.h"

#include "base58.h"
#include "chain_log.h"
#include "sha256.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t cs_len(const char *s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#endif

static void write_u32_le(uint8_t *p, uint32_t u) {
    p[0] = (uint8_t)(u);
    p[1] = (uint8_t)(u >> 8);
    p[2] = (uint8_t)(u >> 16);
    p[3] = (uint8_t)(u >> 24);
}

static void write_u64_le(uint8_t *p, uint64_t u) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(u >> (i * 8));
}

static void write_f32_le(uint8_t *p, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    write_u32_le(p, u);
}

bool highscore_submit(highscore_table_t *t,
                      const char *callsign, float credits_earned,
                      uint32_t world_id, uint32_t world_seq,
                      uint32_t build_id,
                      uint64_t epoch_tick, const uint8_t killed_by[8])
{
    if (!t) return false;
    if (!isfinite(credits_earned) || credits_earned < 0.0f) return false;

    /* Dedup by callsign only. Newer world (greater world_seq) replaces
     * any prior entry regardless of credits; same-seq runs promote only
     * on a strictly higher score; older worlds never displace newer. */
    if (callsign && callsign[0]) {
        size_t cn = cs_len(callsign, sizeof(t->entries[0].callsign));
        for (int i = 0; i < t->count; i++) {
            size_t en = cs_len(t->entries[i].callsign,
                                sizeof(t->entries[i].callsign));
            if (en != cn || memcmp(t->entries[i].callsign, callsign, cn) != 0)
                continue;
            uint32_t cur_seq = t->entries[i].world_seq;
            if (world_seq < cur_seq) return false;
            if (world_seq == cur_seq &&
                credits_earned <= t->entries[i].credits_earned) return false;
            for (int j = i; j < t->count - 1; j++) t->entries[j] = t->entries[j + 1];
            t->count--;
            memset(&t->entries[t->count], 0, sizeof(t->entries[t->count]));
            break;
        }
    }

    int ins = t->count;
    for (int i = 0; i < t->count; i++) {
        if (credits_earned > t->entries[i].credits_earned) { ins = i; break; }
    }
    if (ins >= HIGHSCORE_TOP_N) return false;

    int end = (t->count < HIGHSCORE_TOP_N) ? t->count : HIGHSCORE_TOP_N - 1;
    for (int i = end; i > ins; i--) t->entries[i] = t->entries[i - 1];

    highscore_entry_t *e = &t->entries[ins];
    memset(e, 0, sizeof(*e));
    if (callsign) {
        size_t n = cs_len(callsign, sizeof(e->callsign));
        memcpy(e->callsign, callsign, n);
    }
    e->credits_earned = credits_earned;
    e->world_id = world_id;
    e->world_seq = world_seq;
    e->build_id = build_id;
    e->epoch_tick = epoch_tick;
    if (killed_by) memcpy(e->killed_by, killed_by, 8);

    if (t->count < HIGHSCORE_TOP_N) t->count++;
    return true;
}

int highscore_serialize(uint8_t *buf, const highscore_table_t *t) {
    buf[0] = NET_MSG_HIGHSCORES;
    buf[1] = (uint8_t)t->count;
    for (int i = 0; i < t->count; i++) {
        uint8_t *p = &buf[HIGHSCORE_HEADER + i * HIGHSCORE_ENTRY_SIZE];
        memcpy(p, t->entries[i].callsign, 8);
        write_f32_le(&p[8], t->entries[i].credits_earned);
        write_u32_le(&p[12], t->entries[i].world_id);
        write_u32_le(&p[16], t->entries[i].world_seq);
        write_u32_le(&p[20], t->entries[i].build_id);
        write_u64_le(&p[24], t->entries[i].epoch_tick);
        memcpy(&p[32], t->entries[i].killed_by, 8);
    }
    return HIGHSCORE_HEADER + t->count * HIGHSCORE_ENTRY_SIZE;
}

/* ------------------------------------------------------------------ */
/* Replay                                                              */
/* ------------------------------------------------------------------ */

#define CHAIN_HDR_SIZE 184

/* Walk events out of a chain log without verifying signatures; the
 * verifier ran at append time and we trust the on-disk record. Calls
 * `cb(type, payload, payload_len, epoch_tick, user)` for each event;
 * cb may be NULL to skip events of a kind. Returns count parsed. */
typedef void (*chain_walk_cb)(uint8_t type, const uint8_t *payload,
                              uint16_t payload_len, uint64_t epoch_tick,
                              void *user);

static int chain_log_walk_events(FILE *f, chain_walk_cb cb, void *user) {
    int count = 0;
    for (;;) {
        uint8_t hdr[CHAIN_HDR_SIZE];
        size_t got = fread(hdr, 1, CHAIN_HDR_SIZE, f);
        if (got != CHAIN_HDR_SIZE) break;
        /* epoch (8 LE) | event_id (8) | type (1) | pad (7) | ... */
        uint64_t epoch = 0;
        for (int i = 0; i < 8; i++) epoch |= (uint64_t)hdr[i] << (i * 8);
        uint8_t type = hdr[16];
        uint16_t payload_len = 0;
        if (fread(&payload_len, sizeof(payload_len), 1, f) != 1) break;
        uint8_t buf[4096];
        if (payload_len > sizeof(buf)) {
            /* implausible; bail out so we don't desync. */
            break;
        }
        if (payload_len > 0 && fread(buf, payload_len, 1, f) != 1) break;
        if (cb) cb(type, payload_len ? buf : NULL, payload_len, epoch, user);
        count++;
    }
    return count;
}

/* ------------- token -> victim-callsign fallback map ------------- */

typedef struct {
    uint8_t token[8];
    char    callsign[8];
} token_callsign_t;

typedef struct {
    token_callsign_t *entries;
    int count;
    int cap;
} token_map_t;

static void token_map_set(token_map_t *m, const uint8_t token[8],
                          const uint8_t callsign[8]) {
    static const uint8_t zero[8] = {0};
    if (memcmp(token, zero, 8) == 0) return;
    /* First-write-wins: stable mapping if a token gets reused. */
    for (int i = 0; i < m->count; i++) {
        if (memcmp(m->entries[i].token, token, 8) == 0) return;
    }
    if (m->count >= m->cap) {
        int new_cap = m->cap ? m->cap * 2 : 32;
        token_callsign_t *p = (token_callsign_t *)realloc(m->entries,
                                                          (size_t)new_cap * sizeof(*p));
        if (!p) return;
        m->entries = p;
        m->cap = new_cap;
    }
    memcpy(m->entries[m->count].token, token, 8);
    memcpy(m->entries[m->count].callsign, callsign, 8);
    m->count++;
}

static bool token_map_get(const token_map_t *m, const uint8_t token[8],
                          uint8_t out_callsign[8]) {
    static const uint8_t zero[8] = {0};
    if (memcmp(token, zero, 8) == 0) return false;
    for (int i = 0; i < m->count; i++) {
        if (memcmp(m->entries[i].token, token, 8) == 0) {
            memcpy(out_callsign, m->entries[i].callsign, 8);
            return true;
        }
    }
    return false;
}

static void map_collect_cb(uint8_t type, const uint8_t *payload,
                           uint16_t payload_len, uint64_t epoch_tick,
                           void *user) {
    (void)epoch_tick;
    if (type != CHAIN_EVT_DEATH) return;
    /* Legacy 88-byte payloads predate killed_by_callsign; victim_*
     * fields sit before that offset and are valid in both layouts. */
    if (payload_len < offsetof(chain_payload_death_t, killed_by_callsign) || !payload) return;
    const chain_payload_death_t *d = (const chain_payload_death_t *)payload;
    token_map_set((token_map_t *)user, d->victim_session_token, d->victim_callsign);
}

/* ------------- Project death events ------------- */

typedef struct {
    highscore_table_t *table;
    const token_map_t *map;
    /* World identity cursor: last WORLD_INFO operator post seen in this
     * file determines the world_id / world_seq assigned to subsequent
     * deaths. If no WORLD_INFO is seen before a DEATH,
     * fallback_world_id (derived from the filename pubkey) is used and
     * world_seq stays 0 (oldest possible). */
    uint32_t cursor_world_id;
    uint32_t cursor_world_seq;
    uint32_t cursor_build_id;
    bool     world_info_seen;
    uint32_t fallback_world_id;
} replay_ctx_t;

static void replay_event_cb(uint8_t type, const uint8_t *payload,
                            uint16_t payload_len, uint64_t epoch_tick,
                            void *user) {
    replay_ctx_t *r = (replay_ctx_t *)user;
    if (type == CHAIN_EVT_OPERATOR_POST) {
        /* Fixed prefix: kind(1) tier(1) ref_id(2) sha256(32) text_len(2) = 38 */
        if (payload_len < 38 || !payload) return;
        uint8_t kind = payload[0];
        uint16_t text_len = (uint16_t)(payload[36] | ((uint16_t)payload[37] << 8));
        if (38 + text_len > payload_len) return;
        const uint8_t *text = &payload[38];
        if (kind == 3) {
            /* BUILD_INFO: text is hex-ish build id; truncate to first 8
             * hex chars and parse as u32 (loose: ignore non-hex). */
            uint32_t bid = 0;
            int hex_count = 0;
            for (int i = 0; i < text_len && hex_count < 8; i++) {
                char c = (char)text[i];
                int v = -1;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (c >= 'a' && c <= 'f') v = 10 + (c - 'a');
                else if (c >= 'A' && c <= 'F') v = 10 + (c - 'A');
                if (v < 0) continue;
                bid = (bid << 4) | (uint32_t)v;
                hex_count++;
            }
            r->cursor_build_id = bid;
        } else if (kind == 4) {
            /* WORLD_INFO. Modern emit: belt_seed:u32 LE || world_seq:u32 LE
             * || build hex. Pre-v52 emit: belt_seed:u32 LE || build hex
             * (text_len < 8 means world_seq absent → default 0). */
            if (text_len >= 4) {
                r->cursor_world_id = (uint32_t)text[0]
                    | ((uint32_t)text[1] << 8)
                    | ((uint32_t)text[2] << 16)
                    | ((uint32_t)text[3] << 24);
                r->world_info_seen = true;
                int hex_start = 4;
                if (text_len >= 8) {
                    r->cursor_world_seq = (uint32_t)text[4]
                        | ((uint32_t)text[5] << 8)
                        | ((uint32_t)text[6] << 16)
                        | ((uint32_t)text[7] << 24);
                    hex_start = 8;
                } else {
                    r->cursor_world_seq = 0;
                }
                /* Optional trailing build hex. */
                uint32_t bid = 0;
                int hex_count = 0;
                for (int i = hex_start; i < text_len && hex_count < 8; i++) {
                    char c = (char)text[i];
                    int v = -1;
                    if (c >= '0' && c <= '9') v = c - '0';
                    else if (c >= 'a' && c <= 'f') v = 10 + (c - 'a');
                    else if (c >= 'A' && c <= 'F') v = 10 + (c - 'A');
                    if (v < 0) continue;
                    bid = (bid << 4) | (uint32_t)v;
                    hex_count++;
                }
                if (hex_count > 0) r->cursor_build_id = bid;
            }
        }
        return;
    }
    if (type != CHAIN_EVT_DEATH) return;
    /* Legacy 88-byte payloads predate killed_by_callsign. Accept them
     * down to the offset of that field; everything before it is
     * binary-compatible with the widened layout. */
    if (payload_len < offsetof(chain_payload_death_t, killed_by_callsign) || !payload) return;

    const chain_payload_death_t *d = (const chain_payload_death_t *)payload;
    char callsign[9] = {0};
    memcpy(callsign, d->victim_callsign, 8);
    callsign[8] = '\0';

    uint8_t killed_by[8] = {0};
    static const uint8_t zero8[8] = {0};
    if (payload_len >= sizeof(chain_payload_death_t)) {
        memcpy(killed_by, d->killed_by_callsign, 8);
    }
    if (memcmp(killed_by, zero8, 8) == 0) {
        (void)token_map_get(r->map, d->killer_token, killed_by);
    }

    uint32_t world_id = r->world_info_seen ? r->cursor_world_id : r->fallback_world_id;
    uint32_t world_seq = r->world_info_seen ? r->cursor_world_seq : 0u;
    uint32_t build_id = r->cursor_build_id;

    (void)highscore_submit(r->table, callsign, d->credits_earned,
                           world_id, world_seq, build_id,
                           epoch_tick != 0 ? epoch_tick : d->epoch_tick,
                           killed_by);
}

/* Produce a stable u32 fallback from a base58 filename (orphan logs
 * with no WORLD_INFO event). Hash the bytes so different stations from
 * the same world remap to the same world_id collision rate as random;
 * acceptable — the prompt explicitly says these are tagged with a
 * synthesized id derived from the station pubkey. */
static uint32_t fallback_world_id_from_name(const char *name) {
    uint8_t digest[32];
    sha256_bytes((const uint8_t *)name, strlen(name), digest);
    return (uint32_t)digest[0]
         | ((uint32_t)digest[1] << 8)
         | ((uint32_t)digest[2] << 16)
         | ((uint32_t)digest[3] << 24);
}

/* Iterate every regular file under chain_dir whose name ends in ".log"
 * and call cb(path, name_without_ext, user). */
typedef void (*log_dir_cb)(const char *full_path, const char *base_name, void *user);

static void iterate_log_dir(const char *chain_dir, log_dir_cb cb, void *user) {
    if (!chain_dir || !chain_dir[0]) return;
#if defined(_WIN32)
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s\\*.log", chain_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char full[512];
        snprintf(full, sizeof(full), "%s\\%s", chain_dir, fd.cFileName);
        char base[256];
        snprintf(base, sizeof(base), "%s", fd.cFileName);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        cb(full, base, user);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dir = opendir(chain_dir);
    if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        const char *n = de->d_name;
        size_t nl = strlen(n);
        if (nl < 5) continue;
        if (strcmp(n + nl - 4, ".log") != 0) continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", chain_dir, n);
        char base[256];
        snprintf(base, sizeof(base), "%.*s", (int)(nl - 4), n);
        cb(full, base, user);
    }
    closedir(dir);
#endif
}

/* Verify the on-disk chain log via the pubkey decoded from its
 * filename (signature + linkage + payload-hash + monotonic). Replay
 * sources its data from these logs, so an unverified file must never
 * become canonical leaderboard input — a corrupted active log or a
 * forged orphan is exactly the threat the chain log was designed
 * against. Returns true only if the filename decodes to a 32-byte
 * pubkey and the full log walks cleanly under that pubkey. */
static bool verify_chain_log_at(const char *full_path, const char *base_name) {
    uint8_t pubkey[32];
    if (base58_decode(base_name, pubkey, 32) != 32) return false;
    FILE *f = fopen(full_path, "rb");
    if (!f) return false;
    chain_log_verify_report_t report;
    bool ok = chain_log_verify_with_pubkey(f, pubkey, &report);
    fclose(f);
    return ok;
}

/* Map-build pass: open file and walk for victim-token/callsign collection. */
typedef struct {
    token_map_t *map;
} pass_map_user_t;

static void pass_map_cb(const char *full_path, const char *base_name, void *user) {
    if (!verify_chain_log_at(full_path, base_name)) return;
    pass_map_user_t *p = (pass_map_user_t *)user;
    FILE *f = fopen(full_path, "rb");
    if (!f) return;
    (void)chain_log_walk_events(f, map_collect_cb, p->map);
    fclose(f);
}

/* Projection pass: open file and walk for death-event projection. */
typedef struct {
    highscore_table_t *table;
    const token_map_t *map;
} pass_replay_user_t;

static void pass_replay_cb(const char *full_path, const char *base_name, void *user) {
    if (!verify_chain_log_at(full_path, base_name)) return;
    pass_replay_user_t *p = (pass_replay_user_t *)user;
    FILE *f = fopen(full_path, "rb");
    if (!f) return;
    replay_ctx_t r = {
        .table = p->table,
        .map = p->map,
        .cursor_world_id = 0,
        .cursor_world_seq = 0,
        .cursor_build_id = 0,
        .world_info_seen = false,
        .fallback_world_id = fallback_world_id_from_name(base_name),
    };
    (void)chain_log_walk_events(f, replay_event_cb, &r);
    fclose(f);
}

void highscore_replay_from_chain(highscore_table_t *t, const char *chain_dir) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    if (!chain_dir || !chain_dir[0]) return;

    /* Build a victim-token → callsign fallback map for legacy DEATH
     * events that didn't carry killed_by_callsign in the payload. New
     * events ride with their killer callsign attached and never consult
     * the map. */
    token_map_t map = {0};
    pass_map_user_t pm = { .map = &map };
    iterate_log_dir(chain_dir, pass_map_cb, &pm);

    pass_replay_user_t pr = { .table = t, .map = &map };
    iterate_log_dir(chain_dir, pass_replay_cb, &pr);

    free(map.entries);
}
