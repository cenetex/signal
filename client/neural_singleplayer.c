#include "neural_checkpoint.h"
#include "signal_brain.h"
#include <stdio.h>
#include <string.h>
#include <emscripten.h>

static bool neural_checkpoint_loaded = false;

/* Shorter aliases for the xxd -i generated names */
#define CKPT_DATA _Users_ratimics_develop_crlplrimes_build_float_signal_flight_longhorizon_live_signal_flight_nnckpt
#define CKPT_LEN  _Users_ratimics_develop_crlplrimes_build_float_signal_flight_longhorizon_live_signal_flight_nnckpt_len

/* Load the embedded neural checkpoint into the brain engine. */
EMSCRIPTEN_KEEPALIVE
bool neural_singleplayer_init(void) {
    if (neural_checkpoint_loaded) return true;
    
    FILE *fp = fopen("/neural_brain.nnckpt", "wb");
    if (!fp) {
        printf("[neural] cannot open temp file\n");
        return false;
    }
    fwrite(CKPT_DATA, 1, CKPT_LEN, fp);
    fclose(fp);
    
    char err[256] = {0};
    if (!signal_brain_load_checkpoint("/neural_brain.nnckpt", err, sizeof(err))) {
        printf("[neural] checkpoint load failed: %s\n", err);
        return false;
    }
    
    neural_checkpoint_loaded = true;
    printf("[neural] brain loaded (%u bytes)\n", (unsigned)CKPT_LEN);
    return true;
}

bool neural_singleplayer_active(void) {
    return neural_checkpoint_loaded;
}
