# Sokol Space Miner

A small C + Sokol prototype with a simple mining loop:

- Fly a ship with `W/S` and `A/D`
- Line up asteroids and hold `Space` to fracture rocks
- Sweep through fragments to pull ferrite, cuprite, and crystal ore into the hold
- Enter a station ring and press `E` to dock
- Use specialist stations:
  `Prospect Refinery` buys raw ore, repairs, and stocks ingots
  `Kepler Yard` repairs and expands the hold
  `Helios Works` repairs and tunes the laser/tractor

## Controls

- Flight: `W/S` thrust, `A/D` turn, `Space` mine, `E` dock
- Docked everywhere: `2` repair, `E` launch
- Refinery: `1` sell ore
- Yard: `4` upgrade hold
- Beamworks: `3` upgrade laser, `5` upgrade tractor
- Global: `R` reset, `Esc` quit

## Build

Native desktop:

```sh
cmake -S . -B build
cmake --build build
./build/space_miner
```

Browser / WebAssembly with Emscripten:

```sh
emcmake cmake -S . -B build-web
cmake --build build-web
python3 -m http.server 8080 --directory build-web
```

That produces `build-web/space_miner.html` plus the `.js` and `.wasm` files.

Open `http://127.0.0.1:8080/space_miner.html` and sanity-check browser input by holding `W` or `Space`, alt-tabbing away, then returning. The ship should stop taking active input when focus is lost.

## Notes

- The game is set up to stay asset-free: geometry and HUD text are drawn directly with Sokol.
- Native builds use Metal on macOS, OpenGL on Linux, and OpenGL on Windows through Sokol.
- The browser target uses WebGL 2 via Emscripten.
- Browser audio may stay muted until the page receives a click or key press to unlock WebAudio.
