#ifndef SHARED_BASE64_H
#define SHARED_BASE64_H
#include <stdint.h>
#include <stddef.h>

static const char BASE64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static inline int base64_encode(const uint8_t *data, size_t len, char *out, size_t out_cap) {
    size_t i = 0, j = 0;
    while (i < len) {
        size_t consumed = 0;
        uint32_t a = 0, b = 0, c = 0;
        if (i < len) { a = data[i++]; consumed++; }
        if (i < len) { b = data[i++]; consumed++; }
        if (i < len) { c = data[i++]; consumed++; }
        uint32_t triple = (a << 16) | (b << 8) | c;
        if (j + 4 >= out_cap) return -1;
        out[j++] = BASE64_ALPHABET[(triple >> 18) & 0x3f];
        out[j++] = BASE64_ALPHABET[(triple >> 12) & 0x3f];
        out[j++] = (consumed >= 2) ? BASE64_ALPHABET[(triple >> 6) & 0x3f] : '=';
        out[j++] = (consumed >= 3) ? BASE64_ALPHABET[triple & 0x3f] : '=';
    }
    out[j] = 0;
    return (int)j;
}

static inline int base64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static inline int base64_decode(const char *in, uint8_t *out, size_t out_cap) {
    size_t len = 0;
    while (in[len]) len++;
    if (len % 4 != 0) return -1;
    size_t j = 0;
    for (size_t i = 0; i < len; i += 4) {
        int a = base64_decode_char(in[i]);
        int b = base64_decode_char(in[i+1]);
        int c = (in[i+2] == '=') ? 0 : base64_decode_char(in[i+2]);
        int d = (in[i+3] == '=') ? 0 : base64_decode_char(in[i+3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return -1;
        uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
        if (j >= out_cap) return -1;
        out[j++] = (uint8_t)(triple >> 16);
        if (in[i+2] != '=' && j < out_cap) out[j++] = (uint8_t)(triple >> 8);
        if (in[i+3] != '=' && j < out_cap) out[j++] = (uint8_t)triple;
    }
    return (int)j;
}

#endif
