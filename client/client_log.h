/* client_log.h -- Persistent native client stdout/stderr logging. */
#ifndef SIGNAL_CLIENT_LOG_H
#define SIGNAL_CLIENT_LOG_H

#include <stdbool.h>
#include <stddef.h>

#define SIGNAL_CLIENT_LOG_MAX_BYTES (8u * 1024u * 1024u)

/* Redirect native stdout and stderr to the platform-default append-only log.
 * The streams are line-buffered so newline-terminated telemetry survives an
 * interrupt. Web builds keep the browser console and return true unchanged.
 *
 * SIGNAL_LOG_PATH overrides the default path. SIGNAL_LOG_PERSIST=0 disables
 * file redirection while retaining line buffering for terminal launches. */
bool client_log_init(void);

/* Emit a clean session boundary and flush every stdio stream. */
void client_log_shutdown(void);

/* The active path, or an empty string when persistence is disabled/unavailable. */
const char *client_log_path(void);

/* Rotate path to path.1 when its size reaches max_bytes. Public so the file
 * lifecycle can be tested without redirecting the test runner's stdio. */
bool client_log_rotate_file(const char *path, size_t max_bytes);

#endif /* SIGNAL_CLIENT_LOG_H */
