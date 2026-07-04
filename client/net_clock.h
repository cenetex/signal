/*
 * net_clock.h -- Small shared millisecond clock for client network timing.
 *
 * The value is intentionally 32-bit because wire/debug paths only compare
 * elapsed deltas; unsigned subtraction keeps wraparound well-defined.
 */
#ifndef NET_CLOCK_H
#define NET_CLOCK_H

#include <stdint.h>
#include <time.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static inline uint32_t net_now_ms32(void) {
#ifdef __EMSCRIPTEN__
    double ms = emscripten_get_now();
    return (uint32_t)(ms > 0.0 ? ms : 0.0);
#elif defined(_WIN32)
    return (uint32_t)GetTickCount64();
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        uint64_t ms = (uint64_t)ts.tv_sec * 1000ull +
                      (uint64_t)ts.tv_nsec / 1000000ull;
        return (uint32_t)ms;
    }
    return (uint32_t)((uint64_t)time(NULL) * 1000ull);
#else
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == TIME_UTC) {
        uint64_t ms = (uint64_t)ts.tv_sec * 1000ull +
                      (uint64_t)ts.tv_nsec / 1000000ull;
        return (uint32_t)ms;
    }
    return (uint32_t)((uint64_t)time(NULL) * 1000ull);
#endif
}

#endif
