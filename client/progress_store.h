#ifndef SIGNAL_CLIENT_PROGRESS_STORE_H
#define SIGNAL_CLIENT_PROGRESS_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t story;
    uint16_t guide;
} client_progress_t;

/* The endpoint and public player key identify one progress record. */
bool client_progress_scope_key(char out[65], const uint8_t pubkey[32],
                               const char *authority);
bool client_progress_decode(const char *text, client_progress_t *out);
bool client_progress_encode(char out[12], const client_progress_t *progress);
bool client_progress_read_at(const char *path, client_progress_t *out);
bool client_progress_write_at(const char *path,
                              const client_progress_t *progress);

/* Select once after identity and authority choice, before loading guide/story. */
void client_progress_select(const uint8_t pubkey[32], const char *authority);
client_progress_t client_progress_current(void);
void client_progress_save_story(uint16_t flags);
void client_progress_save_guide(uint16_t flags);
void client_progress_defer_writes(bool deferred);
uint32_t client_progress_pack_local(void);
void client_progress_restore_local(uint32_t flags);

#endif
