#ifndef SIGNAL_SAFE_TYPES_H
#define SIGNAL_SAFE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *ptr;
    size_t len;
} signal_byte_slice_t;

typedef struct {
    uint8_t *ptr;
    size_t len;
} signal_byte_slice_mut_t;

typedef struct {
    const char *ptr;
    size_t len;
} signal_str_t;

typedef struct {
    char *ptr;
    size_t len;
} signal_str_mut_t;

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

static inline bool signal_checked_add_size(size_t a, size_t b, size_t *out) {
    if (out == NULL) return false;
#if __has_builtin(__builtin_add_overflow)
    return !__builtin_add_overflow(a, b, out);
#else
    if ((size_t)-1 - a < b) return false;
    *out = a + b;
    return true;
#endif
}

static inline bool signal_checked_mul_size(size_t a, size_t b, size_t *out) {
    if (out == NULL) return false;
#if __has_builtin(__builtin_mul_overflow)
    return !__builtin_mul_overflow(a, b, out);
#else
    if (a != 0 && b > (size_t)-1 / a) return false;
    *out = a * b;
    return true;
#endif
}

static inline signal_str_t signal_str_from_cstr(const char *s) {
    signal_str_t out = {0};
    if (s == NULL) return out;
    while (s[out.len] != '\0') out.len++;
    out.ptr = s;
    return out;
}

#endif /* SIGNAL_SAFE_TYPES_H */
