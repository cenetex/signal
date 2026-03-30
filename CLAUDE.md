# Sokol Space Miner Notes

## Build

Native desktop:

```sh
cmake -S . -B build
cmake --build build
./build/space_miner
```

Web / Emscripten:

```sh
emcmake cmake -S . -B build-web
cmake --build build-web
python3 -m http.server 8080 --directory build-web
```

Open `http://127.0.0.1:8080/space_miner.html`.

## Current Shape

- Single-file C99 prototype in `src/main.c`
- Fixed-step simulation at `120 Hz`
- Rendering and HUD are procedural: `sokol_gl` + `sokol_debugtext`
- Audio is procedural: `sokol_audio`
- No external assets

## Gameplay Loop

- Launch from a station
- Fracture asteroids from `XL -> L -> M -> S`
- Sweep fragments into the hold with the tractor
- Return ore to the refinery
- Use specialist stations for upgrades

## Stations

- `Prospect Refinery`: buys ore, repairs hull
- `Kepler Yard`: repairs hull, upgrades hold capacity
- `Helios Works`: repairs hull, upgrades laser and tractor

`Repair` is common. Raw ore only sells at the refinery.

## Code Landmarks

- `advance_simulation_frame()` / `sim_step()`: fixed-step runtime
- `draw_hud_panels()` / `draw_hud()` / `draw_station_services()`: HUD and docked UI
- `step_mining_system()`: mining beam, fracture, pickup flow
- `step_station_interaction_system()`: docking and station actions
- `draw_station()`: station world rendering

## Working Style

- Prefer targeted changes over premature file splits
- Keep native and wasm builds green after changes
- If the docked UI changes, verify both fullscreen and smaller browser windows
