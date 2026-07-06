/*
 * game_sim.h -- Headless game simulation types and API for the
 * Signal Space Miner authoritative server.
 *
 * Shared types (vec2, ship_t, station_t, etc.) come from shared/types.h.
 * Server-only types (server_player_t, world_t) are defined here.
 */
#ifndef GAME_SIM_H
#define GAME_SIM_H

#include <stdio.h>
#include <string.h>
#include "math_util.h"
#include "types.h"
#include "commodity.h"
#include "belt.h"
#include "ship.h"
#include "asteroid.h"
#include "economy.h"
#include "signal_model.h"  /* SIGNAL_BAND_OPERATIONAL for outpost placement gate */
#include "signal_field.h"
#include "cargo_receipt.h"
#include "handoff_ticket.h"
#include "tractor.h"

/* ------------------------------------------------------------------ */
/* Constants (server-only)                                            */
/* ------------------------------------------------------------------ */

enum {
    MAX_PLAYERS = 32,
    /* #294 Slice 8: unified NPC ship_t pool. Sized for NPCs only today;
     * widening to include players is a later slice. */
    MAX_SHIPS = MAX_NPC_SHIPS,
    MAX_SHIP_ASSETS = 128,
    MAX_DELIVERY_SHIPMENTS = 24,
    MAX_DELIVERY_BOUND_CARGO = 16,
    MAX_DESTROYED_ROCKS = 4096,
};

enum {
    SIGNAL_BRAIN_FLIGHT_ACTION_COUNT = 9,
    SIGNAL_BRAIN_FLIGHT_FEATURE_COUNT = 48,
};

static const float WORLD_RADIUS = 50000.0f;  /* safety net; gameplay bounded by station signal_range */
/* Ledger balances are still floats until the #588 fixed-point rewrite, so
 * keep station-local credit values inside the exact integer range where
 * one-credit transactions remain representable and non-finite values cannot
 * poison derived station pools. */
#define LEDGER_FLOAT_LIMIT 16000000.0f
/* Belt noise scale: world units per noise period divisor. Smaller =
 * tighter belt structure (more rivers/lakes per signal bubble), larger =
 * broader continents. At 15000, the ridged-noise period is ~5000u so a
 * starting 18000u signal range spans 3-4 belt features. */
static const float BELT_SCALE = 15000.0f;
static const float OUTPOST_CREDIT_COST = 500.0f;
static const float OUTPOST_RADIUS = 40.0f;
static const float OUTPOST_DOCK_RADIUS = 96.0f;
static const float OUTPOST_SIGNAL_RANGE = 6000.0f;
static const float OUTPOST_MIN_DISTANCE = 1500.0f; /* min distance between stations */
/* Signal quality above which new outposts are rejected: the "core" band
 * (>= 0.80) belongs to the existing station's coverage. Forces new
 * outposts out to the fringe, extending the network instead of stacking. */
#define OUTPOST_MAX_SIGNAL SIGNAL_BAND_OPERATIONAL
static const float SIM_DT = 1.0f / 120.0f;
static const float MINING_RANGE = 170.0f;
static const float SHIP_BRAKE = 180.0f;
static const float FRAGMENT_TRACTOR_ACCEL = 380.0f;
static const float FRAGMENT_MAX_SPEED = 210.0f;
static const float FRAGMENT_NEARBY_RANGE = 220.0f;
static const int FIELD_ASTEROID_TARGET = 220;
static const float FIELD_ASTEROID_RESPAWN_DELAY = 0.2f;
static const float FRACTURE_CHILD_CLEANUP_AGE = 30.0f;
static const float FRACTURE_CHILD_CLEANUP_DISTANCE = 4000.0f;
static const float STATION_DOCK_APPROACH_OFFSET = 34.0f;
static const float SHIP_COLLISION_DAMAGE_THRESHOLD = 115.0f;
static const float SHIP_COLLISION_DAMAGE_SCALE = 0.12f;

/* Soft impact -> hull damage. Returns 0 below the (possibly scaled)
 * threshold; otherwise (impact - threshold) * SCALE. Player and NPC
 * collision sites both call this so the formula stays in one place.
 * threshold_mult lets ship-vs-ship ramming cut the bar (0.7×) to make
 * deliberate ramming actually hurt. */
static inline float collision_damage_for(float impact, float threshold_mult) {
    float t = SHIP_COLLISION_DAMAGE_THRESHOLD * threshold_mult;
    return (impact > t) ? (impact - t) * SHIP_COLLISION_DAMAGE_SCALE : 0.0f;
}
static const float NPC_DOCK_TIME = 3.0f;
static const float HAULER_DOCK_TIME = 4.0f;
static const float HAULER_LOAD_TIME = 2.0f;
static const float COLLECTION_FEEDBACK_TIME = 1.1f;


/* ------------------------------------------------------------------ */
/* Sparse spatial hash for O(1) neighbor lookups — no world bounds     */
/* ------------------------------------------------------------------ */

#define SPATIAL_CELL_SIZE 800.0f
#define SPATIAL_MAX_PER_CELL 16
#define SPATIAL_HASH_INITIAL_CAP 512  /* power of 2 */

typedef struct {
    int16_t indices[SPATIAL_MAX_PER_CELL];
    uint8_t count;
} spatial_cell_t;

typedef struct {
    int32_t key_x, key_y;    /* cell coordinates; key_x == INT32_MIN = empty */
    spatial_cell_t cell;
} sparse_cell_entry_t;

typedef struct {
    sparse_cell_entry_t *entries; /* heap-allocated, power-of-2 capacity */
    uint32_t capacity;            /* always power of 2 */
    uint32_t mask;                /* capacity - 1 */
    uint32_t occupied;            /* number of occupied slots */
    uint32_t overflow_count;      /* active asteroids dropped by full cells */
} spatial_grid_t;

/* Map world position to cell coordinates (unbounded). */
static inline void spatial_grid_cell(const spatial_grid_t *g, vec2 pos, int *cx, int *cy) {
    (void)g;
    *cx = (int)floorf(pos.x / SPATIAL_CELL_SIZE);
    *cy = (int)floorf(pos.y / SPATIAL_CELL_SIZE);
}

/* Look up a cell by coordinates. Returns NULL if empty. */
static inline const spatial_cell_t *spatial_grid_lookup(const spatial_grid_t *g, int cx, int cy) {
    if (!g->entries) return NULL;
    /* Mul in unsigned space — signed * 73856093 overflows for |cx| > 29 (UB). */
    uint32_t h = ((uint32_t)cx * 73856093u) ^ ((uint32_t)cy * 19349663u);
    for (uint32_t probes = 0, i = h & g->mask; probes < g->capacity;
         probes++, i = (i + 1) & g->mask) {
        const sparse_cell_entry_t *e = &g->entries[i];
        if (e->key_x == INT32_MIN) return NULL;      /* empty slot */
        if (e->key_x == cx && e->key_y == cy) return &e->cell;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Cached signal strength grid — O(1) lookups instead of O(N_stations)*/
/* ------------------------------------------------------------------ */

#define SIGNAL_GRID_DIM  256
#define SIGNAL_CELL_SIZE 200.0f  /* covers ±25,600 units from origin */
#define SIGNAL_BEACON_MAX (MAX_STATIONS * MAX_MODULES_PER_STATION)

typedef struct {
    float *strength;           /* heap-allocated SIGNAL_GRID_DIM² floats */
    float offset_x, offset_y;  /* world offset to center grid */
    vec2  beacons[SIGNAL_BEACON_MAX];
    uint16_t beacon_count;
    bool  beacon_valid;
    bool  valid;                /* false = needs rebuild */
} signal_grid_t;

typedef struct {
    bool     active;            /* fracture claim window is open */
    bool     resolved;          /* fragment_pub + grade committed */
    bool     challenge_dirty;   /* transport still needs to broadcast challenge */
    bool     resolved_dirty;    /* transport still needs to broadcast resolution */
    uint32_t fracture_id;       /* monotonic runtime id */
    uint32_t deadline_ms;       /* world clock deadline */
    uint16_t burst_cap;         /* client search cap for this fracture */
    uint16_t _pad0;
    uint32_t best_nonce;        /* winning nonce */
    uint8_t  best_grade;        /* mining_grade_t */
    uint8_t  best_player_pub[32];
    uint8_t  seen_claimant_count; /* durable one-claim-per-identity */
    uint8_t  _pad1[3];
    uint8_t  seen_claimant_tokens[MAX_PLAYERS][8];
    /* Rebroadcast throttling (server-only, not persisted). Zero = never
     * broadcast. step_fracture_claims re-arms challenge_dirty whenever
     * now - challenge_last_ms >= FRACTURE_CHALLENGE_REBROADCAST_MS so
     * late joiners in the claim window still receive the challenge. */
    uint32_t challenge_last_ms;
} fracture_claim_state_t;

/* Resolution broadcast queue. fracture_commit_resolution pushes into
 * here so NET_MSG_FRACTURE_RESOLVED reaches clients even if the
 * asteroid is smelted and cleared in the same tick as the resolve —
 * the original resolved_dirty flag lives on the claim state and was
 * wiped by that clear, dropping the message. Queue entries outlive
 * the asteroid and are flushed on later broadcast ticks. */
#define MAX_PENDING_RESOLVES 32
#define FRACTURE_RESOLVE_RETRY_COUNT 3          /* broadcasts before giving up */
#define FRACTURE_RESOLVE_RETRY_PERIOD_MS 100    /* spacing between retries */
#define FRACTURE_CHALLENGE_REBROADCAST_MS 100   /* rebroadcast cadence while active */

typedef struct {
    bool     active;
    uint8_t  tx_count;
    uint8_t  grade;
    uint8_t  _pad;
    uint32_t fracture_id;
    uint32_t last_tx_ms;
    uint8_t  fragment_pub[32];
    uint8_t  winner_pub[32];
} pending_resolve_t;

typedef struct {
    bool active;
    uint8_t _pad0[3];
    int16_t fragment_slots[3];
    float age;
    float start_dist[3];
    vec2 target;
    uint8_t fragment_pubs[3][32];
} ship_birth_assembly_t;

/* ------------------------------------------------------------------ */
/* Server-specific types                                              */
/* ------------------------------------------------------------------ */

enum {
    SERVER_BRAIN_MODE_NONE = 0,
    SERVER_BRAIN_MODE_NEURAL_FLIGHT = 1,
    SERVER_BRAIN_MODE_HEURISTIC_LOGISTICS = 2,
    SERVER_BRAIN_MODE_HOLOGRAPHIC = 3,
};

/* input_intent_t lives in shared/types.h since slice 2 of #294 — both
 * server_player_t and npc_ship_t carry one and feed the same
 * step_player / sim_ship pipeline. */

#define PLAYER_MOVEMENT_QUEUE_CAP 64

typedef struct {
    uint32_t apply_tick;
    uint16_t input_seq;
    uint32_t client_sent_ms;
    uint32_t server_recv_ms;
    input_intent_t intent;
} movement_input_cmd_t;

typedef struct {
    bool valid;
    void *conn;
    uint16_t len;
    uint64_t hash;
} net_payload_cache_t;

typedef struct {
    bool connected;
    uint8_t id;
    void *conn;
    uint64_t client_addr_key; /* per-IP connection limit key; not persisted */
    bool client_addr_key_valid;
    uint8_t session_token[8]; /* stable identity for save persistence */
    bool session_ready;       /* true once client sends SESSION message */
    bool grace_period;        /* true while waiting for reconnect after disconnect */
    float grace_timer;        /* seconds remaining in grace window */
    uint32_t ship_asset_id;   /* SHIP_ASSET_ID_NONE until bound to a hull asset */
    ship_t ship;
    input_intent_t input;
    float boost_hold_timer;    /* seconds SHIFT has been held — drives "takeoff" burst */
    int current_station;
    int nearby_station;
    bool docked;
    bool in_dock_range;
    bool docking_approach;  /* tractor pulling ship toward core berth */
    int dock_berth;         /* berth slot (0-3) when docked */
    bool beam_active;
    bool beam_hit;
    bool beam_ineffective; /* hitting a rock too tough for current laser level */
    bool scan_active;      /* laser scanning a non-asteroid target */
    int scan_target_type;  /* 0=none, 1=station_module, 2=npc, 3=player, 4=cargo_pod */
    int scan_target_index; /* index into stations/npc_ships/players array */
    int scan_module_index; /* module index within station (for type=1) */
    int hover_asteroid;
    vec2 beam_start;
    vec2 beam_end;
    float cargo_sale_value;
    int nearby_fragments;
    int tractor_fragments;
    bool was_in_signal;     /* previous frame's signal state, for edge detection */
    char callsign[8];       /* e.g. "KRX-472" */
    /* Autopilot — server-side AI driving the player's ship.
     * 0 = off (manual control)
     * 1 = mining loop: mine → tow → dock → sell → undock → repeat
     * Manual input (turn/thrust/mine) cancels the autopilot. */
    bool actual_thrusting;      /* true if the ship thrusted this tick (survives input restore) */
    uint8_t autopilot_mode;
    int autopilot_target;       /* asteroid idx or -1 */
    int autopilot_station_target; /* logistics destination station or -1 */
    commodity_t autopilot_cargo;  /* logistics commodity, COMMODITY_COUNT when idle */
    int autopilot_state;        /* internal state machine cursor */
    float autopilot_timer;
    vec2 autopilot_last_pos;    /* position snapshot for stuck detection */
    float autopilot_stuck_timer;/* seconds since meaningful movement */
    uint8_t autopilot_teacher_valid;
    uint8_t autopilot_teacher_forward_blocked;
    uint16_t autopilot_teacher_allowed_mask;
    uint32_t autopilot_teacher_tick;
    int8_t autopilot_teacher_action;
    int8_t autopilot_teacher_turn;
    int8_t autopilot_teacher_thrust;
    float autopilot_teacher_features[
        SIGNAL_BRAIN_FLIGHT_ACTION_COUNT * SIGNAL_BRAIN_FLIGHT_FEATURE_COUNT];
    uint8_t autopilot_decision_valid;
    uint8_t autopilot_decision_action;
    uint8_t autopilot_decision_candidate_count;
    uint8_t autopilot_decision_reserved;
    uint32_t autopilot_decision_flags;
    float autopilot_decision_score;
    float autopilot_decision_neural_score;
    float autopilot_decision_route_risk;
    float autopilot_decision_signal_quality;
    uint8_t hail_decision_valid;
    int8_t hail_decision_station;
    uint8_t hail_decision_candidate_count;
    uint8_t hail_decision_reserved;
    uint32_t hail_decision_flags;
    float hail_decision_score;
    float hail_decision_signal_quality;
    uint64_t hail_decision_source_id;
    uint8_t server_brain_mode;  /* SERVER_BRAIN_MODE_* for headless pilots */
    /* Per-player relevance: tracks which asteroids this player has received */
    bool asteroid_sent[MAX_ASTEROIDS];
    uint32_t asteroid_motion_sent_tick[MAX_ASTEROIDS];
    vec2 asteroid_motion_sent_pos[MAX_ASTEROIDS];
    vec2 asteroid_motion_sent_vel[MAX_ASTEROIDS];
    uint32_t asteroid_state_sent_tick[MAX_ASTEROIDS];
    uint32_t asteroid_state_sent_sig[MAX_ASTEROIDS];
    uint32_t asteroid_state_sent_semantic_sig[MAX_ASTEROIDS];
    uint32_t fracture_challenge_sent_id[MAX_ASTEROIDS];
    uint32_t fracture_resolved_sent_ids[MAX_PENDING_RESOLVES];
    uint8_t fracture_resolved_sent_cursor;
    net_payload_cache_t player_ship_cache;
    net_payload_cache_t hold_ingots_cache;
    net_payload_cache_t player_manifest_cache;
    net_payload_cache_t inspect_snapshot_cache;
    net_payload_cache_t contracts_cache;
    uint64_t contracts_semantic_hash;
    bool contracts_semantic_valid;
    uint64_t contracts_last_sent_ms;
    net_payload_cache_t known_contracts_cache;
    net_payload_cache_t known_ledger_cache;
    net_payload_cache_t delivery_ledger_cache;
    net_payload_cache_t station_identity_cache[MAX_STATIONS];
    net_payload_cache_t world_stations_cache;
    net_payload_cache_t world_players_cache;
    net_payload_cache_t world_player_motion_cache;
    net_payload_cache_t world_player_motion_delta_cache;
    net_payload_cache_t world_player_motion_posed_cache;
    net_payload_cache_t world_player_dock_cache;
    bool player_motion_delta_valid[MAX_PLAYERS];
    int16_t player_motion_delta_qx[MAX_PLAYERS];
    int16_t player_motion_delta_qy[MAX_PLAYERS];
    vec2 player_motion_delta_vel[MAX_PLAYERS];
    uint8_t player_motion_delta_angle[MAX_PLAYERS];
    uint32_t player_motion_delta_tick[MAX_PLAYERS];
    uint32_t player_motion_delta_heartbeat_tick;
    uint64_t world_player_motion_last_sent_ms;
    uint64_t world_players_last_sent_ms;
    bool world_time_sent;
    uint32_t world_time_last_sent_tick;
    net_payload_cache_t world_npcs_cache;
    net_payload_cache_t world_npc_motion_cache;
    uint64_t world_npcs_semantic_hash;
    bool world_npcs_semantic_valid;
    uint32_t world_npcs_last_sent_tick;
    uint32_t world_npc_motion_last_sent_tick;
    uint32_t npc_motion_sent_tick[MAX_NPC_SHIPS];
    uint8_t npc_motion_sent_flags[MAX_NPC_SHIPS];
    vec2 npc_motion_sent_pos[MAX_NPC_SHIPS];
    vec2 npc_motion_sent_vel[MAX_NPC_SHIPS];
    float npc_motion_sent_angle[MAX_NPC_SHIPS];
    net_payload_cache_t world_npc_status_cache;
    uint32_t world_npc_status_last_sent_tick;
    net_payload_cache_t world_scaffolds_cache;
    bool scaffold_sent[MAX_SCAFFOLDS];
    uint64_t scaffold_sent_sig[MAX_SCAFFOLDS];
    uint64_t scaffold_motion_sent_sig[MAX_SCAFFOLDS];
    net_payload_cache_t world_cargo_pods_cache;
    net_payload_cache_t world_cargo_pod_motion_cache;
    uint64_t world_cargo_pods_semantic_hash;
    bool world_cargo_pods_semantic_valid;
    uint32_t world_cargo_pods_last_sent_tick;
    uint32_t world_cargo_pod_motion_last_sent_tick;
    bool cargo_pod_sent[MAX_CARGO_PODS];
    uint64_t cargo_pod_sent_sig[MAX_CARGO_PODS];
    uint32_t cargo_pod_motion_sent_tick[MAX_CARGO_PODS];
    vec2 cargo_pod_motion_sent_pos[MAX_CARGO_PODS];
    vec2 cargo_pod_motion_sent_vel[MAX_CARGO_PODS];
    float cargo_pod_motion_sent_rotation[MAX_CARGO_PODS];
    net_payload_cache_t world_interactions_cache;
    net_payload_cache_t world_interaction_drift_cache;
    uint64_t world_interactions_semantic_hash;
    bool world_interactions_semantic_valid;
    uint32_t world_interactions_last_sent_tick;
    uint32_t world_interaction_drift_last_sent_tick;
    uint32_t world_interaction_drift_block_tick;
    /* Set when an action result is sent. The next player payload broadcast
     * bypasses hash suppression so rejected/no-op actions also reconcile
     * any optimistic client state. */
    bool force_authoritative_resync;
    bool input_ack_state_valid;
    uint32_t input_ack_state_tick;
    vec2 input_ack_state_pos;
    vec2 input_ack_state_vel;
    float input_ack_state_angle;
    uint8_t input_ack_state_flags;
    uint8_t input_ack_state_tractor_level;
    uint8_t input_ack_state_towed_count;
    uint16_t input_ack_state_towed_fragments[10];
    movement_input_cmd_t movement_queue[PLAYER_MOVEMENT_QUEUE_CAP];
    uint8_t movement_queue_count;
    /* Last movement/control input sequence accepted from this client. Private
     * receipts carry freshness; WORLD_PLAYERS mirrors it on semantic heartbeats. */
    uint16_t last_input_seq;
    uint32_t last_input_tick;
    uint32_t last_input_client_sent_ms;
    uint32_t last_input_server_recv_ms;
    /* Runtime-only analytics state. Persisted identity remains session_token
     * / pubkey; these fields only drive stdout JSON and CloudWatch EMF. */
    uint64_t analytics_connected_ms;
    uint64_t analytics_last_activity_ms;
    uint64_t analytics_metrics_last_ms;
    uint32_t analytics_metrics_seq;
    uint32_t analytics_metrics_samples;
    uint16_t analytics_ping_ms;
    uint16_t analytics_ack_ms;
    uint16_t analytics_ack_gap_ms;
    uint16_t analytics_server_turnaround_ms;
    uint16_t analytics_player_interval_ms;
    uint16_t analytics_unacked_inputs;
    uint16_t analytics_replay_depth;
    uint8_t analytics_action_queue_depth;
    uint8_t analytics_recovery_flags;
    /* Last one-shot action id accepted on NET_MSG_INPUT. Retransmitted
     * action frames keep the same id, so the server can ignore duplicates
     * without discarding the packet's current movement flags. */
    uint16_t last_input_action_id;
    bool last_input_action_id_valid;
    bool pending_action_result_valid;
    uint8_t pending_action_result_action;
    uint16_t pending_action_result_id;
    uint16_t pending_action_result_input_seq;
    bool pending_action_before_docked;
    bool pending_action_before_docking_approach;
    int pending_action_before_station;
    uint8_t pending_action_before_autopilot_mode;
    float pending_action_before_hull;
    float pending_action_before_cargo_total;
    uint16_t pending_action_before_manifest_count;
    uint8_t pending_action_before_mining_level;
    uint8_t pending_action_before_hold_level;
    uint8_t pending_action_before_tractor_level;
    int pending_action_before_towed_count;
    int pending_action_before_towed_scaffold;
    int pending_action_before_station_pending_scaffold_count;
    int pending_action_before_station_pending_ship_build_count;
    float pending_action_before_station_balance;
    /* Last damage attribution. Set by apply_ship_damage_attributed and
     * read by emergency_recover_ship when populating SIM_EVENT_DEATH so
     * the death cinematic can name a killer. Cleared when the player
     * docks (a dock = "you survived"). zero token = unattributed. */
    uint8_t last_damage_killer_token[8];
    uint8_t last_damage_cause; /* death_cause_t */
    /* Layer A.2 of #479 — Ed25519 pubkey advertised by the client in
     * NET_MSG_REGISTER_PUBKEY. Zero-filled until the client registers.
     * Persisted with the world save so a returning pubkey can be
     * recognized across server restarts. Identity at the wire level is
     * still the 8-byte session_token; pubkey is additive state. */
    uint8_t pubkey[32];
    bool    pubkey_set;
    bool    pubkey_proof_ok;
    bool    pubkey_identity_finalized;
    /* Runtime-only reconnect latch. Session reattach can happen before
     * PROVE_PUBKEY arrives; when it does, skip the pubkey save reload that
     * would otherwise overwrite the live transferred ship state. */
    bool    preserve_live_state_on_pubkey_finalize;
    /* Layer A.3 of #479 — monotonic per-player nonce for NET_MSG_SIGNED_ACTION.
     * Persisted in the player save (PLY6+). Any signed action whose nonce is
     * <= this value is rejected as a replay; on accept, this becomes the
     * new high-water mark. Zero on a fresh slot — the first action may use
     * any non-zero nonce. */
    uint64_t last_signed_nonce;
} server_player_t;

typedef struct {
    bool active;
    uint16_t shipment_id;
    uint8_t origin_station;
    uint8_t destination_station;
    uint8_t contract_index;
    uint8_t debtor_player;
    uint8_t commodity;
    uint16_t quantity_total;
    uint16_t quantity_bound;
    uint16_t quantity_delivered;
    uint16_t quantity_black_market_sold;
    float debt_principal;
    float destination_payout;
    float origin_completion_credit;
    uint32_t due_tick;
    uint8_t status;
    uint8_t cargo_pub[MAX_DELIVERY_BOUND_CARGO][32];
    cargo_unit_t cargo_units[MAX_DELIVERY_BOUND_CARGO];
    cargo_receipt_chain_t cargo_chains[MAX_DELIVERY_BOUND_CARGO];
} delivery_shipment_t;

typedef struct {
    station_t stations[MAX_STATIONS];
    int station_count;              /* highest existing slot + 1 (seeded stations, then outposts) */
    uint32_t next_station_id;      /* monotonic counter for stable station IDs */
    asteroid_t asteroids[MAX_ASTEROIDS];
    fracture_claim_state_t fracture_claims[MAX_ASTEROIDS];
    /* Server-only, not persisted — broadcast retry queue for resolutions. */
    pending_resolve_t pending_resolves[MAX_PENDING_RESOLVES];
    /* Chunk origin tracking — server-only, not serialized */
    struct {
        int32_t chunk_x, chunk_y;
        bool from_chunk;   /* true = terrain, false = fracture child */
    } asteroid_origin[MAX_ASTEROIDS];
    /* Permanent floating-terrain ledger (#285 slice 1): rocks are
     * unique destructible entities, not respawning props. Each
     * terrain rock is born at first-contact materialization with a
     * stable rock_pub derived from (belt_seed, cx, cy, slot);
     * fracturing it retires its pub forever by writing here.
     * Subsequent visits to the same chunk skip slots whose pub is in
     * the destroyed set — mined regions stay mined.
     *
     * Identity-keyed (full 32-byte pub), not coordinate-keyed: keeps
     * records stable under future cosmic events that reassign chunk
     * coordinates (sector gates, megastructure passage), and lets a
     * chain-walk verifier confirm "this hull's frame's ferrite came
     * from a destroyed rock" without re-deriving from coords.
     *
     * Scope of slice 1 — known limits, all addressed in later slices:
     *
     *   - **Server-authoritative.** This ledger lives on the server
     *     and is never replicated to clients. Clients re-derive
     *     expected rock_pubs from belt_seed + chunk coords and trust
     *     the server's "no rock at slot K" answer. Federation /
     *     decentralized verification (#479 follow-up) will need a
     *     Bloom filter projection at minimum; that's its own work.
     *
     *   - **Per-server, not universal.** Operator A and Operator B
     *     running with the same belt_seed materialize the same
     *     rock_pubs (good — provenance lineage matches across
     *     federation), but mining at A doesn't retire the pub at B
     *     until federation-aware reconciliation lands. This struct
     *     is each operator's local view of the destroyed set.
     *
     *   - **Linear scan, 256-entry cap.** Acceptable at this size
     *     (a long session mines ~hundreds of rocks). Slice 2 lifts
     *     both. The architectural target is a four-tier model where
     *     the destroyed set has a different representation at each
     *     tier, optimized for that tier's access pattern:
     *
     *     ┌──────────┬──────────────────────────────┬───────────┬───────────────┐
     *     │ Tier     │ Structure                    │ Size      │ Use           │
     *     ├──────────┼──────────────────────────────┼───────────┼───────────────┤
     *     │ 1 mem    │ Binary Fuse filter           │ 9 b/elt   │ per-tick      │
     *     │          │   (or sorted-array+bsearch   │ (or 32B/  │ membership    │
     *     │          │    if cardinality stays low) │  elt raw) │ lookup        │
     *     │ 2 disk   │ signed append-only chain log │ ~80 B/    │ canonical     │
     *     │          │   (#479-C, hash-chained)     │ event     │ source-of-    │
     *     │          │                              │           │ truth         │
     *     │ 3 anchor │ Merkle Mountain Range root   │ 32 B      │ on-chain      │
     *     │          │                              │           │ commitment    │
     *     │ 4 proof  │ MMR inclusion proof          │ ~600 B    │ contract      │
     *     │          │   (log₂(n) hashes + key)     │           │ calldata      │
     *     └──────────┴──────────────────────────────┴───────────┴───────────────┘
     *
     *     Each tier is a *projection* of the same underlying
     *     destroyed set; the chain log (Tier 2) is canonical and the
     *     others are derived from it. Two operators with the same
     *     log produce bit-identical Fuse filters AND bit-identical
     *     MMR roots — deterministic by construction, no spec
     *     coordination required.
     *
     *     **Why Binary Fuse for Tier 1, not Bloom**: deterministic
     *     construction. `binary_fuse8_populate_seed(keys, n, &f,
     *     epoch_number)` produces bit-identical output for the same
     *     key set on every machine, so the on-chain commitment can
     *     be the filter's hash and any verifier can recompute it.
     *     Bloom can only match this with a standardized hash family
     *     + bit-vector size + insertion order ritual. Bonus: ~3
     *     memory accesses per query vs. ~8 for Bloom at matched FP
     *     (≈0.39% at ~9 bits/element for Fuse). Construction needs
     *     ≥ ~32 distinct keys; tiny epochs roll forward.
     *
     *     **Why Merkle Mountain Range for Tier 3, not the Fuse
     *     hash**: a Solana bounty contract that wants to verify
     *     "rock X was destroyed before epoch N" can't accept the
     *     full filter as calldata (~150KB at 100k destructions);
     *     it needs a tiny inclusion proof. MMR gives O(log n)
     *     proofs (~17 hashes × 32B ≈ 600B for 100k entries) verified
     *     in microseconds. Append-only matches the access pattern
     *     exactly — destructions never get rewritten. SMT/IMT are
     *     better only if non-membership proofs are required, which
     *     bounty contracts don't need (they care about destroyed
     *     rocks, not surviving ones).
     *
     *     **Slice 2** swaps the inline 256-entry array for an in-
     *     memory sorted log of `(rock_pub[32], destroyed_at_ms[8])`
     *     entries plus a per-station append-only `data/destroyed_
     *     rocks.log` (Tier 2). Hot-path lookup uses bsearch on the
     *     sorted log until cardinality justifies the Fuse build.
     *
     *     **Slice 3** introduces the epoch-boundary writer: at
     *     each close, build the Fuse filter (Tier 1 snapshot) and
     *     the MMR root (Tier 3) from the live log, sign the root +
     *     epoch number, post to signal_anchor. Live log resets
     *     between epoch closes; closed-epoch artifacts (filter +
     *     MMR root) become immutable. Bounty contracts (Tier 4)
     *     consume `(rock_pub, mmr_proof, anchor_epoch)` tuples.
     *
     *   - **Interim cap.** The eventual side-file removes the in-memory
     *     ceiling. Until then MAX_DESTROYED_ROCKS is deliberately sized
     *     above the old 256-entry cliff so ordinary long sessions keep
     *     the permanent tombstone needed by chunk rematerialization and
     *     verifier walks.
     *
     *   - **Cohabitation with fragment_pub on asteroid_t.** Terrain
     *     rocks have rock_pub set + fragment_pub zero; fracture
     *     children have the opposite. A later slice will fold
     *     these into one `pub[32]` field discriminated by
     *     `asteroid_origin.from_chunk`, so identity-handling code
     *     can read `&a->pub` regardless of provenance type. */
    /* Slice 2 representation: sorted by rock_pub for O(log n) bsearch
     * lookup. `destroyed_at_ms` is the world-clock timestamp of the
     * fracture (rounded to milliseconds), recorded so that closed-
     * epoch snapshots in slice 3 can bound "destroyed before epoch N"
     * proofs. Sorted on insert via memmove — at the interim cap a
     * worst-case shift is ~160KB, negligible on a terrain fracture. */
    struct destroyed_rock_s {
        uint8_t  rock_pub[32];
        uint64_t destroyed_at_ms;
    } destroyed_rocks[MAX_DESTROYED_ROCKS];
    uint16_t destroyed_rock_count;
    npc_ship_t npc_ships[MAX_NPC_SHIPS];
    /* Contract-origin hull assets. These are the durable economic
     * records that say a physical hull exists; player/NPC ship fields
     * mirror the assigned asset for protocol compatibility in v1. */
    ship_asset_t ship_assets[MAX_SHIP_ASSETS];
    uint32_t next_ship_asset_id;
    /* Runtime-only ship birth choreography. Pending ship builds stay
     * save-stable; this sidecar reclaims three live ore fragments when
     * one is available and drives them to their centroid before mint. */
    ship_birth_assembly_t ship_birth_assemblies[MAX_STATIONS][4];
    /* #294 Slice 8: unified ship_t pool. Each active NPC owns a slot
     * here; the paired character_t.ship_idx points to it. Players still
     * carry an inline ship_t in server_player_t — converging is a later
     * slice. Manifest lifecycle: bootstrap on alloc, ship_cleanup on
     * free and on world_cleanup/world_reset. */
    ship_t ships[MAX_SHIPS];
    /* Controller pool. Each entry pairs an active NPC (today) or a
     * player (later) to a ship slot via ship_idx. Sized to the
     * NPC + player union so later slices don't need a flag-day resize. */
    character_t characters[MAX_PLAYERS + MAX_NPC_SHIPS];
    scaffold_t scaffolds[MAX_SCAFFOLDS];
    cargo_pod_t cargo_pods[MAX_CARGO_PODS];
    server_player_t players[MAX_PLAYERS];
    uint32_t rng;
    /* belt_seed: the rng value at world_reset time — the deterministic
     * seed for the entire belt's structure. rng evolves during sim
     * (NPC spawns, fracture children, etc.) but the belt + every
     * rock_pub derived from it are anchored to this fixed value. */
    uint32_t belt_seed;
    /* world_seq: monotonic id for total ordering across worlds. Set to
     * wall-clock time at fresh-world creation; preserved across normal
     * server restarts via the save. Lets the leaderboard prefer
     * newer-world runs over older-world runs deterministically (orphan
     * chain logs without a WORLD_INFO event default to 0, the oldest
     * possible). */
    uint32_t world_seq;
    float time;
    uint32_t tick;
    float field_spawn_timer;
    float gravity_accumulator;  /* runs gravity at reduced rate */
    /* Regression telemetry for the retired refinery-hopper smelt path.
     * Fragment-tow smelts are the only valid smelt source now. These
     * counters should remain zero in normal play and in tests. */
    uint64_t hopper_smelt_events;
    double hopper_smelt_units;
    /* Replenish dead haulers / miners. Decremented in step_npc_ships;
     * when it hits zero, replenish_npc_roster spawns AT MOST one NPC
     * (the most-understaffed station/role pair) and resets the timer.
     * Drip-feed is intentional: a full chain wipe takes time to
     * recover so PvP harassment has weight, but the chain isn't a
     * permanent loss. */
    float npc_respawn_timer;
    /* Runtime-only aggregate pilot pressure for frontier expansion.
     * Physical player/NPC/entity counts stay capped by the v1 protocol;
     * these fields let hundreds or thousands of simulated strategic
     * pilots rank expansion work without occupying network slots. */
    int frontier_virtual_pilots;
    float frontier_plan_timer;
    uint32_t frontier_plans_created;
    uint32_t frontier_scaffold_orders;
    uint32_t frontier_module_plans_created;
    uint32_t frontier_module_scaffold_orders;
    uint32_t frontier_virtual_scaffolds_manufactured;
    uint32_t frontier_virtual_scaffold_deliveries;
    uint32_t frontier_virtual_supply_deliveries;
    uint8_t frontier_decision_valid;
    uint8_t frontier_decision_action;
    uint16_t frontier_decision_plan_limit;
    uint32_t frontier_decision_flags;
    float frontier_decision_score;
    float frontier_decision_pressure;
    uint64_t frontier_decision_source_id;
    /* Monotonic counter for npc_ship_t.session_token. Incremented in
     * spawn_npc; the low/high bytes get stamped into the token so each
     * spawn (including respawns of the same role at the same station)
     * gets a fresh ledger identity. */
    uint16_t next_npc_token;
    sim_events_t events;
    sim_interactions_t interactions;
    contract_t contracts[MAX_CONTRACTS];
    delivery_shipment_t delivery_shipments[MAX_DELIVERY_SHIPMENTS];
    uint16_t next_delivery_shipment_id;
    bool player_only_mode;
    uint32_t next_fracture_id;
    belt_field_t belt;
    spatial_grid_t asteroid_grid;
    signal_grid_t signal_cache;
    /* Runtime-only local fuzzy memory over world space. Rebuilt from
     * station-local gossip bootstrap after reset/load and reinforced only by
     * physical dock/contact memory exchange; never serialized or authoritative. */
    signal_field_t signal_field;
    uint32_t signal_field_decay_tick;
    signal_channel_t signal_channel;  /* station broadcast log (#316) */
    /* Layer A.2 of #479 — pubkey registry. Maps a client's persisted
     * Ed25519 pubkey to its current session_token so a reconnecting
     * pubkey can be matched to its existing player record across
     * session_token rotations. Linear scan, bounded by MAX_PLAYERS. */
    struct {
        uint8_t pubkey[32];
        uint8_t session_token[8];
        bool    in_use;
    } pubkey_registry[MAX_PLAYERS];
    /* Runtime replay guard for accepted handoff tickets. The ticket itself is
     * source-signed and hash-bound; this cache prevents a valid ticket from
     * hydrating the same destination ship twice in one authority run. */
    uint8_t handoff_consumed_ticket_hashes[128][32];
    uint16_t handoff_consumed_ticket_count;
    uint16_t handoff_consumed_ticket_next;
} world_t;

/* ------------------------------------------------------------------ */
/* Hull definitions (declared in shared/types.h, defined in game_sim.c) */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Logging — define GAME_SIM_VERBOSE to enable [sim] printf chatter   */
/* ------------------------------------------------------------------ */

#ifdef GAME_SIM_VERBOSE
#define SIM_LOG(...) printf(__VA_ARGS__)
#else
#define SIM_LOG(...) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

float contract_price(const contract_t *c);
void world_reset(world_t *w);
void world_ensure_seeded_freeport(world_t *w);
/* Genesis MOTD/tier chain events for the seeded stations. Caller must
 * invoke this only on a fresh-world boot (no save loaded), AFTER
 * world_reset. See seed_station_motd_chain_events in game_sim.c. */
void world_seed_station_chain_genesis(world_t *w);
void world_cleanup(world_t *w);
void world_sim_step(world_t *w, float dt);
void world_sim_step_player_only(world_t *w, int player_idx, float dt);
void server_player_queue_movement_input(server_player_t *sp,
                                        const input_intent_t *intent,
                                        uint16_t input_seq,
                                        uint32_t apply_tick);
void server_player_queue_ticked_movement_input(server_player_t *sp,
                                               const input_intent_t *intent,
                                               uint16_t input_seq,
                                               uint32_t apply_tick);

typedef struct {
    input_intent_t intent;
    uint8_t action;
    uint8_t ack_status;
    uint16_t action_id;
    uint16_t input_seq;
    uint32_t client_tick;
    uint32_t apply_tick;
    bool rejected_unsigned_action;
    bool force_authoritative_resync;
    int station_identity_dirty;
} server_input_dispatch_result_t;

uint32_t server_input_apply_tick_for_world(const world_t *w,
                                           uint32_t client_tick);
void server_merge_one_shot_input(input_intent_t *dst,
                                 const input_intent_t *src);
bool server_dispatch_input_message(world_t *w, int player_idx,
                                   const uint8_t *data, int len,
                                   uint32_t server_recv_ms,
                                   server_input_dispatch_result_t *out);
uint16_t server_signed_action_payload_id(const uint8_t *payload,
                                         uint16_t payload_len,
                                         uint16_t fixed_len);
bool server_parse_signed_input_action_payload(const uint8_t *payload,
                                              uint16_t payload_len,
                                              input_intent_t *out_intent,
                                              uint16_t *out_action_id,
                                              uint8_t *out_action);
bool server_apply_signed_plan_payload(server_player_t *sp,
                                      const uint8_t *payload,
                                      uint16_t payload_len);

typedef void (*server_receipt_chain_sink_fn)(
    void *user,
    const cargo_receipt_chain_t *chain);

typedef struct {
    int station_identity_dirty;
} server_signed_action_dispatch_result_t;

typedef struct {
    bool rejected_unsigned_action;
} server_unsigned_dispatch_result_t;

typedef server_unsigned_dispatch_result_t server_legacy_cargo_dispatch_result_t;

bool server_dispatch_legacy_plan_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_unsigned_dispatch_result_t *out);

bool server_dispatch_legacy_buy_ingot_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_receipt_chain_sink_fn receipt_sink,
    void *receipt_user,
    server_legacy_cargo_dispatch_result_t *out);

bool server_dispatch_legacy_deliver_ingot_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_receipt_chain_sink_fn receipt_sink,
    void *receipt_user,
    server_legacy_cargo_dispatch_result_t *out);

typedef struct {
    bool evaluated;
    int result;
} server_receipt_presentation_dispatch_result_t;

bool server_dispatch_receipt_presentation_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_receipt_presentation_dispatch_result_t *out);

bool server_dispatch_fracture_claim_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_unsigned_dispatch_result_t *out);

bool server_dispatch_signed_action_payload(
    world_t *w,
    int player_idx,
    uint8_t action_type,
    const uint8_t *payload,
    uint16_t payload_len,
    server_receipt_chain_sink_fn receipt_sink,
    void *receipt_user,
    server_signed_action_dispatch_result_t *out);

typedef void (*server_handoff_ticket_sink_fn)(
    void *user,
    uint8_t status,
    uint8_t source_station,
    uint8_t dest_station,
    const handoff_ticket_t *ticket);

typedef void (*server_handoff_result_sink_fn)(
    void *user,
    uint8_t status,
    uint8_t reason,
    uint8_t dest_station,
    const uint8_t ticket_hash[32]);

bool server_dispatch_handoff_request(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_handoff_ticket_sink_fn ticket_sink,
    void *ticket_user);

bool server_dispatch_handoff_present(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_handoff_result_sink_fn result_sink,
    void *result_user);
void player_init_ship(server_player_t *sp, world_t *w);
bool server_player_has_live_session(const server_player_t *sp);
bool server_player_is_gameplay_ready(const server_player_t *sp);
void server_player_clear_live_session_identity(server_player_t *sp);
void server_player_reset_input_stream(server_player_t *sp);
void server_player_clear_transient_input(server_player_t *sp);

/* Layer A.2 of #479 — pubkey registry. */
/* Look up a player_idx (into world.players[]) by pubkey. Returns -1 if not
 * registered. The lookup walks pubkey_registry to find the binding, then
 * locates the player slot owning that session_token. */
int registry_lookup_by_pubkey(const world_t *w, const uint8_t pubkey[32]);
/* Register / update a (pubkey, session_token) binding for a connection.
 * Returns true if a registry entry was newly added or updated; false on
 * out-of-space (registry full of distinct pubkeys, which equals MAX_PLAYERS).
 * Idempotent: same (pubkey, token) pair is a no-op.
 * If the pubkey was previously bound to a different session_token, the
 * registry entry is rebound to the new token (token rotation across
 * reconnects). The caller is responsible for any state migration on the
 * server_player_t side. */
bool registry_register_pubkey(world_t *w, const uint8_t pubkey[32],
                              const uint8_t session_token[8]);
int server_find_session_reattach_slot(const world_t *w, int player_idx,
                                      const uint8_t session_token[8]);
bool server_player_can_use_pubkey_persistence(const server_player_t *sp);
bool server_finalize_pubkey_identity(world_t *w, int player_idx);

typedef struct {
    uint8_t token[8];
    char callsign[8];
    bool has_callsign;
} server_session_message_t;

typedef struct {
    bool accepted;
    bool same_pubkey;
    uint8_t pubkey[32];
} server_pubkey_register_result_t;

typedef enum {
    SERVER_PUBKEY_PROOF_OK = 0,
    SERVER_PUBKEY_PROOF_MALFORMED,
    SERVER_PUBKEY_PROOF_NO_REGISTRATION,
    SERVER_PUBKEY_PROOF_PUBKEY_MISMATCH,
    SERVER_PUBKEY_PROOF_SESSION_MISMATCH,
    SERVER_PUBKEY_PROOF_BAD_SIGNATURE
} server_pubkey_proof_status_t;

typedef struct {
    server_pubkey_proof_status_t status;
    bool verified;
} server_pubkey_proof_result_t;

bool server_parse_session_message(const uint8_t *data, int len,
                                  server_session_message_t *out);
bool server_apply_session_message(world_t *w, int player_idx,
                                  const server_session_message_t *msg);
bool server_dispatch_register_pubkey_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_pubkey_register_result_t *out);
bool server_dispatch_pubkey_proof_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_pubkey_proof_result_t *out);
const char *server_pubkey_proof_status_name(
    server_pubkey_proof_status_t status);

/* Layer A.3 of #479 — signed-action verification.
 *
 * Reasons a signed action can be rejected. SIGNED_ACTION_OK means the
 * caller may proceed to dispatch the action and update last_signed_nonce.
 * Anything else: silently drop on the wire path; tests inspect the code
 * to assert the rejection reason. */
typedef enum {
    SIGNED_ACTION_OK = 0,
    SIGNED_ACTION_REJECT_NO_PUBKEY,         /* connection has not registered a pubkey */
    SIGNED_ACTION_REJECT_MALFORMED,         /* short message / bad payload_len */
    SIGNED_ACTION_REJECT_BAD_SIG,           /* Ed25519 verify failed */
    SIGNED_ACTION_REJECT_REPLAY,            /* nonce <= last_signed_nonce */
    SIGNED_ACTION_REJECT_UNKNOWN_TYPE       /* action_type out of range */
} signed_action_result_t;

/* Pure verification: parses (data,len), looks up the pubkey for `player_idx`,
 * verifies the Ed25519 signature, and checks the nonce against the player's
 * last_signed_nonce. On SIGNED_ACTION_OK, fills *out_action_type, *out_nonce,
 * and *out_payload (= pointer into `data`) so the caller can dispatch. Does
 * NOT mutate the world; updating last_signed_nonce is the dispatcher's job
 * after the action lands successfully.
 *
 * Used by both server/main.c (live wire path) and the test harness. */
signed_action_result_t signed_action_verify(const world_t *w, int player_idx,
                                            const uint8_t *data, int len,
                                            uint8_t *out_action_type,
                                            uint64_t *out_nonce,
                                            const uint8_t **out_payload,
                                            uint16_t *out_payload_len);
float signal_strength_at(const world_t *w, vec2 pos);
void spatial_grid_build(world_t *w);
void ledger_credit_supply(station_t *st, const uint8_t *token, float ore_value);
float ledger_credit_supply_amount(station_t *st, const uint8_t *token, float ore_value);

/* Nav API — canonical declarations in sim_nav.h.
 * Repeated here because sim_nav.h includes game_sim.h (circular).
 * Client code (client/) includes game_sim.h but not sim_nav.h. */
int nav_get_player_path(int player_id, vec2 *out_waypoints, int max_count, int *out_current);
int nav_compute_path(const world_t *w, vec2 start, vec2 goal, float clearance,
                     vec2 *out_waypoints, int max_count);
bool nav_segment_clear(const world_t *w, vec2 start, vec2 goal, float clearance);
void station_rebuild_all_nav(const world_t *w);
void rebuild_signal_chain(world_t *w);

/* Ring rotation dynamics: each populated ring can carry a small ambient
 * drift bias via arm_speed[], while cross-ring spokes add equal-and-opposite
 * angular spring torque and viscous drag. The station's silhouette is
 * therefore an emergent property of its spoke graph plus seeded drift:
 * adding/removing a producer or hopper visibly retorques the rings. */
void step_station_ring_dynamics(world_t *w, float dt);

/* Pairwise station jostling — when two stations crowd into each
 * other's "personal space" (dock_radius × STATION_PERSONAL_SPACE_FACTOR),
 * a soft repulsion nudges them apart. High drag settles them
 * within a couple seconds, no permanent oscillation. Slow enough
 * to feel like "stations finding their place" rather than physics. */
void step_station_jostle(world_t *w, float dt);
bool can_place_outpost(const world_t *w, vec2 pos);
void begin_module_construction(world_t *w, station_t *st, int station_idx, module_type_t type);
void begin_module_construction_at(world_t *w, station_t *st, int station_idx, module_type_t type, int ring, int slot);
/* Deliver build material from `ship` (player or NPC) into modules at
 * this station awaiting supply. `filter` restricts which commodity may
 * be consumed; pass COMMODITY_COUNT to allow any. The filter prevents
 * "deliver ingots only" from also draining frames into a half-built
 * module behind the player's back. */
/* Returns the credit owed to the ship for materials it donated to
 * AWAITING_SUPPLY scaffolds. Caller pays via ledger_earn / equivalent.
 * Materials drawn from station inventory (NPC-side restock) are NOT
 * counted — those were already paid for at intake. */
float step_module_delivery(world_t *w, station_t *st, int station_idx,
                           ship_t *ship, commodity_t filter);

/* Backfill every active station's manifest from its seeded float
 * inventory (RECIPE_LEGACY_MIGRATE units, deterministic per-station
 * origin). Idempotent: skips zero-inventory commodities and only adds
 * units the manifest doesn't already represent. world_reset leaves
 * manifests pristine so tests stay clean; this is the seed path that
 * both the dedicated server (after world_load fails) and the singleplayer
 * embedded server (after world_reset) call so the manifest-only TRADE
 * picker surfaces the seed stock. */
void world_seed_station_manifests(world_t *w);
int spawn_scaffold(world_t *w, module_type_t type, vec2 pos, int owner);
bool shipyard_hull_cost(hull_class_t hull_class, int *out_frames,
                        int *out_lasers, int *out_tractors);
bool shipyard_can_commission_hull(const station_t *st, hull_class_t hull_class);
bool shipyard_queue_ship_commission(world_t *w, int station_idx, int owner,
                                    hull_class_t hull_class);
ship_asset_t *world_ship_asset_by_id(world_t *w, uint32_t asset_id);
const ship_asset_t *world_ship_asset_by_id_const(const world_t *w, uint32_t asset_id);
ship_asset_t *world_ship_asset_mint(world_t *w, hull_class_t hull_class,
                                    ship_asset_owner_kind_t owner_kind,
                                    int owner_station, int custody_station,
                                    ship_asset_provenance_t provenance,
                                    bool loaner, int build_station,
                                    const uint8_t owner_pubkey[32],
                                    const uint8_t owner_session[8]);
int world_station_stored_hull_count(const world_t *w, int station_idx,
                                    hull_class_t hull_class);
void world_refresh_station_hull_inventories(world_t *w);
bool world_ship_asset_sync_from_player(world_t *w, server_player_t *sp);
bool world_ship_asset_sync_from_npc(world_t *w, int npc_slot);
bool world_player_release_ship_asset(world_t *w, int player_slot);
bool world_player_transfer_ship_state(world_t *w, int dst_slot, int src_slot);
bool ship_asset_claim_for_player(world_t *w, int player_slot, int station_idx);
int ship_asset_claim_for_npc(world_t *w, int station_idx, npc_role_t role);
bool shipyard_queue_station_hull_request(world_t *w, int requester_station,
                                         hull_class_t hull_class);
bool world_ship_assets_ensure_legacy_bindings(world_t *w);
int spawn_cargo_pod(world_t *w, vec2 pos, vec2 vel, commodity_t commodity,
                    uint16_t quantity, cargo_pod_kind_t kind);
int spawn_cargo_pod_with_manifest(world_t *w, vec2 pos, vec2 vel,
                                  commodity_t commodity,
                                  const cargo_unit_t *units,
                                  uint16_t unit_count,
                                  cargo_pod_kind_t kind);
int spawn_cargo_pod_with_manifest_deterministic(world_t *w, vec2 pos,
                                                vec2 vel,
                                                commodity_t commodity,
                                                const cargo_unit_t *units,
                                                uint16_t unit_count,
                                                cargo_pod_kind_t kind,
                                                float rotation,
                                                float spin);
int world_ensure_starter_frame_pods(world_t *w);
int world_ensure_starter_laser_module_reserve(world_t *w);
bool cargo_pod_has_exact_manifest(const cargo_pod_t *pod,
                                  commodity_t commodity);
void cargo_pod_set_shell_frame(cargo_pod_t *pod, const cargo_unit_t *frame);
bool cargo_pod_fold_shell_to_frame(cargo_pod_t *pod);
bool cargo_pod_take_manifest_unit(cargo_pod_t *pod, commodity_t commodity,
                                  cargo_unit_t *out_unit);
void step_station_cargo_pod_tractors(world_t *w, float dt);
int ship_tow_body_capacity(const ship_t *ship);
int ship_towed_body_count(const ship_t *ship);
int ship_tow_body_space(const ship_t *ship);
int ship_towed_pods_manifest_count(const world_t *w, const ship_t *ship,
                                   commodity_t commodity);
bool ship_towed_pods_take_manifest_unit(world_t *w, ship_t *ship,
                                        commodity_t commodity,
                                        cargo_unit_t *out_unit);
bool world_save(const world_t *w, const char *path);
bool world_load(world_t *w, const char *path);
/* v51 cargo-in-space schema migration: tag untagged furnaces by
 * station-furnace-count heuristic and auto-spawn missing output
 * hoppers in free outer-ring slots. Run automatically by world_load
 * for v50 saves; exposed so tests can exercise directly. Idempotent. */
void world_apply_cargo_schema_migration(world_t *w);
/* Station catalog — per-station identity persistence (sim_catalog.c) */
int  station_catalog_load_all(station_t *stations, int max, const char *dir);
bool station_catalog_save_all(const station_t *stations, int count, const char *dir);
bool player_save(const server_player_t *sp, const char *dir, int slot);
bool player_load(server_player_t *sp, world_t *w, const char *dir, int slot);
bool player_load_by_token(server_player_t *sp, world_t *w, const char *dir,
                          const uint8_t token[8]);
/* Layer A.4 of #479 — load a player save keyed by pubkey. Returns true
 * on hit. Searches <dir>/pubkey/<base58(pubkey)>.sav. */
bool player_load_by_pubkey(server_player_t *sp, world_t *w, const char *dir,
                           const uint8_t pubkey[32]);
/* Compute the on-disk save path for this player. See sim_save.c. */
bool player_save_path(char *out, size_t outlen, const char *dir,
                      const server_player_t *sp, int slot);
/* Migrate top-level *.sav files into <dir>/legacy/. Idempotent; safe to
 * call on every server start. Layer A.4 of #479. */
void player_save_migrate_legacy_layout(const char *dir);
/* Enumerate up to `cap` legacy saves under <dir>/legacy/. Each entry's
 * 8-char prefix and full base name (no .sav suffix) are written into
 * the parallel arrays. Returns the count. */
int player_save_list_legacy(const char *dir,
                            char prefixes[][9],
                            char names[][64],
                            int cap);
/* Rename <dir>/legacy/<basename>.sav -> <dir>/pubkey/<base58(pubkey)>.sav.
 * Refuses to clobber an existing pubkey save. Caller must verify any
 * authentication first. Returns true on success. */
bool player_save_rename_legacy_to_pubkey(const char *dir,
                                         const char *basename,
                                         const uint8_t pubkey[32]);
/* Append a durable audit row for a verified legacy-save claim attempt.
 * This does not prove original ownership of the legacy token; it records
 * which pubkey presented a valid claim signature and whether the rename won. */
bool player_save_audit_legacy_claim(const char *dir,
                                    const char *basename,
                                    const uint8_t pubkey[32],
                                    bool success,
                                    const char *reason);

/* Cross-module sim helpers — defined in game_sim.c, used by sim_*.c. */
void anchor_ship_in_station(server_player_t *sp, world_t *w);
asteroid_tier_t max_mineable_tier(int mining_level);
int mining_required_level_for_commodity(commodity_t commodity);
bool mining_level_can_fracture_asteroid(int mining_level, const asteroid_t *asteroid);
/* Station traffic waypoints:
 *   entry    = outside mouth of the outermost ring's open roadway
 *   approach = inner dock lane nearest the caller
 *   exit     = inner dock lane first when needed, then entry lane out
 */
vec2 station_entry_target(const station_t *st, vec2 from);
vec2 station_approach_target(const station_t *st, vec2 from);
vec2 station_exit_target(const station_t *st, vec2 from);
vec2 player_launch_lane_for_berth(const station_t *st, int dock_berth,
                                  int player_slot, vec2 away);
vec2 player_launch_clear_position(const world_t *w, int player_slot,
                                  const station_t *st, const ship_t *ship,
                                  vec2 away);
void emit_event(world_t *w, sim_event_t ev);
/* Station-local ledger economy */
float ledger_balance(const station_t *st, const uint8_t *token);
/* Net currency a station has issued, derived from the ledger as
 * -Σ(balance) over all entries. Replaces the old stored credit_pool
 * field; conservation is now structural. */
float station_credit_pool(const station_t *st);
void ledger_earn(station_t *st, const uint8_t *token, float amount);
void ledger_credit_supply(station_t *st, const uint8_t *token, float ore_value);
/* Like ledger_credit_supply but returns the actual amount credited
 * (post 35% smelt cut). Use when emitting +N popup events so they
 * reflect what the player actually got. */
float ledger_credit_supply_amount(station_t *st, const uint8_t *token, float ore_value);
/* Returns false if the player can't afford `amount` at this station;
 * otherwise debits the ledger and bumps the ship's stat_credits_spent. */
bool ledger_spend(station_t *st, const uint8_t *token, float amount, ship_t *ship);
/* Always-succeeds debit for unrefusable services (spawn, repair).
 * Allows the balance to go negative (debt). */
void ledger_force_debit(station_t *st, const uint8_t *token, float amount, ship_t *ship);
/* Full-price transfer from credit_pool to a ledger entry. Used by
 * NPC haulers (and any future caller) to pay the contract value at
 * delivery time, with no smelt cut applied. */
void ledger_earn_from_pool(station_t *st, const uint8_t *token, float amount);

/* ---- PubKey-based ledger API (#257 #479) ---- */
/* New ledger functions keyed by player pubkey (32B) instead of session
 * token (8B). Relationships survive session-token rotation. */
int ledger_find_or_create_by_pubkey(station_t *st, const uint8_t pubkey[32]);
float ledger_balance_by_pubkey(const station_t *st, const uint8_t pubkey[32]);
void ledger_sanitize_station(station_t *st);
void ledger_earn_by_pubkey(station_t *st, const uint8_t pubkey[32], float amount);
bool ledger_spend_by_pubkey(station_t *st, const uint8_t pubkey[32], float amount, ship_t *ship);
void ledger_force_debit_by_pubkey(station_t *st, const uint8_t pubkey[32], float amount, ship_t *ship);
void ledger_credit_supply_by_pubkey(station_t *st, const uint8_t pubkey[32], float ore_value);
/* Same as ledger_credit_supply_by_pubkey but returns the supplier-share
 * actually credited (after the 35% station cut). Use this when callers
 * need to emit accurate +N UI events. */
float ledger_credit_supply_amount_by_pubkey(station_t *st, const uint8_t pubkey[32], float ore_value);
void ledger_record_ore_sold(station_t *st, const uint8_t pubkey[32], uint32_t ore_units, uint8_t commodity);
void ledger_record_dock(station_t *st, const uint8_t pubkey[32], uint64_t tick);

/* Signal channel — station broadcast log (#316). */
uint64_t signal_channel_post(world_t *w, int sender_station, const char *text, const char *audio_url);
const signal_channel_msg_t *signal_channel_at(const world_t *w, int i);
void signal_chain_set_disk_enabled(bool enabled);

/* Replay the on-disk hash chain into the world's signal_channel ring
 * buffer at server boot. Idempotent — safe to call once after world
 * init. Reads chain dir entries written by signal_channel_post. */
void signal_chain_load(world_t *w);
/* Maps a producer commodity to the module type that fabricates it.
 * Returns MODULE_COUNT for raw ore / unknown inputs. Test-exposed; the
 * sim only calls it from shipyard_intake_rate. */
module_type_t producer_module_for_commodity(commodity_t c);
void player_seed_credits(server_player_t *sp, world_t *w);
void fracture_asteroid(world_t *w, int idx, vec2 outward_dir, int8_t fractured_by);
void activate_outpost(world_t *w, int station_idx);

#define DOCK_APPROACH_RANGE 300.0f /* range to detect station for docking */

/* Hopper/furnace constants — shared between game_sim.c and sim_production.c */
#define HOPPER_PULL_RANGE 300.0f    /* furnace attracts fragments from this far */
#define HOPPER_PULL_ACCEL 500.0f    /* base pull strength */
#define HOPPER_INTAKE_STAGING_RANGE 132.0f /* pod must be at the tagged intake mouth */

/* Cargo-pod module tractor tuning. Docks can retain sold/delivery custody
 * from farther out, but loose-pod acquisition still uses the shorter
 * CARGO_POD_DOCK_TRACTOR_RANGE at call sites. */
#define CARGO_POD_DOCK_TRACTOR_RANGE (HOPPER_PULL_RANGE * 1.65f)
#define CARGO_POD_DOCK_CUSTODY_RANGE (HOPPER_PULL_RANGE * 200.0f)
#define CARGO_POD_MODULE_TRACTOR_BEAM_INIT(range_value) { \
    .rest_length     = 0.0f, \
    .pull_strength   = 0.0f, \
    .push_strength   = 0.0f, \
    .pull_constant   = HOPPER_PULL_ACCEL * 3.60f, \
    .push_constant   = 0.0f, \
    .range           = (range_value), \
    .axial_damping   = 9.0f, \
    .tangent_damping = 3.6f, \
    .speed_cap       = 320.0f, \
    .falloff         = TRACTOR_FALLOFF_LINEAR, \
}

static inline bool cargo_pod_uses_dock_custody_range(const cargo_pod_t *pod) {
    return pod && pod->kind == CARGO_POD_CARGO && pod->towed_by < 0 &&
           (pod->manifest_count > 0 || pod->has_shell_frame);
}

static inline float cargo_pod_module_tractor_range(module_type_t module_type) {
    return module_type == MODULE_DOCK
        ? CARGO_POD_DOCK_TRACTOR_RANGE
        : HOPPER_PULL_RANGE;
}

static inline float cargo_pod_module_tractor_range_for_pod(
    module_type_t module_type,
    const cargo_pod_t *pod) {
    if (module_type == MODULE_DOCK &&
        cargo_pod_uses_dock_custody_range(pod)) {
        return CARGO_POD_DOCK_CUSTODY_RANGE;
    }
    return cargo_pod_module_tractor_range(module_type);
}

#endif /* GAME_SIM_H */
