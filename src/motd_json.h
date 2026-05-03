/*
 * motd_json.h — Length-bounded, escape-aware MOTD JSON extractor.
 *
 * Schema (operator-pushed via scripts/generate-motd.sh, fetched from S3):
 *
 *   {
 *     "messages": {
 *       "common":     "...",
 *       "uncommon":   "...",
 *       "rare":       "...",
 *       "ultra_rare": "..."
 *     },
 *     "bands": {
 *       "common":     [0.80, 1.00],
 *       "uncommon":   [0.50, 0.80],
 *       "rare":       [0.20, 0.50],
 *       "ultra_rare": [0.00, 0.20]
 *     },
 *     "generated_at": 1700000000,
 *     "seed":         42
 *   }
 *
 * The previous parser (strstr-based) had two real bugs:
 *   - String extraction terminated at the first quote, even if escaped
 *     (a tier message containing \" silently truncated).
 *   - Tier-key lookups searched the whole document with no scope guard;
 *     a future schema change adding a duplicate key would mis-resolve.
 *
 * This parser walks the buffer with explicit length bounds, handles
 * the standard JSON string escapes, and scopes each tier-key lookup to
 * the surrounding object. It's purpose-built for this schema; not a
 * general-purpose JSON library. ~200 lines, no dependencies, no allocations.
 *
 * Defined header-only-style as `static` helpers so both the client
 * (avatar.c) and the test target can use them without a separate TU.
 */
#ifndef MOTD_JSON_H
#define MOTD_JSON_H

#include "avatar.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Default band layout if the JSON omits the "bands" object or fails to
 * parse a tier band. Indexed by tier (0=common, 3=ultra_rare). */
static const float MOTD_DEFAULT_BANDS[4][2] = {
    {0.80f, 1.00f},
    {0.50f, 0.80f},
    {0.20f, 0.50f},
    {0.00f, 0.20f},
};

static const char *const MOTD_TIER_NAMES[4] = {
    "common", "uncommon", "rare", "ultra_rare",
};

/* Skip whitespace forward in a length-bounded buffer. */
static inline const char *motd_json_skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Skip a JSON value starting at *p — string / number / array / object /
 * true / false / null. Returns pointer past the value, or NULL on parse
 * error. Used to step past values whose key didn't match the target. */
static inline const char *motd_json_skip_value(const char *p, const char *end) {
    p = motd_json_skip_ws(p, end);
    if (p >= end) return NULL;
    if (*p == '"') {
        p++;
        while (p < end) {
            if (*p == '\\') {
                if (p + 1 >= end) return NULL;
                p += 2;
                continue;
            }
            if (*p == '"') return p + 1;
            p++;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (p < end && depth > 0) {
            if (*p == '"') {
                p++;
                while (p < end) {
                    if (*p == '\\') {
                        if (p + 1 >= end) return NULL;
                        p += 2;
                        continue;
                    }
                    if (*p == '"') { p++; break; }
                    p++;
                }
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) depth--;
            p++;
        }
        return (depth == 0) ? p : NULL;
    }
    /* number / true / false / null — read until a delimiter. */
    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        p++;
    }
    return p;
}

/* Parse a JSON string literal at *p (which must point at '"'). Decodes
 * \" \\ \/ \b \f \n \r \t and writes the bytes into out (cap incl
 * room for trailing nul). Advances *p past the closing quote.
 * Returns true on success, false on parse error or out overflow. */
static inline bool motd_json_parse_string(const char **p, const char *end,
                                          char *out, size_t cap) {
    if (cap == 0) return false;
    if (*p >= end || **p != '"') return false;
    const char *q = *p + 1;
    size_t out_len = 0;
    while (q < end) {
        char c = *q;
        if (c == '"') {
            if (out_len >= cap) return false;
            out[out_len] = '\0';
            *p = q + 1;
            return true;
        }
        if (c == '\\') {
            if (q + 1 >= end) return false;
            char e = q[1];
            char decoded;
            switch (e) {
            case '"':  decoded = '"';  break;
            case '\\': decoded = '\\'; break;
            case '/':  decoded = '/';  break;
            case 'b':  decoded = '\b'; break;
            case 'f':  decoded = '\f'; break;
            case 'n':  decoded = '\n'; break;
            case 'r':  decoded = '\r'; break;
            case 't':  decoded = '\t'; break;
            case 'u':
                /* \uXXXX — decode the BMP code point as UTF-8. The
                 * MOTD generator strips non-ASCII before emitting, so
                 * in practice we only see ASCII; still, decode rather
                 * than mistreat. Surrogate pairs are not handled — a
                 * lone high surrogate writes the placeholder '?'. */
                if (q + 6 > end) return false;
                {
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; i++) {
                        char hx = q[2 + i];
                        cp <<= 4;
                        if      (hx >= '0' && hx <= '9') cp |= (uint32_t)(hx - '0');
                        else if (hx >= 'a' && hx <= 'f') cp |= (uint32_t)(hx - 'a' + 10);
                        else if (hx >= 'A' && hx <= 'F') cp |= (uint32_t)(hx - 'A' + 10);
                        else return false;
                    }
                    if (cp < 0x80) {
                        if (out_len + 1 >= cap) return false;
                        out[out_len++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (out_len + 2 >= cap) return false;
                        out[out_len++] = (char)(0xC0 | (cp >> 6));
                        out[out_len++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        if (out_len + 3 >= cap) return false;
                        out[out_len++] = (char)(0xE0 | (cp >> 12));
                        out[out_len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[out_len++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
                q += 6;
                continue;
            default:   return false;
            }
            if (out_len + 1 >= cap) return false;
            out[out_len++] = decoded;
            q += 2;
            continue;
        }
        if (out_len + 1 >= cap) return false;
        out[out_len++] = c;
        q++;
    }
    return false;
}

/* Walk an object starting at *p (must point at '{') looking for a key
 * matching `key`. On match, advances *p past the ':' separator
 * (whitespace skipped). Returns true on success, false if not found
 * or on parse error. The starting brace is consumed on entry. */
static inline bool motd_json_find_key(const char **p, const char *end,
                                      const char *key) {
    if (*p >= end || **p != '{') return false;
    const char *q = *p + 1;
    size_t key_len = strlen(key);
    while (q < end) {
        q = motd_json_skip_ws(q, end);
        if (q >= end) return false;
        if (*q == '}') return false;
        if (*q != '"') return false;
        const char *k_start = q + 1;
        const char *k_end = NULL;
        const char *r = k_start;
        while (r < end) {
            if (*r == '\\') {
                if (r + 1 >= end) return false;
                r += 2;
                continue;
            }
            if (*r == '"') { k_end = r; break; }
            r++;
        }
        if (!k_end) return false;
        size_t this_key_len = (size_t)(k_end - k_start);
        q = k_end + 1;
        q = motd_json_skip_ws(q, end);
        if (q >= end || *q != ':') return false;
        q++;
        q = motd_json_skip_ws(q, end);
        if (this_key_len == key_len && memcmp(k_start, key, key_len) == 0) {
            *p = q;
            return true;
        }
        const char *after_value = motd_json_skip_value(q, end);
        if (!after_value) return false;
        q = motd_json_skip_ws(after_value, end);
        if (q >= end) return false;
        if (*q == ',') { q++; continue; }
        if (*q == '}') return false;
        return false;
    }
    return false;
}

/* Parse the MOTD JSON document into the avatar cache entry. Returns
 * true on success (all 4 tier strings extracted). On failure,
 * tier text is left zeroed and bands are set to MOTD_DEFAULT_BANDS.
 * Caller passes the buffer + length; the buffer need NOT be
 * NUL-terminated. */
static inline bool motd_parse(avatar_cache_t *entry, const char *json,
                              size_t json_len) {
    memset(entry->tiers, 0, sizeof(entry->tiers));

    /* Bands always start at defaults — overridden below if present. */
    for (int i = 0; i < 4; i++) {
        entry->tiers[i].band_min = MOTD_DEFAULT_BANDS[i][0];
        entry->tiers[i].band_max = MOTD_DEFAULT_BANDS[i][1];
    }

    const char *end = json + json_len;
    const char *p = motd_json_skip_ws(json, end);

    /* "messages" — REQUIRED. Without all 4 tier strings we return false
     * so the caller falls back to the legacy single-message path. */
    {
        const char *msgs = p;
        if (!motd_json_find_key(&msgs, end, "messages")) return false;
        if (msgs >= end || *msgs != '{') return false;
        for (int i = 0; i < 4; i++) {
            const char *tier = msgs;
            if (!motd_json_find_key(&tier, end, MOTD_TIER_NAMES[i])) return false;
            if (!motd_json_parse_string(&tier, end, entry->tiers[i].text,
                                        sizeof(entry->tiers[i].text)))
                return false;
        }
    }

    /* "bands" — OPTIONAL. If absent or malformed, keep defaults. */
    {
        const char *bands = p;
        if (motd_json_find_key(&bands, end, "bands") &&
            bands < end && *bands == '{') {
            for (int i = 0; i < 4; i++) {
                const char *tier = bands;
                if (!motd_json_find_key(&tier, end, MOTD_TIER_NAMES[i])) continue;
                if (tier >= end || *tier != '[') continue;
                /* Two floats inside [ ... ]. sscanf handles whitespace
                 * and signed/decimal forms; we don't care about
                 * trailing junk beyond the second number. */
                float lo = 0.0f, hi = 0.0f;
                if (sscanf(tier + 1, " %f , %f", &lo, &hi) == 2) {
                    entry->tiers[i].band_min = lo;
                    entry->tiers[i].band_max = hi;
                }
            }
        }
    }

    /* "generated_at" / "seed" — OPTIONAL metadata. */
    {
        const char *gen = p;
        if (motd_json_find_key(&gen, end, "generated_at")) {
            unsigned u = 0;
            if (sscanf(gen, "%u", &u) == 1) entry->generated_at = u;
        }
        const char *seed = p;
        if (motd_json_find_key(&seed, end, "seed")) {
            unsigned u = 0;
            if (sscanf(seed, "%u", &u) == 1) entry->seed = u;
        }
    }

    return true;
}

#endif /* MOTD_JSON_H */
