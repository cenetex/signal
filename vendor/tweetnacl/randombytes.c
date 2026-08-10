/*
 * randombytes.c -- TweetNaCl's required randombytes() symbol.
 *
 * TweetNaCl declares `extern void randombytes(u8 *, u64)` but ships no
 * implementation. We provide a portable one:
 *   - POSIX (macOS, Linux):  getentropy(3)
 *   - Windows:               BCryptGenRandom
 *   - Emscripten / wasm:     EM_JS into crypto.getRandomValues
 *
 * Public domain. Layer A.1 of #479.
 */

#include <stddef.h>
#include <stdint.h>

#include "signal_memzero.h"

static void signal_randombytes_clear(uint8_t *buf, unsigned long long n) {
    while (buf && n > 0) {
        size_t chunk = n > (unsigned long long)SIZE_MAX
            ? SIZE_MAX : (size_t)n;
        signal_memzero_explicit(buf, chunk);
        buf += chunk;
        n -= (unsigned long long)chunk;
    }
}

#if defined(__EMSCRIPTEN__)
  #include <emscripten.h>

  /* Pull bytes from the host's WebCrypto via JS interop. We chunk at
   * 65536 because crypto.getRandomValues caps each call at 64 KiB. */
  EM_JS(int, signal_js_random_checked, (uint8_t *buf, int n), {
      try {
          var source = globalThis.crypto;
          if (!source || typeof source.getRandomValues !== 'function') return 0;
          var view = HEAPU8.subarray(buf, buf + n);
          var off = 0;
          while (off < n) {
              var chunk = Math.min(65536, n - off);
              source.getRandomValues(view.subarray(off, off + chunk));
              off += chunk;
          }
          return 1;
      } catch (e) {
          return 0;
      }
  })

  int signal_randombytes_checked(uint8_t *buf, unsigned long long n) {
      uint8_t *start = buf;
      unsigned long long total = n;
      if (!buf && n > 0) return 0;
      while (n > 0) {
          int chunk = (n > 0x7fffffffULL) ? 0x7fffffff : (int)n;
          if (!signal_js_random_checked(buf, chunk)) {
              signal_randombytes_clear(start, total);
              return 0;
          }
          buf += chunk;
          n   -= (unsigned long long)chunk;
      }
      return 1;
  }

#elif defined(_WIN32)
  #include <windows.h>
  #include <bcrypt.h>
  #pragma comment(lib, "bcrypt.lib")

  int signal_randombytes_checked(uint8_t *buf, unsigned long long n) {
      uint8_t *start = buf;
      unsigned long long total = n;
      if (!buf && n > 0) return 0;
      while (n > 0) {
          ULONG chunk = (n > 0x7fffffffULL) ? 0x7fffffff : (ULONG)n;
          NTSTATUS s = BCryptGenRandom(NULL, buf, chunk,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
          if (s != 0) {
              signal_randombytes_clear(start, total);
              return 0;
          }
          buf += chunk;
          n   -= chunk;
      }
      return 1;
  }

#else
  /* POSIX. getentropy is on macOS 10.12+, glibc 2.25+, OpenBSD, FreeBSD. */
  #include <unistd.h>
  #if defined(__APPLE__)
    #include <sys/random.h>
  #elif defined(__linux__)
    #include <sys/random.h>
  #endif

  #include <errno.h>

  int signal_randombytes_checked(uint8_t *buf, unsigned long long n) {
      uint8_t *start = buf;
      unsigned long long total = n;
      if (!buf && n > 0) return 0;
      while (n > 0) {
          /* getentropy caps at 256 bytes per call. */
          size_t chunk = (n > 256) ? 256 : (size_t)n;
          int rc;
          do {
              rc = getentropy(buf, chunk);
          } while (rc != 0 && errno == EINTR);
          if (rc != 0) {
              signal_randombytes_clear(start, total);
              return 0;
          }
          buf += chunk;
          n   -= chunk;
      }
      return 1;
  }
#endif

/* TweetNaCl requires this legacy void symbol for primitives that generate
 * their own keys. Signal's public wrapper uses the checked function above;
 * keep the legacy entry point fail-closed for any remaining internal use. */
void randombytes(uint8_t *buf, unsigned long long n) {
    (void)signal_randombytes_checked(buf, n);
}
