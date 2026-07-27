/*
 * cargo_receipt_trust.h -- Station-composed cargo trust policy.
 *
 * Gameplay mutation sites call this read-only evaluator before changing
 * manifests, balances, contracts, construction, or ownership. It composes
 * cryptographic receipt validation, an exact verified origin event, public
 * authority lifecycle, and the evaluating station's current policy.
 */
#ifndef SERVER_CARGO_RECEIPT_TRUST_H
#define SERVER_CARGO_RECEIPT_TRUST_H

#include "game_sim.h"
#include "cargo_legality.h"
#include "cargo_receipt_issue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool accepted;
    bool local_origin_without_receipt;
    cargo_receipt_trust_result_t trust;
    cargo_receipt_origin_resolve_status_t origin_status;
    cargo_legality_result_t legality;
    int origin_station;
    int first_rejected_link;
} cargo_receipt_station_evaluation_t;

/*
 * Evaluate one finished cargo unit at a station without mutation.
 *
 * A missing or invalid origin proof, cargo mismatch, bad signature, broken
 * link, or revoked authority always rejects. Unknown and explicitly
 * untrusted authorities are interpreted through the evaluating station's
 * screening/black-market policy. Every receipt author is checked.
 */
cargo_receipt_station_evaluation_t cargo_receipt_evaluate_at_station(
    const world_t *world,
    int evaluating_station,
    const cargo_unit_t *unit,
    const cargo_receipt_chain_t *chain);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CARGO_RECEIPT_TRUST_H */
