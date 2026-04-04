# Anime Integration Plan — Signal Space Miner

## Architecture Decision: Browser-Native Video

The game is deployed via Emscripten at signal.ratimics.com/play. It currently has **zero external assets** — everything is procedural. Rather than pulling in a C video decoder library (complex, large WASM binary increase), we use the **browser's native `<video>` element** overlaid on the canvas.

For native desktop builds, episodes are a no-op (or we add a future stb_image/pl_mpeg path).

---

## File Layout

```
assets/
  anime/
    ep0-first-light.mp4
    ep1-keplers-law.mp4
    ep2-furnace.mp4
    ep3-scaffold.mp4
    ep4-naming.mp4
    ep5-drones.mp4
    ep6-hauler.mp4
    ep7-dark-sector.mp4
    ep8-every-ai-dreams.mp4
    ep9-death.mp4           (death scene)
```

These get copied to the build-web output directory by CMake and served alongside `space_miner.html`.

---

## Implementation Steps

### Step 1: Episode State in `client.h`

Add to `game_t` struct (after death screen fields, ~line 127):

```c
/* --- Episode playback --- */
struct {
    bool active;            /* episode currently playing */
    bool watched[16];       /* persistent: which episodes seen */
    int  current;           /* episode index, -1 = none */
    float fade_timer;       /* fade in/out transition */
    bool loaded;            /* watched state loaded from localStorage */
} episode;
```

### Step 2: Episode System — `episode.h` / `episode.c`

New files. Core API:

```c
#define EPISODE_COUNT 10

typedef struct {
    const char *filename;   /* "ep0-first-light.mp4" */
    const char *title;      /* "FIRST LIGHT" */
} episode_info_t;

void episode_load(void);           /* load watched state from localStorage */
void episode_save(void);           /* persist watched state */
void episode_trigger(int index);   /* start playback if not already watched */
void episode_skip(void);           /* player presses ESC to skip */
void episode_update(float dt);     /* tick fade timer, check if video ended */
bool episode_is_active(void);      /* true while playing */
```

Episode table:

```c
static const episode_info_t episodes[EPISODE_COUNT] = {
    { "ep0-first-light.mp4",     "FIRST LIGHT" },
    { "ep1-keplers-law.mp4",     "KEPLER'S LAW" },
    { "ep2-furnace.mp4",         "FURNACE" },
    { "ep3-scaffold.mp4",        "SCAFFOLD" },
    { "ep4-naming.mp4",          "NAMING" },
    { "ep5-drones.mp4",          "DRONES" },
    { "ep6-hauler.mp4",          "HAULER" },
    { "ep7-dark-sector.mp4",     "DARK SECTOR" },
    { "ep8-every-ai-dreams.mp4", "EVERY AI DREAMS" },
    { "ep9-death.mp4",           "DEATH" },
};
```

### Step 3: Emscripten Video Bridge (JavaScript interop)

`episode_trigger()` calls into JS via `emscripten_run_script()`:

```c
void episode_trigger(int index) {
    if (index < 0 || index >= EPISODE_COUNT) return;
    if (g.episode.watched[index]) return;

    g.episode.active = true;
    g.episode.current = index;
    g.episode.fade_timer = 0.5f;  /* fade-in duration */
    g.episode.watched[index] = true;
    episode_save();

#ifdef __EMSCRIPTEN__
    char js[256];
    snprintf(js, sizeof(js),
        "signalPlayEpisode('assets/anime/%s', '%s')",
        episodes[index].filename, episodes[index].title);
    emscripten_run_script(js);
#endif
}
```

On the HTML side, inject a small JS module into `space_miner.html` (or a `<script>` tag):

```javascript
// Signal Episode Player
function signalPlayEpisode(src, title) {
    let overlay = document.getElementById('episode-overlay');
    if (!overlay) {
        overlay = document.createElement('div');
        overlay.id = 'episode-overlay';
        overlay.style.cssText = `
            position: fixed; top: 0; left: 0; width: 100%; height: 100%;
            background: rgba(0,0,0,0.85); display: flex; flex-direction: column;
            align-items: center; justify-content: center; z-index: 1000;
            opacity: 0; transition: opacity 0.5s;
        `;
        document.body.appendChild(overlay);
    }
    overlay.innerHTML = `
        <div style="color: #c8a030; font-family: monospace; font-size: 14px;
                    margin-bottom: 12px; letter-spacing: 4px;">${title}</div>
        <video id="episode-video" autoplay style="max-width: 90%; max-height: 80%;
               border: 1px solid rgba(200,160,48,0.3);">
            <source src="${src}" type="video/mp4">
        </video>
        <div style="color: #555; font-family: monospace; font-size: 11px;
                    margin-top: 12px;">ESC to skip</div>
    `;
    requestAnimationFrame(() => overlay.style.opacity = '1');

    const video = document.getElementById('episode-video');
    video.onended = () => signalEndEpisode();
}

function signalEndEpisode() {
    const overlay = document.getElementById('episode-overlay');
    if (overlay) {
        overlay.style.opacity = '0';
        setTimeout(() => overlay.remove(), 500);
    }
    // Notify C side
    if (Module._episode_ended) Module._episode_ended();
}

function signalIsEpisodePlaying() {
    return document.getElementById('episode-video') !== null;
}
```

Export `episode_ended` from C:

```c
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE void episode_ended(void) {
    g.episode.active = false;
    g.episode.current = -1;
}
#endif
```

### Step 4: Hook Triggers into Game Events

In `process_sim_events()` (main.c, ~line 173):

```c
case SIM_EVENT_LAUNCH:
    if (ev->player_id == g.local_player_slot) {
        audio_play_launch(&g.audio);
        set_notice("Launch corridor clear.");
        onboarding_mark_launched();
        episode_trigger(0);  // Ep 0: First Light
    }
    break;

case SIM_EVENT_DEATH:
    if (ev->player_id == g.local_player_slot) {
        // ... existing death screen code ...
        episode_trigger(9);  // Ep 9: Death
    }
    break;
```

In `onboarding_per_frame()` or a new `episode_per_frame()` (main.c, after line 251):

```c
static void episode_per_frame(void) {
    /* Ep 1: Kepler's Law — docked at all 3 original stations */
    // Track via a bitmask of visited stations (add to game_t)

    /* Ep 2: Furnace — smelted all 3 ore types */
    // Check furnace interaction events

    /* Ep 3: Scaffold — bought first scaffold kit */
    if (!g.episode.watched[3] && g.onboarding.got_scaffold)
        episode_trigger(3);

    /* Ep 4: Naming — placed first outpost */
    if (!g.episode.watched[4] && g.onboarding.placed_outpost)
        episode_trigger(4);

    /* Ep 5: Drones — first NPC mining drone at player outpost */
    // Check NPC spawn events at player-owned stations

    /* Ep 6: Hauler — first cargo hauler completes supply contract */
    // Hook into SIM_EVENT_CONTRACT_COMPLETE for supply type

    /* Ep 7: Dark Sector — enter zero-signal area */
    // Check signal_quality_at(player_pos) < SIGNAL_FRONTIER threshold

    /* Ep 8: Every AI Dreams — 5+ connected stations */
    // Count stations where signal_connected == true
}
```

### Step 5: Input Blocking During Playback

In `sim_step()` (main.c, ~line 272), after the death screen block:

```c
/* Episode playback — block game input */
if (g.episode.active) {
    /* ESC to skip */
    if (g.input.key_pressed[KEY_MENU]) {
        episode_skip();
    }
    consume_pressed_input();
    return;
}
```

### Step 6: HUD Overlay During Playback

In `draw_hud()` (hud.c, ~line 544), before the death screen check:

```c
/* --- Episode playback overlay --- */
if (g.episode.active) {
    /* Dark scrim (video is in HTML layer, but we dim the game canvas) */
    float alpha = fminf(g.episode.fade_timer * 2.0f, 0.8f);
    draw_ui_scrim(alpha);

    /* Episode title at top of canvas */
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);
    sdtx_color3b(200, 160, 48);  /* signal gold */
    const char *title = "SIGNAL INCOMING...";
    float tw = (float)strlen(title) * 8.0f;
    sdtx_pos((screen_w * 0.5f - tw * 0.5f) / 8.0f, 2.0f);
    sdtx_puts(title);

    return; /* skip normal HUD */
}
```

### Step 7: Persistence

Mirror the onboarding pattern — pack `watched[0..15]` into a 16-bit int, store in `localStorage`:

```c
void episode_save(void) {
#ifdef __EMSCRIPTEN__
    int flags = 0;
    for (int i = 0; i < EPISODE_COUNT; i++)
        if (g.episode.watched[i]) flags |= (1 << i);
    char js[80];
    snprintf(js, sizeof(js),
        "localStorage.setItem('signal_episodes','%d')", flags);
    emscripten_run_script(js);
#endif
}

void episode_load(void) {
#ifdef __EMSCRIPTEN__
    int flags = emscripten_run_script_int(
        "(function(){var s=localStorage.getItem('signal_episodes');"
        "if(!s)return 0;return parseInt(s,10)||0;})()");
    for (int i = 0; i < EPISODE_COUNT; i++)
        g.episode.watched[i] = (flags & (1 << i)) != 0;
    g.episode.loaded = true;
#endif
}
```

Call `episode_load()` in `init()` after `onboarding_load()`.

### Step 8: CMake — Copy Assets to Web Build

In `CMakeLists.txt`, add asset copying for Emscripten:

```cmake
if(EMSCRIPTEN)
    file(GLOB ANIME_ASSETS "${CMAKE_SOURCE_DIR}/assets/anime/*.mp4")
    foreach(ASSET ${ANIME_ASSETS})
        get_filename_component(ASSET_NAME ${ASSET} NAME)
        configure_file(${ASSET} ${CMAKE_BINARY_DIR}/assets/anime/${ASSET_NAME} COPYONLY)
    endforeach()
    # Add --preload-file or serve from same directory
endif()
```

For web deployment: serve `assets/anime/` alongside the WASM build. The `<video>` element fetches them on demand — no preloading into WASM memory.

---

## Trigger Summary

| Episode | Title | Trigger | Hook Point |
|---------|-------|---------|------------|
| 0 | First Light | First launch | `SIM_EVENT_LAUNCH` + `!watched[0]` |
| 1 | Kepler's Law | Dock at all 3 stations | New visited-stations bitmask |
| 2 | Furnace | Smelt all 3 ore types | Furnace interaction tracking |
| 3 | Scaffold | Buy first scaffold kit | `onboarding.got_scaffold` |
| 4 | Naming | Place first outpost | `onboarding.placed_outpost` |
| 5 | Drones | NPC drone spawns at your outpost | NPC spawn event at player station |
| 6 | Hauler | Hauler completes supply contract | `SIM_EVENT_CONTRACT_COMPLETE` |
| 7 | Dark Sector | Enter zero-signal space | `signal_quality < FRONTIER` check |
| 8 | Every AI Dreams | 5+ connected stations | Station count per frame |
| 9 | Death | Ship destroyed | `SIM_EVENT_DEATH` |

---

## What Ships First

1. **Copy & rename the 10 mp4s into `assets/anime/`**
2. **Add `episode.h` + `episode.c`** with state, persistence, trigger/skip
3. **JS bridge** in the HTML template for `<video>` overlay
4. **Hook 3 easy triggers**: Ep 0 (launch), Ep 3 (scaffold), Ep 4 (outpost) — these piggyback on existing onboarding flags
5. **Input blocking + HUD scrim** — copy death screen pattern
6. **Test in browser**, then wire remaining triggers incrementally

---

## Future: Signal Ghost Discovery Layer

The current plan plays episodes as full-screen overlays triggered by progression. Phase 2 adds the **in-world signal ghost** discovery mechanic from the framework doc:

- Audio fragments playing spatially when near ghost nodes in deep space
- Visual echo entities (animated sprite/shader quads) in the asteroid field
- Signal Archive station module that assembles fragments into full episodes
- This requires the sector system (epic #23) to have "uncharted space" to explore
