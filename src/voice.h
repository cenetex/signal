/*
 * voice.h -- Interface to the local voicebox subprocess for TTS hailing.
 * When SIGNAL_VOICE is enabled, spawns voicebox as a child process at
 * startup and sends hail events through the stdin pipe for TTS playback.
 */
#ifndef VOICE_H
#define VOICE_H

#include <stdbool.h>

/* Initialize the voicebox subprocess. Called once at startup. */
void voice_init(void);

/* Send a hail event to voicebox for TTS playback.
 * persona: station persona name (e.g. "prospect", "kepler", "helios")
 * line: the hail message to speak
 * Best-effort; silently drops if pipe is full. */
void voice_event(const char *persona, const char *line);

/* Control mic input state. When enabled and STT dir is present, voicebox
 * captures audio and processes it via whisper STT. */
void voice_mic_enable(bool enabled);

/* Send ship state to voicebox for context-aware elaboration.
 * Sends a STATE line that voicebox can reference in elaborations.
 * fields: semicolon-separated key=value pairs, e.g. "callsign=alice;sector=1;credits=1500"
 * Best-effort; silently drops if pipe is full. */
void voice_state(const char *fields);

/* Request LLM elaboration on a station hail.
 * Sends an ASK line that voicebox processes with its LLM.
 * persona: station persona name (e.g. "prospect", "kepler", "helios")
 * directive: what to elaborate on, e.g. "haul_value_update" or "rare_ore_found"
 * Best-effort; silently drops if pipe is full. */
void voice_ask(const char *persona, const char *directive);

/* Shut down the voicebox subprocess gracefully. Called at shutdown. */
void voice_quit(void);

#endif /* VOICE_H */
