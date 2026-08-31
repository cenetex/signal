#ifndef HOLOGRAPHIC_NN_BACKEND_H
#define HOLOGRAPHIC_NN_BACKEND_H

#include <stdbool.h>
#include <stddef.h>

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

#endif /* HOLOGRAPHIC_NN_BACKEND_H */
