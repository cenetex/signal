/*
 * signal_contract_brain.c -- Optional neural contract scorer.
 *
 * This is intentionally on-demand: docked bots enumerate valid protocol
 * actions, then score that small candidate set instead of running per tick.
 */
#include "signal_contract_brain.h"

#include "commodity.h"
#include "manifest.h"
#include "ship.h"
#include "station_util.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SCB_FEATURE_COUNT = 40,
    SCB_LAYER_COUNT = 4,
    SCB_HIDDEN0 = 32,
    SCB_HIDDEN1 = 16,
};

typedef struct {
    size_t previous;
    size_t current;
    double *weights;
    double *biases;
} contract_brain_layer_t;

typedef struct {
    int loaded;
    uint32_t hidden_activation;
    uint32_t output_activation;
    uint32_t feature_encoder_version;
    char feature_set[32];
    contract_brain_layer_t layers[SCB_LAYER_COUNT - 1];
    uint64_t inference_count;
    uint64_t decision_count;
    uint64_t teacher_decision_count;
} contract_brain_model_t;

static contract_brain_model_t g_contract_brain;

static void set_err(char *err, size_t err_size, const char *msg) {
    if (!err || err_size == 0) return;
    snprintf(err, err_size, "%s", msg ? msg : "unknown error");
}

static void contract_brain_free(void) {
    uint64_t decisions = g_contract_brain.decision_count;
    uint64_t teacher = g_contract_brain.teacher_decision_count;
    for (int i = 0; i < SCB_LAYER_COUNT - 1; i++) {
        free(g_contract_brain.layers[i].weights);
        free(g_contract_brain.layers[i].biases);
    }
    memset(&g_contract_brain, 0, sizeof(g_contract_brain));
    g_contract_brain.decision_count = decisions;
    g_contract_brain.teacher_decision_count = teacher;
}

static int read_exact(FILE *fp, void *dst, size_t len) {
    return fread(dst, 1, len, fp) == len;
}

static int read_u32_le(FILE *fp, uint32_t *out) {
    uint8_t b[4];
    if (!read_exact(fp, b, sizeof(b))) return 0;
    *out = ((uint32_t)b[0]) |
           ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
    return 1;
}

static int read_u64_le(FILE *fp, uint64_t *out) {
    uint8_t b[8];
    if (!read_exact(fp, b, sizeof(b))) return 0;
    *out = ((uint64_t)b[0]) |
           ((uint64_t)b[1] << 8) |
           ((uint64_t)b[2] << 16) |
           ((uint64_t)b[3] << 24) |
           ((uint64_t)b[4] << 32) |
           ((uint64_t)b[5] << 40) |
           ((uint64_t)b[6] << 48) |
           ((uint64_t)b[7] << 56);
    return 1;
}

static int read_scalar(FILE *fp, uint32_t scalar_type, double *out) {
    if (scalar_type == 1) {
        uint8_t b[4];
        float f = 0.0f;
        if (!read_exact(fp, b, sizeof(b))) return 0;
        memcpy(&f, b, sizeof(f));
        *out = (double)f;
        return 1;
    }
    if (scalar_type == 2) {
        uint8_t b[8];
        double d = 0.0;
        if (!read_exact(fp, b, sizeof(b))) return 0;
        memcpy(&d, b, sizeof(d));
        *out = d;
        return 1;
    }
    return 0;
}

static int skip_bytes(FILE *fp, uint64_t len) {
    uint8_t scratch[256];
    while (len > 0) {
        size_t n = len < sizeof(scratch) ? (size_t)len : sizeof(scratch);
        if (!read_exact(fp, scratch, n)) return 0;
        len -= (uint64_t)n;
    }
    return 1;
}

static int read_fixed_string(FILE *fp, char *dst, size_t dst_size, size_t len) {
    char *tmp = (char *)malloc(len);
    if (!tmp) return 0;
    int ok = read_exact(fp, tmp, len);
    if (ok && dst && dst_size > 0) {
        size_t copy = 0;
        while (copy < len && tmp[copy] != '\0' && copy + 1 < dst_size) copy++;
        memcpy(dst, tmp, copy);
        dst[copy] = '\0';
    }
    free(tmp);
    return ok;
}

static double activation(double x, uint32_t kind) {
    switch (kind) {
    case 1: return x > 0.0 ? x : 0.0;
    case 2: return tanh(x);
    case 3: return x;
    case 0:
    default:
        if (x >= 0.0) return 1.0 / (1.0 + exp(-x));
        {
            double ex = exp(x);
            return ex / (1.0 + ex);
        }
    }
}

bool signal_contract_brain_load_checkpoint(const char *path,
                                           char *err,
                                           size_t err_size) {
    if (!path || path[0] == '\0') {
        set_err(err, err_size, "missing checkpoint path");
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        char buf[256];
        snprintf(buf, sizeof(buf), "open failed: %s", strerror(errno));
        set_err(err, err_size, buf);
        return false;
    }

    contract_brain_free();
    char magic[8];
    uint32_t checkpoint_version = 0;
    uint32_t scalar_type = 0;
    uint64_t layer_count = 0;
    uint64_t sizes[SCB_LAYER_COUNT] = {0};
    uint32_t loss = 0;
    bool ok = read_exact(fp, magic, sizeof(magic)) &&
              memcmp(magic, "NNCKPT01", sizeof(magic)) == 0 &&
              read_u32_le(fp, &checkpoint_version) &&
              checkpoint_version == 1 &&
              read_u32_le(fp, &scalar_type) &&
              read_u64_le(fp, &layer_count) &&
              layer_count == SCB_LAYER_COUNT;
    for (int i = 0; ok && i < SCB_LAYER_COUNT; i++) ok = read_u64_le(fp, &sizes[i]);
    ok = ok && sizes[0] == SCB_FEATURE_COUNT && sizes[1] == SCB_HIDDEN0 &&
         sizes[2] == SCB_HIDDEN1 && sizes[3] == 1 &&
         read_u32_le(fp, &g_contract_brain.hidden_activation) &&
         read_u32_le(fp, &g_contract_brain.output_activation) &&
         read_u32_le(fp, &loss);
    (void)loss;

    for (int layer = 1; ok && layer < SCB_LAYER_COUNT; layer++) {
        contract_brain_layer_t *dst = &g_contract_brain.layers[layer - 1];
        dst->previous = (size_t)sizes[layer - 1];
        dst->current = (size_t)sizes[layer];
        size_t weight_count = dst->previous * dst->current;
        dst->weights = (double *)calloc(weight_count, sizeof(double));
        dst->biases = (double *)calloc(dst->current, sizeof(double));
        if (!dst->weights || !dst->biases) {
            ok = 0;
            break;
        }
        for (size_t i = 0; ok && i < weight_count; i++)
            ok = read_scalar(fp, scalar_type, &dst->weights[i]);
        for (size_t i = 0; ok && i < dst->current; i++)
            ok = read_scalar(fp, scalar_type, &dst->biases[i]);
    }

    uint64_t metadata_size = 0;
    ok = ok && read_u64_le(fp, &metadata_size) &&
         (metadata_size == 455u || metadata_size == 487u) &&
         read_u32_le(fp, &g_contract_brain.feature_encoder_version) &&
         skip_bytes(fp, 65u * 3u + 256u);
    if (ok && metadata_size == 487u) {
        ok = read_fixed_string(fp, g_contract_brain.feature_set,
                               sizeof(g_contract_brain.feature_set), 32u);
    }

    fclose(fp);

    if (!ok) {
        contract_brain_free();
        set_err(err, err_size, "unsupported or truncated signal-contract checkpoint");
        return false;
    }
    if (g_contract_brain.feature_encoder_version != 1u ||
        strcmp(g_contract_brain.feature_set, "signal-contract-live-v1") != 0) {
        contract_brain_free();
        set_err(err, err_size, "checkpoint is not signal-contract-live-v1");
        return false;
    }

    g_contract_brain.loaded = 1;
    return true;
}

bool signal_contract_brain_loaded(void) {
    return g_contract_brain.loaded != 0;
}

uint64_t signal_contract_brain_inference_count(void) {
    return g_contract_brain.inference_count;
}

uint64_t signal_contract_brain_decision_count(void) {
    return g_contract_brain.decision_count;
}

uint64_t signal_contract_brain_teacher_decision_count(void) {
    return g_contract_brain.teacher_decision_count;
}

static double clip(double x, double lo, double hi) {
    if (!isfinite(x)) return 0.0;
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static double scale(double x, double denom) {
    return denom > 0.0 ? clip(x / denom, 0.0, 1.0) : 0.0;
}

static double signed_scale(double x, double denom) {
    return denom > 0.0 ? clip(x / denom, -1.0, 1.0) : 0.0;
}

static void fill_features(const world_t *w,
                          const server_player_t *sp,
                          const signal_contract_candidate_t *c,
                          double row[SCB_FEATURE_COUNT]) {
    memset(row, 0, SCB_FEATURE_COUNT * sizeof(*row));
    float profit = c->contract_price - c->source_price;
    int src = c->source_station;
    int dst = c->dest_station;
    const station_t *src_st = (src >= 0 && src < MAX_STATIONS) ? &w->stations[src] : NULL;
    const station_t *dst_st = (dst >= 0 && dst < MAX_STATIONS) ? &w->stations[dst] : NULL;
    float capacity = ship_cargo_capacity(&sp->ship);
    int held = (c->commodity < COMMODITY_COUNT)
        ? ship_finished_count(&sp->ship, c->commodity)
        : 0;

    row[0] = 1.0;
    row[1] = scale((double)c->action, 3.0);
    row[2] = scale((double)(src + 1), (double)(MAX_STATIONS + 1));
    row[3] = scale((double)(dst + 1), (double)(MAX_STATIONS + 1));
    row[4] = (src == dst) ? 1.0 : 0.0;
    row[5] = scale((double)c->commodity, (double)(COMMODITY_COUNT - 1));
    row[6] = scale(c->source_stock, MAX_PRODUCT_STOCK);
    row[7] = scale(c->dest_stock, MAX_PRODUCT_STOCK);
    row[8] = scale(c->quantity_needed, MAX_PRODUCT_STOCK);
    row[9] = scale(c->contract_price, 100.0);
    row[10] = scale(c->source_price, 100.0);
    row[11] = scale(profit > 0.0f ? profit : 0.0f, 100.0);
    row[12] = signed_scale(c->ledger_balance, 1000.0);
    row[13] = c->ledger_balance + 0.01f >= c->source_price ? 1.0 : 0.0;
    row[14] = held > 0 ? 1.0 : 0.0;
    row[15] = scale(c->free_cargo, capacity > 0.0f ? capacity : 1.0f);
    row[16] = scale(c->distance, 9000.0);
    row[17] = scale(c->age, 300.0);
    row[18] = clip(c->hull_ratio, 0.0, 1.0);
    row[19] = scale((double)ship_towed_body_count(&sp->ship), 10.0);
    row[20] = (src_st && station_produces(src_st, c->commodity)) ? 1.0 : 0.0;
    row[21] = (dst_st && station_consumes(dst_st, c->commodity)) ? 1.0 : 0.0;
    row[22] = (src == 0) ? 1.0 : 0.0;
    row[23] = (src == 1) ? 1.0 : 0.0;
    row[24] = (src == 2) ? 1.0 : 0.0;
    row[25] = (dst == 0) ? 1.0 : 0.0;
    row[26] = (dst == 1) ? 1.0 : 0.0;
    row[27] = (dst == 2) ? 1.0 : 0.0;
    row[28] = scale((double)w->time, 900.0);
    row[29] = scale((double)w->tick, 120000.0);
    row[30] = scale(c->teacher_score, 100.0);
    row[31] = c->action == SIGNAL_CONTRACT_ACTION_DELIVER_LOCAL ? 1.0 : 0.0;
    row[32] = c->action == SIGNAL_CONTRACT_ACTION_BUY_AND_DELIVER ? 1.0 : 0.0;
    row[33] = c->action == SIGNAL_CONTRACT_ACTION_WAIT_FOR_STOCK ? 1.0 : 0.0;
    row[34] = c->commodity == COMMODITY_FERRITE_INGOT ? 1.0 : 0.0;
    row[35] = c->commodity == COMMODITY_FRAME ? 1.0 : 0.0;
    row[36] = c->commodity == COMMODITY_LASER_MODULE ? 1.0 : 0.0;
    row[37] = c->commodity == COMMODITY_TRACTOR_MODULE ? 1.0 : 0.0;
    row[38] = c->commodity == COMMODITY_REPAIR_KIT ? 1.0 : 0.0;
    row[39] = 1.0 - scale(c->distance, 9000.0);
}

static double forward_model(const double input[SCB_FEATURE_COUNT]) {
    double hidden0[SCB_HIDDEN0] = {0.0};
    double hidden1[SCB_HIDDEN1] = {0.0};
    double output[1] = {0.0};
    const double *src = input;
    double *dsts[SCB_LAYER_COUNT - 1] = {hidden0, hidden1, output};

    for (int li = 0; li < SCB_LAYER_COUNT - 1; li++) {
        const contract_brain_layer_t *layer = &g_contract_brain.layers[li];
        double *dst = dsts[li];
        uint32_t act = (li == SCB_LAYER_COUNT - 2)
            ? g_contract_brain.output_activation
            : g_contract_brain.hidden_activation;
        for (size_t neuron = 0; neuron < layer->current; neuron++) {
            double sum = layer->biases[neuron];
            const double *w = &layer->weights[neuron * layer->previous];
            for (size_t i = 0; i < layer->previous; i++) sum += w[i] * src[i];
            dst[neuron] = activation(sum, act);
        }
        src = dst;
    }
    g_contract_brain.inference_count++;
    return output[0];
}

int signal_contract_brain_choose(const world_t *w,
                                 const server_player_t *sp,
                                 const signal_contract_candidate_t *candidates,
                                 int count) {
    return signal_contract_brain_choose_with_scores(w, sp, candidates, count,
                                                    NULL, 0);
}

int signal_contract_brain_choose_with_scores(
    const world_t *w,
    const server_player_t *sp,
    const signal_contract_candidate_t *candidates,
    int count,
    double *scores,
    int score_count) {
    if (!w || !sp || !candidates || count <= 0) return -1;

    g_contract_brain.decision_count++;
    int best = -1;
    double best_score = -1.0e300;

    if (!signal_contract_brain_loaded()) {
        g_contract_brain.teacher_decision_count++;
        for (int i = 0; i < count; i++) {
            double score = candidates[i].teacher_score;
            if (scores && i < score_count) scores[i] = score;
            if (best < 0 || score > best_score) {
                best = i;
                best_score = score;
            }
        }
        return best;
    }

    for (int i = 0; i < count; i++) {
        double row[SCB_FEATURE_COUNT];
        fill_features(w, sp, &candidates[i], row);
        double score = forward_model(row);
        if (scores && i < score_count) scores[i] = score;
        if (best < 0 || score > best_score) {
            best = i;
            best_score = score;
        }
    }
    return best;
}
