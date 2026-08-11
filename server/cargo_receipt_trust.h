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
    cargo_craft_provenance_status_t craft_provenance;
    bool craft_input_lineage_proven;
    bool craft_conservation_proven;
} cargo_receipt_station_evaluation_t;

/*
 * Evaluate one finished cargo unit at a station without mutation.
 *
 * A missing or invalid origin proof, cargo mismatch, bad signature, broken
 * link, or revoked authority always rejects. Unknown and explicitly
 * untrusted authorities are interpreted through the evaluating station's
 * screening/black-market policy. Every receipt author is checked. A valid
 * CRAFT V1 origin is reported as station-attested only; the input-lineage
 * and conservation fields remain false until a future proof version exists.
 */
cargo_receipt_station_evaluation_t cargo_receipt_evaluate_at_station(
    const world_t *world,
    int evaluating_station,
    const cargo_unit_t *unit,
    const cargo_receipt_chain_t *chain);

/*
 * Evaluate receipt-less cargo that is still represented by its physical pod.
 * The caller must separately prove that the evaluating station owns that pod;
 * this function proves the exact unit against its declared origin station's
 * durable SMELT/CRAFT history, then applies the evaluating station's authority
 * and origin policy.  This is the physical-custody counterpart to a receipt
 * chain, not permission to trust an origin_station label by itself.
 */
cargo_receipt_station_evaluation_t
cargo_receipt_evaluate_physical_origin_at_station(
    const world_t *world,
    int evaluating_station,
    const cargo_unit_t *unit);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CARGO_RECEIPT_TRUST_H */
