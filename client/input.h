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

/* Preserve Space down/up timing at the event boundary so a short tap cannot
 * be lost or lengthened while the render/simulation loop is busy. */
void input_tractor_key_down(void);
void input_tractor_key_up(void);

/* Query held key state. */
bool is_key_down(sapp_keycode key);

/* Query one-shot pressed key state. */
bool is_key_pressed(sapp_keycode key);

/* Sample continuous flight controls only. */
void input_sample_movement(input_intent_t *intent);
void input_sample_network_controls(input_intent_t *intent);

/* Shared intent classifiers for prediction suppression and network packets. */
bool input_intent_has_network_action(const input_intent_t *intent);
uint8_t input_intent_net_flags(const input_intent_t *intent);

/* Sample the current keyboard state into an input_intent_t.
 * This is the main input handler (~200 lines) covering flight,
 * docked menus, build overlay, contracts, and outpost placement. */
input_intent_t sample_input_intent(void);

/* Submit an input intent to the authoritative server (local or remote).
 * Handles local server dispatch, multiplayer network encoding, and
 * action prediction timing. */
void submit_input(const input_intent_t *intent, float dt);

/* Set the HUD notice message (printf-style). */
void set_notice(const char* fmt, ...);

#endif /* INPUT_H */
