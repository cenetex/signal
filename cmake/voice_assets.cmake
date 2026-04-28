# Fetch and extract voice assets (voicebox binary and Kokoro models)
# Only runs if SIGNAL_VOICE is ON
# For Emscripten builds, assets are loaded from JavaScript; native fetching is skipped.

if(NOT SIGNAL_VOICE)
    return()
endif()

if(EMSCRIPTEN)
    message(STATUS "[SIGNAL_VOICE] Emscripten build: assets will be loaded from JavaScript")
    return()
endif()

set(VOICE_ASSETS_DIR "${CMAKE_SOURCE_DIR}/assets/voice")

# --- Kokoro TTS Models (~333 MB) ---
set(KOKORO_URL "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-multi-lang-v1_0.tar.bz2")
set(KOKORO_DIR "${VOICE_ASSETS_DIR}/kokoro")
set(KOKORO_MARKER "${KOKORO_DIR}/.fetched")

if(NOT EXISTS "${KOKORO_MARKER}")
    message(STATUS "[SIGNAL_VOICE] Fetching Kokoro TTS models...")

    # Create temp dir for download
    file(MAKE_DIRECTORY "${VOICE_ASSETS_DIR}")
    set(KOKORO_TARBALL "${CMAKE_BINARY_DIR}/kokoro-multi-lang-v1_0.tar.bz2")

    # Download if not already cached
    if(NOT EXISTS "${KOKORO_TARBALL}")
        message(STATUS "[SIGNAL_VOICE] Downloading Kokoro from ${KOKORO_URL}")
        file(DOWNLOAD
            "${KOKORO_URL}"
            "${KOKORO_TARBALL}"
            SHOW_PROGRESS
            TIMEOUT 600
        )
    endif()

    # Extract to assets/voice/kokoro/
    message(STATUS "[SIGNAL_VOICE] Extracting Kokoro to ${KOKORO_DIR}")
    file(MAKE_DIRECTORY "${KOKORO_DIR}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xjf "${KOKORO_TARBALL}"
        WORKING_DIRECTORY "${KOKORO_DIR}"
        RESULT_VARIABLE EXTRACT_RESULT
    )

    if(NOT EXTRACT_RESULT EQUAL 0)
        message(FATAL_ERROR "[SIGNAL_VOICE] Failed to extract Kokoro models")
    endif()

    # Mark as fetched
    file(WRITE "${KOKORO_MARKER}" "fetched at ${CMAKE_CURRENT_LIST_FILE}\n")
    message(STATUS "[SIGNAL_VOICE] Kokoro models ready at ${KOKORO_DIR}")
else()
    message(STATUS "[SIGNAL_VOICE] Kokoro models already present at ${KOKORO_DIR}")
endif()

# --- Whisper-Tiny.en STT Model (~75 MB, sherpa-onnx ONNX bundle) ---
# Voicebox uses sherpa-onnx's offline recognizer, which expects the ONNX
# bundle from k2-fsa's release tarball, NOT raw safetensors from HF.
# The tarball contains tiny.en-encoder/decoder.int8.onnx, tokens.txt,
# silero_vad.onnx — voicebox loads the renamed canonical files.
set(WHISPER_URL "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-whisper-tiny.en.tar.bz2")
set(WHISPER_DIR "${VOICE_ASSETS_DIR}/whisper")
set(WHISPER_MARKER "${WHISPER_DIR}/.fetched")

if(NOT EXISTS "${WHISPER_MARKER}")
    message(STATUS "[SIGNAL_VOICE] Fetching Whisper STT model...")

    file(MAKE_DIRECTORY "${WHISPER_DIR}")
    set(WHISPER_TARBALL "${CMAKE_BINARY_DIR}/sherpa-onnx-whisper-tiny.en.tar.bz2")

    if(NOT EXISTS "${WHISPER_TARBALL}")
        message(STATUS "[SIGNAL_VOICE] Downloading Whisper ONNX bundle from ${WHISPER_URL}")
        file(DOWNLOAD
            "${WHISPER_URL}"
            "${WHISPER_TARBALL}"
            SHOW_PROGRESS
            TIMEOUT 600
        )
    endif()

    message(STATUS "[SIGNAL_VOICE] Extracting Whisper to ${WHISPER_DIR}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xjf "${WHISPER_TARBALL}"
        WORKING_DIRECTORY "${WHISPER_DIR}"
        RESULT_VARIABLE EXTRACT_RESULT
    )
    if(NOT EXTRACT_RESULT EQUAL 0)
        message(FATAL_ERROR "[SIGNAL_VOICE] Failed to extract Whisper bundle")
    endif()

    # Tarball extracts into a subdir; flatten + rename to voicebox's expected names
    file(GLOB _whisper_inner "${WHISPER_DIR}/*/*.onnx" "${WHISPER_DIR}/*/*.txt")
    foreach(_f ${_whisper_inner})
        get_filename_component(_n "${_f}" NAME)
        file(RENAME "${_f}" "${WHISPER_DIR}/${_n}")
    endforeach()
    # Voicebox looks for whisper-encoder.onnx / whisper-decoder.onnx / whisper-tokens.txt
    if(EXISTS "${WHISPER_DIR}/tiny.en-encoder.int8.onnx")
        file(RENAME "${WHISPER_DIR}/tiny.en-encoder.int8.onnx" "${WHISPER_DIR}/whisper-encoder.onnx")
    endif()
    if(EXISTS "${WHISPER_DIR}/tiny.en-decoder.int8.onnx")
        file(RENAME "${WHISPER_DIR}/tiny.en-decoder.int8.onnx" "${WHISPER_DIR}/whisper-decoder.onnx")
    endif()
    if(EXISTS "${WHISPER_DIR}/tiny.en-tokens.txt")
        file(RENAME "${WHISPER_DIR}/tiny.en-tokens.txt" "${WHISPER_DIR}/whisper-tokens.txt")
    endif()

    file(WRITE "${WHISPER_MARKER}" "fetched at ${CMAKE_CURRENT_LIST_FILE}\n")
    message(STATUS "[SIGNAL_VOICE] Whisper model ready at ${WHISPER_DIR}")
else()
    message(STATUS "[SIGNAL_VOICE] Whisper model already present at ${WHISPER_DIR}")
endif()

# --- Voicebox Binary ---
# The voicebox binary needs to be obtained separately:
# 1. Build from cenetex/voicebox source (preferred if no prebuilt available)
# 2. Copy a prebuilt binary to assets/voice/voicebox (or voicebox.exe on Windows)
#
# For now, we just verify the expected path exists and provide helpful error if missing.
if(WIN32)
    set(VOICEBOX_PATH "${VOICE_ASSETS_DIR}/voicebox.exe")
else()
    set(VOICEBOX_PATH "${VOICE_ASSETS_DIR}/voicebox")
endif()

if(NOT EXISTS "${VOICEBOX_PATH}")
    message(WARNING
        "[SIGNAL_VOICE] voicebox binary not found at ${VOICEBOX_PATH}\n"
        "To build it:\n"
        "  git clone https://github.com/cenetex/voicebox.git\n"
        "  cd voicebox && make\n"
        "  cp voicebox ${VOICEBOX_PATH}\n"
        "\n"
        "The Signal build will proceed, but SIGNAL_VOICE mode will fail at runtime"
        " until the binary is in place."
    )
else()
    message(STATUS "[SIGNAL_VOICE] voicebox binary found at ${VOICEBOX_PATH}")
endif()

message(STATUS "[SIGNAL_VOICE] Voice assets configured")
