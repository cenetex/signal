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

set(SIGNAL_CRYPTO_SOURCES
    vendor/tweetnacl/tweetnacl.c
    vendor/tweetnacl/randombytes.c
    vendor/tweetnacl/signal_crypto_tweetnacl.c
)
set(SIGNAL_CRYPTO_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tweetnacl"
)
# Vendored TweetNaCl is public-domain and not written with -Wpedantic
# in mind (uses unsigned long for u32, etc.). Compile with /W0 (MSVC)
# or -w (GCC/Clang) so it doesn't trip our -Werror at the project level.
if(MSVC)
    set_source_files_properties(${SIGNAL_CRYPTO_SOURCES}
        PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(${SIGNAL_CRYPTO_SOURCES}
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

# ------------------------------------------------------------------
# Canonical source lists — every target picks from these. Adding a
# new server .c file only needs editing here.
# ------------------------------------------------------------------

# Headless simulation — used by signal_server, signal_test, and the
# client (which runs the same sim in-process for singleplayer via
# local_server.c). New server sim files go here.
set(SIGNAL_SIM_SOURCES
    server/game_sim.c
    server/sim_ai.c
    server/sim_autopilot.c
    server/sim_flight.c
    server/sim_nav.c
    server/sim_save.c
    server/sim_catalog.c
    server/sim_asteroid.c
    server/sim_physics.c
    server/sim_production.c
    server/sim_construction.c
    server/sim_mining.c
)

# Shared libraries used by both sim and client render — pure C, no
# sokol / audio / GL dependencies.
set(SIGNAL_SIM_HELPERS
    src/commodity.c
    src/manifest.c
    src/ship.c
    src/economy.c
    src/asteroid.c
    src/rng.c
    shared/belt.c
    shared/module_schema.c
    shared/station_util.c
)

# Client-only: rendering, input, HUD, audio, network plumbing. Not
# compiled into signal_server or signal_test.
set(SIGNAL_CLIENT_SOURCES
    src/main.c
    src/net.c
    src/station_ui.c
    src/hud.c
    src/world_draw.c
    src/audio.c
    src/asteroid_field.c
    src/npc.c
    src/render.c
    src/input.c
    src/net_sync.c
    src/local_server.c
    src/onboarding.c
    src/episode.c
    src/music.c
    src/avatar.c
    src/mining_client.c
    src/identity.c
)

set(SIGNAL_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/shared"
    "${CMAKE_CURRENT_SOURCE_DIR}/server"
)

# ------------------------------------------------------------------
# signal_server — authoritative game server (also the Docker image).
# ------------------------------------------------------------------
if(NOT EMSCRIPTEN AND NOT BUILD_TESTS_ONLY)
    add_executable(signal_server
        server/main.c
        server/mongoose.c
        server/highscore.c
        ${SIGNAL_SIM_SOURCES}
        ${SIGNAL_SIM_HELPERS}
        ${SIGNAL_CRYPTO_SOURCES}
    )
    target_include_directories(signal_server PRIVATE
        ${SIGNAL_INCLUDE_DIRS}
        ${SIGNAL_CRYPTO_INCLUDE_DIR})
    target_compile_definitions(signal_server PRIVATE
        GIT_HASH="${GIT_HASH}"
        MG_ENABLE_LOG=0    # silence mongoose's internal [INFO]/[ERROR] spam
        GAME_SIM_VERBOSE   # enable [sim] log lines (matches prod Docker)
    )
    if(NOT MSVC)
        target_link_libraries(signal_server PRIVATE m)
        target_compile_options(signal_server PRIVATE
            -Wall -Wextra -Wpedantic -Werror -fstack-protector-strong)
        # _FORTIFY_SOURCE requires optimization; only set for Release-ish.
        target_compile_definitions(signal_server PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>)
    else()
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
        src/test_main.c
        src/tests/test_harness.c
        src/tests/test_commodity.c
        src/tests/test_math.c
        src/tests/test_ship.c
        src/tests/test_manifest.c
        src/tests/test_economy.c
        src/tests/test_world_sim.c
        src/tests/test_protocol.c
        src/tests/test_bug_regression.c
        src/tests/test_save.c
        src/tests/test_construction.c
        src/tests/test_navigation.c
        src/tests/test_econ_sim.c
        src/tests/test_asteroid.c
        src/tests/test_chain.c
        src/tests/test_labels.c
        src/tests/test_pvp_rocks.c
        src/tests/test_crypto.c
        src/tests/test_identity.c
        src/tests/test_registry.c
        src/tests/test_signed_action.c
        src/identity.c
        ${SIGNAL_SIM_SOURCES}
        ${SIGNAL_SIM_HELPERS}
        ${SIGNAL_CRYPTO_SOURCES}
    )
    target_include_directories(signal_test PRIVATE
        ${SIGNAL_INCLUDE_DIRS}
        ${SIGNAL_CRYPTO_INCLUDE_DIR})
    if(NOT MSVC)
        target_link_libraries(signal_test PRIVATE m)
        target_compile_options(signal_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    else()
        target_compile_options(signal_test PRIVATE
            /W4 /WX /wd4127 /wd4459 /wd4996 /wd5287)
        target_compile_definitions(signal_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
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
        src/episode.c       # includes pl_mpeg.h
        src/music.c         # includes minimp3.h
        server/mongoose.c   # vendor single-file WebSocket server
        PROPERTIES COMPILE_OPTIONS "/W0"
    )
else()
    # -Wno-strict-prototypes is client-only: sokol_* headers intentionally
    # use K&R-style () prototypes for C++ compat.
    target_compile_options(signal PRIVATE -Wall -Wextra -Wpedantic -Werror -Wno-strict-prototypes)

    # src/main.c is the single SOKOL_IMPL translation unit. GCC still emits
    # -Wpedantic warnings from system headers when they are "required
    # diagnostics" — e.g. ISO C's forbid on object↔function pointer casts
    # inside sokol_app.h's GLX loader. The SYSTEM include above isn't enough.
    # Suppress -Wpedantic only for this one TU, so all other sources stay
    # strict.
    set_source_files_properties(src/main.c PROPERTIES
        COMPILE_OPTIONS "-Wno-pedantic;-Wno-error=pedantic")
endif()

if(EMSCRIPTEN)
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

if(EMSCRIPTEN)
    set_target_properties(signal PROPERTIES SUFFIX ".html")
    target_link_options(signal PRIVATE
        "SHELL:-sWASM=1"
        "SHELL:-sALLOW_MEMORY_GROWTH=1"
        "SHELL:-sUSE_WEBGL2=1"
        "SHELL:-sMIN_WEBGL_VERSION=2"
        "SHELL:-sMAX_WEBGL_VERSION=2"
        "SHELL:-sNO_EXIT_RUNTIME=1"
        "SHELL:-sEXPORTED_FUNCTIONS=['_main','__saudio_emsc_pull','_get_signal_strength']"
        "SHELL:-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
        "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
        "SHELL:-lwebsocket.js"
    )
    # Copy play.html into the build-web output dir so it's reachable from
    # the SimpleHTTP server (and so docker bind-mounts of build-web serve
    # it without needing the image's COPY layer).
    add_custom_command(TARGET signal POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/web/play.html
            ${CMAKE_CURRENT_BINARY_DIR}/play.html
    )
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
        pthread
        dl
    )
endif()
