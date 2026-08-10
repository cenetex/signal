/*
 * local_authority.h -- Lazy ownership for the in-process authority world.
 *
 * Remote clients must not reserve or clear a second world_t.  The small owner
 * below is embedded in local_server_t; its world is acquired only when the
 * client actually enters local-loopback mode.
 */
#ifndef LOCAL_AUTHORITY_H
#define LOCAL_AUTHORITY_H

#include "game_sim.h"

#include <stdbool.h>

typedef struct {
    world_t *world;
} local_authority_t;

/* Zero-initialized owners are empty and need no explicit constructor. */
bool local_authority_acquire(local_authority_t *owner);
void local_authority_release(local_authority_t *owner);
world_t *local_authority_world(local_authority_t *owner);
const world_t *local_authority_world_const(const local_authority_t *owner);
bool local_authority_is_allocated(const local_authority_t *owner);

#if defined(SIGNAL_LOCAL_AUTHORITY_TESTING)
/* Fail exactly the next allocation attempt.  Compiled out of clients. */
void local_authority_test_fail_next_allocation(void);
#endif

#endif /* LOCAL_AUTHORITY_H */
