/* story_runtime.h -- Client persistence and event bridge for the worker story. */
#ifndef SIGNAL_CLIENT_STORY_RUNTIME_H
#define SIGNAL_CLIENT_STORY_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>

void story_runtime_load(void);

bool story_runtime_mark_hail(int station_index,
                             char *notice, size_t notice_size);
bool story_runtime_mark_signal_gap(char *notice, size_t notice_size);
bool story_runtime_mark_outpost_placed(char *notice, size_t notice_size);
bool story_runtime_mark_outpost_active(int station_index,
                                       char *notice, size_t notice_size);
bool story_runtime_mark_delivery(char *notice, size_t notice_size);
bool story_runtime_mark_dock(int station_index,
                             char *notice, size_t notice_size);

bool story_runtime_hint(char *label, size_t label_size,
                        char *message, size_t message_size);
bool story_runtime_is_complete(void);

#endif /* SIGNAL_CLIENT_STORY_RUNTIME_H */
