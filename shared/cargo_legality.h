/*
 * cargo_legality.h -- Station-policy interpretation of cargo provenance.
 *
 * Cargo does not carry a mutable "contraband" bit. A station derives a local
 * legal classification by combining the cargo receipt chain with that station's
 * current policy cards.
 */
#ifndef SHARED_CARGO_LEGALITY_H
#define SHARED_CARGO_LEGALITY_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cargo_receipt.h"
#include "station_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CARGO_LEGALITY_CLEAN = 0,
    CARGO_LEGALITY_SUSPICIOUS,
    CARGO_LEGALITY_CONTRABAND
} cargo_legality_status_t;

enum {
    CARGO_LEGALITY_REASON_NONE                   = 0,
    CARGO_LEGALITY_REASON_MISSING_RECEIPT        = 1u << 0,
    CARGO_LEGALITY_REASON_RECEIPT_REJECTED       = 1u << 1,
    CARGO_LEGALITY_REASON_BROKEN_LINK            = 1u << 2,
    CARGO_LEGALITY_REASON_BAD_SIGNATURE          = 1u << 3,
    CARGO_LEGALITY_REASON_UNKNOWN_AUTHORITY      = 1u << 4,
    CARGO_LEGALITY_REASON_BLACK_MARKET_AUTHORITY = 1u << 5,
    CARGO_LEGALITY_REASON_BANNED_ORIGIN          = 1u << 6,
    CARGO_LEGALITY_REASON_POLICY_SCREENS         = 1u << 7,
    CARGO_LEGALITY_REASON_POLICY_TOLERATES       = 1u << 8,
    CARGO_LEGALITY_REASON_CHAIN_TOO_LONG         = 1u << 9
};

typedef struct {
    cargo_legality_status_t status;
    uint32_t reasons;
    cargo_receipt_result_t receipt_result;
    int origin_station;
    int black_market_station;
} cargo_legality_result_t;

static inline int cargo_legality_find_station_by_pubkey(
    const station_t *stations, int station_count, const uint8_t pubkey[32])
{
    static const uint8_t zero32[32] = {0};
    if (!stations || station_count <= 0 || !pubkey ||
        memcmp(pubkey, zero32, 32) == 0) {
        return -1;
    }
    if (station_count > MAX_STATIONS) station_count = MAX_STATIONS;
    for (int i = 0; i < station_count; i++) {
        if (memcmp(stations[i].station_pubkey, pubkey, 32) == 0)
            return i;
    }
    return -1;
}

static inline bool cargo_legality_station_has_policy(
    const station_t *st, int station_idx, station_policy_card_id_t id)
{
    if (!st) return false;
    if (station_policy_cached_has(st, id)) return true;
    station_policy_selection_t selection;
    station_policy_select_cards(st, station_idx, &selection);
    return station_policy_selection_has(&selection, id);
}

static inline bool cargo_legality_station_screens(
    const station_t *st, int station_idx)
{
    return cargo_legality_station_has_policy(
        st, station_idx, STATION_POLICY_CARD_PROVENANCE_SCREENING) ||
        cargo_legality_station_has_policy(
            st, station_idx, STATION_POLICY_CARD_HOSTILE_ORIGIN_EMBARGO);
}

static inline bool cargo_legality_station_tolerates_contraband(
    const station_t *st, int station_idx)
{
    return cargo_legality_station_has_policy(
        st, station_idx, STATION_POLICY_CARD_BLACK_MARKET);
}

static inline uint32_t cargo_legality_reason_from_receipt_result(
    cargo_receipt_result_t result)
{
    switch (result) {
        case CARGO_RECEIPT_OK:
            return 0;
        case CARGO_RECEIPT_REJECT_TOO_LONG:
            return CARGO_LEGALITY_REASON_RECEIPT_REJECTED |
                   CARGO_LEGALITY_REASON_CHAIN_TOO_LONG;
        case CARGO_RECEIPT_REJECT_BAD_SIGNATURE:
            return CARGO_LEGALITY_REASON_RECEIPT_REJECTED |
                   CARGO_LEGALITY_REASON_BAD_SIGNATURE;
        case CARGO_RECEIPT_REJECT_BROKEN_LINKAGE:
            return CARGO_LEGALITY_REASON_RECEIPT_REJECTED |
                   CARGO_LEGALITY_REASON_BROKEN_LINK;
        default:
            return CARGO_LEGALITY_REASON_RECEIPT_REJECTED;
    }
}

static inline cargo_legality_result_t cargo_legality_classify(
    const station_t *stations,
    int station_count,
    int evaluating_station,
    const cargo_unit_t *unit,
    const cargo_receipt_chain_t *chain)
{
    cargo_legality_result_t out = {
        .status = CARGO_LEGALITY_CLEAN,
        .receipt_result = CARGO_RECEIPT_OK,
        .origin_station = -1,
        .black_market_station = -1,
    };
    if (!stations || !unit || evaluating_station < 0 ||
        evaluating_station >= station_count || evaluating_station >= MAX_STATIONS) {
        out.status = CARGO_LEGALITY_SUSPICIOUS;
        out.reasons |= CARGO_LEGALITY_REASON_UNKNOWN_AUTHORITY;
        return out;
    }

    const station_t *viewer = &stations[evaluating_station];
    bool screens = cargo_legality_station_screens(viewer, evaluating_station);
    bool tolerates = cargo_legality_station_tolerates_contraband(
        viewer, evaluating_station);
    if (screens) out.reasons |= CARGO_LEGALITY_REASON_POLICY_SCREENS;
    if (tolerates) out.reasons |= CARGO_LEGALITY_REASON_POLICY_TOLERATES;

    if (!chain || chain->len == 0) {
        out.reasons |= CARGO_LEGALITY_REASON_MISSING_RECEIPT;
        out.status = (screens || tolerates)
            ? CARGO_LEGALITY_CONTRABAND
            : CARGO_LEGALITY_SUSPICIOUS;
        return out;
    }

    out.receipt_result = cargo_receipt_chain_verify(
        chain->links, chain->len, unit->pub);
    if (out.receipt_result != CARGO_RECEIPT_OK) {
        out.reasons |= cargo_legality_reason_from_receipt_result(
            out.receipt_result);
        out.status = (screens || tolerates)
            ? CARGO_LEGALITY_CONTRABAND
            : CARGO_LEGALITY_SUSPICIOUS;
        return out;
    }

    out.origin_station = cargo_legality_find_station_by_pubkey(
        stations, station_count, chain->links[0].authoring_station);
    if (out.origin_station < 0) {
        out.reasons |= CARGO_LEGALITY_REASON_UNKNOWN_AUTHORITY;
        out.status = screens ? CARGO_LEGALITY_CONTRABAND
                             : CARGO_LEGALITY_SUSPICIOUS;
    } else {
        uint64_t forbidden = station_policy_forbidden_origin_mask(
            evaluating_station, (commodity_t)unit->commodity);
        if (out.origin_station < 64 &&
            (forbidden & (1ULL << out.origin_station)) != 0) {
            out.reasons |= CARGO_LEGALITY_REASON_BANNED_ORIGIN;
            out.status = CARGO_LEGALITY_CONTRABAND;
        }
    }

    for (uint8_t i = 0; i < chain->len; i++) {
        int author = cargo_legality_find_station_by_pubkey(
            stations, station_count, chain->links[i].authoring_station);
        if (author < 0) {
            out.reasons |= CARGO_LEGALITY_REASON_UNKNOWN_AUTHORITY;
            if (screens) out.status = CARGO_LEGALITY_CONTRABAND;
            continue;
        }
        if (cargo_legality_station_tolerates_contraband(&stations[author],
                                                        author)) {
            out.reasons |= CARGO_LEGALITY_REASON_BLACK_MARKET_AUTHORITY;
            out.black_market_station = author;
            out.status = CARGO_LEGALITY_CONTRABAND;
        }
    }

    if (tolerates && out.status == CARGO_LEGALITY_CONTRABAND)
        out.reasons |= CARGO_LEGALITY_REASON_POLICY_TOLERATES;
    return out;
}

static inline bool cargo_legality_station_accepts(
    cargo_legality_result_t result)
{
    if (result.status != CARGO_LEGALITY_CONTRABAND) return true;
    return (result.reasons & CARGO_LEGALITY_REASON_POLICY_TOLERATES) != 0;
}

#ifdef __cplusplus
}
#endif

#endif /* SHARED_CARGO_LEGALITY_H */
