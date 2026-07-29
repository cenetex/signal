#include "test_harness.h"

#include "local_authority.h"

static void test_local_authority_remote_owner_stays_empty(void) {
    local_authority_t owner = {0};

    ASSERT(!local_authority_is_allocated(&owner));
    ASSERT(local_authority_world(&owner) == NULL);
    ASSERT(local_authority_world_const(&owner) == NULL);

    local_authority_release(&owner);
    ASSERT(!local_authority_is_allocated(&owner));
}

static void test_local_authority_allocate_release_restart(void) {
    local_authority_t owner = {0};

    ASSERT(local_authority_acquire(&owner));
    world_t *first = local_authority_world(&owner);
    ASSERT(first != NULL);
    ASSERT(first->tick == 0);
    first->tick = 77;

    ASSERT(local_authority_acquire(&owner));
    ASSERT(local_authority_world(&owner) == first);
    ASSERT(local_authority_world(&owner)->tick == 77);

    local_authority_release(&owner);
    ASSERT(!local_authority_is_allocated(&owner));
    local_authority_release(&owner);

    ASSERT(local_authority_acquire(&owner));
    ASSERT(local_authority_world(&owner) != NULL);
    ASSERT(local_authority_world(&owner)->tick == 0);
    local_authority_release(&owner);
}

static void test_local_authority_allocation_failure_is_retryable(void) {
    local_authority_t owner = {0};

    local_authority_test_fail_next_allocation();
    ASSERT(!local_authority_acquire(&owner));
    ASSERT(!local_authority_is_allocated(&owner));
    ASSERT(local_authority_world(&owner) == NULL);

    ASSERT(local_authority_acquire(&owner));
    ASSERT(local_authority_is_allocated(&owner));
    local_authority_release(&owner);
}

void register_local_authority_tests(void) {
    RUN(test_local_authority_remote_owner_stays_empty);
    RUN(test_local_authority_allocate_release_restart);
    RUN(test_local_authority_allocation_failure_is_retryable);
}
