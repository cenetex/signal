/*
 * net_latency.h -- Small client-side latency sample accumulator.
 *
 * Kept independent of the full client.h/Sokol stack so unit tests can exercise
 * the multiplayer telemetry math without pulling in renderer headers.
 */
#ifndef NET_LATENCY_H
#define NET_LATENCY_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define NET_LATENCY_SAMPLE_CAP 32
#define NET_LATENCY_STALE_SEC 3.0f
#define NET_LATENCY_STABLE_MIN_SAMPLES 3u

typedef struct {
    float samples[NET_LATENCY_SAMPLE_CAP];
    float sample_at[NET_LATENCY_SAMPLE_CAP];
    uint8_t next;
    uint8_t count;
    float ema;
    float last;
    float max_run;
    float last_sample_at;
} net_latency_stats_t;

static inline void net_latency_stats_reset(net_latency_stats_t *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
}

static inline void net_latency_stats_observe(net_latency_stats_t *stats,
                                             float value_sec,
                                             float now_sec) {
    if (!stats || !isfinite(value_sec) || value_sec <= 0.0f) return;
    uint8_t slot = stats->next;
    stats->samples[slot] = value_sec;
    stats->sample_at[slot] = now_sec;
    stats->next = (uint8_t)((stats->next + 1u) % NET_LATENCY_SAMPLE_CAP);
    if (stats->count < NET_LATENCY_SAMPLE_CAP) stats->count++;
    stats->last = value_sec;
    stats->last_sample_at = now_sec;
    if (stats->ema <= 0.0f)
        stats->ema = value_sec;
    else
        stats->ema += (value_sec - stats->ema) * 0.25f;
    if (value_sec > stats->max_run) stats->max_run = value_sec;
}

static inline float net_latency_stats_window_max_sec(
    const net_latency_stats_t *stats,
    float now_sec,
    float window_sec) {
    if (!stats || stats->count == 0 || window_sec <= 0.0f) return 0.0f;
    float max_value = 0.0f;
    for (uint8_t i = 0; i < stats->count; i++) {
        float age = now_sec - stats->sample_at[i];
        if (age < 0.0f || age > window_sec) continue;
        if (stats->samples[i] > max_value)
            max_value = stats->samples[i];
    }
    return max_value;
}

static inline bool net_latency_stats_fresh(const net_latency_stats_t *stats,
                                           float now_sec,
                                           float stale_sec) {
    if (!stats || stats->last <= 0.0f || stats->last_sample_at <= 0.0f)
        return false;
    return now_sec - stats->last_sample_at <= stale_sec;
}

static inline float net_latency_stats_smoothed_sec(
    const net_latency_stats_t *stats) {
    if (!stats) return 0.0f;
    return stats->ema > 0.0f ? stats->ema : stats->last;
}

static inline float net_latency_transport_rtt_sec(float raw_rtt_sec,
                                                  float server_turnaround_sec) {
    if (!isfinite(raw_rtt_sec) || raw_rtt_sec <= 0.0f) return 0.0f;
    if (!isfinite(server_turnaround_sec) || server_turnaround_sec <= 0.0f)
        return raw_rtt_sec;
    if (server_turnaround_sec >= raw_rtt_sec)
        return raw_rtt_sec;
    return raw_rtt_sec - server_turnaround_sec;
}

static inline float net_latency_control_rtt_sec(
    const net_latency_stats_t *ping,
    const net_latency_stats_t *ack,
    float now_sec,
    float stale_sec,
    float last_ping_sec,
    float last_ack_sec) {
    if (net_latency_stats_fresh(ping, now_sec, stale_sec)) {
        float rtt = net_latency_stats_smoothed_sec(ping);
        if (rtt > 0.0f && isfinite(rtt)) return rtt;
    }
    if (net_latency_stats_fresh(ack, now_sec, stale_sec)) {
        float rtt = net_latency_stats_smoothed_sec(ack);
        if (rtt > 0.0f && isfinite(rtt)) return rtt;
    }
    if (last_ping_sec > 0.0f && isfinite(last_ping_sec))
        return last_ping_sec;
    if (last_ack_sec > 0.0f && isfinite(last_ack_sec))
        return last_ack_sec;
    return 0.0f;
}

static inline float net_latency_smoothed_gap_sec(
    const net_latency_stats_t *ack,
    const net_latency_stats_t *ping,
    float now_sec,
    float stale_sec) {
    if (!net_latency_stats_fresh(ack, now_sec, stale_sec) ||
        !net_latency_stats_fresh(ping, now_sec, stale_sec)) {
        return 0.0f;
    }
    float ack_sec = net_latency_stats_smoothed_sec(ack);
    float ping_sec = net_latency_stats_smoothed_sec(ping);
    if (!isfinite(ack_sec) || !isfinite(ping_sec) ||
        ack_sec <= 0.0f || ping_sec <= 0.0f ||
        ack_sec <= ping_sec) {
        return 0.0f;
    }
    return ack_sec - ping_sec;
}

static inline bool net_latency_gap_exceeds_sec(
    const net_latency_stats_t *ack,
    const net_latency_stats_t *ping,
    float now_sec,
    float stale_sec,
    float threshold_sec) {
    if (!isfinite(threshold_sec) || threshold_sec <= 0.0f) return false;
    return net_latency_smoothed_gap_sec(ack, ping, now_sec, stale_sec) >
        threshold_sec;
}

static inline bool net_latency_control_lane_stable(
    const net_latency_stats_t *ping,
    const net_latency_stats_t *ack,
    float now_sec,
    float stale_sec,
    float max_gap_sec,
    uint8_t min_samples) {
    if (!net_latency_stats_fresh(ping, now_sec, stale_sec) ||
        !net_latency_stats_fresh(ack, now_sec, stale_sec)) {
        return false;
    }
    if (min_samples > 0u &&
        (!ping || ping->count < min_samples ||
         !ack || ack->count < min_samples)) {
        return false;
    }
    if (!isfinite(max_gap_sec) || max_gap_sec < 0.0f)
        return false;
    return net_latency_smoothed_gap_sec(ack, ping, now_sec, stale_sec) <=
        max_gap_sec;
}

static inline float net_latency_ping_interval_for_state(
    const net_latency_stats_t *ping,
    const net_latency_stats_t *ack,
    float now_sec,
    float stale_sec,
    uint32_t ping_samples,
    float boot_interval_sec,
    float recovery_interval_sec,
    float steady_interval_sec,
    float stable_interval_sec,
    float recovery_gap_sec,
    float stable_gap_sec,
    uint8_t stable_min_samples) {
    if (ping_samples == 0u) return boot_interval_sec;
    if (!net_latency_stats_fresh(ping, now_sec, stale_sec) ||
        net_latency_gap_exceeds_sec(ack, ping, now_sec, stale_sec,
                                    recovery_gap_sec)) {
        return recovery_interval_sec;
    }
    if (stable_interval_sec > steady_interval_sec &&
        net_latency_control_lane_stable(ping, ack, now_sec, stale_sec,
                                        stable_gap_sec,
                                        stable_min_samples)) {
        return stable_interval_sec;
    }
    return steady_interval_sec;
}

static inline uint32_t net_latency_stale_window_miss_count(
    const net_latency_stats_t *stats,
    float now_sec,
    float stale_sec) {
    if (!stats || stats->last <= 0.0f || stats->last_sample_at <= 0.0f ||
        !isfinite(now_sec) || !isfinite(stale_sec) || stale_sec <= 0.0f) {
        return 0;
    }
    float age = now_sec - stats->last_sample_at;
    if (!isfinite(age) || age <= stale_sec) return 0;
    return (uint32_t)((age - stale_sec) / stale_sec) + 1u;
}

enum {
    NET_LATENCY_ACK_RECOVERY_STEADY = 0,
    NET_LATENCY_ACK_RECOVERY_MILD = 1,
    NET_LATENCY_ACK_RECOVERY_HOT = 2,
};

static inline uint8_t net_latency_ack_recovery_tier(
    uint16_t unacked_inputs,
    bool ack_stale,
    uint32_t ack_miss_windows,
    float ack_gap_sec,
    uint16_t mild_unacked,
    uint16_t hot_unacked,
    float mild_gap_sec,
    float hot_gap_sec) {
    if ((hot_unacked > 0 && unacked_inputs >= hot_unacked) ||
        ack_miss_windows > 0 ||
        (hot_gap_sec > 0.0f && isfinite(ack_gap_sec) &&
         ack_gap_sec > hot_gap_sec)) {
        return NET_LATENCY_ACK_RECOVERY_HOT;
    }
    if ((mild_unacked > 0 && unacked_inputs >= mild_unacked) ||
        ack_stale ||
        (mild_gap_sec > 0.0f && isfinite(ack_gap_sec) &&
         ack_gap_sec > mild_gap_sec)) {
        return NET_LATENCY_ACK_RECOVERY_MILD;
    }
    return NET_LATENCY_ACK_RECOVERY_STEADY;
}

static inline float net_latency_ack_recovery_age_threshold_sec(
    float control_rtt_sec,
    float min_sec,
    float rtt_multiplier) {
    if (!isfinite(min_sec) || min_sec < 0.0f)
        min_sec = 0.0f;
    if (!isfinite(rtt_multiplier) || rtt_multiplier <= 0.0f)
        rtt_multiplier = 1.0f;
    float by_rtt = 0.0f;
    if (isfinite(control_rtt_sec) && control_rtt_sec > 0.0f)
        by_rtt = control_rtt_sec * rtt_multiplier;
    return by_rtt > min_sec ? by_rtt : min_sec;
}

static inline bool net_latency_unacked_age_needs_recovery(
    float oldest_unacked_age_sec,
    float control_rtt_sec,
    float min_sec,
    float rtt_multiplier) {
    if (!isfinite(oldest_unacked_age_sec) ||
        oldest_unacked_age_sec <= 0.0f) {
        return false;
    }
    return oldest_unacked_age_sec >
        net_latency_ack_recovery_age_threshold_sec(control_rtt_sec,
                                                   min_sec,
                                                   rtt_multiplier);
}

#endif
