#ifndef SIGNAL_SERVER_GOSSIP_H
#define SIGNAL_SERVER_GOSSIP_H

/* Gossip-contract dock handshake.
 *
 * Contracts spread between stations and ships as bounded copies in
 * known_contracts pools. Information speed = ship speed; no station-
 * to-station radio. The full contract_t is the authoritative storage
 * at the issuing station; the contract_summary_t is the gossip payload
 * embedded in station_t and ship/npc_ship_t known_contracts arrays.
 *
 * The handshake is bidirectional: station merges its locally-issued
 * contracts into its own known pool, then station and ship copy
 * unmatched/newer summaries to each other (FIFO eviction on overflow,
 * newer-wins on dedup match by age_at_copy).
 */

#include "../shared/types.h"
#include "game_sim.h"  /* world_t */

contract_summary_t contract_summary_make(const contract_t *ct);

void contract_pool_insert(contract_summary_t *list, uint8_t *count, int cap,
                          const contract_summary_t *s);

/* Run the bidirectional handshake at `station_index` between the station
 * and the ship pool passed in. The world is needed only for reading the
 * station's locally-issued contracts (filter w->contracts[] by
 * station_index == self) — that's a local read at the station, not
 * peer-station radio. */
void gossip_dock_handshake(world_t *w, int station_index,
                           contract_summary_t *ship_pool,
                           uint8_t *ship_count, int ship_cap);

#endif
