/*
 * input.h -- Input handling for the Signal Space Miner client.
 * Manages keyboard state, intent sampling, and notice messages.
 */
#ifndef INPUT_H
#define INPUT_H

#include "client.h"

/* Clear all held and transient key state (e.g. on focus loss). */
void clear_input_state(void);

/* Consume all one-shot key_pressed flags after processing. */
void consume_pressed_input(void);

/* Query held key state. */
bool is_key_down(sapp_keycode key);

/* Query one-shot pressed key state. */
bool is_key_pressed(sapp_keycode key);

/* Sample the current keyboard state into an input_intent_t.
 * This is the main input handler (~200 lines) covering flight,
 * docked menus, build overlay, contracts, and outpost placement. */
input_intent_t sample_input_intent(void);

/* Set the HUD notice message (printf-style). */
void set_notice(const char* fmt, ...);

#endif /* INPUT_H */
