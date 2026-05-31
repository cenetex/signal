#include "neural_checkpoint.h"
#include "signal_brain.h"
#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

static bool neural_checkpoint_loaded = false;

#define CKPT_DATA _Users_ratimics_develop_crlplrimes_build_float_signal_flight_longhorizon_live_signal_flight_nnckpt
#define CKPT_LEN  _Users_ratimics_develop_crlplrimes_build_float_signal_flight_longhorizon_live_signal_flight_nnckpt_len

EXPORT
bool neural_singleplayer_init(void) {
    if (neural_checkpoint_loaded) return true;
    
    FILE *fp = fopen("/tmp/neural_brain.nnckpt", "wb");
    if (!fp) {
        printf("[neural] cannot open temp file\n");
        return false;
    }
    fwrite(CKPT_DATA, 1, CKPT_LEN, fp);
    fclose(fp);
    
    char err[256] = {0};
    if (!signal_brain_load_checkpoint("/tmp/neural_brain.nnckpt", err, sizeof(err))) {
        printf("[neural] load failed: %s\n", err);
        return false;
    }
    
    neural_checkpoint_loaded = true;
    printf("[neural] brain loaded (%u bytes)\n", (unsigned)CKPT_LEN);
    return true;
}
