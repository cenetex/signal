#include "tow_presentation_diagnostics.h"

#include <math.h>
#include <string.h>

static uint16_t abs_i16(int16_t value)
{
    return (uint16_t)(value < 0 ? -value : value);
}

static int16_t scheduled_jitter(
    const tow_adverse_profile_t *profile, uint32_t sequence)
{
    static const int8_t pattern[8] = {-4, 2, 4, -1, 3, -3, 1, 0};
    int value = pattern[(sequence - 1u) % 8u];
    return (int16_t)(
        value * (int)profile->max_jitter_ms / 4);
}

static int16_t scheduled_reorder(
    const tow_adverse_profile_t *profile, uint32_t sequence)
{
    if (profile->reorder_modulus == 0) return 0;
    uint32_t phase = sequence % profile->reorder_modulus;
    if (phase == 1u)
        return (int16_t)profile->max_reorder_ms;
    if (phase == 2u)
        return -(int16_t)(profile->max_reorder_ms / 2u);
    return 0;
}

tow_adverse_profile_t tow_adverse_profile(uint16_t base_latency_ms)
{
    tow_adverse_profile_t profile = {
        .base_latency_ms = base_latency_ms,
        .drop_modulus = 7,
        .duplicate_modulus = 5,
        .reorder_modulus = 4,
    };
    switch (base_latency_ms) {
    case 50:
        profile.max_jitter_ms = 12;
        profile.max_reorder_ms = 24;
        break;
    case 125:
        profile.max_jitter_ms = 20;
        profile.max_reorder_ms = 32;
        break;
    case 250:
        profile.max_jitter_ms = 28;
        profile.max_reorder_ms = 40;
        break;
    default:
        profile.max_jitter_ms = 0;
        profile.max_reorder_ms = 0;
        profile.drop_modulus = 0;
        profile.duplicate_modulus = 0;
        profile.reorder_modulus = 0;
        break;
    }
    return profile;
}

static bool delivery_before(const tow_adverse_delivery_t *a,
                            const tow_adverse_delivery_t *b)
{
    if (a->delivery_ms != b->delivery_ms)
        return a->delivery_ms < b->delivery_ms;
    if (a->packet.sequence != b->packet.sequence)
        return a->packet.sequence < b->packet.sequence;
    return a->duplicate_ordinal < b->duplicate_ordinal;
}

static bool insert_delivery(
    tow_adverse_delivery_t *out,
    int *count,
    int cap,
    tow_adverse_delivery_t delivery)
{
    if (!out || !count || *count < 0 || *count >= cap) return false;
    int insert = *count;
    while (insert > 0 &&
           delivery_before(&delivery, &out[insert - 1])) {
        out[insert] = out[insert - 1];
        insert--;
    }
    out[insert] = delivery;
    (*count)++;
    return true;
}

int tow_adverse_schedule(
    const tow_adverse_profile_t *profile,
    const tow_adverse_packet_t *packets,
    int packet_count,
    tow_adverse_delivery_t *out,
    int out_cap,
    tow_adverse_schedule_stats_t *stats)
{
    if (!profile || !packets || packet_count < 0 ||
        packet_count > TOW_ADVERSE_MAX_PACKETS ||
        !out || out_cap < 0 || out_cap > TOW_ADVERSE_MAX_DELIVERIES) {
        return -1;
    }
    tow_adverse_schedule_stats_t local = {0};
    local.input_packets = (uint32_t)packet_count;
    int count = 0;
    for (int i = 0; i < packet_count; i++) {
        const tow_adverse_packet_t *packet = &packets[i];
        if (packet->sequence == 0 ||
            (packet->channel != TOW_ADVERSE_CHANNEL_TARGET &&
             packet->channel != TOW_ADVERSE_CHANNEL_RELATION)) {
            return -1;
        }
        if (profile->drop_modulus > 0 &&
            packet->sequence % profile->drop_modulus == 0) {
            local.dropped_packets++;
            continue;
        }
        int16_t jitter = scheduled_jitter(profile, packet->sequence);
        int16_t reorder = scheduled_reorder(profile, packet->sequence);
        int64_t delivery_ms =
            (int64_t)packet->send_ms +
            (int64_t)profile->base_latency_ms +
            jitter + reorder;
        if (delivery_ms < 0) delivery_ms = 0;
        if (delivery_ms > UINT32_MAX) return -1;
        tow_adverse_delivery_t delivery = {
            .packet = *packet,
            .delivery_ms = (uint32_t)delivery_ms,
            .jitter_ms = jitter,
            .reorder_ms = reorder,
            .duplicate_ordinal = 0,
        };
        if (!insert_delivery(out, &count, out_cap, delivery))
            return -1;
        uint16_t abs_jitter = abs_i16(jitter);
        uint16_t abs_reorder = abs_i16(reorder);
        if (local.max_abs_jitter_ms < abs_jitter)
            local.max_abs_jitter_ms = abs_jitter;
        if (local.max_abs_reorder_ms < abs_reorder)
            local.max_abs_reorder_ms = abs_reorder;

        if (profile->duplicate_modulus > 0 &&
            packet->sequence % profile->duplicate_modulus == 0) {
            if (delivery.delivery_ms > UINT32_MAX - 3u)
                return -1;
            delivery.delivery_ms += 3u;
            delivery.duplicate_ordinal = 1;
            if (!insert_delivery(out, &count, out_cap, delivery))
                return -1;
            local.duplicated_packets++;
        }
    }

    uint32_t greatest_sequence = 0;
    uint32_t last_counted_sequence = 0;
    for (int i = 0; i < count; i++) {
        uint32_t sequence = out[i].packet.sequence;
        if (out[i].duplicate_ordinal != 0 ||
            sequence == last_counted_sequence) {
            continue;
        }
        if (sequence < greatest_sequence)
            local.reordered_packets++;
        if (sequence > greatest_sequence)
            greatest_sequence = sequence;
        last_counted_sequence = sequence;
    }
    local.delivered_packets = (uint32_t)count;
    if (stats) *stats = local;
    return count;
}

static float vec_length(vec2 value)
{
    return sqrtf(value.x * value.x + value.y * value.y);
}

static bool finite_vec(vec2 value)
{
    return isfinite(value.x) && isfinite(value.y);
}

void tow_presentation_diagnostics_reset(
    tow_presentation_diagnostics_t *diagnostics)
{
    if (diagnostics) memset(diagnostics, 0, sizeof(*diagnostics));
}

void tow_presentation_diagnostics_snapshot(
    tow_presentation_diagnostics_t *diagnostics,
    vec2 presented_pos,
    vec2 presented_vel,
    vec2 authoritative_pos,
    vec2 authoritative_vel)
{
    if (!diagnostics ||
        !finite_vec(presented_pos) || !finite_vec(presented_vel) ||
        !finite_vec(authoritative_pos) ||
        !finite_vec(authoritative_vel)) {
        return;
    }
    float correction = vec_length(
        v2_sub(authoritative_pos, presented_pos));
    float velocity_discontinuity = vec_length(
        v2_sub(authoritative_vel, presented_vel));
    if (diagnostics->max_correction_world < correction)
        diagnostics->max_correction_world = correction;
    if (diagnostics->max_velocity_discontinuity <
        velocity_discontinuity) {
        diagnostics->max_velocity_discontinuity =
            velocity_discontinuity;
    }
    if (diagnostics->have_snapshot &&
        diagnostics->max_snapshot_gap_sec <
            diagnostics->seconds_since_snapshot) {
        diagnostics->max_snapshot_gap_sec =
            diagnostics->seconds_since_snapshot;
    }
    diagnostics->snapshot_samples++;
    diagnostics->have_snapshot = true;
    diagnostics->seconds_since_snapshot = 0.0f;
    diagnostics->starvation_active = false;
}

void tow_presentation_diagnostics_frame(
    tow_presentation_diagnostics_t *diagnostics,
    bool visible,
    vec2 presented_pos,
    float dt,
    float pixels_per_world)
{
    if (!diagnostics || !isfinite(dt) || dt <= 0.0f) return;
    if (!visible || !finite_vec(presented_pos)) {
        diagnostics->have_snapshot = false;
        diagnostics->seconds_since_snapshot = 0.0f;
        diagnostics->have_position = false;
        diagnostics->have_frame_velocity = false;
        diagnostics->have_acceleration = false;
        diagnostics->starvation_active = false;
        return;
    }
    diagnostics->presentation_samples++;
    if (diagnostics->have_snapshot) {
        diagnostics->seconds_since_snapshot += dt;
        if (diagnostics->max_snapshot_gap_sec <
            diagnostics->seconds_since_snapshot) {
            diagnostics->max_snapshot_gap_sec =
                diagnostics->seconds_since_snapshot;
        }
        if (!diagnostics->starvation_active &&
            diagnostics->seconds_since_snapshot >
                TOW_PRESENTATION_STARVATION_BEGIN_SEC) {
            diagnostics->starvation_events++;
            diagnostics->starvation_active = true;
        }
    }

    if (!diagnostics->have_position) {
        diagnostics->last_position = presented_pos;
        diagnostics->have_position = true;
        return;
    }
    vec2 frame_velocity = v2_scale(
        v2_sub(presented_pos, diagnostics->last_position), 1.0f / dt);
    diagnostics->last_position = presented_pos;
    if (!diagnostics->have_frame_velocity) {
        diagnostics->last_frame_velocity = frame_velocity;
        diagnostics->have_frame_velocity = true;
        return;
    }
    vec2 acceleration = v2_scale(
        v2_sub(frame_velocity, diagnostics->last_frame_velocity),
        1.0f / dt);
    diagnostics->last_frame_velocity = frame_velocity;
    if (!diagnostics->have_acceleration) {
        diagnostics->last_acceleration = acceleration;
        diagnostics->have_acceleration = true;
        return;
    }
    vec2 jerk = v2_scale(
        v2_sub(acceleration, diagnostics->last_acceleration),
        1.0f / dt);
    diagnostics->last_acceleration = acceleration;
    float world_jerk = vec_length(jerk);
    float screen_scale =
        isfinite(pixels_per_world) && pixels_per_world > 0.0f
            ? pixels_per_world : 1.0f;
    float screen_jerk = world_jerk * screen_scale;
    if (diagnostics->max_world_jerk < world_jerk)
        diagnostics->max_world_jerk = world_jerk;
    if (diagnostics->max_screen_jerk < screen_jerk)
        diagnostics->max_screen_jerk = screen_jerk;
    diagnostics->jerk_samples++;
}

bool tow_presentation_diagnostics_within_thresholds(
    const tow_presentation_diagnostics_t *diagnostics)
{
    return diagnostics &&
        diagnostics->snapshot_samples > 0 &&
        diagnostics->presentation_samples > 0 &&
        diagnostics->jerk_samples > 0 &&
        isfinite(diagnostics->max_correction_world) &&
        isfinite(diagnostics->max_velocity_discontinuity) &&
        isfinite(diagnostics->max_snapshot_gap_sec) &&
        isfinite(diagnostics->max_world_jerk) &&
        isfinite(diagnostics->max_screen_jerk) &&
        diagnostics->max_correction_world <=
            TOW_PRESENTATION_MAX_CORRECTION_WORLD &&
        diagnostics->max_velocity_discontinuity <=
            TOW_PRESENTATION_MAX_VELOCITY_DISCONTINUITY &&
        diagnostics->max_snapshot_gap_sec <=
            TOW_PRESENTATION_MAX_SNAPSHOT_GAP_SEC &&
        diagnostics->max_world_jerk <=
            TOW_PRESENTATION_MAX_WORLD_JERK &&
        diagnostics->max_screen_jerk <=
            TOW_PRESENTATION_MAX_SCREEN_JERK;
}
