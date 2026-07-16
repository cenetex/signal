#ifndef SIGNAL_SERVER_GOSSIP_H
#define SIGNAL_SERVER_GOSSIP_H

/* Gossip / knowledge dock handshake.
 *
 * Contracts spread between stations and ships as KNOW_CONTRACT items in
 * bounded knowledge views. Information speed = ship speed; no station-
 * to-station radio. Full contract_t rows remain authoritative at issuers.
 *
 * The handshake is bidirectional: station merges its locally-issued
 * contracts into its local view, then station and ship exchange unmatched
 * or newer items with bounded eviction.
 */

#include "../shared/types.h"
#include "game_sim.h"  /* world_t */

contract_summary_t contract_summary_make(const contract_t *ct);

void contract_pool_insert(contract_summary_t *list, uint8_t *count, int cap,
                          const contract_summary_t *s);

void knowledge_view_configure(knowledge_view_t *view, uint8_t capacity);
void knowledge_view_insert(knowledge_view_t *view, const knowledge_item_t *item);
void knowledge_view_exchange(knowledge_view_t *a, knowledge_view_t *b);
bool knowledge_view_insert_contract(knowledge_view_t *view,
                                    const contract_summary_t *summary);
uint8_t knowledge_view_collect_contracts(const knowledge_view_t *view,
                                         contract_summary_t *out,
                                         uint8_t out_cap);
uint8_t knowledge_view_contract_count(const knowledge_view_t *view);

bool knowledge_item_from_contract_summary(const contract_summary_t *s,
                                          knowledge_item_t *out);
bool contract_summary_from_knowledge_item(const knowledge_item_t *item,
                                          contract_summary_t *out);
bool knowledge_item_from_market_memory(const market_memory_t *memory,
                                       knowledge_item_t *out);
bool market_memory_from_knowledge_item(const knowledge_item_t *item,
                                       market_memory_t *out);
bool market_memory_from_contract_summary(const contract_summary_t *s,
                                         market_memory_t *out);
bool market_memory_from_station_supply(const station_t *st,
                                       int station_index,
                                       commodity_t commodity,
                                       uint32_t observed_tick,
                                       market_memory_t *out);
bool market_memory_from_ore_pressure(const station_t *st,
                                     int station_index,
                                     commodity_t ore,
                                     uint32_t observed_tick,
                                     market_memory_t *out);
bool market_memory_from_scaffold_pressure(int destination_station,
                                          int source_station,
                                          module_type_t module_type,
                                          uint32_t observed_tick,
                                          uint64_t scaffold_nonce,
                                          market_memory_t *out);
bool market_memory_from_delivery_receipt(int origin_station,
                                         int destination_station,
                                         commodity_t commodity,
                                         uint16_t units,
                                         float value_hint,
                                         uint32_t observed_tick,
                                         uint64_t receipt_nonce,
                                         market_memory_t *out);
bool market_memory_from_route_reputation(int origin_station,
                                         int destination_station,
                                         commodity_t commodity,
                                         uint16_t evidence_count,
                                         float value_hint,
                                         uint32_t observed_tick,
                                         bool risk,
                                         market_memory_t *out);
bool market_memory_from_station_trust(int station_index,
                                      uint8_t action,
                                      commodity_t commodity,
                                      uint16_t evidence_count,
                                      float value_hint,
                                      uint32_t observed_tick,
                                      market_memory_t *out);
bool market_memory_from_station_risk(int station_index,
                                     uint8_t action,
                                     commodity_t commodity,
                                     uint16_t evidence_count,
                                     float value_hint,
                                     uint32_t observed_tick,
                                     market_memory_t *out);
typedef enum {
    GOSSIP_HNN_JOB_HAUL = 1,
    GOSSIP_HNN_JOB_MINE,
    GOSSIP_HNN_JOB_TOW,
    GOSSIP_HNN_JOB_SCOUT,
    GOSSIP_HNN_JOB_DELIVER_PROOF,
    GOSSIP_HNN_JOB_REPAIR,
} gossip_hnn_job_t;
bool gossip_hnn_store_market_memory(hnn_memory_t *mem,
                                    const market_memory_t *memory);
float gossip_hnn_market_resonance(const hnn_memory_t *mem,
                                  const market_memory_t *memory,
                                  gossip_hnn_job_t job);
void knowledge_view_reinforce_route_reputation(knowledge_view_t *view,
                                               const market_memory_t *memory);
void knowledge_view_reinforce_station_trust(knowledge_view_t *view,
                                            const market_memory_t *memory);
void knowledge_view_decay(knowledge_view_t *view,
                          uint8_t confidence_decay,
                          uint8_t salience_decay);
void knowledge_view_forget_contract(knowledge_view_t *view, uint8_t action,
                                    int station_idx, commodity_t commodity);

/* Nearby ship contact exchange. This is bounded in-flight gossip: ships that
 * pass close enough exchange structured knowledge, and neural workers also
 * blend carried market HNN traces. Returns the number of ship pairs touched. */
int gossip_ship_contact_exchange(world_t *w);

/* Run the bidirectional handshake at `station_index` between the station
 * and the ship pool passed in. The world is needed only for reading the
 * station's locally-issued contracts (filter w->contracts[] by
 * station_index == self) — that's a local read at the station, not
 * peer-station radio. */
void gossip_dock_handshake(world_t *w, int station_index,
                           knowledge_view_t *ship_knowledge);

/* Cold-start bootstrap: refresh every active station's own situated
 * pressure from local state. This seeds local stock supply, ore pressure,
 * scaffold pressure, and contracts visible at that station; it does not
 * copy contracts from peer stations. Called once at world_reset and once
 * after save-load completes — never at runtime.
 *
 * Why this is needed: gossip pools are bounded, ephemeral memory, while
 * station state and contracts are authoritative. Reset/load must rebuild
 * the local view so docked players and workers can see the work and
 * pressure physically present at their station before fresh dock contact
 * occurs. Cross-station knowledge still has to move by ship dock contact
 * or nearby in-flight contact. */
void gossip_bootstrap_world_stations(world_t *w);

/* Holographic experience exchange: when a neural worker docks, structured
 * market memories are encoded into the worker's HNN trace as advisory job
 * resonance. When a holographic pilot docks, their accumulated flight memory
 * is bundled into the station's bounded local cell only when the pilot has
 * new local experience or is carrying another station's cell. The station's
 * current cell is then stamped onto the pilot for physical transport. Same-
 * station repeat docks without new stores are no-ops, so traces do not amplify
 * just because a worker stays parked. Carried cross-station cells must match
 * the runtime HNN contract and source-station provenance, and must retain
 * enough estimated fidelity before they can seed another station.
 *
 * Called alongside gossip_dock_handshake during dock events.
 * HNN market memories never pay, verify, or mutate station authority. */
void gossip_hnn_decay_market_memory(hnn_memory_t *mem,
                                    uint32_t *last_decay_tick,
                                    uint32_t now_tick);
void gossip_hnn_exchange(world_t *w, int station_idx, npc_ship_t *npc);

#endif
