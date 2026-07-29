#include "signal_memzero.h"

#if !defined(SIGNAL_MEMZERO_FORCE_FALLBACK) && defined(_WIN32)
#include <windows.h>
#elif !defined(SIGNAL_MEMZERO_FORCE_FALLBACK) && \
      defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#include <string.h>
#endif

void signal_memzero_explicit(void *ptr, size_t len) {
    if (!ptr || len == 0) return;

#if !defined(SIGNAL_MEMZERO_FORCE_FALLBACK) && defined(_WIN32)
    (void)SecureZeroMemory(ptr, len);
#elif !defined(SIGNAL_MEMZERO_FORCE_FALLBACK) && \
      defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    memset_explicit(ptr, 0, len);
#else
    /*
     * C11 fallback: stores through a volatile-qualified lvalue are observable
     * side effects and cannot be removed as dead stores. Keep the fallback
     * small so optimized-code inspection has one stable review point.
     */
    volatile unsigned char *bytes = (volatile unsigned char *)ptr;
    while (len-- > 0) *bytes++ = 0;
#endif
}

const char *signal_memzero_backend(void) {
#if !defined(SIGNAL_MEMZERO_FORCE_FALLBACK) && defined(_WIN32)
    return "windows-secure-zero-memory";
#elif !defined(SIGNAL_MEMZERO_FORCE_FALLBACK) && \
      defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    return "c23-memset-explicit";
#else
    return "c11-volatile-store";
#endif
}
