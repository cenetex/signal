/*
 * local_authority.c -- Lazy world_t allocation for single-player authority.
 */
#include "local_authority.h"

#include <stdlib.h>

#if defined(SIGNAL_LOCAL_AUTHORITY_TESTING)
static bool local_authority_fail_next_allocation;

void local_authority_test_fail_next_allocation(void) {
    local_authority_fail_next_allocation = true;
}
#endif

bool local_authority_acquire(local_authority_t *owner) {
    if (!owner) return false;
    if (owner->world) return true;

#if defined(SIGNAL_LOCAL_AUTHORITY_TESTING)
    if (local_authority_fail_next_allocation) {
        local_authority_fail_next_allocation = false;
        return false;
    }
#endif

    world_t *world = (world_t *)calloc(1, sizeof(*world));
    if (!world) return false;
    owner->world = world;
    return true;
}

void local_authority_release(local_authority_t *owner) {
    if (!owner || !owner->world) return;
    world_cleanup(owner->world);
    free(owner->world);
    owner->world = NULL;
}

world_t *local_authority_world(local_authority_t *owner) {
    return owner ? owner->world : NULL;
}

const world_t *local_authority_world_const(const local_authority_t *owner) {
    return owner ? owner->world : NULL;
}

bool local_authority_is_allocated(const local_authority_t *owner) {
    return owner && owner->world;
}
