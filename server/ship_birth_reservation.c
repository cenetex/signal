#include "ship_birth_reservation.h"

#include <string.h>

static bool reservation_bytes_nonzero(const uint8_t *bytes, size_t size) {
    if (!bytes) return false;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != 0) return true;
    }
    return false;
}

bool world_ship_birth_fragment_reserved(const world_t *w, int asteroid_idx) {
    if (!w || asteroid_idx < 0 || asteroid_idx >= MAX_ASTEROIDS)
        return false;

    const asteroid_t *asteroid = &w->asteroids[asteroid_idx];
    if (!asteroid->active) return false;

    const bool live_identity =
        reservation_bytes_nonzero(asteroid->fragment_pub,
                                  sizeof(asteroid->fragment_pub));

    for (int station_idx = 0; station_idx < MAX_STATIONS; station_idx++) {
        for (size_t build_idx = 0;
             build_idx < sizeof(w->ship_birth_assemblies[station_idx]) /
                             sizeof(w->ship_birth_assemblies[station_idx][0]);
             build_idx++) {
            const ship_birth_assembly_t *birth =
                &w->ship_birth_assemblies[station_idx][build_idx];
            if (!birth->active) continue;

            for (size_t fragment_idx = 0;
                 fragment_idx < sizeof(birth->fragment_pubs) /
                                    sizeof(birth->fragment_pubs[0]);
                 fragment_idx++) {
                const bool saved_identity =
                    reservation_bytes_nonzero(
                        birth->fragment_pubs[fragment_idx],
                        sizeof(birth->fragment_pubs[fragment_idx]));

                if (live_identity && saved_identity &&
                    memcmp(asteroid->fragment_pub,
                           birth->fragment_pubs[fragment_idx],
                           sizeof(asteroid->fragment_pub)) == 0) {
                    return true;
                }

                /*
                 * Old/incomplete in-memory assemblies may be missing one
                 * side of the stable identity. Keep that referenced object
                 * protected until validation can cancel or repair the
                 * assembly. Once both identities exist, an exact mismatch
                 * deliberately does not transfer the reservation to a new
                 * object occupying the same slot.
                 */
                if (birth->fragment_slots[fragment_idx] == asteroid_idx &&
                    (!live_identity || !saved_identity)) {
                    return true;
                }
            }
        }
    }
    return false;
}
