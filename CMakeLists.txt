cmake_minimum_required(VERSION 3.21)

project(signal C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Build-mode toggles. Exactly one of these may be ON at a time; with none
# set, every target (server, test, client) is built.
option(BUILD_TESTS_ONLY  "Only build the test executable"       OFF)
option(BUILD_SERVER_ONLY "Only build the headless game server"  OFF)

# --- GIT_HASH baked into binaries for /health and client mismatch checks ---
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
)

# Shared libraries used by both sim and client render — pure C, no
# sokol / audio / GL dependencies.
set(SIGNAL_SIM_HELPERS
    src/commodity.c
    src/ship.c
    src/economy.c
    src/asteroid.c
    src/rng.c
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
        ${SIGNAL_SIM_SOURCES}
        ${SIGNAL_SIM_HELPERS}
    )
    target_include_directories(signal_server PRIVATE ${SIGNAL_INCLUDE_DIRS})
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
        target_compile_options(signal_server PRIVATE /W4 /WX)
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
        ${SIGNAL_SIM_SOURCES}
        ${SIGNAL_SIM_HELPERS}
    )
    target_include_directories(signal_test PRIVATE ${SIGNAL_INCLUDE_DIRS})
    if(NOT MSVC)
        target_link_libraries(signal_test PRIVATE m)
        target_compile_options(signal_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    else()
        target_compile_options(signal_test PRIVATE /W4 /WX)
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
target_include_directories(signal PRIVATE ${SIGNAL_INCLUDE_DIRS})
target_compile_definitions(signal PRIVATE GIT_HASH="${GIT_HASH}")

# mongoose on Linux needs _GNU_SOURCE (same reason as signal_server).
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    target_compile_definitions(signal PRIVATE _GNU_SOURCE)
endif()

if(MSVC)
    target_compile_definitions(signal PRIVATE _CRT_SECURE_NO_WARNINGS)
    # /WX mirrors -Werror; keep the client strict on MSVC alongside GCC/Clang.
    target_compile_options(signal PRIVATE /W4 /WX)
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
