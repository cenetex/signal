#ifndef EPISODE_MEDIA_H
#define EPISODE_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#include "episode_lifecycle.h"

/*
 * Sokol-free production seams for native file loading and MPEG decoder
 * validation. Returned file data is heap-owned. Decoder creation takes
 * ownership of data on both success and failure.
 */
uint8_t *episode_media_read_file(const char *path, size_t *out_size,
                                 episode_failure_t *out_failure);
void *episode_media_create_decoder(uint8_t *data, size_t size,
                                   episode_failure_t *out_failure);

#endif
