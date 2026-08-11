# Milestone Video Integration Plan

This document tracks the shipped milestone-video playback architecture. Product
scope is intentionally narrow: play the existing videos at meaningful progress
beats, and do not build a broader anime/narrative content system.

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
The old Signal Ghost / Signal Archive roadmap is also cut from MVP scope.

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
| `client/episode_lifecycle.c` | Sokol-free pending/start/failure state machine and stale-attempt tokens. |
| `client/episode_media.c` | Native file-read seam and validated `pl_mpeg` decoder construction. |
| `client/episode.c` | Episode table, CDN/local loading, decode callbacks, Sokol texture upload/render, watched-state persistence. |
| `client/main.c` | Initializes episodes, mixes episode audio, hooks triggers, updates decode state, uploads one frame per render frame, renders popup UI. |
| `client/hud.c` | Suppresses some HUD affordances while an episode popup is active. |
| `client/palette.h` | Episode UI colors. |
| `client/pl_mpeg.h` | Vendored MPEG decoder. |
| `tests/c/test_episode_lifecycle.c` | Native missing-file, decoder, startup-failure, skip, retry, and stale-callback coverage. |
| `tests/browser-smoke.spec.ts` | Browser 404-to-success retry and persisted watched-state coverage. |

## Playback Lifecycle

1. `episode_init` zeros state and prepares the audio ring buffer.
2. `episode_load` restores the watched bitset from browser `localStorage`.
   Native builds currently treat watched state as session-local only.
3. `episode_trigger` records an uncommitted pending attempt with a monotonic
   token, chooses the filename, and starts an async CDN fetch on Emscripten or
   a local file load on native. Triggering does not change or persist watched
   state.
4. Once bytes are available, `episode_start_playback` creates a `pl_mpeg`
   decoder, validates that it has a usable video stream, and registers
   video/audio callbacks.
5. `episode_update` advances the decoder during normal sim stepping.
6. The first usable video callback converts a frame to RGBA, creates valid
   texture resources, transitions the matching pending token to started, and
   only then persists the watched bit.
7. Fetch, file-read, allocation, decoder, and startup texture failures make
   the pending attempt terminal without setting watched. Decoder destruction
   is deferred until after `plm_decode` returns, and a later frame from that
   same decode call cannot revive the failed attempt.
8. Async callbacks carry the attempt token; callbacks from an older request
   or reset are ignored and cannot commit or clear a newer attempt.
9. `episode_upload_frame` uploads at most one stashed frame per render frame.
   This avoids multiple `sg_update_image` calls for the same image in one
   frame.
10. `episode_render` draws the popup in the bottom-right UI layer.
11. `episode_read_audio` drains decoded audio into the main audio stream.
12. `episode_skip` or decoder end tears down an already-started episode.
    `episode_reset` separately cancels pending or active work and invalidates
    outstanding callback tokens while preserving watched history.

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
- `Esc` is an explicit skip only after the first usable frame has committed
  playback. Pressing it during an uncommitted load does not convert a
  technical failure into a watched episode.
- Death playback is special: the death handler stops any current episode,
  clears watched flags, triggers episode 9, and keeps episode decode/audio
  running while the death cinematic owns input.

This keeps videos secondary to play: the player can keep flying, and the video
does not introduce a separate collection or archive loop.

## Persistence

Browser builds store the watched bitset in:

```text
localStorage["signal_episodes"]
```

The value is a decimal integer with one bit per episode. Native builds do not
currently persist watched state to disk; they rely on in-memory session state.
Browser persistence is updated when playback reaches its first usable frame,
not when an attempt starts. Infrastructure failures therefore leave the bit
clear and a later trigger can retry.

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
3. Add deterministic Sokol resource-failure injection if direct native
   coverage of texture allocation/creation becomes necessary; the current
   native lifecycle matrix covers the fail-closed transition without a GPU
   context.
4. Extend the browser smoke beyond lifecycle state to verify a nonblank
   episode texture plus audio-buffer activity.
5. Revisit the episode 2 trigger if the "all three ore types" milestone is
   still the intended narrative beat.

## Explicitly Out Of Scope

- New episode discovery entities.
- Signal Archive station modules.
- Audio-fragment collection.
- Future video seasons or narrative questlines.
- Any milestone video work that delays core mining, hauling, construction,
  station economy, or rock-combat readability.
