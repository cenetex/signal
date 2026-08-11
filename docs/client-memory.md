# Client memory budgets

Issue #664 moved the in-process single-player authority world out of
`game_t`. Remote clients now retain only the replicated `game_t::world`; a
second `world_t` is allocated after local-loopback mode is selected and is
released by `local_server_shutdown()`.

## Measured layout

The native layout probe (`make client-memory-budget`) reports the current
working-tree values:

| Item | Bytes |
| --- | ---: |
| `world_t` | 16,880,848 |
| `game_t` | 22,111,520 |
| lazy `local_server_t` | 24 |
| eager `local_server_t` counterfactual | 16,880,856 |
| eager `game_t` counterfactual | 38,992,352 |

The lazy layout removes 16,880,832 bytes (16.10 MiB) from every client's
static state. A local release build made immediately before the change used
1,136 initial WebAssembly pages; the post-change release build uses 880
pages. That is an exact 256-page (16 MiB) reduction before a single-player
world is requested.

Remote startup also no longer explicitly clears all of `game_t` and then
clears `game_t::world` twice. Static storage supplies the initial zero state,
and `world_reset()` is the sole replicated-world cleanup/reset pass. This
avoids touching roughly 38 MiB of redundant memory while preserving the
reusable signal-cache allocation on later resets.

CI ceilings are deliberately close to those measurements:

- `world_t`: 17,000,000 bytes
- `game_t`: 22,250,000 bytes
- release WebAssembly initial memory: 896 pages (56 MiB)

The C layout ceilings are compile-time assertions and are also printed by
`signal_client_memory`. `scripts/check_client_memory_budget.py` reads the
release `.wasm` memory section directly, so debug builds or generated
JavaScript cannot mask a page regression.

## Retained client arrays

The native probe also inventories the large interpolation sidecars:

| Sidecar | Bytes | Decision |
| --- | ---: | --- |
| cargo-pod previous/current state | 2,078,976 | Retain: sparse packet classes can update any pod slot independently, and local tow prediction needs a stable prior/current pair. |
| asteroid previous/current state and clocks | 892,928 | Retain: every terrain/fracture slot is network-addressable and reconciled independently. |
| NPC previous/current state | 14,408 | Retain: bounded and small. |
| player previous/current state | 6,408 | Retain: bounded and small. |
| scaffold previous/current state | 2,568 | Retain: bounded and small. |

Together these sidecars are 2,995,288 bytes (2.86 MiB). Converting the two
larger arrays to sparse maps would change packet application, interpolation,
local prediction adoption, and deterministic smoke fixtures; that is a
separate design change, not part of the authority-ownership fix.

The local-loopback serializer also retains 320,870 bytes of static bounded
packet scratch. It is shared across local frames and avoids large stack
frames. Lazily allocating it would save only 0.31 MiB remotely while adding
another failure-bearing lifetime, so it remains static for now.
