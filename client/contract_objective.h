/*
 * contract_objective.h -- Client-side "what next?" resolver for tracked work.
 *
 * This is the shared objective surface for SIGNAL copy, station work verbs,
 * recommendation, and world/compass markers. It intentionally interprets
 * existing station contracts; it does not mint new quest state.
 */
#ifndef CONTRACT_OBJECTIVE_H
#define CONTRACT_OBJECTIVE_H

#include <stdbool.h>
#include "types.h"

typedef enum {
    CONTRACT_OBJECTIVE_NONE = 0,
    CONTRACT_OBJECTIVE_FRACTURE,
    CONTRACT_OBJECTIVE_TRACTOR,
    CONTRACT_OBJECTIVE_TOW,
    CONTRACT_OBJECTIVE_DELIVER,
    CONTRACT_OBJECTIVE_PICKUP,
    CONTRACT_OBJECTIVE_MAKE,
    CONTRACT_OBJECTIVE_UPGRADE,
} contract_objective_kind_t;

typedef enum {
    CONTRACT_OBJECTIVE_TARGET_NONE = 0,
    CONTRACT_OBJECTIVE_TARGET_EXACT,
    CONTRACT_OBJECTIVE_TARGET_SUGGESTED_SOURCE,
    CONTRACT_OBJECTIVE_TARGET_DESTINATION,
    CONTRACT_OBJECTIVE_TARGET_PRODUCER,
    CONTRACT_OBJECTIVE_TARGET_FURNACE,
} contract_objective_target_kind_t;

typedef struct {
    bool active;
    contract_objective_kind_t kind;
    contract_objective_target_kind_t target_kind;
    int contract_index;
    int source_station;
    int target_station;
    commodity_t commodity;
    int quantity;
    char job[16];
    char label[48];
    char body[160];
    char message[192];
    bool has_world_target;
    vec2 world_target;
    float world_radius;
} contract_objective_t;

bool contract_objective_for_contract(int contract_index,
                                     contract_objective_t *out);
bool contract_objective_for_tracked(contract_objective_t *out);
bool contract_objective_for_recommended(contract_objective_t *out);
bool contract_objective_ready_upgrade(contract_objective_t *out);

#endif /* CONTRACT_OBJECTIVE_H */
