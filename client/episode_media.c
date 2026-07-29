#include "episode_media.h"

#include <stdio.h>
#include <stdlib.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

uint8_t *episode_media_read_file(const char *path, size_t *out_size,
                                 episode_failure_t *out_failure) {
    if (out_size) *out_size = 0;
    if (out_failure) *out_failure = EPISODE_FAILURE_FILE_READ;
    if (!path || !out_size || !out_failure) return NULL;

    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long file_size = ftell(file);
    if (file_size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    uint8_t *data = (uint8_t *)malloc((size_t)file_size);
    if (!data) {
        fclose(file);
        *out_failure = EPISODE_FAILURE_ALLOCATION;
        return NULL;
    }
    size_t read_size = fread(data, 1, (size_t)file_size, file);
    fclose(file);
    if (read_size != (size_t)file_size) {
        free(data);
        return NULL;
    }

    *out_size = (size_t)file_size;
    *out_failure = EPISODE_FAILURE_NONE;
    return data;
}

void *episode_media_create_decoder(uint8_t *data, size_t size,
                                   episode_failure_t *out_failure) {
    if (out_failure) *out_failure = EPISODE_FAILURE_DECODER;
    if (!data || size == 0 || !out_failure) {
        free(data);
        return NULL;
    }

    plm_t *decoder = plm_create_with_memory(data, size, 1);
    if (!decoder) {
        free(data);
        return NULL;
    }
    if (!plm_has_headers(decoder) ||
        plm_get_num_video_streams(decoder) < 1 ||
        plm_get_width(decoder) <= 0 ||
        plm_get_height(decoder) <= 0) {
        plm_destroy(decoder);
        return NULL;
    }

    *out_failure = EPISODE_FAILURE_NONE;
    return decoder;
}
