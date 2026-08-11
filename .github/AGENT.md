# Contributor Instructions for cenetex/signal

## What This Is

Signal is a multiplayer space-mining game written in C11 with Sokol
(`sokol_gfx`, `sokol_gl`, `sokol_debugtext`, and `sokol_audio`). World geometry
and HUD drawing are procedural, but the game is asset-light rather than
asset-free: station portraits/MOTD data, MP3 music, and MPEG episode clips are
runtime media described by `assets/manifest.txt`. It runs as a native desktop
client, an authoritative headless server, and an Emscripten web client.

## Build and Test

Use the maintained Make targets so CMake options and safety launchers stay
consistent:

```sh
make build                 # native desktop client
make build-server          # authoritative headless server
make build-web             # Emscripten client
make test                  # fast native suite, sharded
make test-soak             # long-running tests only
make test-all              # fast + soak
make test-serial           # fast suite in one process
make test-san              # ASan + UBSan
make banned-apis deterministic-libm doc-freshness vendor-drift
make cppcheck
```

On Linux, do not invoke `signal_test` directly. Use
`scripts/run_signal_test.sh <binary> [arguments...]`; it enforces the 64 MiB
stack required by legacy `WORLD_DECL` fixtures. The Make test targets already
use that launcher.

## Architecture

### File layout

- `client/` — rendering, HUD, input, audio, networking, and the in-process
  singleplayer server adapter.
- `server/` — authoritative simulation, persistence, AI, chain logging, and
  the multiplayer relay.
- `shared/` — protocol types and dependency-light helpers used by client and
  server.
- `tests/c/` — the native C test runner, harness, and subsystem test files.
- `tests/` — browser, fuzz, and non-C fixtures in addition to `tests/c/`.
- `tools/` — standalone replay, verification, export, and profiling tools.
- `vendor/` — third-party dependencies. Do not edit these except as an
  intentional vendor upgrade.
- `web/` — browser shell and static web files.
- `assets/` — the tracked external-media manifest plus ignored/local runtime
  station, music, and episode files when provisioned.

### Key conventions

- **C11, not C99.** Use `_Static_assert`, designated initializers, and the
  project’s existing C11 patterns.
- **No dynamic allocation in the hot path.** Prefer fixed arrays, bounded
  buffers, and existing arena/ownership conventions.
- **Immediate-mode rendering.** The client redraws with Sokol every frame.
- **Two color scales.** `sdtx_color3b` takes 0–255 byte values;
  `sgl_c4f` takes 0.0–1.0 floats.
- **Server-authoritative.** Clients send intents; the server validates and
  applies them. Authoritative game logic does not belong in `client/`.
- **Shared types are shared.** Changes under `shared/` affect native client,
  server, tests, replay tools, and often WebAssembly.

### HUD and UI files

- `client/hud.c` — flight HUD panels, meters, messages, and damage effects.
- `client/station_ui.c` — docked station panels and actions.
- `client/world_draw.c` — world-space stations, asteroids, borders, and ships.
- `client/render.c` — drawing primitives.
- `client/input.c` — flight, docked, plan, and tow input.
- `client/station_voice.h` — station hail and personality content.

### Simulation files

- `server/game_sim.c` — central authoritative simulation flow.
- `server/sim_production.c` — module production and material delivery.
- `server/sim_save.c` — versioned world/player persistence.

## Working Style

- Keep changes focused on the issue.
- Read the surrounding code before changing conventions.
- Add behavior tests under `tests/c/`; register new test files in
  `CMakeLists.txt` and their registry in `tests/c/test_main.c`.
- Compile both the affected native target and WebAssembly when shared/client
  code changes.
- Treat `assets/manifest.txt` as an inventory, not as proof that every large
  external asset is present in a checkout.
