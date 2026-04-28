/*
 * voice.h -- Interface to the local voicebox subprocess for TTS hailing.
 * When SIGNAL_VOICE is enabled, spawns voicebox as a child process at
 * startup and sends hail events through the stdin pipe for TTS playback.
 */
#ifndef VOICE_H
#define VOICE_H

/* Initialize the voicebox subprocess. Called once at startup. */
void voice_init(void);

/* Send a hail event to voicebox for TTS playback.
 * persona: station persona name (e.g. "prospect", "kepler", "helios")
 * line: the hail message to speak
 * Best-effort; silently drops if pipe is full. */
void voice_event(const char *persona, const char *line);

/* Shut down the voicebox subprocess gracefully. Called at shutdown. */
void voice_quit(void);

#endif /* VOICE_H */
