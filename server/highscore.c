#include "highscore.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define HIGHSCORE_MAGIC   0x48494753u  /* 'H' 'I' 'G' 'S' */
#define HIGHSCORE_VERSION 1u

static void write_f32_le(uint8_t *p, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    p[0] = (uint8_t)(u);
    p[1] = (uint8_t)(u >> 8);
    p[2] = (uint8_t)(u >> 16);
    p[3] = (uint8_t)(u >> 24);
}

void highscore_load(highscore_table_t *t, const char *path) {
    memset(t, 0, sizeof(*t));
    if (!path) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint32_t magic = 0, version = 0;
    int32_t  count = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1 ||
        fread(&version, sizeof(version), 1, f) != 1 ||
        fread(&count, sizeof(count), 1, f) != 1 ||
        magic != HIGHSCORE_MAGIC || version != HIGHSCORE_VERSION ||
        count < 0 || count > HIGHSCORE_TOP_N) {
        fclose(f);
        return;
    }
    for (int i = 0; i < count; i++) {
        if (fread(&t->entries[i], sizeof(highscore_entry_t), 1, f) != 1) {
            memset(t, 0, sizeof(*t));
            fclose(f);
            return;
        }
    }
    t->count = count;
    fclose(f);
}

bool highscore_save(const highscore_table_t *t, const char *path) {
    if (!path) return false;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    uint32_t magic = HIGHSCORE_MAGIC;
    uint32_t version = HIGHSCORE_VERSION;
    int32_t  count = t->count;
    if (fwrite(&magic, sizeof(magic), 1, f) != 1 ||
        fwrite(&version, sizeof(version), 1, f) != 1 ||
        fwrite(&count, sizeof(count), 1, f) != 1) {
        fclose(f);
        remove(tmp);
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (fwrite(&t->entries[i], sizeof(highscore_entry_t), 1, f) != 1) {
            fclose(f);
            remove(tmp);
            return false;
        }
    }
    fclose(f);
    return rename(tmp, path) == 0;
}

bool highscore_submit(highscore_table_t *t,
                      const char *callsign, float credits_earned)
{
    if (!t) return false;
    /* Reject NaN/inf and negative runs. Zero-credit deaths still
     * qualify — they show the player they're on the board (sorted to
     * the bottom) and get replaced once they earn something. NaN
     * compares false against everything, so the explicit isfinite
     * check is required. */
    if (!isfinite(credits_earned) || credits_earned < 0.0f) return false;

    /* Dedup by callsign: if this player already has an entry, only
     * mutate when the new run is strictly better (then re-sort by
     * removing the old + inserting fresh). One row per callsign keeps
     * the table from being overrun by repeated low-credit deaths. */
    if (callsign && callsign[0]) {
        for (int i = 0; i < t->count; i++) {
            if (memcmp(t->entries[i].callsign, callsign,
                       strnlen(callsign, sizeof(t->entries[i].callsign))) == 0
                && (size_t)strnlen(callsign, sizeof(t->entries[i].callsign))
                   == strnlen(t->entries[i].callsign, sizeof(t->entries[i].callsign))) {
                if (credits_earned <= t->entries[i].credits_earned) return false;
                /* Remove the old entry; fall through to insert. */
                for (int j = i; j < t->count - 1; j++) t->entries[j] = t->entries[j + 1];
                t->count--;
                memset(&t->entries[t->count], 0, sizeof(t->entries[t->count]));
                break;
            }
        }
    }

    /* Find insertion position — descending by credits_earned. */
    int ins = t->count;
    for (int i = 0; i < t->count; i++) {
        if (credits_earned > t->entries[i].credits_earned) { ins = i; break; }
    }
    if (ins >= HIGHSCORE_TOP_N) return false;

    /* Shift down (drop tail when full). */
    int end = (t->count < HIGHSCORE_TOP_N) ? t->count : HIGHSCORE_TOP_N - 1;
    for (int i = end; i > ins; i--) t->entries[i] = t->entries[i - 1];

    highscore_entry_t *e = &t->entries[ins];
    memset(e, 0, sizeof(*e));
    if (callsign) {
        size_t n = strnlen(callsign, sizeof(e->callsign));
        memcpy(e->callsign, callsign, n);
    }
    e->credits_earned = credits_earned;

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
    }
    return HIGHSCORE_HEADER + t->count * HIGHSCORE_ENTRY_SIZE;
}
