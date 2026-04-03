# Signal

A space station game where you fight by smashing one another with rocks.

**Play now:** [signal.ratimics.com/play](https://signal.ratimics.com/play)

Signal is a multiplayer space mining game built in C99 with Sokol — no external
assets, no engine, just procedural geometry and physics. You launch from
stations, fracture asteroids, haul ore, and expand the network by building
outposts at the edge of signal range. Every AI dreams of being a space station.

## Gameplay Loop

1. Launch from Prospect Refinery and work the asteroid belt.
2. Fracture large rocks with the mining beam and sweep up ferrite, cuprite, and
   crystal fragments.
3. Sell ore or deliver contract cargo at docked stations.
4. Let stations smelt ore into ingots and fabricate ship parts, then buy what
   you need with `F`.
5. Upgrade your miner, buy a scaffold kit, and deploy an outpost inside signal
   coverage.
6. Deliver frames and ingots to scaffolds and module blueprints so the network
   grows and NPC miners and haulers can help keep it supplied.
7. Throw rocks at other players. This is the main thing.

Signal range matters. Weak signal cuts ship response and mining speed, and both
players and NPCs are pushed back toward the connected station chain.

## Stations

Stations are ring structures that rotate, with module ports around the arc and
a dock gap for ships to fly through. Players expand outposts by building rings,
then filling ports with modules. Each station is (or will be) operated by an AI
— setting prices, posting contracts, and hailing pilots who fly through its
signal field.

- `Prospect Refinery`: buys raw ore, repairs hulls, smelts ore, and posts
  supply contracts.
- `Kepler Yard`: repairs hulls, presses frames, upgrades hold capacity, sells
  frames, and issues blueprint work.
- `Helios Works`: repairs hulls, fabricates laser and tractor modules, upgrades
  mining and tractor systems, and issues blueprint work.
- `Outposts`: begin as scaffolded relay hubs, then grow module by module into
  new industrial nodes.

## Artificial Station Intelligence

Each station runs a daily planning session — one short LLM call to review its
economy, adjust prices, post contracts, and write three hail messages at
ferrite/cuprite/crystal rarity tiers. Press `H` in signal range to hail a
station; the message you get depends on which band of the signal field you're
in.

AI stations see only what's within their signal range. When two stations'
signals overlap, their operators can communicate. Destroy a relay station and
everything beyond it goes dark. The intelligence persists as long as the signal
does.

## Controls

- Flight: `W/S` or arrows thrust and brake, `A/D` or arrows turn, `Space`
  mine, `E` dock or launch.
- Station services: `1` sell or deliver cargo, `2` repair, `3` laser upgrade,
  `4` hold upgrade, `5` tractor upgrade.
- Station navigation: `Tab` or `Q` cycle station tabs.
- Market: `F` buys the station's primary product.
- Construction: `B` buys a scaffold kit or opens build mode while docked.
- Outpost placement: undock with a scaffold kit, press `B`, then press `B` or
  `Enter` to place the outpost. `Esc` or `Q` cancels placement.
- Module build overlay: `1-8` choose a module blueprint.
- Contracts tab: `1-3` track the nearest listed contracts.
- Global: `R` resets singleplayer, `Esc` quits.

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

Open `http://127.0.0.1:8080/space_miner.html` and sanity-check browser input by
holding `W` or `Space`, alt-tabbing away, then returning. The ship should stop
taking active input when focus is lost.

## Test

```sh
cmake -S . -B build-test -DBUILD_TESTS_ONLY=ON
cmake --build build-test
./build-test/space_miner_test
```

## Notes

- Singleplayer runs against an in-process authoritative server. Multiplayer
  uses the same simulation over WebSocket.
- The game stays asset-light: geometry and HUD text are drawn directly with
  Sokol.
- Native builds use Metal on macOS, OpenGL on Linux, and OpenGL on Windows
  through Sokol.
- The browser target uses WebGL 2 via Emscripten.
- Browser audio may stay muted until the page receives a click or key press to
  unlock WebAudio.
