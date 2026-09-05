#define _POSIX_C_SOURCE 200809L
#include "progress_store.h"

#include "identity.h"
#include "persistence_io.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static char selected_key[65];
static client_progress_t current;
static bool save_warning_shown;

bool client_progress_scope_key(char out[65], const uint8_t pubkey[32],
                               const char *authority)
{
    if (!out) return false;
    out[0] = '\0';
    if (!pubkey) return false;
    uint8_t any = 0;
    for (int i = 0; i < 32; i++) any |= pubkey[i];
    if (!any) return false;
    const char *scope = authority && authority[0] ? authority : "local:sector-one";
    sha256_ctx_t hash;
    uint8_t digest[32];
    sha256_init(&hash);
    sha256_update(&hash, "signal-progress-v2", 18);
    sha256_update(&hash, pubkey, 32);
    sha256_update(&hash, scope, strlen(scope));
    sha256_final(&hash, digest);
    const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = '\0';
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool progress_valid(const client_progress_t *progress)
{
    /* Story events form an ordered prefix; guide events may arrive freely. */
    return progress && progress->story <= 255u && progress->guide <= 1023u &&
        (progress->story & (progress->story + 1u)) == 0;
}

bool client_progress_decode(const char *text, client_progress_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!text || strlen(text) != 11 || memcmp(text, "SGP2:", 5) != 0 ||
        text[7] != ':') return false;
    const int positions[] = {5, 6, 8, 9, 10};
    unsigned value = 0;
    for (int i = 0; i < 5; i++) {
        int digit = hex_digit(text[positions[i]]);
        if (digit < 0) return false;
        value = (value << 4) | (unsigned)digit;
    }
    client_progress_t decoded = {
        .story = (uint16_t)(value >> 12),
        .guide = (uint16_t)(value & 4095u),
    };
    if (!progress_valid(&decoded)) return false;
    *out = decoded;
    return true;
}

bool client_progress_encode(char out[12], const client_progress_t *progress)
{
    if (!out) return false;
    out[0] = '\0';
    if (!progress_valid(progress)) return false;
    return snprintf(out, 12, "SGP2:%02x:%03x",
                    (unsigned)progress->story, (unsigned)progress->guide) == 11;
}

bool client_progress_read_at(const char *path, client_progress_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!path) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char text[13] = {0};
    size_t count = fread(text, 1, 12, file);
    bool ok = count == 11 && !ferror(file);
    fclose(file);
    return ok && client_progress_decode(text, out);
}

bool client_progress_write_at(const char *path,
                              const client_progress_t *progress)
{
    char text[12];
    if (!path || !client_progress_encode(text, progress)) return false;
    char temporary[2304];
    int count = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (count <= 0 || (size_t)count >= sizeof(temporary)) return false;
    FILE *file = fopen(temporary, "wb");
    if (!file) return false;
    bool ok = fwrite(text, 1, 11, file) == 11 && persistence_flush_durable(file);
    if (fclose(file) != 0) ok = false;
    if (ok) ok = persistence_replace_file(temporary, path);
    if (!ok) remove(temporary);
    return ok;
}

#ifdef __EMSCRIPTEN__
EM_JS(int, browser_progress_load, (const char *scope, char *out, int cap), {
    try {
        var owner = UTF8ToString(scope);
        var key = 'signal_progress_v2:' + owner;
        var value = localStorage.getItem(key);
        if (value === null) {
            // The first selected identity/world receives the old shared record.
            var migrationOwner = localStorage.getItem('signal_progress_v2_legacy_owner');
            if (migrationOwner === null || migrationOwner === owner) {
                var story = localStorage.getItem('signal_story_loop_v1') || '0';
                var guide = localStorage.getItem('signal_onboarding') || '0';
                var s = Number(story);
                var g = Number(guide);
                if (!Number.isInteger(s) || s < 0 || s > 255 ||
                    String(s) !== story || (s & (s + 1)) !== 0) s = 0;
                if (!Number.isInteger(g) || g < 0 || g > 1023 ||
                    String(g) !== guide) g = 0;
                value = 'SGP2:' + s.toString(16).padStart(2, '0') + ':' +
                    g.toString(16).padStart(3, '0');
                localStorage.setItem(key, value);
                localStorage.setItem('signal_progress_v2_legacy_owner', owner);
            }
        }
        if (value === null) return 0;
        if (value.length !== 11) return 0;
        stringToUTF8(value, out, cap);
        return 1;
    } catch (error) {
        console.warn('[progress] Browser storage is unavailable; progress is in memory.');
        return 0;
    }
})

EM_JS(int, browser_progress_save, (const char *scope, const char *text), {
    try {
        localStorage.setItem('signal_progress_v2:' + UTF8ToString(scope),
                             UTF8ToString(text));
        return 1;
    } catch (error) { return 0; }
})
#else
static bool progress_path(char *out, size_t cap)
{
    char filename[96];
    snprintf(filename, sizeof(filename), "progress-%s.txt", selected_key);
    return identity_data_path(out, cap, filename);
}
#endif

void client_progress_select(const uint8_t pubkey[32], const char *authority)
{
    memset(&current, 0, sizeof(current));
    save_warning_shown = false;
    if (!client_progress_scope_key(selected_key, pubkey, authority)) return;
#ifdef __EMSCRIPTEN__
    char text[12] = {0};
    if (browser_progress_load(selected_key, text, sizeof(text)))
        (void)client_progress_decode(text, &current);
#else
    char path[2304];
    if (progress_path(path, sizeof(path)))
        (void)client_progress_read_at(path, &current);
#endif
}

client_progress_t client_progress_current(void)
{
    return current;
}

static void progress_save(void)
{
    if (!selected_key[0]) return;
    bool ok;
#ifdef __EMSCRIPTEN__
    char text[12];
    ok = client_progress_encode(text, &current) &&
        browser_progress_save(selected_key, text) != 0;
#else
    char path[2304];
    ok = progress_path(path, sizeof(path)) &&
        client_progress_write_at(path, &current);
#endif
    if (!ok && !save_warning_shown) {
        fprintf(stderr, "[progress] Save failed; progress is in memory.\n");
        save_warning_shown = true;
    }
}

void client_progress_save_story(uint16_t flags)
{
    current.story = flags;
    progress_save();
}

void client_progress_save_guide(uint16_t flags)
{
    current.guide = flags;
    progress_save();
}
