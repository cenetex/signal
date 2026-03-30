cmake_minimum_required(VERSION 3.21)

project(space_miner C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(WIN32 AND NOT EMSCRIPTEN)
    add_executable(space_miner WIN32 src/main.c)
else()
    add_executable(space_miner src/main.c)
endif()

target_include_directories(space_miner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sokol")

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
        "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
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
