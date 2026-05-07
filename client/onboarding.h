/*
 * onboarding.h — First-run guide objectives.
 * First-run teaching state plus accessors for the SIGNAL hint surface.
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
void onboarding_mark_boosted(void);

/* Returns true and fills message with the tracked contract's next concrete
 * player step: fracture, tow to furnace, deliver, or make the upstream good. */
bool contract_step_hint(char *message, size_t message_size);

/* Returns true and fills label/message when onboarding, tracked work, or a
 * ready ship upgrade has a timely hint. */
bool onboarding_hint(char *label, size_t label_size,
                     char *message, size_t message_size);

#endif /* ONBOARDING_H */
