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
#include <string.h>

#if defined(__EMSCRIPTEN__)
  #include <emscripten.h>

  /* Pull bytes from the host's WebCrypto via JS interop. We chunk at
   * 65536 because crypto.getRandomValues caps each call at 64 KiB. */
  EM_JS(void, signal_js_random, (uint8_t *buf, int n), {
      var view = HEAPU8.subarray(buf, buf + n);
      var off = 0;
      while (off < n) {
          var chunk = Math.min(65536, n - off);
          crypto.getRandomValues(view.subarray(off, off + chunk));
          off += chunk;
      }
  })

  void randombytes(uint8_t *buf, unsigned long long n) {
      while (n > 0) {
          int chunk = (n > 0x7fffffffULL) ? 0x7fffffff : (int)n;
          signal_js_random(buf, chunk);
          buf += chunk;
          n   -= (unsigned long long)chunk;
      }
  }

#elif defined(_WIN32)
  #include <windows.h>
  #include <bcrypt.h>
  #pragma comment(lib, "bcrypt.lib")

  void randombytes(uint8_t *buf, unsigned long long n) {
      while (n > 0) {
          ULONG chunk = (n > 0x7fffffffULL) ? 0x7fffffff : (ULONG)n;
          NTSTATUS s = BCryptGenRandom(NULL, buf, chunk,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
          if (s != 0) {
              /* Hard fail: better to leave the buffer zeroed and let
               * the caller's verify catch it than ship weak keys. */
              memset(buf, 0, chunk);
          }
          buf += chunk;
          n   -= chunk;
      }
  }

#else
  /* POSIX. getentropy is on macOS 10.12+, glibc 2.25+, OpenBSD, FreeBSD. */
  #include <unistd.h>
  #if defined(__APPLE__)
    #include <sys/random.h>
  #elif defined(__linux__)
    #include <sys/random.h>
  #endif

  void randombytes(uint8_t *buf, unsigned long long n) {
      while (n > 0) {
          /* getentropy caps at 256 bytes per call. */
          size_t chunk = (n > 256) ? 256 : (size_t)n;
          if (getentropy(buf, chunk) != 0) {
              /* Fail closed — zero rather than fall back to time(). */
              memset(buf, 0, chunk);
          }
          buf += chunk;
          n   -= chunk;
      }
  }
#endif
