#include "legacy_recovery_ui.h"

#include <string.h>

enum {
    LEGACY_RECOVERY_UI_RESULT_LIFETIME_MS = 4000,
};

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static legacy_recovery_ui_semantic_t semantic_from_result(
    legacy_recovery_result_status_t status)
{
    switch (status) {
    case LEGACY_RECOVERY_RESULT_NO_MATCH:
        return LEGACY_RECOVERY_UI_SEMANTIC_NO_MATCH;
    case LEGACY_RECOVERY_RESULT_STALE_OFFER:
        return LEGACY_RECOVERY_UI_SEMANTIC_STALE_OFFER;
    case LEGACY_RECOVERY_RESULT_REPLAY:
        return LEGACY_RECOVERY_UI_SEMANTIC_REPLAY;
    case LEGACY_RECOVERY_RESULT_INVALID_SOURCE:
        return LEGACY_RECOVERY_UI_SEMANTIC_INVALID_SOURCE;
    case LEGACY_RECOVERY_RESULT_DESTINATION_CONFLICT:
        return LEGACY_RECOVERY_UI_SEMANTIC_DESTINATION_CONFLICT;
    case LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE:
        return LEGACY_RECOVERY_UI_SEMANTIC_MIGRATION_FAILURE;
    case LEGACY_RECOVERY_RESULT_SUCCESS:
        return LEGACY_RECOVERY_UI_SEMANTIC_SUCCESS;
    default:
        return LEGACY_RECOVERY_UI_SEMANTIC_NONE;
    }
}

static void enter_result(
    legacy_recovery_ui_t *ui,
    legacy_recovery_ui_semantic_t semantic,
    uint32_t now_ms)
{
    ui->phase = LEGACY_RECOVERY_UI_RESULT;
    ui->semantic = semantic;
    ui->offer_expires_at_ms = 0;
    ui->result_expires_at_ms =
        now_ms + LEGACY_RECOVERY_UI_RESULT_LIFETIME_MS;
    ui->input_armed = false;
}

void legacy_recovery_ui_reset(legacy_recovery_ui_t *ui) {
    if (!ui) return;
    memset(ui, 0, sizeof(*ui));
}

bool legacy_recovery_ui_begin_offer(
    legacy_recovery_ui_t *ui,
    uint32_t now_ms,
    uint16_t expires_in_seconds)
{
    if (!ui ||
        ui->phase != LEGACY_RECOVERY_UI_IDLE ||
        expires_in_seconds == 0) {
        return false;
    }
    ui->phase = LEGACY_RECOVERY_UI_OFFERED;
    ui->semantic = LEGACY_RECOVERY_UI_SEMANTIC_OFFER;
    ui->offer_expires_at_ms =
        now_ms + (uint32_t)expires_in_seconds * 1000u;
    ui->result_expires_at_ms = 0;
    ui->input_armed = false;
    return true;
}

legacy_recovery_ui_action_t legacy_recovery_ui_update(
    legacy_recovery_ui_t *ui,
    uint32_t now_ms,
    legacy_recovery_ui_input_t input)
{
    if (!ui) return LEGACY_RECOVERY_UI_ACTION_NONE;

    if (ui->phase == LEGACY_RECOVERY_UI_RESULT) {
        if (deadline_reached(now_ms, ui->result_expires_at_ms))
            legacy_recovery_ui_reset(ui);
        return LEGACY_RECOVERY_UI_ACTION_NONE;
    }
    if (ui->phase != LEGACY_RECOVERY_UI_OFFERED)
        return LEGACY_RECOVERY_UI_ACTION_NONE;

    if (deadline_reached(now_ms, ui->offer_expires_at_ms)) {
        enter_result(
            ui, LEGACY_RECOVERY_UI_SEMANTIC_STALE_OFFER, now_ms);
        return LEGACY_RECOVERY_UI_ACTION_EXPIRE;
    }

    if (!ui->input_armed) {
        if (!input.confirm_down && !input.cancel_down)
            ui->input_armed = true;
        return LEGACY_RECOVERY_UI_ACTION_NONE;
    }

    /* Prefer the non-mutating choice if two controls land together. */
    if (input.cancel_pressed) {
        enter_result(
            ui, LEGACY_RECOVERY_UI_SEMANTIC_CANCELLED, now_ms);
        return LEGACY_RECOVERY_UI_ACTION_CANCEL;
    }
    if (input.confirm_pressed) {
        ui->phase = LEGACY_RECOVERY_UI_CONFIRMING;
        ui->semantic = LEGACY_RECOVERY_UI_SEMANTIC_CONFIRMING;
        ui->input_armed = false;
        return LEGACY_RECOVERY_UI_ACTION_CONFIRM;
    }
    return LEGACY_RECOVERY_UI_ACTION_NONE;
}

void legacy_recovery_ui_note_send(
    legacy_recovery_ui_t *ui,
    bool admitted)
{
    if (!ui || ui->phase != LEGACY_RECOVERY_UI_CONFIRMING)
        return;
    if (admitted)
        return;
    ui->phase = LEGACY_RECOVERY_UI_OFFERED;
    ui->semantic = LEGACY_RECOVERY_UI_SEMANTIC_RETRYABLE_SEND;
    ui->input_armed = false;
}

bool legacy_recovery_ui_apply_result(
    legacy_recovery_ui_t *ui,
    legacy_recovery_result_status_t status,
    uint32_t now_ms)
{
    if (!ui ||
        (ui->phase != LEGACY_RECOVERY_UI_OFFERED &&
         ui->phase != LEGACY_RECOVERY_UI_CONFIRMING)) {
        return false;
    }
    legacy_recovery_ui_semantic_t semantic =
        semantic_from_result(status);
    if (semantic == LEGACY_RECOVERY_UI_SEMANTIC_NONE)
        return false;
    enter_result(ui, semantic, now_ms);
    return true;
}

bool legacy_recovery_ui_visible(const legacy_recovery_ui_t *ui) {
    return ui && ui->phase != LEGACY_RECOVERY_UI_IDLE;
}

bool legacy_recovery_ui_blocks_gameplay(const legacy_recovery_ui_t *ui) {
    return legacy_recovery_ui_visible(ui);
}

bool legacy_recovery_ui_can_confirm(const legacy_recovery_ui_t *ui) {
    return ui &&
        ui->phase == LEGACY_RECOVERY_UI_OFFERED &&
        ui->input_armed;
}

bool legacy_recovery_ui_can_cancel(const legacy_recovery_ui_t *ui) {
    return legacy_recovery_ui_can_confirm(ui);
}

uint32_t legacy_recovery_ui_seconds_remaining(
    const legacy_recovery_ui_t *ui,
    uint32_t now_ms)
{
    if (!ui || ui->phase != LEGACY_RECOVERY_UI_OFFERED ||
        deadline_reached(now_ms, ui->offer_expires_at_ms)) {
        return 0;
    }
    uint32_t remaining_ms = ui->offer_expires_at_ms - now_ms;
    return (remaining_ms + 999u) / 1000u;
}

const char *legacy_recovery_ui_semantic_name(
    legacy_recovery_ui_semantic_t semantic)
{
    switch (semantic) {
    case LEGACY_RECOVERY_UI_SEMANTIC_NONE:
        return "none";
    case LEGACY_RECOVERY_UI_SEMANTIC_OFFER:
        return "offer";
    case LEGACY_RECOVERY_UI_SEMANTIC_RETRYABLE_SEND:
        return "retryable-send";
    case LEGACY_RECOVERY_UI_SEMANTIC_CONFIRMING:
        return "confirming";
    case LEGACY_RECOVERY_UI_SEMANTIC_CANCELLED:
        return "cancelled";
    case LEGACY_RECOVERY_UI_SEMANTIC_NO_MATCH:
        return "no-match";
    case LEGACY_RECOVERY_UI_SEMANTIC_STALE_OFFER:
        return "stale-offer";
    case LEGACY_RECOVERY_UI_SEMANTIC_REPLAY:
        return "replay";
    case LEGACY_RECOVERY_UI_SEMANTIC_INVALID_SOURCE:
        return "invalid-source";
    case LEGACY_RECOVERY_UI_SEMANTIC_DESTINATION_CONFLICT:
        return "destination-conflict";
    case LEGACY_RECOVERY_UI_SEMANTIC_MIGRATION_FAILURE:
        return "migration-failure";
    case LEGACY_RECOVERY_UI_SEMANTIC_SUCCESS:
        return "success";
    default:
        return "unknown";
    }
}

const char *legacy_recovery_ui_title(const legacy_recovery_ui_t *ui) {
    (void)ui;
    return "IDENTITY RECOVERY // SECURE DOCK";
}

const char *legacy_recovery_ui_status(const legacy_recovery_ui_t *ui) {
    if (!ui) return "";
    switch (ui->semantic) {
    case LEGACY_RECOVERY_UI_SEMANTIC_OFFER:
        return "OPAQUE CANDIDATE AUTHORIZED";
    case LEGACY_RECOVERY_UI_SEMANTIC_RETRYABLE_SEND:
        return "CONFIRMATION NOT SENT";
    case LEGACY_RECOVERY_UI_SEMANTIC_CONFIRMING:
        return "VERIFYING ATOMIC IMPORT";
    case LEGACY_RECOVERY_UI_SEMANTIC_CANCELLED:
        return "RECOVERY CANCELLED";
    case LEGACY_RECOVERY_UI_SEMANTIC_NO_MATCH:
        return "NO MATCH REMAINED";
    case LEGACY_RECOVERY_UI_SEMANTIC_STALE_OFFER:
        return "OFFER EXPIRED";
    case LEGACY_RECOVERY_UI_SEMANTIC_REPLAY:
        return "CONFIRMATION ALREADY USED";
    case LEGACY_RECOVERY_UI_SEMANTIC_INVALID_SOURCE:
        return "SOURCE REJECTED";
    case LEGACY_RECOVERY_UI_SEMANTIC_DESTINATION_CONFLICT:
        return "IDENTITY SAVE KEPT";
    case LEGACY_RECOVERY_UI_SEMANTIC_MIGRATION_FAILURE:
        return "IMPORT NOT COMMITTED";
    case LEGACY_RECOVERY_UI_SEMANTIC_SUCCESS:
        return "RECOVERY COMPLETE";
    default:
        return "";
    }
}

const char *legacy_recovery_ui_body(const legacy_recovery_ui_t *ui) {
    if (!ui) return "";
    switch (ui->semantic) {
    case LEGACY_RECOVERY_UI_SEMANTIC_OFFER:
        return "One legacy save is authorized for this verified identity. "
               "Recover it into the current identity?";
    case LEGACY_RECOVERY_UI_SEMANTIC_RETRYABLE_SEND:
        return "The confirmation was not admitted to the transport. "
               "Nothing changed; retry while this offer is active.";
    case LEGACY_RECOVERY_UI_SEMANTIC_CONFIRMING:
        return "Recovery confirmation was sent once. Waiting for the "
               "authoritative atomic result.";
    case LEGACY_RECOVERY_UI_SEMANTIC_CANCELLED:
        return "Recovery was cancelled locally. The legacy save was left "
               "untouched.";
    case LEGACY_RECOVERY_UI_SEMANTIC_NO_MATCH:
        return "No matching legacy save remained. Nothing was changed.";
    case LEGACY_RECOVERY_UI_SEMANTIC_STALE_OFFER:
        return "This offer expired or belongs to an older connection. "
               "Reconnect to retry.";
    case LEGACY_RECOVERY_UI_SEMANTIC_REPLAY:
        return "This one-time confirmation was already used. Nothing was "
               "imported twice.";
    case LEGACY_RECOVERY_UI_SEMANTIC_INVALID_SOURCE:
        return "The authorized legacy save is corrupt or unsupported. "
               "Nothing was changed.";
    case LEGACY_RECOVERY_UI_SEMANTIC_DESTINATION_CONFLICT:
        return "A save already exists for this identity. It was kept and "
               "was not overwritten.";
    case LEGACY_RECOVERY_UI_SEMANTIC_MIGRATION_FAILURE:
        return "The atomic import could not be completed. Nothing was "
               "changed.";
    case LEGACY_RECOVERY_UI_SEMANTIC_SUCCESS:
        return "Legacy save recovered. Authoritative ship, economy, and "
               "ownership state refreshed.";
    default:
        return "";
    }
}

const char *legacy_recovery_ui_detail(const legacy_recovery_ui_t *ui) {
    if (!ui) return "";
    switch (ui->phase) {
    case LEGACY_RECOVERY_UI_OFFERED:
        return "No filename, account token, reconnect secret, or other "
               "player save is shown. Import is atomic and one-time.";
    case LEGACY_RECOVERY_UI_CONFIRMING:
        return "Gameplay remains locked until the server publishes the "
               "recovered state or rejects the transaction.";
    case LEGACY_RECOVERY_UI_RESULT:
        return ui->semantic == LEGACY_RECOVERY_UI_SEMANTIC_SUCCESS
            ? "The offer is cleared; this console will close automatically."
            : "The offer is cleared and cannot be reused.";
    default:
        return "";
    }
}
