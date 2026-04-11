/*
 * onboarding.h — First-run checklist (4 steps, any order).
 * After all steps complete, stations take over via contextual hails.
 */
#ifndef ONBOARDING_H
#define ONBOARDING_H

#include <stddef.h>
#include <stdbool.h>

void onboarding_load(void);
void onboarding_mark_moved(void);
void onboarding_mark_fractured(void);
void onboarding_mark_tractored(void);
void onboarding_mark_hailed(void);

/* Returns true and fills label/message with checklist if onboarding active. */
bool onboarding_hint(char *label, size_t label_size,
                     char *message, size_t message_size);

#endif /* ONBOARDING_H */
