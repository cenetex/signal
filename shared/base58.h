/*
 * base58.h -- Bitcoin-style base58 encoder (public domain).
 *
 * Alphabet: no '0', 'O', 'I', 'l' (visually ambiguous characters
 * omitted). Used for callsign derivation, grade classification,
 * and display — never for game state bytes (those stay binary).
 */
#ifndef SHARED_BASE58_H
#define SHARED_BASE58_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char BASE58_ALPHABET[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/* Encode `in` (len bytes) into `out` as a null-terminated base58 string.
 * out_cap must be large enough — for a 32-byte input, worst case is 45
 * chars + null. Returns the length of the written string (excluding
 * null), or 0 on insufficient buffer. */
static inline size_t base58_encode(const uint8_t *in, size_t len,
                                   char *out, size_t out_cap) {
    if (!in || !out || out_cap == 0) return 0;
    /* Count leading zero bytes — each maps to '1' in the output. */
    size_t leading_zeros = 0;
    while (leading_zeros < len && in[leading_zeros] == 0) leading_zeros++;

    /* Big-endian digit stream; need ceil(len * log(256) / log(58)) ≈
     * len * 138/100 + 1 output digits. */
    size_t digits_cap = len * 138 / 100 + 1;
    /* Bounded stack allocation — 32-byte input → ~45 digits. */
    uint8_t digits[80];
    if (digits_cap > sizeof(digits)) digits_cap = sizeof(digits);
    memset(digits, 0, digits_cap);
    size_t digits_len = 0;

    for (size_t i = leading_zeros; i < len; i++) {
        uint32_t carry = in[i];
        for (size_t j = 0; j < digits_cap; j++) {
            if (j >= digits_len && carry == 0) break;
            carry += (uint32_t)digits[digits_cap - 1 - j] << 8;
            digits[digits_cap - 1 - j] = (uint8_t)(carry % 58);
            carry /= 58;
            if (digits_cap - 1 - j == digits_cap - 1 - digits_len) digits_len++;
        }
    }

    /* Skip leading zeros in the digit stream. */
    size_t di = digits_cap - digits_len;
    while (di < digits_cap && digits[di] == 0) di++;

    size_t out_len = leading_zeros + (digits_cap - di);
    if (out_len + 1 > out_cap) return 0;

    size_t o = 0;
    for (size_t i = 0; i < leading_zeros; i++) out[o++] = '1';
    for (size_t i = di; i < digits_cap; i++) out[o++] = BASE58_ALPHABET[digits[i]];
    out[o] = '\0';
    return o;
}

#endif /* SHARED_BASE58_H */
