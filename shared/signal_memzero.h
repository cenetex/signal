/*
 * signal_memzero.h -- non-elidable clearing for secret material.
 *
 * Ordinary memset calls may be removed when the compiler proves that a
 * buffer is dead. Use this API for keys, secret-derived seeds, and signing
 * scratch at lifecycle boundaries. It is not a general object reset helper.
 */
#ifndef SHARED_SIGNAL_MEMZERO_H
#define SHARED_SIGNAL_MEMZERO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clear ptr[0..len) through a platform primitive or observable fallback.
 * A null pointer is accepted only as a no-op; callers still own all bounds. */
void signal_memzero_explicit(void *ptr, size_t len);

/* Stable diagnostic string used by runtime tests and operator diagnostics. */
const char *signal_memzero_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_SIGNAL_MEMZERO_H */
