#include "holographic_nn_backend.h"

#include "holographic_nn.h"

#ifndef SIGNAL_SOURCE_REVISION
#define SIGNAL_SOURCE_REVISION "dev"
#endif

#define LECORE_PIN_VERSION "0.1.0"
#define LECORE_PIN_ABI_VERSION 0u
#define LECORE_PIN_SOURCE_REVISION \
    "sha256:5da2817e8f2addcc15d3a97c17107c22289bb2609bbdd19f2c199d33238a5a55"
#define LECORE_PIN_SOURCE_CHECKSUM \
    "sha256:f3283ebb033e295e5dbdc46d95add91ab154253169d6a2a1ab696464b051ed07"

hnn_backend_metadata_t hnn_backend_metadata(void) {
    hnn_backend_metadata_t metadata = {
        .active_library = "signal",
        .active_library_version = "hnn-contract-1",
        .active_abi_version = HNN_CONTRACT_VERSION,
        .active_backend = "builtin-radix2",
        .dimension = HNN_DIM,
        .active_source_revision = SIGNAL_SOURCE_REVISION,
        .scratch_bytes = (size_t)HNN_DIM * 4u * sizeof(float),
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
        .liblecore_compiled = true,
#else
        .liblecore_compiled = false,
#endif
        .liblecore_version = LECORE_PIN_VERSION,
        .liblecore_abi_version = LECORE_PIN_ABI_VERSION,
        .liblecore_source_revision = LECORE_PIN_SOURCE_REVISION,
        .liblecore_source_checksum = LECORE_PIN_SOURCE_CHECKSUM,
    };
    return metadata;
}
