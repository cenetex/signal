cmake_minimum_required(VERSION 3.21)

project(signal C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(BUILD_TESTS_ONLY "Only build the test executable" OFF)

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

if(NOT EMSCRIPTEN AND NOT BUILD_TESTS_ONLY)
    # --- Headless game server ---
    add_executable(signal_server server/main.c server/game_sim.c server/sim_ai.c server/sim_autopilot.c server/sim_flight.c server/sim_nav.c server/sim_save.c server/sim_catalog.c server/sim_asteroid.c server/sim_physics.c server/sim_production.c server/sim_construction.c server/mongoose.c src/commodity.c src/ship.c src/economy.c src/asteroid.c src/rng.c)
    target_include_directories(signal_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/shared" "${CMAKE_CURRENT_SOURCE_DIR}/server")
    target_compile_definitions(signal_server PRIVATE GIT_HASH="${GIT_HASH}" SIM_LOG_ENABLED=0)
    if(NOT MSVC)
        target_link_libraries(signal_server PRIVATE m)
        target_compile_options(signal_server PRIVATE -Wall -Wextra -Wpedantic -fstack-protector-strong)
        # _FORTIFY_SOURCE requires optimization; only set for Release/RelWithDebInfo
        target_compile_definitions(signal_server PRIVATE $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>)
    endif()

    add_executable(signal_test src/test_main.c src/commodity.c src/ship.c src/economy.c src/asteroid.c src/rng.c server/game_sim.c server/sim_ai.c server/sim_autopilot.c server/sim_flight.c server/sim_nav.c server/sim_save.c server/sim_catalog.c server/sim_asteroid.c server/sim_physics.c server/sim_production.c server/sim_construction.c)
    target_include_directories(signal_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/shared" "${CMAKE_CURRENT_SOURCE_DIR}/server")
    if(NOT MSVC)
        target_link_libraries(signal_test PRIVATE m)
        target_compile_options(signal_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(BUILD_TESTS_ONLY)
    add_executable(signal_test src/test_main.c src/commodity.c src/ship.c src/economy.c src/asteroid.c src/rng.c server/game_sim.c server/sim_ai.c server/sim_autopilot.c server/sim_flight.c server/sim_nav.c server/sim_save.c server/sim_catalog.c server/sim_asteroid.c server/sim_physics.c server/sim_production.c server/sim_construction.c)
    target_include_directories(signal_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/shared" "${CMAKE_CURRENT_SOURCE_DIR}/server")
    if(NOT MSVC)
        target_link_libraries(signal_test PRIVATE m)
        target_compile_options(signal_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    return()
endif()

set(GAME_SOURCES src/main.c src/net.c src/station_ui.c src/hud.c src/world_draw.c src/commodity.c src/ship.c src/audio.c src/economy.c src/asteroid.c src/asteroid_field.c src/npc.c src/render.c src/rng.c src/input.c src/net_sync.c src/local_server.c src/onboarding.c src/episode.c src/music.c src/avatar.c server/game_sim.c server/sim_ai.c server/sim_autopilot.c server/sim_flight.c server/sim_nav.c server/sim_save.c server/sim_catalog.c server/sim_asteroid.c server/sim_physics.c server/sim_production.c server/sim_construction.c)

# Native builds link mongoose for WebSocket client networking
if(NOT EMSCRIPTEN)
    list(APPEND GAME_SOURCES server/mongoose.c)
    # mongoose.c needs POSIX + BSD APIs on Linux (ip_mreq, alloca, etc.)
    if(UNIX AND NOT APPLE)
        add_compile_definitions(_GNU_SOURCE)
    endif()
endif()

if(WIN32 AND NOT EMSCRIPTEN)
    add_executable(signal WIN32 ${GAME_SOURCES})
else()
    add_executable(signal ${GAME_SOURCES})
endif()

target_include_directories(signal PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sokol" "${CMAKE_CURRENT_SOURCE_DIR}/vendor" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/shared" "${CMAKE_CURRENT_SOURCE_DIR}/server")
target_compile_definitions(signal PRIVATE GIT_HASH="${GIT_HASH}")

if(MSVC)
    target_compile_definitions(signal PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(signal PRIVATE /W4)
else()
    target_compile_options(signal PRIVATE -Wall -Wextra -Wpedantic)
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
