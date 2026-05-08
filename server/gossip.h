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

/* Cold-start bootstrap: seed every active station's known_contracts pool
 * with every currently-active contract in w->contracts[]. Called once at
 * world_reset and once after save-load completes — never at runtime.
 *
 * Why this is needed: contracts use station_index = destination, and the
 * hauler picker filters out contracts whose station_index == own home
 * (no self-delivery). So a hauler at home Prospect can only act on
 * contracts to OTHER stations. Those contracts only enter Prospect's
 * gossip pool via dock contact from a ship that has previously visited
 * the issuing station. With no traffic at world_reset, no ship has been
 * anywhere yet — Prospect's pool only contains Prospect-as-destination
 * contracts (which the picker ignores), so haulers idle forever. The
 * bootstrap breaks this deadlock by injecting an initial "everyone
 * knows everyone's demands" state — equivalent to "every station heard
 * about every contract at world creation."
 *
 * Why this is a bounded violation rather than ongoing radio: it runs
 * once per world-state-event (reset or load), not on every tick. New
 * contracts spawned at runtime — e.g. when a player plants an outpost
 * and a new SUPPLY contract spawns at the outpost's station_index —
 * land only in their issuer's pool and propagate purely via ship dock
 * contact. The bootstrap is initial conditions, not steady-state radio.
 *
 * Proper fix (TODO, post-v0): replace with pressure-driven contract
 * issuance that produces local contracts at every station as a function
 * of local state (modules awaiting supply, scaffolds in progress, hopper
 * pressure). Each station's pool fills from its own demands without
 * needing this cross-pollination; cold-start NPCs find work at their
 * home pool's locally-issued contracts (after picker semantics evolve to
 * allow some of them) or via early ship traffic. */
void gossip_bootstrap_world_stations(world_t *w);

#endif
