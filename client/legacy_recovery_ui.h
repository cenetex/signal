#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

/*
 * Player-facing recovery state deliberately contains no offer ID, token,
 * path, basename, reconnect secret, or other bearer material. The network
 * layer owns the connection-local opaque offer; this module owns only the
 * presentation and one-shot input lifecycle.
 */
typedef enum {
    LEGACY_RECOVERY_UI_IDLE = 0,
    LEGACY_RECOVERY_UI_OFFERED,
    LEGACY_RECOVERY_UI_CONFIRMING,
    LEGACY_RECOVERY_UI_RESULT,
} legacy_recovery_ui_phase_t;

typedef enum {
    LEGACY_RECOVERY_UI_SEMANTIC_NONE = 0,
    LEGACY_RECOVERY_UI_SEMANTIC_OFFER,
    LEGACY_RECOVERY_UI_SEMANTIC_RETRYABLE_SEND,
    LEGACY_RECOVERY_UI_SEMANTIC_CONFIRMING,
    LEGACY_RECOVERY_UI_SEMANTIC_CANCELLED,
    LEGACY_RECOVERY_UI_SEMANTIC_NO_MATCH,
    LEGACY_RECOVERY_UI_SEMANTIC_STALE_OFFER,
    LEGACY_RECOVERY_UI_SEMANTIC_REPLAY,
    LEGACY_RECOVERY_UI_SEMANTIC_INVALID_SOURCE,
    LEGACY_RECOVERY_UI_SEMANTIC_DESTINATION_CONFLICT,
    LEGACY_RECOVERY_UI_SEMANTIC_MIGRATION_FAILURE,
    LEGACY_RECOVERY_UI_SEMANTIC_SUCCESS,
} legacy_recovery_ui_semantic_t;

typedef enum {
    LEGACY_RECOVERY_UI_ACTION_NONE = 0,
    LEGACY_RECOVERY_UI_ACTION_CONFIRM,
    LEGACY_RECOVERY_UI_ACTION_CANCEL,
    LEGACY_RECOVERY_UI_ACTION_EXPIRE,
} legacy_recovery_ui_action_t;

typedef struct {
    bool confirm_down;
    bool cancel_down;
    bool confirm_pressed;
    bool cancel_pressed;
} legacy_recovery_ui_input_t;

typedef struct {
    legacy_recovery_ui_phase_t phase;
    legacy_recovery_ui_semantic_t semantic;
    uint32_t offer_expires_at_ms;
    uint32_t result_expires_at_ms;
    bool input_armed;
} legacy_recovery_ui_t;

void legacy_recovery_ui_reset(legacy_recovery_ui_t *ui);

/* Returns false for a zero lifetime or while any offer/result is active. */
bool legacy_recovery_ui_begin_offer(
    legacy_recovery_ui_t *ui,
    uint32_t now_ms,
    uint16_t expires_in_seconds);

/*
 * Samples only explicit recovery controls. Cancellation wins if confirm and
 * cancel arrive on the same frame. A newly displayed offer is not armed until
 * both controls have been observed released.
 */
legacy_recovery_ui_action_t legacy_recovery_ui_update(
    legacy_recovery_ui_t *ui,
    uint32_t now_ms,
    legacy_recovery_ui_input_t input);

/*
 * Complete the local transport attempt. A rejected write returns to the
 * offered state with retryable copy and requires another release/press cycle.
 * A successful write remains confirming until the server result.
 */
void legacy_recovery_ui_note_send(
    legacy_recovery_ui_t *ui,
    bool admitted);

/*
 * Applies only a bounded server semantic result to an active offer. Unsolicited
 * and duplicate results are ignored. Result copy stays visible briefly, then
 * the state machine returns to idle without requiring navigation input.
 */
bool legacy_recovery_ui_apply_result(
    legacy_recovery_ui_t *ui,
    legacy_recovery_result_status_t status,
    uint32_t now_ms);

bool legacy_recovery_ui_visible(const legacy_recovery_ui_t *ui);
bool legacy_recovery_ui_blocks_gameplay(const legacy_recovery_ui_t *ui);
bool legacy_recovery_ui_can_confirm(const legacy_recovery_ui_t *ui);
bool legacy_recovery_ui_can_cancel(const legacy_recovery_ui_t *ui);
uint32_t legacy_recovery_ui_seconds_remaining(
    const legacy_recovery_ui_t *ui,
    uint32_t now_ms);

const char *legacy_recovery_ui_semantic_name(
    legacy_recovery_ui_semantic_t semantic);
const char *legacy_recovery_ui_title(const legacy_recovery_ui_t *ui);
const char *legacy_recovery_ui_status(const legacy_recovery_ui_t *ui);
const char *legacy_recovery_ui_body(const legacy_recovery_ui_t *ui);
const char *legacy_recovery_ui_detail(const legacy_recovery_ui_t *ui);

/* Sokol renderer implemented separately so the state machine stays pure. */
void draw_legacy_recovery_ui(
    const legacy_recovery_ui_t *ui,
    uint32_t now_ms);
