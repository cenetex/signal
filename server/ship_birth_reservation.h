/*
 * ship_birth_reservation.h -- Stable fragment reservations held by
 * in-flight ship-birth assemblies.
 */
#ifndef SHIP_BIRTH_RESERVATION_H
#define SHIP_BIRTH_RESERVATION_H

#include "game_sim.h"

/*
 * True when the live asteroid's stable fragment identity is reserved by an
 * active ship-birth assembly. A referenced slot with missing identity data is
 * treated as reserved (fail closed), but a different non-zero fragment_pub
 * does not inherit a stale slot reservation.
 */
bool world_ship_birth_fragment_reserved(const world_t *w, int asteroid_idx);

#endif /* SHIP_BIRTH_RESERVATION_H */
