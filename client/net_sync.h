/*
 * net_sync.h -- Multiplayer network state synchronization for the
 * Signal Space Miner client.  Handles applying server-authoritative
 * state to the local world and interpolating for smooth rendering.
 */
#ifndef NET_SYNC_H
#define NET_SYNC_H

#include "client.h"
#include "net.h"

/* Player join/leave callbacks. */
void on_player_join(uint8_t player_id);
void on_player_leave(uint8_t player_id);

/* Local prediction/reconciliation contract
 *
 * Multiplayer flight prediction is enabled only after the server proves the
 * tick-addressed input protocol by acknowledging an applied input tick. Until
 * then, snapshots are plain server authority.
 *
 * Once enabled, the client owns only the local player's movement controls
 * inside the predicted window: thrust, reverse thrust, boost, mining/tractor
 * holds, steering input, and the heading those inputs integrate. Prediction
 * frames are keyed by target server tick and replayed from the newest
 * authoritative baseline. Stale snapshots must not blend angle/steering over
 * active local turn prediction; they can only prune or rebase replay history.
 *
 * The server remains authoritative for docked/undocked state, launch
 * acceptance, position/velocity outside the replayable window, large tick
 * skew, damage/cargo/economy state, beams/scans/tractor visuals, and every
 * one-shot action result. When the server reports a docked state, an
 * accepted launch, an initial local state, or a correction too far ahead of
 * replay, the client accepts that baseline and resets or prunes prediction.
 *
 * Reconnects and local slot changes start a new input stream: pending actions,
 * input sequence/ack tracking, replay frames, tick anchors, action queues, and
 * motion telemetry are cleared. Fresh low sequence numbers after reconnect
 * must be accepted instead of compared against the previous stream.
 */
bool net_local_prediction_enabled(void);
float net_prediction_control_rtt_sec(void);
float net_prediction_latency_blend(void);
void net_replay_reset(void);
void net_replay_record_prediction(const input_intent_t *intent, float dt);
uint32_t net_estimated_server_tick_now(uint32_t server_tick);
void net_observe_server_tick(uint32_t server_tick);
void net_anchor_prediction_tick(uint32_t server_tick, bool clear_replay);
void net_observe_transport_latency_sample(float rtt_ms,
                                          float server_turnaround_ms,
                                          uint32_t server_tick,
                                          bool from_input_ack);
void net_adopt_local_tow_prediction(float dt);

/* Apply server-authoritative world state. */
void reset_remote_dynamic_sync(void);
void apply_remote_asteroids(const NetAsteroidState* asteroids, int count);
void apply_remote_asteroid_motion(const NetAsteroidMotionState* asteroids,
                                  int count);
void apply_remote_asteroid_state_q(const NetAsteroidStateQ* asteroids,
                                   int count);
void apply_remote_npcs(const NetNpcState* npcs, int count);
void apply_remote_npc_motion(const NetNpcMotionState* npcs, int count);
void apply_remote_npc_pos(const NetNpcPosState* npcs, int count);
void apply_remote_npc_pose(const NetNpcPoseState* npcs, int count);
void apply_remote_npc_linear(const NetNpcLinearState* npcs, int count);
void apply_remote_npc_status(const NetNpcStatusState* npcs, int count);
void apply_remote_stations(uint8_t index, const float* inventory, float credit_pool);
void apply_remote_contracts(const contract_t* contracts, int count);
void apply_remote_player_known_contracts(uint32_t mask);
void apply_remote_player_known_ledger(const NetKnownLedgerEntry *entries,
                                      int count);
void apply_remote_delivery_ledger(const NetDeliveryLedgerEntry *entries,
                                  int count);
void apply_remote_station_identity(const NetStationIdentity* si);
void apply_remote_station_diag(uint8_t station_id, const uint8_t *diag,
                               int module_count);
void apply_remote_scaffolds(const NetScaffoldState* scaffolds, int count);
void apply_remote_scaffold_remove(const uint8_t* indices, int count);
void apply_remote_scaffold_motion(const NetScaffoldMotionState* scaffolds,
                                  int count);
void apply_remote_cargo_pods(const NetCargoPodState* pods, int count);
void apply_remote_cargo_pod_remove(const uint8_t* indices, int count);
void apply_remote_cargo_pod_motion(const NetCargoPodMotionState* pods,
                                   int count);
void apply_remote_cargo_pod_linear(const NetCargoPodLinearState* pods,
                                   int count);
void apply_remote_interactions(const sim_interaction_t *items, int count);
void apply_remote_interaction_drift(const NetInteractionDriftState *items,
                                    int count);
void apply_remote_hail_response(uint8_t station,
                                float credits,
                                int contract_index,
                                const NetHailReason *reason);
void apply_remote_signal_channel(const NetSignalChannelMsg *msgs, int count);
/* Phase 2 — station manifest summary (per-{commodity, grade} counts). */
void apply_remote_station_manifest(uint8_t station_id,
                                   const NetStationManifestEntry *entries,
                                   int count);
/* Local player ship manifest summary (server-mirrored). */
void apply_remote_player_manifest(const NetStationManifestEntry *entries,
                                  int count);
/* Portable cargo receipt bundle for the local player's carried manifest. */
void apply_remote_cargo_receipt_bundle(const cargo_receipt_t *receipts,
                                       int count);
/* Detailed named-ingot snapshots that supplement manifest summaries with
 * per-unit provenance for trade-row display. */
void apply_remote_station_ingots(uint8_t station_id,
                                 const NetNamedIngotEntry *entries,
                                 int count);
void apply_remote_hold_ingots(const NetNamedIngotEntry *entries, int count);
void apply_remote_inspect_snapshot(const NetInspectSnapshot *snapshot);
/* Global leaderboard snapshot. */
void apply_remote_highscores(const NetHighscoreEntry *entries, int count);
void apply_remote_events(const sim_event_t *events, int count);
void begin_player_state_batch(void);
void net_record_input_ack(uint16_t input_seq_ack,
                          uint32_t server_tick,
                          uint32_t input_tick_ack);
void net_reset_local_input_stream(void);
bool net_remote_player_scanned(int player_id);
void net_update_remote_player_scans(const NetPlayerState *players);
void apply_remote_player_state(const NetPlayerState* state);
void apply_remote_player_ship(const NetPlayerShipState* state);

/* Death event from server — drives the death cinematic. respawn_station
 * + respawn_fee carry the per-station spawn fee that was just debited
 * (rendered on the death overlay). */
void on_remote_death(uint8_t player_id, float pos_x, float pos_y,
                     float vel_x, float vel_y, float angle,
                     float ore_mined, float credits_earned, float credits_spent,
                     int asteroids_fractured,
                     uint8_t respawn_station, float respawn_fee);

/* World time sync from server. */
void on_remote_world_time(float server_time);

/* Multiplayer station ring prediction/correction. */
void reset_station_ring_smoothing(void);
void step_remote_station_rings(float dt);

/* Sync local player slot to the network-assigned ID. */
void sync_local_player_slot_from_network(void);

/* Interpolate asteroid, NPC, and player positions for smooth multiplayer rendering. */
void interpolate_world_for_render(void);

/* Get interpolated remote player states for rendering. */
const NetPlayerState* net_get_interpolated_players(void);

#endif /* NET_SYNC_H */
