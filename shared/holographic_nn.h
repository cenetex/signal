/*
 * holographic_nn.h — Vector Symbolic Architecture (VSA) primitives and
 * holographic associative memory for ship pilot neural networks.
 *
 * Operations:
 *   bind(k, v)    — associative pairing via circular convolution (FFT domain)
 *   bundle(a, b)  — superposition via normalized addition
 *   unbind(S, k)  — retrieve value from store S given key k
 *
 * All vectors live on S^{d-1} (the unit hypersphere in R^HNN_DIM).
 *
 * Key vectors are constructed in the frequency domain with unit magnitude
 * (flat power spectrum) so that bind/unbind are near-perfect inverses.
 *
 * The holographic pilot memory stores (state → action) associations as
 * a single fixed-size vector that can be queried each tick to select
 * the best flight action.
 */
#ifndef HOLOGRAPHIC_NN_H
#define HOLOGRAPHIC_NN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Must be a power of 2 for efficient radix-2 FFT. */
#define HNN_DIM 1024

/* Discrete flight actions the holographic brain can select. */
enum {
    HNN_ACTION_COUNT = 9,
    HNN_FEATURE_COUNT = 24,
};

/* Pre-generated action vectors (deterministic from seed). */
typedef struct {
    float vecs[HNN_ACTION_COUNT][HNN_DIM];
} hnn_action_table_t;

/*
 * Holographic pilot memory.
 *
 * Stores bundled (state_i → action_i) associations in a single
 * HNN_DIM-dimensional vector.  Retrieval is O(HNN_DIM log HNN_DIM)
 * per query (one FFT pair), dominated by bind/bundle operations.
 */
typedef struct {
    float  store[HNN_DIM];  /* bundled associative memory trace */
    int    experience_count; /* number of stored experiences */
    float  last_retrieval_similarity; /* diagnostic: how well the last query matched */
} hnn_memory_t;

/* --- Core VSA operations --- */

/*
 * Generate a deterministic unit hypersphere vector with flat power
 * spectrum (every frequency bin has unit magnitude). The seed fully
 * determines the vector. The result is normalized to unit length.
 */
void hnn_key_vector(uint64_t seed, float out[HNN_DIM]);

/*
 * Normalize a vector onto the unit hypersphere in-place.
 * Returns the original L2 norm (0 if input was zero).
 */
float hnn_normalize(float v[HNN_DIM]);

/*
 * Circular convolution in the FFT domain: c = a (x) b.
 * a and b are not modified; c is normalized onto the hypersphere.
 */
void hnn_bind(const float a[HNN_DIM], const float b[HNN_DIM],
              float c[HNN_DIM]);

/*
 * Circular correlation: c = a (-) b.
 * Implemented as bind with conjugate in FFT space.
 */
void hnn_unbind(const float a[HNN_DIM], const float b[HNN_DIM],
                float c[HNN_DIM]);

/*
 * Superpose b onto a (a = a + b), then renormalize.
 */
void hnn_bundle(float a[HNN_DIM], const float b[HNN_DIM]);

/*
 * Cosine similarity (dot product) between two unit vectors.
 */
float hnn_similarity(const float a[HNN_DIM], const float b[HNN_DIM]);

/* --- Holographic memory --- */

/*
 * Initialize an empty holographic memory.
 */
void hnn_memory_init(hnn_memory_t *mem);

/*
 * Store a key-value association into holographic memory.
 * M += bind(key, value); then re-normalizes.
 * Increments experience_count.
 */
void hnn_memory_store(hnn_memory_t *mem,
                      const float key[HNN_DIM],
                      const float value[HNN_DIM]);

/*
 * Retrieve a value from holographic memory given a key.
 * Returns unbind(normalize(M), key).
 */
void hnn_memory_retrieve(const hnn_memory_t *mem,
                         const float key[HNN_DIM],
                         float value_out[HNN_DIM]);

/*
 * Iterative cleanup: repeatedly subtract the current best-guess
 * contribution from memory and decode the residual. Improves
 * retrieval fidelity.
 */
void hnn_memory_cleanup(const hnn_memory_t *mem,
                        const float key[HNN_DIM],
                        float value_out[HNN_DIM],
                        int steps);

/* --- Pilot feature encoding --- */

/*
 * Pilot state features that get encoded into a holographic state vector.
 * Each feature is a (feature_index, value) pair bound together, then
 * all pairs are bundled into a single state vector.
 */
typedef struct {
    float target_dist;          /* distance to target, scaled 0..1 */
    float heading_error;        /* heading error in radians, scaled -1..1 */
    float heading_cos;          /* cos(heading_error), -1..1 */
    float heading_sin;          /* sin(heading_error), -1..1 */
    float speed;                /* current speed, scaled 0..1 */
    float forward_speed;        /* forward component, scaled -1..1 */
    float lateral_speed;        /* lateral component, scaled -1..1 */
    float brake_distance;       /* braking distance factor, scaled 0..1 */
    float fwd_clear;            /* forward clearance, 0..1 */
    float left_clear;           /* left clearance, 0..1 */
    float right_clear;          /* right clearance, 0..1 */
    float signal_quality;       /* signal strength 0..1 */
    float hull_ratio;           /* hull / max_hull, 0..1 */
    float path_count;           /* nav path count, scaled 0..1 */
    float path_current;         /* nav path current, scaled 0..1 */
    float fwd_blocked;          /* forward path blocked, 0 or 1 */
    float left_blocked;         /* left path blocked, 0 or 1 */
    float right_blocked;        /* right path blocked, 0 or 1 */
    float goal_close;           /* 1.0 - scaled_dist, high when near */
    float action_delta_turn;    /* -1, 0, or 1 for action candidate */
    float action_delta_thrust;  /* -1, 0, or 1 for action candidate */
    float action_is_none;       /* 1 if action is idle, 0 otherwise */
    float action_is_reverse;    /* 1 if action is reverse thrust, 0 otherwise */
    float composite_dot;        /* turn * heading_sin — directional alignment */
} hnn_pilot_features_t;

/*
 * Encode pilot features into a holographic state key vector.
 * Uses feature_index (0..HNN_FEATURE_COUNT-1) bound with
 * feature_value, then bundles all pairs.
 */
void hnn_encode_state(const hnn_pilot_features_t *features,
                      float state_out[HNN_DIM]);

/* --- Action table --- */

/*
 * Initialize the action table with deterministic action vectors.
 * Action 0 = NONE, 1 = W, 2 = A, 3 = D, 4 = S,
 * 5 = WA, 6 = WD, 7 = SA, 8 = SD.
 */
void hnn_action_table_init(hnn_action_table_t *table);

/*
 * Select the best action from holographic memory given current state.
 * Returns the action index (0..8) with the highest cosine similarity
 * between the retrieved vector and the action vectors.
 */
int hnn_select_action(const hnn_memory_t *mem,
                      const hnn_action_table_t *actions,
                      const hnn_pilot_features_t *state,
                      float *out_confidence);

#endif /* HOLOGRAPHIC_NN_H */

