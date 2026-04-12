# Agent Instructions for cenetex/signal

## What This Is

A multiplayer space mining game. Pure C11 + Sokol (sokol_gfx, sokol_gl, sokol_debugtext, sokol_audio). Zero external assets — all rendering and audio is procedural. Runs native (CMake) and web (Emscripten).

## Build & Test

```sh
# Native
cmake -S . -B build && cmake --build build
./build/signal --test        # runs all tests (~257)

# Web
emcmake cmake -S . -B build-web && cmake --build build-web
```

Both must stay green. CI runs: build-client, build-server, test-basic, test-static, test-asan, test-valgrind, native (linux/mac/windows).

## Architecture

### File layout
- `src/` — Client code (rendering, HUD, input, audio)
- `server/` — Authoritative sim (game_sim.c, sim_production.c)
- `shared/` — Types and utilities shared between client and server (types.h, station_util.h, module_schema.h)
- `vendor/` — Sokol headers (do not modify)
- `web/` — Emscripten shell (shell.html)
- `src/test_main.c` — All tests in one file

### Key conventions
- **C11, not C99.** Use `_Static_assert`, designated initializers, anonymous structs where appropriate.
- **No dynamic allocation in the hot path.** Fixed arrays, stack buffers, arena patterns.
- **Immediate-mode rendering.** No retained UI, no component tree. Every frame redraws everything via `sokol_gl` (geometry) and `sokol_debugtext` (text).
- **Two color scales.** `sdtx_color3b(r, g, b)` takes 0-255 bytes for text. `sgl_c4f(r, g, b, a)` takes 0.0-1.0 floats for geometry. Both are used throughout HUD code.
- **Server-authoritative.** Client sends intents, server validates and applies. Don't put game logic in `src/`.
- **Shared types are shared.** `shared/types.h` is included by both client and server. Changes there affect both.

### HUD/UI files
- `src/hud.c` / `src/hud.h` — Flight HUD panels, meters, message system, damage vignette
- `src/station_ui.c` — Docked station panel (tabs: STATUS, MARKET, CONTRACTS, SHIPYARD)
- `src/world_draw.c` — World-space rendering (stations, modules, asteroids, signal borders, ships)
- `src/render.c` — Drawing primitives (panels, meters, service cards, pips)
- `src/input.c` — Input handling (flight, docked, plan mode, tow mode)
- `src/station_voice.h` — Station personality: hail conditions, dock tips, voice content

### Sim files
- `server/game_sim.c` — Main simulation: docking, mining, scaffold placement, contracts
- `server/sim_production.c` — Refinery smelting, module flow graph, material delivery

## Working Style

- **Targeted changes.** Don't refactor beyond the issue scope.
- **No premature file splits.** Big files are intentional — `hud.c` at 1300 lines is fine.
- **Test what you change.** If you add a header, make sure it compiles on all platforms. If you change behavior, add or update a test in `test_main.c`.
- **Both scales.** When adding color constants, provide both byte (0-255) and float (0.0-1.0) versions if the color is used in both text and geometry contexts.
- **Read before writing.** The codebase has strong internal conventions. Match the patterns in the file you're editing.
