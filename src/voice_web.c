/*
 * voice_web.c -- WASM browser voice variant using JS bridge.
 * Calls voicebox web module functions via JavaScript.
 */

#include "voice.h"
#include <emscripten.h>

void voice_init(void) {
    EM_ASM(
        if (typeof window.voicebox !== 'undefined' && window.voicebox.init) {
            window.voicebox.init();
        }
    );
}

void voice_event(const char *persona, const char *line) {
    EM_ASM({
        if (typeof window.voicebox !== 'undefined' && window.voicebox.event) {
            window.voicebox.event(UTF8ToString($0), UTF8ToString($1));
        }
    }, persona, line);
}

void voice_mic_enable(bool enabled) {
    EM_ASM({
        if (typeof window.voicebox !== 'undefined' && window.voicebox.setMicEnabled) {
            window.voicebox.setMicEnabled(!!$0);
        }
    }, enabled);
}

void voice_state(const char *fields) {
    EM_ASM({
        if (typeof window.voicebox !== 'undefined' && window.voicebox.setState) {
            window.voicebox.setState(UTF8ToString($0));
        }
    }, fields);
}

void voice_ask(const char *persona, const char *directive) {
    EM_ASM({
        if (typeof window.voicebox !== 'undefined' && window.voicebox.ask) {
            window.voicebox.ask(UTF8ToString($0), UTF8ToString($1));
        }
    }, persona, directive);
}

void voice_quit(void) {
    EM_ASM(
        if (typeof window.voicebox !== 'undefined' && window.voicebox.quit) {
            window.voicebox.quit();
        }
    );
}

/* PCM ingest is native-only — voice audio in the browser is mixed by the JS
   side directly into Web Audio. These stubs satisfy audio.c's references. */
bool voice_pcm_init(void)        { return false; }
void voice_pcm_read(void)        {}
int  voice_pcm_queue_depth(void) { return 0; }
