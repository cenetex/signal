#include "story_runtime.h"

#include "client.h"
#include "story_loop.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static bool story_runtime_is_local_outpost(const station_t *station)
{
    if (!station || !g.identity_ready) return false;
    uint8_t combined = 0;
    for (size_t i = 0; i < sizeof(station->outpost_founder_pubkey); i++)
        combined |= station->outpost_founder_pubkey[i];
    return combined != 0u &&
        memcmp(station->outpost_founder_pubkey,
               g.identity.pubkey,
               sizeof(g.identity.pubkey)) == 0;
}

static void story_runtime_save(void)
{
#ifdef __EMSCRIPTEN__
    char js[112];
    snprintf(js, sizeof(js),
             "localStorage.setItem('signal_story_loop_v1','%u')",
             (unsigned)g.worker_story.flags);
    emscripten_run_script(js);
#endif
}

void story_runtime_load(void)
{
    if (g.worker_story.loaded) return;
    g.worker_story.loaded = true;
#ifdef __EMSCRIPTEN__
    int flags = emscripten_run_script_int(
        "(function(){var s=localStorage.getItem('signal_story_loop_v1');"
        "if(!s)return 0;return parseInt(s,10)||0;})()");
    g.worker_story.flags = (uint16_t)((unsigned)flags & 0xffu);
#endif
}

static bool story_runtime_finish_mark(bool changed,
                                      worker_story_beat_t before,
                                      char *notice,
                                      size_t notice_size)
{
    if (notice && notice_size > 0) notice[0] = '\0';
    if (!changed) return false;
    story_runtime_save();
    if (worker_story_beat(&g.worker_story) == before) return false;
    const char *line = worker_story_transition_line(before);
    if (!line[0]) return false;
    if (notice && notice_size > 0)
        snprintf(notice, notice_size, "%s", line);
    return true;
}

bool story_runtime_mark_hail(int station_index,
                             char *notice, size_t notice_size)
{
    worker_story_beat_t before = worker_story_beat(&g.worker_story);
    bool changed = worker_story_mark_hail(&g.worker_story, station_index);
    return story_runtime_finish_mark(changed, before, notice, notice_size);
}

bool story_runtime_mark_signal_gap(char *notice, size_t notice_size)
{
    worker_story_beat_t before = worker_story_beat(&g.worker_story);
    bool changed = worker_story_mark_signal_gap(&g.worker_story);
    return story_runtime_finish_mark(changed, before, notice, notice_size);
}

bool story_runtime_mark_outpost_placed(char *notice, size_t notice_size)
{
    worker_story_beat_t before = worker_story_beat(&g.worker_story);
    bool changed = worker_story_mark_outpost_placed(&g.worker_story);
    return story_runtime_finish_mark(changed, before, notice, notice_size);
}

bool story_runtime_mark_outpost_active(int station_index,
                                       char *notice, size_t notice_size)
{
    if (station_index < SIGNAL_FIRST_OUTPOST_INDEX ||
        station_index >= MAX_STATIONS) {
        if (notice && notice_size > 0) notice[0] = '\0';
        return false;
    }
    const station_t *station = &g.world.stations[station_index];
    if (!story_runtime_is_local_outpost(station)) {
        if (notice && notice_size > 0) notice[0] = '\0';
        return false;
    }

    worker_story_beat_t before = worker_story_beat(&g.worker_story);
    bool changed = worker_story_mark_outpost_active(&g.worker_story);
    return story_runtime_finish_mark(changed, before, notice, notice_size);
}

bool story_runtime_mark_delivery(char *notice, size_t notice_size)
{
    worker_story_beat_t before = worker_story_beat(&g.worker_story);
    bool changed = worker_story_mark_delivery(&g.worker_story);
    return story_runtime_finish_mark(changed, before, notice, notice_size);
}

bool story_runtime_mark_dock(int station_index,
                             char *notice, size_t notice_size)
{
    worker_story_beat_t before = worker_story_beat(&g.worker_story);
    bool changed = worker_story_mark_dock(&g.worker_story, station_index);
    return story_runtime_finish_mark(changed, before, notice, notice_size);
}

static void story_runtime_reconcile_outpost(void)
{
    worker_story_beat_t beat = worker_story_beat(&g.worker_story);
    if (beat != WORKER_STORY_APPROACH && beat != WORKER_STORY_REWARD)
        return;

    for (int i = SIGNAL_FIRST_OUTPOST_INDEX; i < MAX_STATIONS; i++) {
        const station_t *station = &g.world.stations[i];
        if (!story_runtime_is_local_outpost(station)) continue;
        if (beat == WORKER_STORY_APPROACH && station_exists(station)) {
            if (worker_story_mark_outpost_placed(&g.worker_story))
                story_runtime_save();
            beat = worker_story_beat(&g.worker_story);
        }
        if (beat == WORKER_STORY_REWARD && station_is_active(station)) {
            if (worker_story_mark_outpost_active(&g.worker_story))
                story_runtime_save();
        }
        return;
    }
}

bool story_runtime_hint(char *label, size_t label_size,
                        char *message, size_t message_size)
{
    /* A returning player can use relay work already present in the
     * authoritative station snapshot. */
    story_runtime_reconcile_outpost();
    return worker_story_directive(&g.worker_story,
                                  label, label_size,
                                  message, message_size);
}

bool story_runtime_is_complete(void)
{
    return worker_story_is_complete(&g.worker_story);
}
