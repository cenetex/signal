/*
 * onboarding.h — First-run progression hints.
 */
#ifndef ONBOARDING_H
#define ONBOARDING_H

#include <stddef.h>
#include <stdbool.h>

void onboarding_load(void);
void onboarding_mark_launched(void);
void onboarding_mark_mined(void);
void onboarding_mark_collected(void);
void onboarding_mark_sold(void);
void onboarding_mark_bought(void);
void onboarding_mark_upgraded(void);
void onboarding_mark_got_scaffold(void);
void onboarding_mark_placed_outpost(void);

/* Returns true and fills label/message if onboarding has a hint to show. */
bool onboarding_hint(char *label, size_t label_size,
                     char *message, size_t message_size);

#endif /* ONBOARDING_H */
