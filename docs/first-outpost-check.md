# First Outpost release check

`signal_first_outpost` drives a fresh server world with player movement and
normal action inputs. It registers a key, answers the server challenge,
finishes identity admission, and signs crate unpack requests. Mining, smelting,
trade, upgrades, production, and construction run through the game simulation.

Build and run on a native host:

```sh
cmake -S . -B build-route -DCMAKE_BUILD_TYPE=Release -DBUILD_TOOLS=ON
cmake --build build-route --target signal_first_outpost --parallel
./build-route/signal_first_outpost 3600 /tmp/signal-first-outpost-new
```

Choose a fresh directory for every run. The first argument is a duration of
1 to 3,600 game seconds. The driver writes a CSV motion trace to stdout and
milestones to stderr. Exit 0 requires an active relay owned by the player and
successful saved checkpoints. Exit 1 records an incomplete route or failed
checkpoint; exit 2 reports setup or argument errors. Ship loss stops the run.

The route follows the open lane of each occupied station ring. It unpacks
starter ingots at Prospect, delivers the starter refit order at Kepler, feeds
physical ingots into the frame press, and collects frames for an upgrade and
relay construction. The player keeps a picked-up source crate until release.
The station can collect it again after release. Foreign deliveries still use
the normal hopper handoff and station charge boundary.

At payment, upgrades, relay order, activation-frame collection, and outpost
creation, the check writes a complete generation. It loads that generation in
a second world and proves the same player identity. It compares the ship asset,
signed nonce, upgrades, exact held cargo identities, tow counts, station
balances, and outpost ownership. A final checkpoint covers the active relay.

These are deterministic controller results. A new-player session supplies the
release's pacing and clarity evidence. The current results and remaining
checks are in [the release plan](release-plan-v0.6.0.md).

## Server package restart

On a POSIX host, run the server from the extracted archive:

```sh
python3 scripts/check_release_restart.py \
  --server /tmp/extracted-signal-server/signal_server \
  --output /tmp/signal-server-restart-new --protocol 8
```

The check creates a disposable data directory and a fresh local authority key.
It binds localhost, runs two starts, and checks protocol discovery, world time,
four healthy station chains, graceful shutdown, and a new save generation.
It writes a JSON report and server logs in the output directory. The startup
limit is 30 seconds; each graceful shutdown has a 10-second limit. Slow-disk
qualification remains a separate release check.
