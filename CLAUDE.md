# Signal — Claude Code Context

Live at **[signal.ratimics.com/play](https://signal.ratimics.com/play)**. Multiplayer space-station game in C99 / Sokol. PvP rock-flinging is the hook; the rest of the systems feed that hook.

## Build

Native desktop:

```sh
cmake -S . -B build
cmake --build build
./build/signal
```

Web / Emscripten:

```sh
emcmake cmake -S . -B build-web
cmake --build build-web
python3 -m http.server 8080 --directory build-web
```

Open `http://127.0.0.1:8080/signal.html`.

Tests (preferred — always rebuilds before running, quiet output, fails loudly):

```sh
make test                   # quiet: failures + summary only
make test TEST_VERBOSE=1    # full per-test stream
```

Or directly:

```sh
cmake -S . -B build-test -DBUILD_TESTS_ONLY=ON
cmake --build build-test
./build-test/signal_test            # verbose
./build-test/signal_test --quiet    # quiet (default for `make test`)
./build-test/signal_test --shard=0/4   # one of 4 parallel shards
```

## Architecture

- **Client / server split**, not a single file anymore. `src/` is the client (render, HUD, input, net, audio, episodes). `server/` is the authoritative sim (physics, production, AI, save, construction). `shared/` holds the wire protocol, types, module schema, and economy constants used by both.
- **Singleplayer** runs the server in-process (`src/local_server.c`). Multiplayer uses the same sim over WebSocket (`server/main.c` with `mongoose`). Sim is fixed-step at `120 Hz`.
- **Rendering** is procedural `sokol_gl` + `sokol_debugtext` for world and HUD. Avatars use `stb_image`; episode cutscenes decode MP4 via `pl_mpeg`; music decodes MP3 via `minimp3`. So: geometry is assetless, but audio + avatars + cutscenes ship as assets under `assets/`.

## Gameplay Loop

1. Launch from a station. Fracture asteroids `XL → L → M → S` with the mining beam.
2. Tractor-sweep fragments of ferrite, cuprite, and crystal ore into the hold.
3. Dock and sell ore, or deliver named contract cargo (manifest / `cargo_unit_t` ingots).
4. Stations internally smelt ore → ingots and fabricate frames / lasers / tractor modules. Press `F` docked to buy the station's primary product.
5. Enter plan mode (`B` undocked) to reserve a module slot or plant a new outpost. Order a scaffold at a shipyard, then tractor-tow the scaffold into place (`E`).
6. Throw rocks at other players.

## Stations

Stations are rotating ring structures with module slots arranged around each arc. Modules are tier-gated (`shared/module_schema.h`) — a module is unlocked once its prerequisite has been ordered at least once. The three seeded stations are currently specialized along commodity lines:

- **Prospect Refinery** — ferrite specialty. Ore intake + iron furnace + frame press. Starter dock.
- **Kepler Yard** — frame/shipyard hub. Sells frames, offers ship upgrades, can print scaffolds.
- **Helios Works** — cuprite + crystal processing, laser and tractor module fabs, its own shipyard.
- **Outposts** — player-planted; begin as scaffolds, materialize once frames are delivered, then grow via module scaffolds snapped onto ring slots.

The economy model is under active redesign toward a stricter 3-tier structure (Prospect = T1 smelt, Kepler = T2 fab, Helios = T3 assembly) — see memory and in-flight issues. Don't assume the current module layout is the intended end state.

## Economy: per-station credits

**There is no global player wallet.** Credits are *per-station ledgers*, keyed by the player's session token. See `station_t.ledger[]` and `currency_name` in `shared/types.h`.

- Each station keeps its own balance for each supplier (e.g. "helios credits", "prospect credits"). Selling ore or completing a contract credits *that station's* ledger.
- Balances are spendable only at the station that issued them.
- Prices are dynamic: station buy-price scales 1.0× (empty hopper) down to 0.5× (full) of `base_price`; product sell-price scales 2.0× (empty) down to 1.0× (full stock). See `station_buy_price` / `station_sell_price` and the `test_dynamic_ore_price_*` tests.
- Players collect pending supplier credits at the station they're standing on — there's no cross-station sweep.

This means haulers and arbitrage are first-class: credits earned at Prospect can't be spent at Helios. Carrying value between stations means carrying *goods*, not currency.

**Stations are sovereign currency issuers.** A station's `credit_pool` can go arbitrarily negative — it represents the running count of currency in circulation, not a bounded resource. There is no money-supply cap, no policy floor, no risk of "the station runs out of money." Cross-station value transfer happens exclusively through goods (the hauler IS the FX desk; the miner is the only source of new value). On-chain wrapping (#480) will eventually allow cross-currency settlement; until then, currency is strictly per-station.

## Signal

Stations emit signal, and signal range matters mechanically. Weak signal throttles mining speed and ship response; players and NPCs get pushed back toward the connected station chain. `H` while undocked in strong signal hails the nearest station and collects pending supplier credits at the station you're nearest to.

## Ships and manifest

Ships carry commodities in their hold. The **manifest layer** (`shared/manifest.h`, `cargo_unit_t`) adds named, traceable ingots — so a specific batch of ferrite ingots smelted at Prospect can be contracted for delivery to Kepler. This is live under the min-flow grade and is the foundation for the T1/T2/T3 chain work.

## Save layout

Per-player saves live under `saves/`:

- `saves/pubkey/<base58(pubkey)>.sav` — once a client has registered
  its persistent Ed25519 pubkey (Layer A.4 of #479). This is the
  canonical layout: a returning player is recognized by their
  cryptographic identity across server restarts and session-token
  rotation.
- `saves/legacy/<token_hex>.sav` — fallback for anonymous / pre-A.1
  clients that haven't registered a pubkey, plus the destination of
  any pre-A.4 saves migrated at startup. Players claim their legacy
  save by signing `"claim-legacy-save-v1" || <token_hex>` with their
  identity secret; the server verifies and renames the legacy file
  into `saves/pubkey/`. First-claim-wins — see #479-A.4.

`world.sav` is unchanged.

## Working Style

- Prefer targeted changes over premature file splits. The codebase already split once; don't split further without need.
- Keep native and wasm builds green after changes. Run the test binary.
- If the docked UI changes, verify fullscreen and narrow browser windows both.
- Don't assume what's in `MEMORY.md` is still true — verify against code before acting on a remembered fact.
- Be careful with economy/ledger invariants. Credits are per-station; do not silently globalize them, and do not assume a single balance exists on the player.
