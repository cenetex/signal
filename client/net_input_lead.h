/*
 * net_input_lead.h -- Client-side input apply-tick lead tuning.
 *
 * The wire format carries a client-requested server tick for movement input.
 * Keeping this math isolated lets tests exercise the latency/adaptation policy
 * without linking the full renderer client.
 */
#ifndef NET_INPUT_LEAD_H
#define NET_INPUT_LEAD_H

#include <math.h>
#include <stdint.h>

#include "protocol.h"

#define NET_INPUT_LEAD_DEFAULT_MARGIN_TICKS 1
#define NET_INPUT_LEAD_MAX_MARGIN_TICKS 4
#define NET_INPUT_LEAD_EXACT_DECAY_ACKS 8u

static inline int32_t net_input_lead_clamp_margin(int32_t margin_ticks) {
    if (margin_ticks < 0) return 0;
    if (margin_ticks > NET_INPUT_LEAD_MAX_MARGIN_TICKS)
        return NET_INPUT_LEAD_MAX_MARGIN_TICKS;
    return margin_ticks;
}

static inline uint32_t net_input_lead_ticks_from_rtt(float rtt_sec,
                                                    float sim_dt,
                                                    int32_t margin_ticks) {
    uint32_t lead = NET_INPUT_LEAD_MIN_TICKS;
    if (rtt_sec > 0.0f && sim_dt > 0.0f && isfinite(rtt_sec) &&
        isfinite(sim_dt)) {
        float one_way_ticks = (rtt_sec * 0.5f) / sim_dt;
        lead = (uint32_t)ceilf(one_way_ticks) + NET_INPUT_LEAD_MIN_TICKS;
    }
    lead += (uint32_t)net_input_lead_clamp_margin(margin_ticks);
    if (lead < NET_INPUT_LEAD_MIN_TICKS) lead = NET_INPUT_LEAD_MIN_TICKS;
    if (lead > NET_INPUT_LEAD_MAX_TICKS) lead = NET_INPUT_LEAD_MAX_TICKS;
    return lead;
}

static inline int32_t net_input_lead_margin_after_ack(
    int32_t margin_ticks,
    uint32_t *exact_ack_count,
    int32_t apply_error_ticks) {
    margin_ticks = net_input_lead_clamp_margin(margin_ticks);
    if (apply_error_ticks > 0) {
        if (exact_ack_count) *exact_ack_count = 0;
        return net_input_lead_clamp_margin(margin_ticks + apply_error_ticks);
    }
    if (apply_error_ticks < 0) {
        if (exact_ack_count) *exact_ack_count = 0;
        return net_input_lead_clamp_margin(margin_ticks - 1);
    }
    if (exact_ack_count && margin_ticks > 0) {
        (*exact_ack_count)++;
        if (*exact_ack_count >= NET_INPUT_LEAD_EXACT_DECAY_ACKS) {
            *exact_ack_count = 0;
            return net_input_lead_clamp_margin(margin_ticks - 1);
        }
    }
    return margin_ticks;
}

#endif /* NET_INPUT_LEAD_H */
