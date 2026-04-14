/*
 * sim_catalog.h -- Per-station identity catalog persistence.
 * Saves permanent station fields (name, position, modules, geometry, etc.)
 * to individual files in the stations/ directory, surviving server resets.
 */
#ifndef SIM_CATALOG_H
#define SIM_CATALOG_H

#include "game_sim.h"

/* Save one station's identity to stations/{index}.cat */
bool station_catalog_save(const station_t *st, int index, const char *dir);

/* Load all station identities from stations/ directory into stations array.
 * Returns number of stations loaded.  Session-tier fields are zeroed. */
int station_catalog_load_all(station_t *stations, int max_stations, const char *dir);

/* Save entire catalog (convenience wrapper). */
bool station_catalog_save_all(const station_t *stations, int count, const char *dir);

#endif /* SIM_CATALOG_H */
