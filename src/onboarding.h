/*
 * onboarding.h — Minimal first-run hints (3 steps).
 * After HAIL, stations take over via contextual hail responses.
 */
#ifndef ONBOARDING_H
#define ONBOARDING_H

#include <stddef.h>
#include <stdbool.h>

void onboarding_load(void);
void onboarding_mark_fractured(void);
void onboarding_mark_tractored(void);
void onboarding_mark_hailed(void);

/* Returns true and fills label/message if onboarding has a hint to show. */
bool onboarding_hint(char *label, size_t label_size,
                     char *message, size_t message_size);

#endif /* ONBOARDING_H */
