#ifndef SHARED_COMPACT_H
#define SHARED_COMPACT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

static inline int compact_u16_size(uint16_t value) {
    if (value <= 0x7f) return 1;
    if (value <= 0x3fff) return 2;
    return 3;
}

static inline int compact_u16_encode(uint16_t value, uint8_t out[3]) {
    if (value <= 0x7f) {
        out[0] = (uint8_t)value;
        return 1;
    }
    if (value <= 0x3fff) {
        out[0] = (uint8_t)(value & 0x7f) | 0x80;
        out[1] = (uint8_t)(value >> 7);
        return 2;
    }
    out[0] = (uint8_t)(value & 0x7f) | 0x80;
    out[1] = (uint8_t)((value >> 7) & 0x7f) | 0x80;
    out[2] = (uint8_t)(value >> 14);
    return 3;
}

static inline int compact_u16_decode(const uint8_t *data, size_t data_len, uint16_t *value_out) {
    if (data_len < 1) return -1;
    uint16_t v = data[0] & 0x7f;
    if (!(data[0] & 0x80)) {
        *value_out = v;
        return 1;
    }
    if (data_len < 2) return -1;
    v |= ((uint16_t)(data[1] & 0x7f)) << 7;
    if (!(data[1] & 0x80)) {
        *value_out = v;
        return 2;
    }
    if (data_len < 3) return -1;
    v |= ((uint16_t)data[2]) << 14;
    *value_out = v;
    return 3;
}

#endif
