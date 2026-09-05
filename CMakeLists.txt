cmake_minimum_required(VERSION 3.21)

project(signal C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Crypto backend selector for shared/signal_crypto.h. Currently only
# tweetnacl is supported; the option is forward-looking — if a future
# slice swaps to libsodium for Argon2 or batch-verify, only the impl
# .c file changes (the public interface in shared/signal_crypto.h is
# stable). See vendor/tweetnacl/signal_crypto_tweetnacl.c.
set(SIGNAL_CRYPTO_BACKEND "tweetnacl" CACHE STRING
    "Crypto backend (tweetnacl|libsodium). Only tweetnacl is implemented.")
if(NOT SIGNAL_CRYPTO_BACKEND STREQUAL "tweetnacl")
    message(FATAL_ERROR
        "SIGNAL_CRYPTO_BACKEND=${SIGNAL_CRYPTO_BACKEND} not implemented; "
        "use 'tweetnacl'.")
endif()

set(SIGNAL_CRYPTO_UPSTREAM_SOURCES
    vendor/tweetnacl/tweetnacl.c
)
set(SIGNAL_CRYPTO_OWNED_SOURCES
    vendor/tweetnacl/randombytes.c
    vendor/tweetnacl/signal_crypto_tweetnacl.c
    shared/signal_memzero.c
)
set(SIGNAL_CRYPTO_SOURCES
    ${SIGNAL_CRYPTO_UPSTREAM_SOURCES}
    ${SIGNAL_CRYPTO_OWNED_SOURCES}
    vendor/raticross/raticross.c
)
set(SIGNAL_CRYPTO_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tweetnacl"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/raticross"
)

set(RATICROSS_SOURCES
    vendor/raticross/raticross.c
)
# Vendored TweetNaCl is public-domain and not written with -Wpedantic
# in mind (uses unsigned long for u32, etc.). Compile with /W0 (MSVC)
# or -w (GCC/Clang) so it doesn't trip our -Werror at the project level.
#
# UBSan flags also need to be loosened: tweetnacl.c does signed left
# shifts that are UB by the letter of C99 but are deliberate in
# reference crypto (matching what NaCl, libsodium, Bitcoin all ship).
# Disable shift + signed-overflow sanitizers for that upstream file only.
# Project-owned entropy, wrapper, and wipe code remain under the normal
# warning policy and full sanitizer instrumentation.
# `-fno-sanitize=...` is silently accepted in non-sanitized builds.
if(MSVC)
    set_source_files_properties(${SIGNAL_CRYPTO_UPSTREAM_SOURCES}
        PROPERTIES COMPILE_OPTIONS "/W0")
    set_source_files_properties(vendor/liblecore/lecore.c
        PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(${SIGNAL_CRYPTO_UPSTREAM_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-w;-fno-sanitize=shift;-fno-sanitize=signed-integer-overflow")
    set_source_files_properties(vendor/liblecore/lecore.c
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

# Always emit compile_commands.json so clangd / editor LSPs can resolve
# include paths without per-developer config. The Makefile's `build`
# target symlinks this into the repo root after configure.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build-mode toggles. Exactly one of these may be ON at a time; with none
# set, every target (server, test, client) is built.
option(BUILD_TESTS_ONLY  "Only build the test executable"       OFF)
option(BUILD_SERVER_ONLY "Only build the headless game server"  OFF)
# Layer E of #479: standalone CLI tools for verifying/exporting station
# chain logs post-mortem. Default ON so dev builds get them; the
# server-only Docker image flips it off.
option(BUILD_TOOLS       "Build standalone CLI tools (signal_verify, signal_chain_assets, signal_rati_receipt, signal_replay)" ON)
option(BUILD_WASM_REPLAY "Build signal_replay as an Emscripten Node CLI" OFF)
option(SIGNAL_SIM_PROFILE "Compile world_sim_step phase profiling timers" OFF)
option(SIGNAL_HNN_ENABLE_LECORE
    "Compile the pinned liblecore pilot dependency"
    OFF)
set(SIGNAL_HNN_BACKEND "builtin" CACHE STRING
    "HNN numeric backend: builtin, lecore-direct, or lecore-radix2")
set_property(CACHE SIGNAL_HNN_BACKEND PROPERTY STRINGS
    builtin lecore-direct lecore-radix2)
string(TOLOWER "${SIGNAL_HNN_BACKEND}" SIGNAL_HNN_BACKEND_NORMALIZED)
if(NOT SIGNAL_HNN_BACKEND_NORMALIZED STREQUAL "builtin" AND
   NOT SIGNAL_HNN_BACKEND_NORMALIZED STREQUAL "lecore-direct" AND
   NOT SIGNAL_HNN_BACKEND_NORMALIZED STREQUAL "lecore-radix2")
    message(FATAL_ERROR
        "SIGNAL_HNN_BACKEND must be builtin, lecore-direct, or lecore-radix2")
endif()
if(NOT SIGNAL_HNN_BACKEND_NORMALIZED STREQUAL "builtin")
    set(SIGNAL_HNN_ENABLE_LECORE ON)
endif()

set(SIGNAL_LECORE_C_SHA256
    "f3283ebb033e295e5dbdc46d95add91ab154253169d6a2a1ab696464b051ed07")
set(SIGNAL_LECORE_H_SHA256
    "0ce1fb68095323450087b18b4f8fcbf76ec74fbb68e936f5649f2494b2634302")
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/vendor/liblecore/lecore.c"
    SIGNAL_LECORE_C_ACTUAL_SHA256)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/vendor/liblecore/lecore.h"
    SIGNAL_LECORE_H_ACTUAL_SHA256)
if(NOT SIGNAL_LECORE_C_ACTUAL_SHA256 STREQUAL SIGNAL_LECORE_C_SHA256 OR
   NOT SIGNAL_LECORE_H_ACTUAL_SHA256 STREQUAL SIGNAL_LECORE_H_SHA256)
    message(FATAL_ERROR
        "Pinned liblecore amalgamation checksum mismatch; see vendor/liblecore/PIN.md")
endif()

if(SIGNAL_HNN_ENABLE_LECORE)
    add_compile_definitions(
        SIGNAL_HNN_LECORE_AVAILABLE=1
        LECORE_ENABLE_FORMAT=0)
endif()
if(SIGNAL_HNN_BACKEND_NORMALIZED STREQUAL "lecore-direct")
    add_compile_definitions(SIGNAL_HNN_BACKEND_LECORE_DIRECT=1)
elseif(SIGNAL_HNN_BACKEND_NORMALIZED STREQUAL "lecore-radix2")
    add_compile_definitions(SIGNAL_HNN_BACKEND_LECORE_RADIX2=1)
endif()

if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
endif()

function(signal_apply_sim_profile target_name)
    if(SIGNAL_SIM_PROFILE)
        target_compile_definitions(${target_name} PRIVATE SIM_PROFILE=1)
    endif()
endfunction()

# --- GIT_HASH baked into binaries for /health and client mismatch checks ---
# Captured at configure time. To refresh after a commit/pull without
# nuking the cache, re-run `cmake -S . -B <build_dir> -DGIT_HASH=$(git
# rev-parse --short HEAD)`. Otherwise the value sticks across rebuilds
# and the browser-visible /health version drifts behind HEAD.
if(NOT DEFINED GIT_HASH)
    execute_process(COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT GIT_HASH)
    set(GIT_HASH "dev")
endif()
add_compile_definitions(SIGNAL_SOURCE_REVISION="${GIT_HASH}")

# ------------------------------------------------------------------
# Canonical source lists — every target picks from these. Adding a
# new server .c file only needs editing here.
# ------------------------------------------------------------------

set(SIGNAL_STATIC_FLIGHT_BRAIN_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/client/integration/work/signal")
set(SIGNAL_STATIC_FLIGHT_BRAIN_SOURCES)
if(EXISTS "${SIGNAL_STATIC_FLIGHT_BRAIN_DIR}/signal_client_flight.c" AND
   EXISTS "${SIGNAL_STATIC_FLIGHT_BRAIN_DIR}/signal_client_flight.h")
    list(APPEND SIGNAL_STATIC_FLIGHT_BRAIN_SOURCES
        client/integration/work/signal/signal_client_flight.c)
    add_compile_definitions(SIGNAL_HAS_STATIC_FLIGHT_BRAIN=1)
endif()

# The generated inference loops request vectorization for production builds.
# Clang's sanitizer instrumentation can make that requested transform
# impossible and reports the miss as -Wpass-failed, which becomes fatal under
# the project's -Werror policy. Suppress only that optimizer diagnostic, only
# for the generated source, and only in Clang sanitizer configurations; normal
# builds still fail if the production optimization unexpectedly regresses.
if(SIGNAL_STATIC_FLIGHT_BRAIN_SOURCES AND
   CMAKE_C_COMPILER_ID MATCHES "Clang" AND
   CMAKE_C_FLAGS MATCHES "(^| )-fsanitize=")
    set_property(SOURCE ${SIGNAL_STATIC_FLIGHT_BRAIN_SOURCES} APPEND PROPERTY
        COMPILE_OPTIONS -Wno-pass-failed)
endif()

# Headless simulation — used by signal_server, signal_test, and the
# client (which runs the same sim in-process for singleplayer via
# local_server.c). New server sim files go here.
set(SIGNAL_SIM_SOURCES
    server/game_sim.c
    server/state_digest.c
    server/actor_principal.c
    server/public_actor_resolver.c
    server/contract_ownership.c
    server/ownership_quarantine.c
    server/player_identity_registry.c
    server/cargo_legacy_classify.c
    server/cargo_legacy_inventory.c
    server/cargo_craft_provenance.c
    server/cargo_smelt_provenance.c
    server/ship_birth_reservation.c
    server/sim_ai.c
    server/gossip.c
    server/sim_autopilot.c
    server/signal_brain.c
    server/signal_intelligence.c
    server/signal_contract_brain.c
    server/signal_npc_worker_brain.c
    server/sim_flight.c
    server/sim_nav.c
    server/sim_save.c
    server/sim_catalog.c
    server/station_authority.c
    server/chain_log.c
    server/chain_log_verify.c
    server/cargo_receipt_issue.c
    server/cargo_receipt_trust.c
    server/handoff_flow.c
    server/sim_asteroid.c
    server/sim_physics.c
    server/sim_production.c
    server/sim_construction.c
    server/sim_mining.c
    server/sim_anchor.c
    server/sim_ship.c
)

# Native authoritative persistence orchestration.  This is intentionally not
# part of SIGNAL_SIM_SOURCES: browser/single-player simulations do not own the
# server's crash-consistent generation directory.
set(SIGNAL_SERVER_PERSISTENCE_SOURCES
    server/legacy_save_recovery.c
    server/persistence_generation.c
)

# Shared libraries used by both sim and client render — pure C, no
# sokol / audio / GL dependencies.
set(SIGNAL_SIM_HELPERS
    shared/actor_principal.c
    shared/public_actor_id.c
    shared/cell_geometry.c
    shared/cell_stress.c
    shared/cargo_package.c
    shared/commodity.c
    shared/manifest.c
    shared/ship.c
    shared/economy.c
    shared/asteroid.c
    shared/rng.c
    shared/fixpoint.c
    shared/belt.c
    shared/module_schema.c
    shared/station_util.c
    shared/cargo_receipt.c
    shared/pubkey_proof.c
    shared/rx_envelope.c
    shared/handoff_ticket.c
    shared/tractor.c
    shared/laser.c
    shared/trade_paging.c
    shared/settlement_engine.c
    shared/holographic_nn.c
    shared/holographic_nn_backend.c
    shared/holographic_nn_confidence.c
    shared/signal_field.c
    shared/npc_radio.c
)
if(SIGNAL_HNN_ENABLE_LECORE)
    list(APPEND SIGNAL_SIM_HELPERS vendor/liblecore/lecore.c)
endif()
set(SIGNAL_TEST_LECORE_SOURCES)
if(NOT SIGNAL_HNN_ENABLE_LECORE)
    list(APPEND SIGNAL_TEST_LECORE_SOURCES vendor/liblecore/lecore.c)
endif()

# Client-only: rendering, input, HUD, audio, network plumbing. Not
# compiled into signal_server or signal_test.
set(SIGNAL_CLIENT_SOURCES
    client/main.c
    client/public_actor_presentation.c
    client/legacy_recovery_ui.c
    client/legacy_recovery_view.c
    client/client_log.c
    client/reconciliation_diagnostics.c
    client/gameplay_observability.c
    client/asteroid_presentation.c
    client/tow_presentation_diagnostics.c
    client/tow_adverse_gate.c
    client/zero_latency_gate.c
    client/net.c
    client/station_ui.c
    client/hud.c
    client/world_draw.c
    client/audio.c
    client/npc.c
    client/render.c
    client/input.c
    client/net_sync.c
    client/neural_singleplayer.c
    client/integration/work/signal/signal_client_brain.c
    client/integration/work/signal/signal_client_flight.c
    client/integration/work/signal/signal_client_tactical.c
    client/integration/work/signal/signal_client_strategic.c
    client/local_authority.c
    client/local_server.c
    client/onboarding.c
    client/story_runtime.c
    client/progress_store.c
    shared/story_loop.c
    client/contract_objective.c
    client/episode_lifecycle.c
    client/episode_media.c
    client/episode.c
    client/music.c
    client/avatar.c
    client/mining_client.c
    client/identity.c
)

set(SIGNAL_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/client"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared"
    "${CMAKE_CURRENT_SOURCE_DIR}/server"
)
if(SIGNAL_HNN_ENABLE_LECORE)
    list(APPEND SIGNAL_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/liblecore")
endif()
if(SIGNAL_STATIC_FLIGHT_BRAIN_SOURCES)
    list(APPEND SIGNAL_INCLUDE_DIRS "${SIGNAL_STATIC_FLIGHT_BRAIN_DIR}")
endif()

# ------------------------------------------------------------------
# signal_server — authoritative game server (also the Docker image).
# ------------------------------------------------------------------
if(NOT EMSCRIPTEN AND NOT BUILD_TESTS_ONLY)
    add_executable(signal_server
        server/main.c
        server/mongoose.c
        server/highscore.c
        server/ws_outbox.c
        server/persistence_writer.c
        ${SIGNAL_SERVER_PERSISTENCE_SOURCES}
        ${SIGNAL_SIM_SOURCES}
        ${SIGNAL_SIM_HELPERS}
        ${SIGNAL_STATIC_FLIGHT_BRAIN_SOURCES}
        ${SIGNAL_CRYPTO_SOURCES}
    )
    target_include_directories(signal_server PRIVATE
        ${SIGNAL_INCLUDE_DIRS}
        ${SIGNAL_CRYPTO_INCLUDE_DIR})
    target_compile_definitions(signal_server PRIVATE
        GIT_HASH="${GIT_HASH}"
        MG_ENABLE_LOG=0    # silence mongoose's internal [INFO]/[ERROR] spam
        SIGNAL_SERVER_PERSISTENCE=1
        GAME_SIM_VERBOSE   # enable [sim] log lines (matches prod Docker)
    )
    signal_apply_sim_profile(signal_server)
    if(NOT MSVC)
        target_link_libraries(signal_server PRIVATE m Threads::Threads)
        target_compile_options(signal_server PRIVATE
            -Wall -Wextra -Wpedantic -Werror -fstack-protector-strong -ffp-contract=off -fno-fast-math)
        # _FORTIFY_SOURCE requires optimization; only set for Release-ish.
        target_compile_definitions(signal_server PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>)
    else()
        target_link_libraries(signal_server PRIVATE Threads::Threads)
        # Mirror GCC/Clang -Werror so Windows CI fails on regressions.
        # Selective /wd* mirror the -Wno-* we accept on GCC/Clang:
        #   4127 = constant conditional (common in ASSERT/while(true))
        #   4459 = local hides global (we use r,g,b as color params)
        #   4996 = deprecated POSIX names (setbuf/sscanf — _CRT_SECURE_NO_WARNINGS is set)
        #   5287 = mixed unnamed-enum types in comparisons (design constants)
        target_compile_options(signal_server PRIVATE
            /W4 /WX /wd4127 /wd4459 /wd4996 /wd5287)
        target_compile_definitions(signal_server PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    # mongoose.c needs POSIX + BSD APIs on Linux (ip_mreq, alloca, etc.)
    if(UNIX AND NOT APPLE)
        target_compile_definitions(signal_server PRIVATE _GNU_SOURCE)
    endif()
endif()

# ------------------------------------------------------------------
# signal_test — unit + integration tests. Skipped for Docker-only
# builds and for emscripten.
# ------------------------------------------------------------------
if(NOT EMSCRIPTEN AND NOT BUILD_SERVER_ONLY)
    add_executable(signal_test
        tests/c/test_main.c
        tests/c/test_harness.c
        tests/c/test_commodity.c
        tests/c/test_math.c
        tests/c/test_ship.c
        tests/c/test_client_log.c
        tests/c/test_episode_lifecycle.c
        tests/c/test_story_loop.c
        tests/c/test_progress_store.c
        tests/c/test_local_authority.c
        tests/c/test_legacy_recovery_ui.c
        tests/c/test_cell_geometry.c
        tests/c/test_cell_stress.c
        tests/c/test_cargo_package.c
        tests/c/test_manifest.c
        tests/c/test_economy.c
        tests/c/test_world_sim.c
        tests/c/test_protocol.c
        tests/c/test_actor_principal.c
        tests/c/test_public_actor_id.c
        tests/c/test_public_actor_resolver.c
        tests/c/test_public_actor_presentation.c
        tests/c/test_contract_ownership.c
        tests/c/test_station_catalog.c
        tests/c/test_ownership_quarantine.c
        tests/c/test_player_identity_registry.c
        tests/c/test_cargo_legacy_classify.c
        tests/c/test_cargo_legacy_inventory.c
        tests/c/test_cargo_craft_provenance.c
        tests/c/test_cargo_smelt_provenance.c
        tests/c/test_ship_birth_reservation.c
        tests/c/test_bug_regression.c
        tests/c/test_save.c
        tests/c/test_persistence_generation.c
        tests/c/test_construction.c
        tests/c/test_settlement_engine.c
        tests/c/test_tractor.c
        tests/c/test_laser.c
        tests/c/test_navigation.c
        tests/c/test_econ_sim.c
        tests/c/test_asteroid.c
        tests/c/test_chain.c
        tests/c/test_labels.c
        tests/c/test_pvp_rocks.c
        tests/c/test_crypto.c
        tests/c/test_identity.c
        tests/c/test_registry.c
        tests/c/test_signed_action.c
        tests/c/test_save_keyed_by_pubkey.c
        tests/c/test_anchor.c
        tests/c/test_station_authority.c
        tests/c/test_rx_envelope.c
        tests/c/test_chain_log.c
        tests/c/test_highscore_replay.c
        server/highscore.c
        tests/c/test_signal_verify.c
        tests/c/test_cross_station_settlement.c
        tests/c/test_sovereign_ledger.c
        tests/c/test_prefix_class_pricing.c
        tests/c/test_furnace_color.c
        tests/c/test_respawn_fee.c
        tests/c/test_relationship_tracking.c
        tests/c/test_motd_rarity.c
        tests/c/test_cargo_lineage.c
        tests/c/test_trade_paging.c
        tests/c/test_inspect_anim.c
        tests/c/test_inspect_labels.c
        tests/c/test_route_history_labels.c
        tests/c/test_rock_usefulness.c
        tests/c/test_ws_outbox.c
        server/mongoose.c
        server/ws_outbox.c
        server/persistence_writer.c
        tests/c/test_signal_field.c
        tests/c/test_state_digest.c
        tests/c/test_reconciliation_diagnostics.c
        tests/c/test_gameplay_observability.c
        tests/c/test_asteroid_presentation.c
        tests/c/test_tow_presentation_diagnostics.c
        tests/c/test_hnn_backend.c
        tests/c/test_hnn_confidence.c
        tests/c/test_gossip.c
        tests/c/test_npc_radio.c
        tests/c/test_ai_feature_contract.c
        tests/c/test_hud_attention.c
        client/identity.c
        client/progress_store.c
        client/public_actor_presentation.c
        client/client_log.c
        client/reconciliation_diagnostics.c
        client/gameplay_observability.c
        client/asteroid_presentation.c
        client/tow_presentation_diagnostics.c
        client/episode_lifecycle.c
        client/episode_media.c
        shared/story_loop.c
        client/local_authority.c
        client/legacy_recovery_ui.c
        ${SIGNAL_SERVER_PERSISTENCE_SOURCES}
        ${SIGNAL_SIM_SOURCES}
        ${SIGNAL_SIM_HELPERS}
        ${SIGNAL_TEST_LECORE_SOURCES}
        ${SIGNAL_STATIC_FLIGHT_BRAIN_SOURCES}
        ${SIGNAL_CRYPTO_SOURCES}
    )
    target_include_directories(signal_test PRIVATE
        ${SIGNAL_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/liblecore"
        ${SIGNAL_CRYPTO_INCLUDE_DIR})
    target_compile_definitions(signal_test PRIVATE
        MG_ENABLE_LOG=0
        SIGNAL_CRYPTO_TESTING=1
        SIGNAL_CARGO_RECEIPT_TESTING=1
        SIGNAL_CHAIN_LOG_TESTING=1
        SIGNAL_HNN_LECORE_AVAILABLE=1
        LECORE_ENABLE_FORMAT=0
        SIGNAL_SAVE_TESTING=1
        SIGNAL_SERVER_PERSISTENCE=1
        SIGNAL_LOCAL_AUTHORITY_TESTING=1
        SIGNAL_SPATIAL_GRID_TESTING=1)
    if(UNIX AND NOT APPLE)
        target_compile_definitions(signal_test PRIVATE _GNU_SOURCE)
    endif()
    signal_apply_sim_profile(signal_test)
    target_link_libraries(signal_test PRIVATE Threads::Threads)
    if(NOT MSVC)
        target_link_libraries(signal_test PRIVATE m)
        target_compile_options(signal_test PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
        if(APPLE)
            # world_t is intentionally large (~11 MiB after provenance/replay
            # state grew). Many legacy tests use WORLD_DECL stack allocation,
            # so give the macOS test runner a main-thread stack that can hold
            # one world plus normal test frames.
            target_link_options(signal_test PRIVATE -Wl,-stack_size,0x4000000)
        endif()
    else()
        target_compile_options(signal_test PRIVATE
            /W4 /WX /wd4127 /wd4459 /wd4996 /wd5287)
        target_compile_definitions(signal_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

# A tiny native layout probe enforces and reports the client-resident struct
# budgets without starting a window or linking the full client.
if(NOT EMSCRIPTEN AND NOT BUILD_SERVER_ONLY)
    add_executable(signal_client_memory tools/signal_client_memory.c)
    target_include_directories(signal_client_memory SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sokol"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor"
    )
    target_include_directories(signal_client_memory PRIVATE
        ${SIGNAL_INCLUDE_DIRS})
    if(NOT MSVC)
        target_compile_options(signal_client_memory PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    else()
        target_compile_options(signal_client_memory PRIVATE
            /W4 /WX /wd4127 /wd4459 /wd4996 /wd5287)
        target_compile_definitions(signal_client_memory PRIVATE
            _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

# ------------------------------------------------------------------
# signal_verify / signal_chain_assets — standalone post-mortem chain-log
# tools. Pure C, no sim / world_t / sokol deps. Can run on archived logs,
# by anyone, without a live server.
# ------------------------------------------------------------------
if(BUILD_TOOLS AND NOT EMSCRIPTEN)
    add_executable(signal_verify
        tools/signal_verify.c
        server/cargo_craft_provenance.c
        server/cargo_legacy_classify.c
        server/chain_log_verify.c
        ${SIGNAL_CRYPTO_SOURCES}
    )
    target_include_directories(signal_verify PRIVATE
        ${SIGNAL_INCLUDE_DIRS}
        ${SIGNAL_CRYPTO_INCLUDE_DIR})
    if(NOT MSVC)
        target_compile_options(signal_verify PRIVATE
            -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
    else()
        target_compile_options(signal_verify PRIVATE
            /W4 /WX /wd4127 /wd4459 /wd4996 /wd5287)
        target_compile_definitions(signal_verify PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    add_executable(signal_chain_assets
        tools/signal_chain_assets.c
        server/cargo_craft_provenance.c
        server/cargo_legacy_classify.c
        ${SIGNAL_CRYPTO_SOURCES}
    )
    target_include_directories(signal_chain_assets PRIVATE
        ${SIGNAL_INCLUDE_DIRS}
        ${SIGNAL_CRYPTO_INCLUDE_DIR})
    if(NOT MSVC)
        target_compile_options(signal_chain_assets PRIVATE
            -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
    else()
        target_compile_options(signal_chain_assets PRIVATE
            /W4 /WX /wd4127 /wd4459 /wd4996 /wd5287)
        target_compile_definitions(signal_chain_assets PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    add_executable(signal_rati_receipt
        tools/signal_rati_receipt.c
        server/cargo_craft_provenance.c
        server/cargo_smelt_provenance.c
        server/cargo_legacy_classify.c
        server/chain_log_verify.c
        ${SIGNAL_CRYPTO_SOURCES}
    )
    target_include_directories(signal_rati_receipt PRIVATE
        ${SIGNAL_INCLUDE_DIRS}
        ${SIGNAL_CRYPTO_INCLUDE_DIR})
    if(NOT MSVC)
        target_compile_options(signal_rati_receipt PRIVATE
            -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
    else()
        target_compile_options(signal_rati_receipt PRIVATE
            /W4 /WX /wd4127 /wd4459 /wd4996 /wd5287)
        target_compile_definitions(signal_rati_receipt PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    add_executable(signal_hnn_pilot_benchmark
        tools/signal_hnn_pilot_benchmark.c
        shared/holographic_nn_backend.c
        shared/fixpoint.c
        vendor/liblecore/lecore.c
    )
    target_include_directories(signal_hnn_pilot_benchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/shared"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/liblecore")
    target_compile_definitions(signal_hnn_pilot_benchmark PRIVATE
        SIGNAL_HNN_LECORE_AVAILABLE=1
        LECORE_ENABLE_FORMAT=0)
    if(NOT MSVC)
        target_link_libraries(signal_hnn_pilot_benchmark PRIVATE m)
        target_compile_options(signal_hnn_pilot_benchmark PRIVATE
            -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
    else()
        target_compile_options(signal_hnn_pilot_benchmark PRIVATE
            /W4 /WX /wd4127 /wd4459 /wd4996 /wd5287)
        target_compile_definitions(signal_hnn_pilot_benchmark PRIVATE
            _CRT_SECURE_NO_WARNINGS)
    endif()

    add_executable(flight_trace
        tools/flight_trace.c
        ${SIGNAL_SIM_SOURCES}
        ${SIGNAL_SERVER_PERSISTENCE_SOURCES}
        ${SIGNAL_SIM_HELPERS}
        ${SIGNAL_STATIC_FLIGHT_BRAIN_SOURCES}
        ${SIGNAL_CRYPTO_SOURCES}
    )
    target_include_directories(flight_trace PRIVATE
        ${SIGNAL_INCLUDE_DIRS}
        ${SIGNAL_CRYPTO_INCLUDE_DIR})
    target_compile_definitions(
        flight_trace PRIVATE SIGNAL_SERVER_PERSISTENCE=1)
    signal_apply_sim_profile(flight_trace)
    if(NOT MSVC)
        target_link_libraries(flight_trace PRIVATE m)
        target_compile_options(flight_trace PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
    else()
        target_compile_options(flight_trace PRIVATE
            /W4 /WX /wd4127 /wd4459 /wd4996 /wd5287)
        target_compile_definitions(flight_trace PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    add_executable(signal_replay
        tools/signal_replay.c
        ${SIGNAL_SIM_SOURCES}
        ${SIGNAL_SERVER_PERSISTENCE_SOURCES}
        ${SIGNAL_SIM_HELPERS}
        ${SIGNAL_STATIC_FLIGHT_BRAIN_SOURCES}
        ${SIGNAL_CRYPTO_SOURCES}
    )
    target_include_directories(signal_replay PRIVATE
        ${SIGNAL_INCLUDE_DIRS}
        ${SIGNAL_CRYPTO_INCLUDE_DIR})
    target_compile_definitions(
        signal_replay PRIVATE SIGNAL_SERVER_PERSISTENCE=1)
    signal_apply_sim_profile(signal_replay)
    if(NOT MSVC)
        target_link_libraries(signal_replay PRIVATE m)
        target_compile_options(signal_replay PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
    else()
        target_compile_options(signal_replay PRIVATE
            /W4 /WX /wd4127 /wd4459 /wd4996 /wd5287)
        target_compile_definitions(signal_replay PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

# ------------------------------------------------------------------
# Fuzz targets (libFuzzer, clang only). Off by default; enable with
# -DSIGNAL_BUILD_FUZZERS=ON. Headless builders should also set
# -DBUILD_TESTS_ONLY=ON -DBUILD_TOOLS=OFF so decoder fuzzing never discovers
# desktop audio/window dependencies. Harnesses live in tests/fuzz/ and probe
# untrusted decode paths (cargo receipts, handoff tickets/snapshots).
# See REMEDIATION_PLAN.md §12.
# ------------------------------------------------------------------
option(SIGNAL_BUILD_FUZZERS "Build libFuzzer harnesses (clang only)" OFF)
if(SIGNAL_BUILD_FUZZERS)
    if(NOT BUILD_TESTS_ONLY OR BUILD_TOOLS)
        message(FATAL_ERROR
            "SIGNAL_BUILD_FUZZERS is a headless decoder configuration. "
            "Reconfigure with -DBUILD_TESTS_ONLY=ON -DBUILD_TOOLS=OFF; "
            "desktop ALSA/X11/OpenGL dependencies must not enter this graph.")
    endif()
    if(EMSCRIPTEN OR MSVC OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "SIGNAL_BUILD_FUZZERS requires a non-MSVC clang toolchain "
            "(got ${CMAKE_C_COMPILER_ID}). Configure with "
            "-DCMAKE_C_COMPILER=clang.")
    endif()
    message(STATUS
        "Configuring headless receipt/handoff fuzzing "
        "(desktop dependency discovery disabled)")
    add_executable(fuzz_cargo_receipt
        tests/fuzz/fuzz_cargo_receipt.c
        shared/cargo_receipt.c
        shared/handoff_ticket.c
        shared/manifest.c
        shared/commodity.c
        ${SIGNAL_CRYPTO_SOURCES}
    )
    target_include_directories(fuzz_cargo_receipt PRIVATE
        ${SIGNAL_INCLUDE_DIRS}
        ${SIGNAL_CRYPTO_INCLUDE_DIR})
    target_compile_options(fuzz_cargo_receipt PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    target_link_options(fuzz_cargo_receipt PRIVATE
        -fsanitize=fuzzer,address,undefined)
    target_link_libraries(fuzz_cargo_receipt PRIVATE m)
endif()

if(BUILD_WASM_REPLAY AND EMSCRIPTEN)
    add_executable(signal_replay
        tools/signal_replay.c
        ${SIGNAL_SIM_SOURCES}
        ${SIGNAL_SIM_HELPERS}
        ${SIGNAL_STATIC_FLIGHT_BRAIN_SOURCES}
        ${SIGNAL_CRYPTO_SOURCES}
    )
    target_include_directories(signal_replay PRIVATE
        ${SIGNAL_INCLUDE_DIRS}
        ${SIGNAL_CRYPTO_INCLUDE_DIR})
    signal_apply_sim_profile(signal_replay)
    target_link_libraries(signal_replay PRIVATE m)
    target_compile_options(signal_replay PRIVATE
        -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
    set_target_properties(signal_replay PROPERTIES SUFFIX ".js")
    target_link_options(signal_replay PRIVATE
        "SHELL:-sENVIRONMENT=node"
        "SHELL:-sEXIT_RUNTIME=1"
        "SHELL:-sNODERAWFS=1"
        "SHELL:-sTOTAL_STACK=8388608"
        "SHELL:-sWASM_ASYNC_COMPILATION=0"
        "SHELL:-sALLOW_MEMORY_GROWTH=1")
endif()

# Short-circuit before wiring up the sokol-dependent client target so
# server-only / tests-only builds don't require vendor/sokol, X11, or
# Emscripten on the builder.
if(BUILD_TESTS_ONLY OR BUILD_SERVER_ONLY)
    return()
endif()

# ------------------------------------------------------------------
# signal — native desktop + WebAssembly client.
# ------------------------------------------------------------------
set(GAME_SOURCES
    ${SIGNAL_CLIENT_SOURCES}
    ${SIGNAL_SIM_SOURCES}
    ${SIGNAL_SIM_HELPERS}
    ${SIGNAL_CRYPTO_SOURCES}
)

# Native builds link mongoose for WebSocket client networking.
if(NOT EMSCRIPTEN)
    list(APPEND GAME_SOURCES server/mongoose.c)
endif()

if(WIN32 AND NOT EMSCRIPTEN)
    add_executable(signal WIN32 ${GAME_SOURCES})
else()
    add_executable(signal ${GAME_SOURCES})
endif()

target_include_directories(signal SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sokol"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor"
)
target_include_directories(signal PRIVATE ${SIGNAL_INCLUDE_DIRS} ${SIGNAL_CRYPTO_INCLUDE_DIR})
target_compile_definitions(signal PRIVATE GIT_HASH="${GIT_HASH}")
signal_apply_sim_profile(signal)

if(SIGNAL_VOICE)
    target_compile_definitions(signal PRIVATE SIGNAL_VOICE)
endif()

# mongoose on Linux needs _GNU_SOURCE (same reason as signal_server).
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    target_compile_definitions(signal PRIVATE _GNU_SOURCE)
endif()

if(MSVC)
    target_compile_definitions(signal PRIVATE _CRT_SECURE_NO_WARNINGS)
    # /WX mirrors -Werror; keep the client strict on MSVC alongside GCC/Clang.
    # See signal_server block for the /wd* rationale.
    target_compile_options(signal PRIVATE
        /W4 /WX /wd4127 /wd4459 /wd4996 /wd5287)
    # Vendor single-file libraries drop warnings we don't own: pl_mpeg has
    # many size_t→int conversions, minimp3 and mongoose a few. Compile the
    # TUs that host them with /W0 so those lines don't fail /WX, while every
    # other client source file still builds under the full /W4 strictness.
    set_source_files_properties(
        client/episode_media.c # includes pl_mpeg implementation
        client/music.c         # includes minimp3.h
        server/mongoose.c   # vendor single-file WebSocket server
        PROPERTIES COMPILE_OPTIONS "/W0"
    )
else()
    # -Wno-strict-prototypes is client-only: sokol_* headers intentionally
    # use K&R-style () prototypes for C++ compat.
    target_compile_options(signal PRIVATE -Wall -Wextra -Wpedantic -Werror -Wno-strict-prototypes -ffp-contract=off -fno-fast-math)

    # client/main.c is the single SOKOL_IMPL translation unit. GCC still emits
    # -Wpedantic warnings from system headers when they are "required
    # diagnostics" — e.g. ISO C's forbid on object↔function pointer casts
    # inside sokol_app.h's GLX loader. The SYSTEM include above isn't enough.
    # Suppress -Wpedantic only for this one TU, so all other sources stay
    # strict.
    set_source_files_properties(client/main.c PROPERTIES
        COMPILE_OPTIONS "-Wno-pedantic;-Wno-error=pedantic")
endif()

if(EMSCRIPTEN)
    set_source_files_properties(client/net.c PROPERTIES
        COMPILE_OPTIONS "-Wno-overlength-strings")
    target_compile_definitions(signal PRIVATE SOKOL_GLES3)
elseif(APPLE)
    target_compile_definitions(signal PRIVATE SOKOL_METAL)
    target_compile_options(signal PRIVATE "-x" "objective-c")
else()
    target_compile_definitions(signal PRIVATE SOKOL_GLCORE)
endif()

if(NOT MSVC)
    target_link_libraries(signal PRIVATE m)
endif()
if(NOT EMSCRIPTEN)
    target_link_libraries(signal PRIVATE Threads::Threads)
endif()

if(EMSCRIPTEN)
    set(SIGNAL_HAS_WEB_EPISODE_ASSETS 0)
    file(GLOB_RECURSE SIGNAL_WEB_ANIME_ASSETS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/anime/*")
    set(SIGNAL_WEB_ASSET_STAMPS)
    set(SIGNAL_WEB_ANIME_ASSETS_COMPLETE OFF)
    if(SIGNAL_WEB_ANIME_ASSETS AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/manifest.txt")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/assets/manifest.txt"
            SIGNAL_WEB_ANIME_MANIFEST REGEX "^anime/.*\\.mpg$")
        set(SIGNAL_WEB_ANIME_MISSING)
        foreach(SIGNAL_WEB_ANIME_PATH IN LISTS SIGNAL_WEB_ANIME_MANIFEST)
            if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/${SIGNAL_WEB_ANIME_PATH}")
                list(APPEND SIGNAL_WEB_ANIME_MISSING
                    "${SIGNAL_WEB_ANIME_PATH}")
            endif()
        endforeach()
        list(LENGTH SIGNAL_WEB_ANIME_MANIFEST SIGNAL_WEB_ANIME_MANIFEST_COUNT)
        if(SIGNAL_WEB_ANIME_MANIFEST_COUNT GREATER 0 AND
           NOT SIGNAL_WEB_ANIME_MISSING)
            set(SIGNAL_WEB_ANIME_ASSETS_COMPLETE ON)
        endif()
    elseif(SIGNAL_WEB_ANIME_ASSETS)
        set(SIGNAL_WEB_ANIME_ASSETS_COMPLETE ON)
    endif()
    if(SIGNAL_WEB_ANIME_ASSETS_COMPLETE)
        set(SIGNAL_HAS_WEB_EPISODE_ASSETS 1)
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/anime_assets.stamp
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                    ${CMAKE_CURRENT_BINARY_DIR}/anime
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    ${CMAKE_CURRENT_BINARY_DIR}/anime
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/assets/anime
                    ${CMAKE_CURRENT_BINARY_DIR}/anime
            COMMAND ${CMAKE_COMMAND} -E touch
                    ${CMAKE_CURRENT_BINARY_DIR}/anime_assets.stamp
            DEPENDS ${SIGNAL_WEB_ANIME_ASSETS}
            VERBATIM
        )
    else()
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/anime_assets.stamp
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                    ${CMAKE_CURRENT_BINARY_DIR}/anime
            COMMAND ${CMAKE_COMMAND} -E touch
                    ${CMAKE_CURRENT_BINARY_DIR}/anime_assets.stamp
            VERBATIM
        )
    endif()
    list(APPEND SIGNAL_WEB_ASSET_STAMPS
        ${CMAKE_CURRENT_BINARY_DIR}/anime_assets.stamp)

    set(SIGNAL_HAS_WEB_MUSIC_ASSETS 0)
    set(SIGNAL_WEB_MUSIC_ASSETS)
    set(SIGNAL_WEB_MUSIC_ASSETS_COMPLETE OFF)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/music")
        file(GLOB_RECURSE SIGNAL_WEB_MUSIC_ASSETS CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/music/*")
    endif()
    if(SIGNAL_WEB_MUSIC_ASSETS AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/manifest.txt")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/assets/manifest.txt"
            SIGNAL_WEB_MUSIC_MANIFEST REGEX "^music/.*\\.mp3$")
        set(SIGNAL_WEB_MUSIC_MISSING)
        foreach(SIGNAL_WEB_MUSIC_PATH IN LISTS SIGNAL_WEB_MUSIC_MANIFEST)
            if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/${SIGNAL_WEB_MUSIC_PATH}")
                list(APPEND SIGNAL_WEB_MUSIC_MISSING
                    "${SIGNAL_WEB_MUSIC_PATH}")
            endif()
        endforeach()
        list(LENGTH SIGNAL_WEB_MUSIC_MANIFEST SIGNAL_WEB_MUSIC_MANIFEST_COUNT)
        if(SIGNAL_WEB_MUSIC_MANIFEST_COUNT GREATER 0 AND
           NOT SIGNAL_WEB_MUSIC_MISSING)
            set(SIGNAL_WEB_MUSIC_ASSETS_COMPLETE ON)
        endif()
    elseif(SIGNAL_WEB_MUSIC_ASSETS)
        set(SIGNAL_WEB_MUSIC_ASSETS_COMPLETE ON)
    endif()
    if(SIGNAL_WEB_MUSIC_ASSETS_COMPLETE)
        set(SIGNAL_HAS_WEB_MUSIC_ASSETS 1)
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/music_assets.stamp
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                    ${CMAKE_CURRENT_BINARY_DIR}/music
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    ${CMAKE_CURRENT_BINARY_DIR}/music
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/assets/music
                    ${CMAKE_CURRENT_BINARY_DIR}/music
            COMMAND ${CMAKE_COMMAND} -E touch
                    ${CMAKE_CURRENT_BINARY_DIR}/music_assets.stamp
            DEPENDS ${SIGNAL_WEB_MUSIC_ASSETS}
            VERBATIM
        )
    else()
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/music_assets.stamp
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                    ${CMAKE_CURRENT_BINARY_DIR}/music
            COMMAND ${CMAKE_COMMAND} -E touch
                    ${CMAKE_CURRENT_BINARY_DIR}/music_assets.stamp
            VERBATIM
        )
    endif()
    list(APPEND SIGNAL_WEB_ASSET_STAMPS
        ${CMAKE_CURRENT_BINARY_DIR}/music_assets.stamp)
    target_compile_definitions(signal PRIVATE
        SIGNAL_HAS_WEB_EPISODE_ASSETS=${SIGNAL_HAS_WEB_EPISODE_ASSETS}
        SIGNAL_HAS_WEB_MUSIC_ASSETS=${SIGNAL_HAS_WEB_MUSIC_ASSETS})

    set(SIGNAL_EMSCRIPTEN_EXPORTS
        "'_main'"
        "'__saudio_emsc_pull'"
        "'_get_signal_strength'"
        "'_get_signal_visual_saturation'"
        "'_get_signal_visual_base_saturation'"
        "'_get_signal_visual_cue_saturation'"
        "'_get_signal_visual_player_saturation'"
        "'_get_player_camera_offset_x'"
        "'_get_player_camera_offset_y'"
        "'_get_player_pos_x'"
        "'_get_player_pos_y'"
        "'_get_player_vel_x'"
        "'_get_player_vel_y'"
        "'_get_player_angle'"
        "'_get_player_docked'"
        "'_get_camera_narrow_focus'"
        "'_reset_net_motion_telemetry'"
        "'_get_net_motion_max_correction'"
        "'_get_net_motion_max_applied_correction'"
        "'_get_net_motion_max_velocity_error'"
        "'_get_net_motion_last_ack_rtt_ms'"
        "'_get_net_motion_total_samples'"
        "'_get_net_motion_total_deferred_samples'"
        "'_get_net_motion_total_replayed_samples'"
        "'_get_net_motion_total_replayed_frames'"
        "'_get_net_motion_player_interval_ms'"
        "'_get_net_motion_max_player_interval_ms'"
        "'_get_net_motion_max_player_jitter_ms'"
        "'_get_net_motion_raw_player_interval_ms'"
        "'_get_net_motion_max_raw_player_interval_ms'"
        "'_get_net_motion_max_raw_player_jitter_ms'"
        "'_get_net_motion_max_ack_rtt_ms'"
        "'_get_net_motion_current_render_offset'"
        "'_get_net_motion_max_render_offset'"
        "'_get_net_motion_total_player_batches'"
        "'_get_net_motion_total_snap_samples'"
        "'_get_net_motion_total_lerp_samples'"
        "'_get_net_motion_total_input_acks'"
        "'_get_net_motion_tick_skew'"
        "'_get_net_motion_max_tick_skew_abs'"
        "'_get_net_motion_input_apply_error_ticks'"
        "'_get_net_motion_max_input_apply_error_abs'"
        "'_get_net_motion_replay_depth'"
        "'_get_net_motion_unacked_inputs'"
        "'_get_net_motion_action_queue_depth'"
        "'_reset_local_asteroid_motion_telemetry'"
        "'_get_local_asteroid_motion_feed_active'"
        "'_get_local_asteroid_motion_frame_samples'"
        "'_get_local_asteroid_motion_presented_samples'"
        "'_get_local_asteroid_motion_starvation_events'"
        "'_get_local_asteroid_motion_max_correction'"
        "'_get_local_asteroid_motion_max_velocity_discontinuity'"
        "'_get_local_asteroid_motion_max_screen_jerk'"
        "'_get_local_asteroid_motion_max_avoided_correction'"
        "'_get_local_asteroid_motion_class_samples'"
        "'_local_asteroid_motion_within_thresholds'"
        "'_signal_zero_latency_gate_run'"
        "'_signal_zero_latency_gate_perturb'"
        "'_signal_zero_latency_gate_ready'"
        "'_signal_zero_latency_gate_status'"
        "'_signal_zero_latency_gate_scenario_mask'"
        "'_get_net_reconcile_exact_samples'"
        "'_get_net_reconcile_bootstrap_samples'"
        "'_get_net_reconcile_input_frontier_samples'"
        "'_get_net_reconcile_semantic_samples'"
        "'_get_net_reconcile_transport_recovery_samples'"
        "'_get_net_reconcile_numeric_drift_samples'"
        "'_get_net_reconcile_asteroid_motion_samples'"
        "'_get_net_reconcile_npc_motion_samples'"
        "'_get_net_reconcile_death_respawn_events'"
        "'_get_net_reconcile_first_drift_json'"
        "'_signal_zero_latency_gate_report_json'"
        "'_signal_smoke_adverse_towable_gate'"
        "'_signal_smoke_adverse_towable_report'"
        "'_get_hud_hint_text'"
        "'_get_hud_action_text'"
        "'_set_smoke_loop_state'"
        "'_signal_hopper_guide_draw_count'"
        "'_signal_hopper_guide_station'"
        "'_signal_hopper_guide_module'"
        "'_signal_hopper_guide_commodity'"
        "'_signal_cargo_readability_draw_count'"
        "'_signal_cargo_readability_min_screen_radius'"
        "'_signal_cargo_readability_max_scale'"
        "'_signal_cargo_readability_towed_screen_radius'"
        "'_signal_station_module_glyph_count'"
        "'_signal_station_hopper_glyph_count'"
        "'_signal_station_producer_glyph_count'"
        "'_signal_smoke_episode_prepare'"
        "'_signal_smoke_episode_trigger'"
        "'_signal_smoke_episode_state'"
        "'_neural_singleplayer_init'"
        "'_neural_singleplayer_ready'"
        "'_signal_mobile_control_flags'"
        "'_signal_mobile_digit_mask'"
        "'_signal_mobile_key'"
        "'_signal_mobile_clear'"
        "'_signal_legacy_recovery_ui_flags'"
        "'_signal_legacy_recovery_ui_semantic'"
        "'_signal_legacy_recovery_ui_copy'"
        "'_signal_smoke_legacy_recovery_offer'"
        "'_signal_smoke_legacy_recovery_result'"
        "'_signal_smoke_legacy_recovery_set_send_admitted'"
        "'_signal_smoke_legacy_recovery_confirm_count'"
        "'_signal_smoke_legacy_recovery_cancel_count'"
        "'_signal_smoke_legacy_recovery_expire_count'"
        "'_signal_smoke_legacy_recovery_reset'"
        "'_signal_debug_held_control_mask'"
        "'_signal_debug_identity_available'"
        "'_signal_debug_player_progress'"
        "'_signal_debug_auth_available'"
        "'_signal_debug_net_connected'"
        "'_signal_smoke_market_memory_packet_parity'"
        "'_signal_debug_local_authority_state'"
        "'_signal_debug_local_authority_generation'"
        "'_signal_debug_restart_local_authority'"
        "'_signal_debug_auth_transport_closes'"
        "'_signal_net_transport_open'"
        "'_signal_net_transport_message'"
        "'_signal_net_transport_close'"
        "'_signal_net_transport_error'"
        "'_malloc'"
        "'_free'"
    )
    list(JOIN SIGNAL_EMSCRIPTEN_EXPORTS "," SIGNAL_EMSCRIPTEN_EXPORTS_CSV)
    set_target_properties(signal PROPERTIES SUFFIX ".js")
    target_link_options(signal PRIVATE
        "SHELL:-sWASM=1"
        "SHELL:-sALLOW_MEMORY_GROWTH=1"
        "SHELL:-sUSE_WEBGL2=1"
        "SHELL:-sMIN_WEBGL_VERSION=2"
        "SHELL:-sMAX_WEBGL_VERSION=2"
        "SHELL:-sNO_EXIT_RUNTIME=1"
        "SHELL:-sTOTAL_STACK=8388608"
        "SHELL:-sDECLARE_ASM_MODULE_EXPORTS=0"
        "SHELL:-sEXPORTED_FUNCTIONS=[${SIGNAL_EMSCRIPTEN_EXPORTS_CSV}]"
        "SHELL:-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
        "SHELL:-lwebsocket.js"
    )
    # Copy static web assets into build-web as real dependencies so JS-only
    # iteration updates the local smoke-test server without forcing a relink.
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/play.html
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/web/play.html
                ${CMAKE_CURRENT_BINARY_DIR}/play.html
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/play.html
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/signal-touch-controls.js
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/web/signal-touch-controls.js
                ${CMAKE_CURRENT_BINARY_DIR}/signal-touch-controls.js
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/signal-touch-controls.js
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/remove_signal_html.stamp
        COMMAND ${CMAKE_COMMAND} -E rm -f
                ${CMAKE_CURRENT_BINARY_DIR}/signal.html
                ${CMAKE_CURRENT_BINARY_DIR}/signal.html.bak
        COMMAND ${CMAKE_COMMAND} -E touch
                ${CMAKE_CURRENT_BINARY_DIR}/remove_signal_html.stamp
        VERBATIM
    )
    add_custom_target(signal_web_assets
        DEPENDS
            ${CMAKE_CURRENT_BINARY_DIR}/play.html
            ${CMAKE_CURRENT_BINARY_DIR}/signal-touch-controls.js
            ${CMAKE_CURRENT_BINARY_DIR}/remove_signal_html.stamp
            ${SIGNAL_WEB_ASSET_STAMPS}
    )
    add_dependencies(signal signal_web_assets)
elseif(APPLE)
    target_link_libraries(signal PRIVATE
        objc
        "-framework Cocoa"
        "-framework AudioToolbox"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework MetalKit"
    )
elseif(WIN32)
    target_link_libraries(signal PRIVATE
        gdi32
        user32
        shell32
        kernel32
        opengl32
    )
else()
    find_package(ALSA REQUIRED)
    find_package(OpenGL REQUIRED)
    find_package(X11 REQUIRED)

    target_include_directories(signal PRIVATE
        "${X11_INCLUDE_DIR}"
        ${ALSA_INCLUDE_DIRS}
    )
    target_link_libraries(signal PRIVATE
        OpenGL::GL
        ${ALSA_LIBRARIES}
        ${X11_LIBRARIES}
        ${X11_Xi_LIB}
        ${X11_Xcursor_LIB}
        dl
    )
endif()
