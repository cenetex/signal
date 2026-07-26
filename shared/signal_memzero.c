#include "signal_memzero.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

void signal_memzero_explicit(void *ptr, size_t len) {
    if (!ptr || len == 0) return;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    memset_explicit(ptr, 0, len);
#elif defined(_WIN32)
    SecureZeroMemory(ptr, len);
#else
    /*
     * C11 fallback: stores through a volatile-qualified lvalue are observable
     * side effects and therefore cannot be removed as dead stores. Keep this
     * small and local so every non-C23 platform follows one reviewed path.
     */
    volatile unsigned char *bytes = (volatile unsigned char *)ptr;
    while (len-- > 0) *bytes++ = 0;
#endif
}

const char *signal_memzero_backend(void) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    return "c23-memset-explicit";
#elif defined(_WIN32)
    return "windows-secure-zero-memory";
#else
    return "c11-volatile-store";
#endif
}
