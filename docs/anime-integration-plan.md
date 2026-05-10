# Anime Integration Plan - Current Implementation

This document tracks the shipped episode-playback architecture. The creative
framework lives in [`anime-framework.md`](./anime-framework.md); this file is
for code and asset integration details.

## Status

Episode playback is implemented in-engine, not through a browser `<video>`
overlay.

- `client/episode.c` decodes MPEG-1 program streams through `pl_mpeg`.
- Decoded frames are uploaded to a Sokol texture and rendered by the normal UI
  pass as a bottom-right signal-artifact popup.
- Decoded audio is mixed into the game's Sokol audio stream through
  `episode_read_audio`.
- Emscripten builds fetch episode files from the asset CDN.
- Native builds load the same files from local `assets/` paths when present.

The old browser-native MP4 plan is retired. There is no `signalPlayEpisode`
JavaScript bridge, no DOM overlay, and no `space_miner.html` integration point.

## Asset Layout

The episode table in `client/episode.c` expects MPEG files under an `anime/`
prefix:

```text
anime/ep0-first-light.mpg
anime/ep1-keplers-law.mpg
anime/ep2-furnace.mpg
anime/ep3-scaffold.mpg
anime/ep4-naming.mpg
anime/ep5-drones.mpg
anime/ep6-hauler.mpg
anime/ep7-dark-sector.mpg
anime/ep8-every-ai-dreams.mpg
anime/ep9-death.mpg
```

Browser builds fetch those paths from:

```text
https://signal-ratimics-assets.s3.amazonaws.com/<filename>
```

Native development tries:

```text
assets/<filename>
```

For example, episode 0 is loaded from
`assets/anime/ep0-first-light.mpg` locally and from
`https://signal-ratimics-assets.s3.amazonaws.com/anime/ep0-first-light.mpg`
in the browser.

The decoder is `pl_mpeg`, so assets should be MPEG-1 program streams (`.mpg`),
not MP4. If the asset pipeline changes back to MP4, the implementation needs a
new decoder or a browser-only playback path.

## Code Map

| File | Role |
| --- | --- |
| `client/episode.h` | Episode state, API, texture IDs, decoded-frame buffers, audio ring buffer, trigger tracking. |
| `client/episode.c` | Episode table, CDN/local loading, `pl_mpeg` decode, Sokol texture upload/render, watched-state persistence. |
| `client/main.c` | Initializes episodes, mixes episode audio, hooks triggers, updates decode state, uploads one frame per render frame, renders popup UI. |
| `client/hud.c` | Suppresses some HUD affordances while an episode popup is active. |
| `client/palette.h` | Episode UI colors. |
| `client/pl_mpeg.h` | Vendored MPEG decoder. |

## Playback Lifecycle

1. `episode_init` zeros state and prepares the audio ring buffer.
2. `episode_load` restores the watched bitset from browser `localStorage`.
   Native builds currently treat watched state as session-local only.
3. `episode_trigger` marks the episode watched, chooses the filename, and
   starts an async CDN fetch on Emscripten or a local file load on native.
4. Once bytes are available, `episode_start_playback` creates a `pl_mpeg`
   decoder and registers video/audio callbacks.
5. `episode_update` advances the decoder during normal sim stepping.
6. Video callbacks convert frames to RGBA and stash the latest frame.
7. `episode_upload_frame` uploads at most one stashed frame per render frame.
   This avoids multiple `sg_update_image` calls for the same image in one
   frame.
8. `episode_render` draws the popup in the bottom-right UI layer.
9. `episode_read_audio` drains decoded audio into the main audio stream.
10. `episode_skip` or decoder end tears down the decoder and texture state.

## Trigger Summary

| Episode | Title | Current trigger |
| --- | --- | --- |
| 0 | First Light | Local launch event or docked-to-undocked transition. |
| 1 | Kepler's Law | Local player has docked at all three seeded stations. |
| 2 | Furnace | First local sell/smelt payout event. |
| 3 | Scaffold | Local player is towing a scaffold. |
| 4 | Naming | Local outpost activation. |
| 5 | Drones | First miner NPC spawned at a player/outpost station (`station >= 3`). |
| 6 | Hauler | Tractor contract completion event. |
| 7 | Dark Sector | Local `SIM_EVENT_SIGNAL_LOST`. |
| 8 | Every AI Dreams | Station-connected event reports at least five connected stations. |
| 9 | Death | Local death event or multiplayer death payload. |

The original creative framework described episode 2 as "smelt all three ore
types." The current code triggers it earlier, on the first local sell/smelt
payout. Change the trigger if the stricter milestone matters for pacing.

## Interaction Model

Episodes are diegetic popups, not full-screen blocking cutscenes.

- The player can keep flying while the popup is active.
- `Esc` skips the active episode.
- Death playback is special: the death handler stops any current episode,
  clears watched flags, triggers episode 9, and keeps episode decode/audio
  running while the death cinematic owns input.

This matches the signal-artifact framing in the creative doc: the episode is
an object in the world, not a hard mode switch away from gameplay.

## Persistence

Browser builds store the watched bitset in:

```text
localStorage["signal_episodes"]
```

The value is a decimal integer with one bit per episode. Native builds do not
currently persist watched state to disk; they rely on in-memory session state.

## Packaging Notes

- Browser deployment does not need CMake to copy episode assets into
  `build-web/` as long as the CDN remains authoritative.
- Native development needs local files under `assets/anime/` if episode
  playback should work offline.
- The root Docker image serves the web bundle but does not package the episode
  files; browser clients still fetch them from the CDN.
- Music uses the same CDN/local-asset pattern through `client/music.c`, but
  music files are MP3 and decoded with `minimp3`.

## Remaining Work

1. Add a repeatable asset-production recipe that outputs `pl_mpeg`-compatible
   `.mpg` files from source video.
2. Decide whether native watched-state persistence matters; if yes, mirror the
   identity/onboarding data-dir pattern instead of inventing a new path.
3. Add a browser smoke case that stubs or serves a tiny `.mpg` and verifies a
   nonblank episode texture plus audio-buffer activity.
4. Revisit the episode 2 trigger if the "all three ore types" milestone is
   still the intended narrative beat.
5. Implement the future Signal Ghost layer from the creative framework:
   spatial audio fragments, in-world echo nodes, and Signal Archive assembly.
