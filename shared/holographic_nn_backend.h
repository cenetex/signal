#ifndef HOLOGRAPHIC_NN_BACKEND_H
#define HOLOGRAPHIC_NN_BACKEND_H

#include <stdbool.h>
#include <stddef.h>

#include "holographic_nn.h"

typedef enum hnn_backend_kind {
    HNN_BACKEND_BUILTIN_RADIX2 = 0,
    HNN_BACKEND_LECORE_DIRECT = 1,
    HNN_BACKEND_LECORE_RADIX2 = 2,
} hnn_backend_kind_t;

typedef enum hnn_backend_status {
    HNN_BACKEND_STATUS_OK = 0,
    HNN_BACKEND_STATUS_INVALID_ARGUMENT = 1,
    HNN_BACKEND_STATUS_NONFINITE = 2,
    HNN_BACKEND_STATUS_UNAVAILABLE = 3,
    HNN_BACKEND_STATUS_INIT_FAILED = 4,
    HNN_BACKEND_STATUS_OPERATION_FAILED = 5,
} hnn_backend_status_t;

/* Private build/replay metadata for Signal's HNN numeric backend. */
typedef struct hnn_backend_metadata {
    const char *active_library;
    const char *active_library_version;
    unsigned active_abi_version;
    const char *active_backend;
    int dimension;
    const char *active_source_revision;
    size_t scratch_bytes;
    bool liblecore_compiled;
    const char *liblecore_version;
    unsigned liblecore_abi_version;
    const char *liblecore_source_revision;
    const char *liblecore_source_checksum;
} hnn_backend_metadata_t;

hnn_backend_metadata_t hnn_backend_metadata(void);
hnn_backend_kind_t hnn_backend_active_kind(void);
bool hnn_backend_is_available(hnn_backend_kind_t kind);
const char *hnn_backend_kind_name(hnn_backend_kind_t kind);

/* Private numeric adapter used by the stable public HNN interface. */
float hnn_backend_normalize(float v[HNN_DIM]);
bool hnn_backend_bind(const float a[HNN_DIM],
                      const float b[HNN_DIM],
                      float out[HNN_DIM]);
bool hnn_backend_unbind(const float composite[HNN_DIM],
                        const float key[HNN_DIM],
                        float out[HNN_DIM]);
bool hnn_backend_bundle(float a[HNN_DIM], const float b[HNN_DIM]);
float hnn_backend_similarity(const float a[HNN_DIM],
                             const float b[HNN_DIM]);
bool hnn_backend_cleanup(const float query[HNN_DIM],
                         const float *candidates,
                         size_t candidate_count,
                         size_t *out_index,
                         float *out_score);

/* Explicit-kind entry points keep conformance checks in one process. */
float hnn_backend_normalize_for(hnn_backend_kind_t kind,
                                float v[HNN_DIM]);
bool hnn_backend_bind_for(hnn_backend_kind_t kind,
                          const float a[HNN_DIM],
                          const float b[HNN_DIM],
                          float out[HNN_DIM]);
bool hnn_backend_unbind_for(hnn_backend_kind_t kind,
                            const float composite[HNN_DIM],
                            const float key[HNN_DIM],
                            float out[HNN_DIM]);
bool hnn_backend_bundle_for(hnn_backend_kind_t kind,
                            float a[HNN_DIM],
                            const float b[HNN_DIM]);
float hnn_backend_similarity_for(hnn_backend_kind_t kind,
                                 const float a[HNN_DIM],
                                 const float b[HNN_DIM]);
bool hnn_backend_cleanup_for(hnn_backend_kind_t kind,
                             const float query[HNN_DIM],
                             const float *candidates,
                             size_t candidate_count,
                             size_t *out_index,
                             float *out_score);

bool hnn_backend_thread_init(hnn_backend_kind_t kind);
size_t hnn_backend_scratch_bytes(hnn_backend_kind_t kind);
size_t hnn_backend_thread_allocation_count(hnn_backend_kind_t kind);
size_t hnn_backend_thread_memory_bytes(hnn_backend_kind_t kind);
void hnn_backend_thread_reset_for_tests(void);
hnn_backend_status_t hnn_backend_last_status(void);
const char *hnn_backend_status_string(hnn_backend_status_t status);

#endif /* HOLOGRAPHIC_NN_BACKEND_H */
