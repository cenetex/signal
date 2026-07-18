/*
 * wire_codec.h -- Canonical little-endian wire primitives.
 *
 * Protocol encoders and decoders must use these helpers instead of carrying
 * private byte-order implementations.  The cursor API is intentionally
 * fail-closed: once a bounds check fails, subsequent operations are no-ops
 * and wire_*_ok() remains false.
 */
#ifndef SIGNAL_WIRE_CODEC_H
#define SIGNAL_WIRE_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline void wire_write_u16_le(uint8_t *buf, uint16_t v) {
    buf[0] = (uint8_t)v;
    buf[1] = (uint8_t)(v >> 8);
}

static inline uint16_t wire_read_u16_le(const uint8_t *buf) {
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static inline void wire_write_i16_le(uint8_t *buf, int16_t v) {
    wire_write_u16_le(buf, (uint16_t)v);
}

static inline int16_t wire_read_i16_le(const uint8_t *buf) {
    return (int16_t)wire_read_u16_le(buf);
}

static inline void wire_write_u32_le(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)v;
    buf[1] = (uint8_t)(v >> 8);
    buf[2] = (uint8_t)(v >> 16);
    buf[3] = (uint8_t)(v >> 24);
}

static inline uint32_t wire_read_u32_le(const uint8_t *buf) {
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

static inline void wire_write_u64_le(uint8_t *buf, uint64_t v) {
    for (unsigned int i = 0; i < 8; i++)
        buf[i] = (uint8_t)(v >> (8u * i));
}

static inline uint64_t wire_read_u64_le(const uint8_t *buf) {
    uint64_t v = 0;
    for (unsigned int i = 0; i < 8; i++)
        v |= ((uint64_t)buf[i]) << (8u * i);
    return v;
}

static inline void wire_write_f32_le(uint8_t *buf, float v) {
    union { float f; uint32_t u; } conv;
    conv.f = v;
    wire_write_u32_le(buf, conv.u);
}

static inline float wire_read_f32_le(const uint8_t *buf) {
    union { float f; uint32_t u; } conv;
    conv.u = wire_read_u32_le(buf);
    return conv.f;
}

typedef struct wire_writer {
    uint8_t *data;
    size_t capacity;
    size_t offset;
    bool ok;
} wire_writer_t;

typedef struct wire_reader {
    const uint8_t *data;
    size_t length;
    size_t offset;
    bool ok;
} wire_reader_t;

static inline wire_writer_t wire_writer_init(uint8_t *data, size_t capacity) {
    wire_writer_t w = {data, capacity, 0, data != NULL || capacity == 0};
    return w;
}

static inline wire_reader_t wire_reader_init(const uint8_t *data, size_t length) {
    wire_reader_t r = {data, length, 0, data != NULL || length == 0};
    return r;
}

static inline bool wire_writer_reserve(wire_writer_t *w, size_t size) {
    if (!w || !w->ok || w->offset > w->capacity ||
        size > w->capacity - w->offset) {
        if (w) w->ok = false;
        return false;
    }
    return true;
}

static inline bool wire_reader_require(wire_reader_t *r, size_t size) {
    if (!r || !r->ok || r->offset > r->length ||
        size > r->length - r->offset) {
        if (r) r->ok = false;
        return false;
    }
    return true;
}

static inline void wire_put_u16(wire_writer_t *w, uint16_t v) {
    if (!wire_writer_reserve(w, 2)) return;
    wire_write_u16_le(w->data + w->offset, v);
    w->offset += 2;
}

static inline void wire_put_i16(wire_writer_t *w, int16_t v) {
    wire_put_u16(w, (uint16_t)v);
}

static inline void wire_put_u32(wire_writer_t *w, uint32_t v) {
    if (!wire_writer_reserve(w, 4)) return;
    wire_write_u32_le(w->data + w->offset, v);
    w->offset += 4;
}

static inline void wire_put_u64(wire_writer_t *w, uint64_t v) {
    if (!wire_writer_reserve(w, 8)) return;
    wire_write_u64_le(w->data + w->offset, v);
    w->offset += 8;
}

static inline void wire_put_f32(wire_writer_t *w, float v) {
    if (!wire_writer_reserve(w, 4)) return;
    wire_write_f32_le(w->data + w->offset, v);
    w->offset += 4;
}

static inline uint16_t wire_get_u16(wire_reader_t *r) {
    if (!wire_reader_require(r, 2)) return 0;
    uint16_t v = wire_read_u16_le(r->data + r->offset);
    r->offset += 2;
    return v;
}

static inline int16_t wire_get_i16(wire_reader_t *r) {
    return (int16_t)wire_get_u16(r);
}

static inline uint32_t wire_get_u32(wire_reader_t *r) {
    if (!wire_reader_require(r, 4)) return 0;
    uint32_t v = wire_read_u32_le(r->data + r->offset);
    r->offset += 4;
    return v;
}

static inline uint64_t wire_get_u64(wire_reader_t *r) {
    if (!wire_reader_require(r, 8)) return 0;
    uint64_t v = wire_read_u64_le(r->data + r->offset);
    r->offset += 8;
    return v;
}

static inline float wire_get_f32(wire_reader_t *r) {
    if (!wire_reader_require(r, 4)) return 0.0f;
    float v = wire_read_f32_le(r->data + r->offset);
    r->offset += 4;
    return v;
}

#endif /* SIGNAL_WIRE_CODEC_H */
