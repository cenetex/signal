# Voice Assets

This directory contains the voicebox subprocess and persona files for the SIGNAL_VOICE feature.

## Files needed:

1. `voicebox` - The vendored voicebox binary (platform-specific)
   - `voicebox` (macOS/Linux)
   - `voicebox.exe` (Windows)

2. `*.persona` - Persona definition files (copy from cenetex/voicebox/personas/):
   - `nav7.persona`
   - `prospect.persona`
   - `kepler.persona`
   - `helios.persona`

3. `kokoro/` - Kokoro TTS model directory (~700 MB)
   - `kokoro-multi-lang-v1_0` and related model files

These are vendored from the cenetex/voicebox repository. To populate:
- Copy the voicebox binary for your platform
- Copy persona files from `~/develop/voicebox/personas/` or `cenetex/voicebox` repo
- Copy the Kokoro model directory from `~/develop/voicebox/models/kokoro/`

The Signal client will run voicebox with these flags:
```
voicebox --ship \
  --persona-add nav7 assets/voice/nav7.persona \
  --persona-add prospect assets/voice/prospect.persona \
  --persona-add kepler assets/voice/kepler.persona \
  --persona-add helios assets/voice/helios.persona \
  assets/voice/kokoro
```
