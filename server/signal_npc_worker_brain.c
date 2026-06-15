/*
 * signal_npc_worker_brain.c -- Optional neural NPC worker scorer.
 *
 * This loader accepts crlplrimes signal-npc-worker-v1 .nnckpt files and
 * evaluates the 56-feature strategic worker candidate encoder.
 */
#include "signal_npc_worker_brain.h"

#include "commodity.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SNWB_FEATURE_COUNT = 56,
    SNWB_LAYER_COUNT = 4,
    SNWB_HIDDEN0 = 32,
    SNWB_HIDDEN1 = 16,
};

typedef struct {
    size_t previous;
    size_t current;
    double *weights;
    double *biases;
} npc_worker_brain_layer_t;

typedef struct {
    int loaded;
    uint32_t hidden_activation;
    uint32_t output_activation;
    uint32_t feature_encoder_version;
    char feature_set[32];
    npc_worker_brain_layer_t layers[SNWB_LAYER_COUNT - 1];
    uint64_t inference_count;
    uint64_t decision_count;
    uint64_t teacher_decision_count;
} npc_worker_brain_model_t;

static npc_worker_brain_model_t g_worker_brain;

static void set_err(char *err, size_t err_size, const char *msg) {
    if (!err || err_size == 0) return;
    snprintf(err, err_size, "%s", msg ? msg : "unknown error");
}

static void worker_brain_free(void) {
    uint64_t decisions = g_worker_brain.decision_count;
    uint64_t teacher = g_worker_brain.teacher_decision_count;
    for (int i = 0; i < SNWB_LAYER_COUNT - 1; i++) {
        free(g_worker_brain.layers[i].weights);
        free(g_worker_brain.layers[i].biases);
    }
    memset(&g_worker_brain, 0, sizeof(g_worker_brain));
    g_worker_brain.decision_count = decisions;
    g_worker_brain.teacher_decision_count = teacher;
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

bool signal_npc_worker_brain_load_checkpoint(const char *path,
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

    worker_brain_free();
    char magic[8];
    uint32_t checkpoint_version = 0;
    uint32_t scalar_type = 0;
    uint64_t layer_count = 0;
    uint64_t sizes[SNWB_LAYER_COUNT] = {0};
    uint32_t loss = 0;
    bool ok = read_exact(fp, magic, sizeof(magic)) &&
              memcmp(magic, "NNCKPT01", sizeof(magic)) == 0 &&
              read_u32_le(fp, &checkpoint_version) &&
              checkpoint_version == 1 &&
              read_u32_le(fp, &scalar_type) &&
              read_u64_le(fp, &layer_count) &&
              layer_count == SNWB_LAYER_COUNT;
    for (int i = 0; ok && i < SNWB_LAYER_COUNT; i++) ok = read_u64_le(fp, &sizes[i]);
    ok = ok && sizes[0] == SNWB_FEATURE_COUNT && sizes[1] == SNWB_HIDDEN0 &&
         sizes[2] == SNWB_HIDDEN1 && sizes[3] == 1 &&
         read_u32_le(fp, &g_worker_brain.hidden_activation) &&
         read_u32_le(fp, &g_worker_brain.output_activation) &&
         read_u32_le(fp, &loss);
    (void)loss;

    for (int layer = 1; ok && layer < SNWB_LAYER_COUNT; layer++) {
        npc_worker_brain_layer_t *dst = &g_worker_brain.layers[layer - 1];
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
         metadata_size == 487u &&
         read_u32_le(fp, &g_worker_brain.feature_encoder_version) &&
         skip_bytes(fp, 65u * 3u + 256u) &&
         read_fixed_string(fp, g_worker_brain.feature_set,
                           sizeof(g_worker_brain.feature_set), 32u);

    fclose(fp);

    if (!ok) {
        worker_brain_free();
        set_err(err, err_size, "unsupported or truncated signal-npc-worker checkpoint");
        return false;
    }
    if (g_worker_brain.feature_encoder_version != 1u ||
        strcmp(g_worker_brain.feature_set, "signal-npc-worker-v1") != 0) {
        worker_brain_free();
        set_err(err, err_size, "checkpoint is not signal-npc-worker-v1");
        return false;
    }

    g_worker_brain.loaded = 1;
    return true;
}

bool signal_npc_worker_brain_loaded(void) {
    return g_worker_brain.loaded != 0;
}

uint64_t signal_npc_worker_brain_inference_count(void) {
    return g_worker_brain.inference_count;
}

uint64_t signal_npc_worker_brain_decision_count(void) {
    return g_worker_brain.decision_count;
}

uint64_t signal_npc_worker_brain_teacher_decision_count(void) {
    return g_worker_brain.teacher_decision_count;
}

const char *signal_npc_worker_option_name(signal_npc_worker_option_t option) {
    switch (option) {
    case SIGNAL_NPC_WORKER_OPTION_WAIT: return "wait";
    case SIGNAL_NPC_WORKER_OPTION_MINE_HOME: return "mine_home_ore";
    case SIGNAL_NPC_WORKER_OPTION_HAUL_CONTRACT: return "haul_best_contract";
    case SIGNAL_NPC_WORKER_OPTION_GOSSIP_COURIER: return "gossip_courier";
    case SIGNAL_NPC_WORKER_OPTION_SELF_REFIT_HOME: return "self_refit_home";
    case SIGNAL_NPC_WORKER_OPTION_IMPORT_FRAME: return "import_frame_modules";
    case SIGNAL_NPC_WORKER_OPTION_IMPORT_LASER: return "import_laser_modules";
    case SIGNAL_NPC_WORKER_OPTION_IMPORT_TRACTOR: return "import_tractor_modules";
    case SIGNAL_NPC_WORKER_OPTION_SUPPLY_FRONTIER: return "supply_frontier";
    case SIGNAL_NPC_WORKER_OPTION_ESCORT_CONVOY: return "escort_convoy";
    case SIGNAL_NPC_WORKER_OPTION_PATROL_ROUTE: return "patrol_route";
    case SIGNAL_NPC_WORKER_OPTION_TAKE_RISKY_PROFIT: return "take_risky_profit";
    case SIGNAL_NPC_WORKER_OPTION_COUNT:
    default: return "unknown";
    }
}

static double clip(double x, double lo, double hi) {
    if (!isfinite(x)) return 0.0;
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static void fill_features(const signal_npc_worker_candidate_t *c,
                          double row[SNWB_FEATURE_COUNT]) {
    memset(row, 0, SNWB_FEATURE_COUNT * sizeof(*row));
    int opt = (int)c->option;
    int v1_opt = opt;
    if (v1_opt < 0 || v1_opt >= SIGNAL_NPC_WORKER_OPTION_V1_COUNT)
        v1_opt = (int)SIGNAL_NPC_WORKER_OPTION_HAUL_CONTRACT;
    row[0] = 1.0;
    row[1] = clip((double)v1_opt /
                      (double)(SIGNAL_NPC_WORKER_OPTION_V1_COUNT - 1),
                  0.0, 1.0);
    if (opt >= 0 && opt < SIGNAL_NPC_WORKER_OPTION_V1_COUNT)
        row[2 + opt] = 1.0;
    row[10] = c->role == NPC_ROLE_MINER ? 1.0 : 0.0;
    row[11] = c->role == NPC_ROLE_HAULER ? 1.0 : 0.0;
    row[12] = clip((double)c->mining_level, 0.0, 5.0) / 5.0;
    row[13] = clip((double)c->hold_level, 0.0, 5.0) / 5.0;
    row[14] = clip((double)c->tractor_level, 0.0, 5.0) / 5.0;
    row[15] = clip((double)c->home_station / 2.0, 0.0, 4.0);
    row[16] = clip((double)c->home_balance / 1500.0, 0.0, 1.5);
    row[17] = clip((double)c->home_refit_stock / 24.0, 0.0, 4.0);
    row[18] = clip((double)c->remote_refit_stock / 24.0, 0.0, 4.0);
    row[19] = clip((double)c->desired_units / 20.0, 0.0, 4.0);
    row[20] = clip((double)c->refit_cost / 1500.0, 0.0, 2.0);
    row[21] = c->home_balance + 0.01f >= c->refit_cost ? 1.0 : 0.0;
    row[22] = c->desired_upgrade == SHIP_UPGRADE_MINING ? 1.0 : 0.0;
    row[23] = c->desired_upgrade == SHIP_UPGRADE_HOLD ? 1.0 : 0.0;
    row[24] = c->desired_upgrade == SHIP_UPGRADE_TRACTOR ? 1.0 : 0.0;
    row[25] = c->desired_commodity == COMMODITY_FRAME ? 1.0 : 0.0;
    row[26] = c->desired_commodity == COMMODITY_LASER_MODULE ? 1.0 : 0.0;
    row[27] = c->desired_commodity == COMMODITY_TRACTOR_MODULE ? 1.0 : 0.0;
    row[28] = clip((double)c->best_contract_value / 600.0, 0.0, 2.0);
    row[29] = clip((double)c->best_contract_stock / 24.0, 0.0, 4.0);
    row[30] = c->best_contract_dest >= 0 &&
              c->best_contract_dest != c->home_station ? 1.0 : 0.0;
    row[31] = c->mine_pressure ? 1.0 : 0.0;
    row[32] = clip(c->persona_risk, 0.0, 1.0);
    row[33] = clip(c->persona_growth, 0.0, 1.0);
    row[34] = clip(c->persona_patience, 0.0, 1.0);
    row[35] = clip((double)c->route_km / 20.0, 0.0, 1.0);
    row[36] = c->home_has_dock ? 1.0 : 0.0;
    row[37] = c->home_has_shipyard ? 1.0 : 0.0;
    row[38] = c->home_has_furnace ? 1.0 : 0.0;
    row[39] = c->home_has_frame_press ? 1.0 : 0.0;
    row[40] = c->home_has_laser_fab ? 1.0 : 0.0;
    row[41] = c->home_has_tractor_fab ? 1.0 : 0.0;
    row[42] = c->option == SIGNAL_NPC_WORKER_OPTION_WAIT ? 1.0 : 0.0;
    row[43] = c->option == SIGNAL_NPC_WORKER_OPTION_SELF_REFIT_HOME &&
              c->home_refit_stock >= (float)c->desired_units ? 1.0 : 0.0;
    row[44] = c->import_module ? 1.0 : 0.0;
    row[45] = c->legal ? 1.0 : 0.0;
    row[46] = c->travel ? 1.0 : 0.0;
    row[47] = c->self_upgrade ? 1.0 : 0.0;
    row[48] = c->import_module ? 1.0 : 0.0;
    row[49] = clip((double)c->credit_delta / 1000.0, -1.5, 2.0);
    row[50] = clip(c->refit_progress, 0.0, 1.0);
    row[51] = clip((double)c->contract_value / 600.0, 0.0, 2.0);
    row[52] = clip((double)c->cargo_moved / 16.0, 0.0, 1.0);
    row[53] = c->best_contract_commodity == COMMODITY_FERRITE_INGOT ? 1.0 : 0.0;
    row[54] = c->best_contract_commodity == COMMODITY_CUPRITE_INGOT ? 1.0 : 0.0;
    row[55] = c->best_contract_commodity == COMMODITY_CRYSTAL_INGOT ? 1.0 : 0.0;
}

static double teacher_augmented_score(const signal_npc_worker_candidate_t *c) {
    if (!c) return -1.0e300;
    double score = c->teacher_score;
    if (!c->legal) score -= 1000.0;
    double route_positive = fmax((double)c->route_success_memory,
                                 (double)c->route_proof_memory);
    score += route_positive * 0.18;
    score -= (double)c->route_danger_memory * 0.22;
    score += (double)c->provenance_pressure * 0.08;
    switch (c->option) {
    case SIGNAL_NPC_WORKER_OPTION_SUPPLY_FRONTIER:
        score += (double)c->frontier_pressure * (0.85 + 0.35 * c->persona_growth);
        if (c->frontier_supply) score += 0.12;
        break;
    case SIGNAL_NPC_WORKER_OPTION_ESCORT_CONVOY:
        score += (double)c->route_danger_memory * (0.75 + 0.25 * c->persona_risk);
        score += (double)c->escort_bonus;
        break;
    case SIGNAL_NPC_WORKER_OPTION_PATROL_ROUTE:
        score += (double)c->route_danger_memory * 0.95;
        score += (double)c->convoy_bonus * 0.35;
        break;
    case SIGNAL_NPC_WORKER_OPTION_TAKE_RISKY_PROFIT:
        score += (double)c->black_market_acceptance * (0.65 + 0.45 * c->persona_risk);
        if (c->contraband_opportunity) score += 0.22;
        if (c->policy_screening && !c->black_market_station) score -= 0.45;
        score -= (double)c->route_danger_memory * 0.18;
        break;
    default:
        break;
    }
    return score;
}

static double forward_model(const double input[SNWB_FEATURE_COUNT]) {
    double hidden0[SNWB_HIDDEN0] = {0.0};
    double hidden1[SNWB_HIDDEN1] = {0.0};
    double output[1] = {0.0};
    const double *src = input;
    double *dsts[SNWB_LAYER_COUNT - 1] = {hidden0, hidden1, output};

    for (int li = 0; li < SNWB_LAYER_COUNT - 1; li++) {
        const npc_worker_brain_layer_t *layer = &g_worker_brain.layers[li];
        double *dst = dsts[li];
        uint32_t act = (li == SNWB_LAYER_COUNT - 2)
            ? g_worker_brain.output_activation
            : g_worker_brain.hidden_activation;
        for (size_t neuron = 0; neuron < layer->current; neuron++) {
            double sum = layer->biases[neuron];
            const double *w = &layer->weights[neuron * layer->previous];
            for (size_t i = 0; i < layer->previous; i++) sum += w[i] * src[i];
            dst[neuron] = activation(sum, act);
        }
        src = dst;
    }
    g_worker_brain.inference_count++;
    return output[0];
}

int signal_npc_worker_brain_choose(const signal_npc_worker_candidate_t *candidates,
                                   int count) {
    return signal_npc_worker_brain_choose_with_scores(candidates, count,
                                                      NULL, 0);
}

int signal_npc_worker_brain_choose_with_scores(
    const signal_npc_worker_candidate_t *candidates,
    int count,
    double *scores,
    int score_count) {
    if (!candidates || count <= 0) return -1;

    g_worker_brain.decision_count++;
    int best = -1;
    double best_score = -1.0e300;

    if (!signal_npc_worker_brain_loaded()) {
        g_worker_brain.teacher_decision_count++;
        for (int i = 0; i < count; i++) {
            double score = teacher_augmented_score(&candidates[i]);
            if (scores && i < score_count) scores[i] = score;
            if (best < 0 || score > best_score) {
                best = i;
                best_score = score;
            }
        }
        return best;
    }

    for (int i = 0; i < count; i++) {
        double row[SNWB_FEATURE_COUNT];
        fill_features(&candidates[i], row);
        double score = forward_model(row);
        if (candidates[i].option >= SIGNAL_NPC_WORKER_OPTION_V1_COUNT)
            score = teacher_augmented_score(&candidates[i]);
        if (scores && i < score_count) scores[i] = score;
        if (best < 0 || score > best_score) {
            best = i;
            best_score = score;
        }
    }
    return best;
}
