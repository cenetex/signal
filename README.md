# Sokol Space Miner

A small C + Sokol prototype with a simple mining loop:

- Fly a ship with `W/S` and `A/D`
- Line up asteroids and hold `Space` to mine ore
- Sweep through fragments to pull them into the hold
- Enter the station ring, press `E` to dock, then use station services:
  `1` sell cargo, `2` repair, `3` mining upgrade, `4` cargo upgrade, `5` tractor upgrade

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
