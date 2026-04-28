# Voice Assets

This directory contains the voicebox subprocess binary and persona files for the SIGNAL_VOICE feature.

## How the build populates these assets

When building with `cmake -DSIGNAL_VOICE=ON`:

1. **Kokoro TTS models** (`kokoro/`) are automatically downloaded from k2-fsa/sherpa-onnx releases
   - ~333 MB tarball, extracted on first build
   - Cached to avoid re-downloading on rebuilds

2. **Persona files** (`.persona` text files) are version-controlled in this repo:
   - `nav7.persona` — NAV-7 station AI
   - `prospect.persona` — Prospect Refinery
   - `kepler.persona` — Kepler Yard
   - `helios.persona` — Helios Works

3. **Voicebox binary** (`voicebox` or `voicebox.exe`) must be obtained separately:
   - If not already present, the build will warn you and provide instructions
   - To build from source:
     ```sh
     git clone https://github.com/cenetex/voicebox.git
     cd voicebox && make
     cp voicebox /path/to/signal/assets/voice/voicebox
     ```

## Files in this directory

- `.gitignore` entries keep the voicebox binary and Kokoro models out of git (they're downloaded/built per-platform)
- Only the `.persona` text files are version-controlled

## Runtime

The Signal client will launch voicebox as a subprocess with these arguments:
```
voicebox --ship \
  --persona-add nav7 assets/voice/nav7.persona \
  --persona-add prospect assets/voice/prospect.persona \
  --persona-add kepler assets/voice/kepler.persona \
  --persona-add helios assets/voice/helios.persona \
  assets/voice/kokoro
```
