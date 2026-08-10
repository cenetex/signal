#include "reconciliation_diagnostics.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t float_bits(float value)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float float_from_bits(uint32_t bits)
{
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool tick_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

net_reconcile_pose_bits_t net_reconcile_pose_bits(float pos_x,
                                                  float pos_y,
                                                  float vel_x,
                                                  float vel_y,
                                                  float angle)
{
    return (net_reconcile_pose_bits_t){
        .pos_x = float_bits(pos_x),
        .pos_y = float_bits(pos_y),
        .vel_x = float_bits(vel_x),
        .vel_y = float_bits(vel_y),
        .angle = float_bits(angle),
    };
}

bool net_reconcile_pose_equal(const net_reconcile_pose_bits_t *a,
                              const net_reconcile_pose_bits_t *b)
{
    return a && b &&
        a->pos_x == b->pos_x &&
        a->pos_y == b->pos_y &&
        a->vel_x == b->vel_x &&
        a->vel_y == b->vel_y &&
        a->angle == b->angle;
}

bool net_reconcile_movement_intent_equal(const input_intent_t *a,
                                         const input_intent_t *b)
{
    if (!a || !b) return false;
    return float_bits(a->turn) == float_bits(b->turn) &&
        float_bits(a->thrust) == float_bits(b->thrust) &&
        a->mine == b->mine &&
        (!(a->mine || b->mine) ||
         a->mining_target_hint == b->mining_target_hint) &&
        a->tractor_hold == b->tractor_hold &&
        a->boost == b->boost &&
        a->reverse_thrust == b->reverse_thrust;
}

net_reconcile_tick_window_t net_reconcile_tick_window(
    uint32_t first_tick, uint32_t last_tick, uint32_t cause_mask)
{
    net_reconcile_tick_window_t window = {0};
    if (first_tick == 0 || last_tick == 0 ||
        cause_mask == NET_RECONCILE_CAUSE_NONE ||
        tick_after(first_tick, last_tick)) {
        return window;
    }
    window.valid = true;
    window.first_tick = first_tick;
    window.last_tick = last_tick;
    window.cause_mask = cause_mask;
    return window;
}

bool net_reconcile_tick_window_contains(
    const net_reconcile_tick_window_t *window, uint32_t tick)
{
    if (!window || !window->valid || tick == 0) return false;
    return !tick_after(window->first_tick, tick) &&
        !tick_after(tick, window->last_tick);
}

net_reconcile_class_t net_reconcile_classify(
    const net_reconcile_sample_t *sample)
{
    if (!sample ||
        net_reconcile_pose_equal(&sample->predicted,
                                 &sample->authoritative)) {
        return NET_RECONCILE_EXACT;
    }
    if (sample->bootstrap) return NET_RECONCILE_BOOTSTRAP;
    if (net_reconcile_tick_window_contains(
            &sample->semantic, sample->server_tick)) {
        return NET_RECONCILE_SEMANTIC_DISCONTINUITY;
    }
    if (net_reconcile_tick_window_contains(
            &sample->transport_recovery, sample->server_tick)) {
        return NET_RECONCILE_TRANSPORT_RECOVERY;
    }
    if (!sample->frontier_known ||
        sample->server_tick == 0 ||
        sample->prediction_tick == 0 ||
        sample->server_tick != sample->prediction_tick ||
        sample->predicted_input_seq != sample->authoritative_input_seq ||
        net_reconcile_tick_window_contains(
            &sample->input_frontier, sample->server_tick)) {
        return NET_RECONCILE_INPUT_FRONTIER;
    }
    return NET_RECONCILE_NUMERIC_DRIFT;
}

void net_reconcile_diagnostics_reset(net_reconcile_diagnostics_t *diagnostics)
{
    if (diagnostics) memset(diagnostics, 0, sizeof(*diagnostics));
}

static net_reconcile_domain_t first_divergent_domain(
    const net_reconcile_pose_bits_t *predicted,
    const net_reconcile_pose_bits_t *authoritative,
    uint32_t *predicted_bits,
    uint32_t *authoritative_bits)
{
#define CHECK_DOMAIN(field, domain_value) \
    do { \
        if (predicted->field != authoritative->field) { \
            *predicted_bits = predicted->field; \
            *authoritative_bits = authoritative->field; \
            return domain_value; \
        } \
    } while (0)

    CHECK_DOMAIN(pos_x, NET_RECONCILE_DOMAIN_PLAYER_POS_X);
    CHECK_DOMAIN(pos_y, NET_RECONCILE_DOMAIN_PLAYER_POS_Y);
    CHECK_DOMAIN(vel_x, NET_RECONCILE_DOMAIN_PLAYER_VEL_X);
    CHECK_DOMAIN(vel_y, NET_RECONCILE_DOMAIN_PLAYER_VEL_Y);
    CHECK_DOMAIN(angle, NET_RECONCILE_DOMAIN_PLAYER_ANGLE);

#undef CHECK_DOMAIN
    *predicted_bits = 0;
    *authoritative_bits = 0;
    return NET_RECONCILE_DOMAIN_NONE;
}

net_reconcile_class_t net_reconcile_diagnostics_observe(
    net_reconcile_diagnostics_t *diagnostics,
    const net_reconcile_sample_t *sample)
{
    net_reconcile_class_t classification = net_reconcile_classify(sample);
    if (!diagnostics || !sample) return classification;

    diagnostics->total_samples++;
    diagnostics->class_count[classification]++;
    if (classification != NET_RECONCILE_EXACT)
        diagnostics->total_corrections++;

    if (classification != NET_RECONCILE_NUMERIC_DRIFT ||
        diagnostics->first_numeric_drift_valid) {
        return classification;
    }

    diagnostics->first_numeric_drift_valid = true;
    diagnostics->first_entity_id = sample->entity_id;
    diagnostics->first_server_tick = sample->server_tick;
    diagnostics->first_prediction_tick = sample->prediction_tick;
    diagnostics->first_predicted_input_seq = sample->predicted_input_seq;
    diagnostics->first_authoritative_input_seq =
        sample->authoritative_input_seq;
    diagnostics->first_predicted_pose = sample->predicted;
    diagnostics->first_authoritative_pose = sample->authoritative;
    diagnostics->first_input_cause_mask =
        net_reconcile_tick_window_contains(
            &sample->input_frontier, sample->server_tick)
            ? sample->input_frontier.cause_mask : 0u;
    diagnostics->first_semantic_cause_mask =
        net_reconcile_tick_window_contains(
            &sample->semantic, sample->server_tick)
            ? sample->semantic.cause_mask : 0u;
    diagnostics->first_transport_cause_mask =
        net_reconcile_tick_window_contains(
            &sample->transport_recovery, sample->server_tick)
            ? sample->transport_recovery.cause_mask : 0u;
    diagnostics->first_domain = first_divergent_domain(
        &sample->predicted,
        &sample->authoritative,
        &diagnostics->first_predicted_bits,
        &diagnostics->first_authoritative_bits);
    if (sample->authoritative_root) {
        diagnostics->first_authoritative_root_valid = true;
        memcpy(diagnostics->first_authoritative_root,
               sample->authoritative_root,
               sizeof(diagnostics->first_authoritative_root));
    }
    return classification;
}

void net_reconcile_note_semantic_window(
    net_reconcile_diagnostics_t *diagnostics,
    uint32_t first_tick,
    uint32_t last_tick,
    uint32_t cause_mask)
{
    if (!diagnostics) return;
    diagnostics->pending_semantic = net_reconcile_tick_window(
        first_tick, last_tick, cause_mask);
}

net_reconcile_tick_window_t net_reconcile_take_semantic_window(
    net_reconcile_diagnostics_t *diagnostics, uint32_t sample_tick)
{
    net_reconcile_tick_window_t none = {0};
    if (!diagnostics || !diagnostics->pending_semantic.valid)
        return none;

    net_reconcile_tick_window_t pending = diagnostics->pending_semantic;
    if (net_reconcile_tick_window_contains(&pending, sample_tick)) {
        memset(&diagnostics->pending_semantic, 0,
               sizeof(diagnostics->pending_semantic));
        return pending;
    }
    if (tick_after(sample_tick, pending.last_tick)) {
        memset(&diagnostics->pending_semantic, 0,
               sizeof(diagnostics->pending_semantic));
    }
    return none;
}

const char *net_reconcile_class_name(net_reconcile_class_t classification)
{
    switch (classification) {
    case NET_RECONCILE_EXACT: return "exact";
    case NET_RECONCILE_BOOTSTRAP: return "bootstrap";
    case NET_RECONCILE_INPUT_FRONTIER: return "input-frontier";
    case NET_RECONCILE_SEMANTIC_DISCONTINUITY:
        return "semantic-discontinuity";
    case NET_RECONCILE_TRANSPORT_RECOVERY: return "transport-recovery";
    case NET_RECONCILE_NUMERIC_DRIFT: return "numeric-drift";
    case NET_RECONCILE_CLASS_COUNT: break;
    }
    return "unknown";
}

const char *net_reconcile_domain_name(net_reconcile_domain_t domain)
{
    switch (domain) {
    case NET_RECONCILE_DOMAIN_NONE: return "none";
    case NET_RECONCILE_DOMAIN_PLAYER_POS_X: return "player.ship.pos.x";
    case NET_RECONCILE_DOMAIN_PLAYER_POS_Y: return "player.ship.pos.y";
    case NET_RECONCILE_DOMAIN_PLAYER_VEL_X: return "player.ship.vel.x";
    case NET_RECONCILE_DOMAIN_PLAYER_VEL_Y: return "player.ship.vel.y";
    case NET_RECONCILE_DOMAIN_PLAYER_ANGLE: return "player.ship.angle";
    }
    return "unknown";
}

static void root_hex(const net_reconcile_diagnostics_t *diagnostics,
                     char out[NET_RECONCILE_ROOT_SIZE * 2u + 1u])
{
    static const char hex[] = "0123456789abcdef";
    if (!diagnostics || !diagnostics->first_authoritative_root_valid) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < NET_RECONCILE_ROOT_SIZE; i++) {
        uint8_t byte = diagnostics->first_authoritative_root[i];
        out[i * 2u] = hex[byte >> 4];
        out[i * 2u + 1u] = hex[byte & 0x0fu];
    }
    out[NET_RECONCILE_ROOT_SIZE * 2u] = '\0';
}

static void json_float_from_bits(uint32_t bits, char out[32])
{
    float value = float_from_bits(bits);
    if (!isfinite(value)) {
        memcpy(out, "null", sizeof("null"));
        return;
    }
    (void)snprintf(out, 32, "%.9g", (double)value);
}

int net_reconcile_first_drift_json(
    const net_reconcile_diagnostics_t *diagnostics,
    const char *root_schema,
    char *out,
    size_t out_size)
{
    if (!out || out_size == 0) return -1;
    if (!diagnostics || !diagnostics->first_numeric_drift_valid) {
        int written = snprintf(out, out_size, "{}");
        return written >= 0 && (size_t)written < out_size ? written : -1;
    }

    char digest[NET_RECONCILE_ROOT_SIZE * 2u + 1u];
    char predicted_value[32];
    char authoritative_value[32];
    root_hex(diagnostics, digest);
    json_float_from_bits(
        diagnostics->first_predicted_bits, predicted_value);
    json_float_from_bits(
        diagnostics->first_authoritative_bits, authoritative_value);
    int written = snprintf(
        out, out_size,
        "{\"class\":\"numeric-drift\",\"server_tick\":%" PRIu32 ","
        "\"prediction_tick\":%" PRIu32 ",\"predicted_input_seq\":%u,"
        "\"authoritative_input_seq\":%u,\"entity\":\"player:%u\","
        "\"domain\":\"%s\",\"predicted\":%s,\"authoritative\":%s,"
        "\"predicted_pose_bits\":{\"x\":\"0x%08" PRIx32
        "\",\"y\":\"0x%08" PRIx32 "\",\"vx\":\"0x%08" PRIx32
        "\",\"vy\":\"0x%08" PRIx32 "\",\"angle\":\"0x%08" PRIx32
        "\"},\"authoritative_pose_bits\":{\"x\":\"0x%08" PRIx32
        "\",\"y\":\"0x%08" PRIx32 "\",\"vx\":\"0x%08" PRIx32
        "\",\"vy\":\"0x%08" PRIx32 "\",\"angle\":\"0x%08" PRIx32
        "\"},\"predicted_bits\":\"0x%08" PRIx32
        "\",\"authoritative_bits\":\"0x%08" PRIx32
        "\",\"input_cause_mask\":%" PRIu32
        ",\"semantic_cause_mask\":%" PRIu32
        ",\"transport_cause_mask\":%" PRIu32
        ",\"root_schema\":\"%s\",\"authoritative_root\":\"%s\"}",
        diagnostics->first_server_tick,
        diagnostics->first_prediction_tick,
        (unsigned)diagnostics->first_predicted_input_seq,
        (unsigned)diagnostics->first_authoritative_input_seq,
        (unsigned)diagnostics->first_entity_id,
        net_reconcile_domain_name(diagnostics->first_domain),
        predicted_value,
        authoritative_value,
        diagnostics->first_predicted_pose.pos_x,
        diagnostics->first_predicted_pose.pos_y,
        diagnostics->first_predicted_pose.vel_x,
        diagnostics->first_predicted_pose.vel_y,
        diagnostics->first_predicted_pose.angle,
        diagnostics->first_authoritative_pose.pos_x,
        diagnostics->first_authoritative_pose.pos_y,
        diagnostics->first_authoritative_pose.vel_x,
        diagnostics->first_authoritative_pose.vel_y,
        diagnostics->first_authoritative_pose.angle,
        diagnostics->first_predicted_bits,
        diagnostics->first_authoritative_bits,
        diagnostics->first_input_cause_mask,
        diagnostics->first_semantic_cause_mask,
        diagnostics->first_transport_cause_mask,
        root_schema ? root_schema : "",
        digest);
    return written >= 0 && (size_t)written < out_size ? written : -1;
}
