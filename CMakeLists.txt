cmake_minimum_required(VERSION 3.21)

project(space_miner C)

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
    add_executable(space_miner_server server/main.c server/game_sim.c server/mongoose.c src/commodity.c src/ship.c src/economy.c src/asteroid.c)
    target_include_directories(space_miner_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/shared" "${CMAKE_CURRENT_SOURCE_DIR}/server")
    target_compile_definitions(space_miner_server PRIVATE GIT_HASH="${GIT_HASH}" SIM_LOG_ENABLED=0)
    if(NOT MSVC)
        target_link_libraries(space_miner_server PRIVATE m)
        target_compile_options(space_miner_server PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(space_miner_test src/test_main.c src/commodity.c src/ship.c src/economy.c src/asteroid.c server/game_sim.c)
    target_include_directories(space_miner_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/shared" "${CMAKE_CURRENT_SOURCE_DIR}/server")
    if(NOT MSVC)
        target_link_libraries(space_miner_test PRIVATE m)
        target_compile_options(space_miner_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(BUILD_TESTS_ONLY)
    add_executable(space_miner_test src/test_main.c src/commodity.c src/ship.c src/economy.c src/asteroid.c server/game_sim.c)
    target_include_directories(space_miner_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/shared" "${CMAKE_CURRENT_SOURCE_DIR}/server")
    if(NOT MSVC)
        target_link_libraries(space_miner_test PRIVATE m)
        target_compile_options(space_miner_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    return()
endif()

set(GAME_SOURCES src/main.c src/net.c src/station_ui.c src/hud.c src/world_draw.c src/commodity.c src/ship.c src/audio.c src/economy.c src/asteroid.c src/asteroid_field.c src/npc.c src/render.c src/rng.c src/input.c src/net_sync.c server/game_sim.c)

# Native builds link mongoose for WebSocket client networking
if(NOT EMSCRIPTEN)
    list(APPEND GAME_SOURCES server/mongoose.c)
    # mongoose.c needs POSIX + BSD APIs on Linux (ip_mreq, alloca, etc.)
    if(UNIX AND NOT APPLE)
        add_compile_definitions(_GNU_SOURCE)
    endif()
endif()

if(WIN32 AND NOT EMSCRIPTEN)
    add_executable(space_miner WIN32 ${GAME_SOURCES})
else()
    add_executable(space_miner ${GAME_SOURCES})
endif()

target_include_directories(space_miner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sokol" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/shared" "${CMAKE_CURRENT_SOURCE_DIR}/server")
target_compile_definitions(space_miner PRIVATE GIT_HASH="${GIT_HASH}")

if(MSVC)
    target_compile_definitions(space_miner PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(space_miner PRIVATE /W4)
else()
    target_compile_options(space_miner PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(EMSCRIPTEN)
    target_compile_definitions(space_miner PRIVATE SOKOL_GLES3)
elseif(APPLE)
    target_compile_definitions(space_miner PRIVATE SOKOL_METAL)
    target_compile_options(space_miner PRIVATE "-x" "objective-c")
else()
    target_compile_definitions(space_miner PRIVATE SOKOL_GLCORE)
endif()

if(NOT MSVC)
    target_link_libraries(space_miner PRIVATE m)
endif()

if(EMSCRIPTEN)
    set_target_properties(space_miner PROPERTIES SUFFIX ".html")
    target_link_options(space_miner PRIVATE
        "SHELL:-sWASM=1"
        "SHELL:-sALLOW_MEMORY_GROWTH=1"
        "SHELL:-sUSE_WEBGL2=1"
        "SHELL:-sMIN_WEBGL_VERSION=2"
        "SHELL:-sMAX_WEBGL_VERSION=2"
        "SHELL:-sNO_EXIT_RUNTIME=1"
        "SHELL:-sEXPORTED_FUNCTIONS=['_main','__saudio_emsc_pull']"
        "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
        "SHELL:-lwebsocket.js"
    )
elseif(APPLE)
    target_link_libraries(space_miner PRIVATE
        objc
        "-framework Cocoa"
        "-framework AudioToolbox"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework MetalKit"
    )
elseif(WIN32)
    target_link_libraries(space_miner PRIVATE
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

    target_include_directories(space_miner PRIVATE
        "${X11_INCLUDE_DIR}"
        ${ALSA_INCLUDE_DIRS}
    )
    target_link_libraries(space_miner PRIVATE
        OpenGL::GL
        ${ALSA_LIBRARIES}
        ${X11_LIBRARIES}
        ${X11_Xi_LIB}
        ${X11_Xcursor_LIB}
        pthread
        dl
    )
endif()
