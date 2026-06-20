/* route_history_labels.h -- Human-readable labels for route-history summaries. */
#ifndef SIGNAL_ROUTE_HISTORY_LABELS_H
#define SIGNAL_ROUTE_HISTORY_LABELS_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "commodity.h"
#include "station_util.h"
#include "types.h"

typedef struct {
    bool used;
    uint8_t memory_kind;
    uint8_t origin_station;
    uint8_t destination_station;
    uint8_t commodity;
    uint8_t action;
    uint16_t event_count;
    uint32_t evidence_sum;
    uint8_t confidence_peak;
    uint8_t salience_peak;
    uint32_t latest_tick;
} route_history_aggregate_row_t;

static inline const char *route_history_memory_kind_label(uint8_t kind) {
    switch ((market_memory_kind_t)kind) {
    case MARKET_MEMORY_ROUTE_REPUTATION: return "route reputation";
    case MARKET_MEMORY_ROUTE_RISK:       return "route risk";
    case MARKET_MEMORY_ROUTE_SUCCESS:    return "route success";
    case MARKET_MEMORY_ROUTE_DANGER:     return "route danger";
    case MARKET_MEMORY_STATION_TRUST:    return "station trust";
    case MARKET_MEMORY_STATION_RISK:     return "station risk";
    case MARKET_MEMORY_DELIVERY_RECEIPT: return "delivery receipt";
    case MARKET_MEMORY_DEMAND:           return "demand";
    case MARKET_MEMORY_SUPPLY:           return "supply";
    case MARKET_MEMORY_ORE_PRESSURE:     return "ore pressure";
    case MARKET_MEMORY_SCAFFOLD_PRESSURE:return "scaffold pressure";
    case MARKET_MEMORY_NONE:
    default:                             return "memory";
    }
}

static inline const char *route_history_action_label(uint8_t action) {
    switch ((contract_action_t)action) {
    case CONTRACT_TRACTOR:  return "haul";
    case CONTRACT_FRACTURE: return "fracture";
    case CONTRACT_DELIVERY: return "delivery";
    default:                return "work";
    }
}

static inline const char *route_history_certainty_label(uint8_t confidence,
                                                        uint8_t salience) {
    unsigned score = ((unsigned)confidence + (unsigned)salience) / 2u;
    if (score >= 216u) return "known";
    if (score >= 176u) return "fresh";
    if (score >= 112u) return "heard";
    return "faint";
}

static inline void route_history_station_label(uint8_t station_idx,
                                               char *out,
                                               size_t cap) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "%s", station_short_name((int)station_idx));
}

static inline void route_history_summary_fields(
    uint8_t memory_kind,
    uint8_t origin_station,
    uint8_t destination_station,
    uint8_t commodity_id,
    uint8_t action,
    uint16_t evidence_count,
    uint8_t confidence,
    char *out,
    size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';

    char origin[24];
    char destination[24];
    route_history_station_label(origin_station, origin, sizeof(origin));
    route_history_station_label(destination_station,
                                destination, sizeof(destination));
    const char *commodity = commodity_id < COMMODITY_COUNT
        ? commodity_code((commodity_t)commodity_id)
        : "UNK";
    snprintf(out, cap, "%s: %s -> %s %s via %s, %u receipts, conf %u",
             route_history_memory_kind_label(memory_kind),
             origin,
             destination,
             commodity,
             route_history_action_label(action),
             (unsigned)evidence_count,
             (unsigned)confidence);
}

static inline void route_history_compact_fields(
    uint8_t memory_kind,
    uint8_t origin_station,
    uint8_t destination_station,
    uint8_t commodity_id,
    uint8_t action,
    uint16_t evidence_count,
    uint8_t confidence,
    char *left,
    size_t left_cap,
    char *right,
    size_t right_cap) {
    if (left && left_cap > 0) {
        char origin[16];
        char destination[16];
        route_history_station_label(origin_station, origin, sizeof(origin));
        route_history_station_label(destination_station,
                                    destination, sizeof(destination));
        snprintf(left, left_cap, "%s %s>%s",
                 route_history_memory_kind_label(memory_kind),
                 origin,
                 destination);
    }
    if (right && right_cap > 0) {
        const char *commodity = commodity_id < COMMODITY_COUNT
            ? commodity_code((commodity_t)commodity_id)
            : "UNK";
        snprintf(right, right_cap, "%s %s x%u c%u",
                 route_history_action_label(action),
                 commodity,
                 (unsigned)evidence_count,
                 (unsigned)confidence);
    }
}

static inline void route_history_detail_fields(
    uint64_t event_id,
    uint64_t epoch,
    uint8_t memory_kind,
    uint8_t origin_station,
    uint8_t destination_station,
    uint8_t commodity_id,
    uint8_t action,
    uint16_t evidence_count,
    uint8_t confidence,
    uint8_t salience,
    uint16_t value_hint,
    uint32_t observed_tick,
    char *title,
    size_t title_cap,
    char *evidence,
    size_t evidence_cap,
    char *meta,
    size_t meta_cap) {
    char origin[24];
    char destination[24];
    route_history_station_label(origin_station, origin, sizeof(origin));
    route_history_station_label(destination_station,
                                destination, sizeof(destination));
    const char *commodity = commodity_id < COMMODITY_COUNT
        ? commodity_code((commodity_t)commodity_id)
        : "UNK";
    if (title && title_cap > 0) {
        snprintf(title, title_cap, "%s %s>%s %s",
                 route_history_memory_kind_label(memory_kind),
                 origin, destination, commodity);
    }
    if (evidence && evidence_cap > 0) {
        snprintf(evidence, evidence_cap, "signed proof: %s via %s, %u receipt%s",
                 route_history_action_label(action),
                 commodity,
                 (unsigned)evidence_count,
                 evidence_count == 1 ? "" : "s");
    }
    if (meta && meta_cap > 0) {
        snprintf(meta, meta_cap, "%s event %llu epoch %llu tick %u value %u",
                 route_history_certainty_label(confidence, salience),
                 (unsigned long long)event_id,
                 (unsigned long long)epoch,
                 (unsigned)observed_tick,
                 (unsigned)value_hint);
    }
}

static inline bool route_history_aggregate_same_fields(
    const route_history_aggregate_row_t *row,
    uint8_t memory_kind,
    uint8_t origin_station,
    uint8_t destination_station,
    uint8_t commodity_id,
    uint8_t action) {
    return row && row->used &&
           row->memory_kind == memory_kind &&
           row->origin_station == origin_station &&
           row->destination_station == destination_station &&
           row->commodity == commodity_id &&
           row->action == action;
}

static inline void route_history_aggregate_add_fields(
    route_history_aggregate_row_t *rows,
    int cap,
    uint8_t memory_kind,
    uint8_t origin_station,
    uint8_t destination_station,
    uint8_t commodity_id,
    uint8_t action,
    uint16_t evidence_count,
    uint8_t confidence,
    uint8_t salience,
    uint32_t observed_tick) {
    if (!rows || cap <= 0) return;

    int slot = -1;
    for (int i = 0; i < cap; i++) {
        if (route_history_aggregate_same_fields(&rows[i],
                                                memory_kind,
                                                origin_station,
                                                destination_station,
                                                commodity_id,
                                                action)) {
            slot = i;
            break;
        }
        if (!rows[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return;

    route_history_aggregate_row_t *row = &rows[slot];
    if (!row->used) {
        memset(row, 0, sizeof(*row));
        row->used = true;
        row->memory_kind = memory_kind;
        row->origin_station = origin_station;
        row->destination_station = destination_station;
        row->commodity = commodity_id;
        row->action = action;
    }

    if (row->event_count < UINT16_MAX) row->event_count++;
    row->evidence_sum += evidence_count;
    if (confidence > row->confidence_peak)
        row->confidence_peak = confidence;
    if (salience > row->salience_peak)
        row->salience_peak = salience;
    if (observed_tick > row->latest_tick)
        row->latest_tick = observed_tick;
}

static inline int route_history_aggregate_rank(
    const route_history_aggregate_row_t *row) {
    if (!row || !row->used) return -1;
    return (int)row->evidence_sum * 4 +
           (int)row->event_count * 8 +
           (int)row->confidence_peak +
           (int)row->salience_peak / 2;
}

static inline int route_history_aggregate_sort(
    route_history_aggregate_row_t *rows,
    int cap) {
    if (!rows || cap <= 0) return 0;
    int count = 0;
    for (int i = 0; i < cap; i++) {
        if (rows[i].used) count++;
    }

    for (int i = 0; i < cap; i++) {
        for (int j = i + 1; j < cap; j++) {
            int ri = route_history_aggregate_rank(&rows[i]);
            int rj = route_history_aggregate_rank(&rows[j]);
            if (rj > ri ||
                (rj == ri && rows[j].latest_tick > rows[i].latest_tick)) {
                route_history_aggregate_row_t tmp = rows[i];
                rows[i] = rows[j];
                rows[j] = tmp;
            }
        }
    }
    return count;
}

static inline void route_history_aggregate_fields(
    uint8_t memory_kind,
    uint8_t origin_station,
    uint8_t destination_station,
    uint8_t commodity_id,
    uint8_t action,
    uint16_t event_count,
    uint32_t evidence_sum,
    uint8_t confidence_peak,
    uint8_t salience_peak,
    uint32_t latest_tick,
    char *title,
    size_t title_cap,
    char *evidence,
    size_t evidence_cap,
    char *freshness,
    size_t freshness_cap) {
    char origin[24];
    char destination[24];
    route_history_station_label(origin_station, origin, sizeof(origin));
    route_history_station_label(destination_station,
                                destination, sizeof(destination));
    const char *commodity = commodity_id < COMMODITY_COUNT
        ? commodity_code((commodity_t)commodity_id)
        : "UNK";
    if (title && title_cap > 0) {
        snprintf(title, title_cap, "%s %s>%s %s",
                 route_history_memory_kind_label(memory_kind),
                 origin, destination, commodity);
    }
    if (evidence && evidence_cap > 0) {
        snprintf(evidence, evidence_cap, "institution memory: %s via %s, %u signed row%s, %u receipt%s",
                 route_history_action_label(action),
                 commodity,
                 (unsigned)event_count,
                 event_count == 1 ? "" : "s",
                 (unsigned)evidence_sum,
                 evidence_sum == 1 ? "" : "s");
    }
    if (freshness && freshness_cap > 0) {
        snprintf(freshness, freshness_cap, "%s, latest tick %u",
                 route_history_certainty_label(confidence_peak, salience_peak),
                 (unsigned)latest_tick);
    }
}

#endif /* SIGNAL_ROUTE_HISTORY_LABELS_H */
