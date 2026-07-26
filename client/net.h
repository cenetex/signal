/*
 * net.h — Multiplayer networking layer for Signal Space Miner.
 *
 * Provides WebSocket-based connectivity to the relay server.
 * Uses emscripten WebSocket API for WASM builds and mongoose's
 * WebSocket client for native builds.
 *
 * Binary protocol (little-endian):
 *   JOIN  (0x01): 1 type + 1 player_id
 *   LEAVE (0x02): 1 type + 1 player_id
 *   STATE (0x03): 45 bytes, optional authoritative ack/timestamp tail
 *   INPUT (0x04): 22 bytes, legacy-compatible prefix + seq + uint16 target + action id + input tick + client_sent_ms
 *   PROTOCOL_INFO (0x41): stream classes, record sizes, max counts, cadences
 *   ASTEROID_UPDATE (0x05): relay-only
 */
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stdint.h>
#include "cargo_receipt.h"
#include "handoff_ticket.h"
#include "types.h"      /* COMMODITY_COUNT */
#include "protocol.h"   /* shared protocol enums, message types, record sizes */

enum {
    NET_MAX_PLAYERS = 32,
};

#define NET_PLAYER_STATE_STATUS_ONLY 0x80u

typedef struct {
    uint8_t player_id;
    float x, y;
    float vx, vy;
    float angle;
    /* Flags bit layout:
     *   bit0 = thrust
     *   bit1 = beam_active (the beam is firing — may or may not have hit)
     *   bit2 = docked
     *   bit3 = scan_active
     *   bit4 = tractor_active
     *   bit5 = beam_ineffective (laser too weak for the target tier)
     *   bit6 = beam_hit (beam terminates on a target instead of empty space)
     *   bit7 = client-local status-only callback marker
     */
    uint8_t flags;
    uint8_t tractor_level;
    uint8_t towed_count;
    uint16_t towed_fragments[10]; /* asteroid indices, 0xFFFF = unused */
    char callsign[8];            /* e.g. "KRX-472" */
    /* Beam endpoints — server-authoritative. Used for both local and
     * remote player beam visuals. */
    float beam_start_x, beam_start_y;
    float beam_end_x, beam_end_y;
    uint16_t input_seq_ack;        /* last input seq the server accepted */
    uint32_t server_tick;          /* authoritative tick for this pose */
    uint32_t input_tick_ack;       /* tick where input_seq_ack was applied */
    uint32_t ack_client_sent_ms;   /* optional transport timestamp tail */
    uint32_t ack_server_recv_ms;
    uint32_t ack_server_send_ms;
    bool has_input_tick_ack;       /* authoritative receipt included input_tick_ack */
    bool active;
} NetPlayerState;

/* Packed asteroid state for world sync. */
typedef struct {
    uint16_t index;     /* asteroid slot 0-2047 */
    uint8_t flags;      /* bit0=active, bit1=fracture_child, bits2-4=tier(3), bits5-7=commodity(3) */
    float x, y;         /* position */
    float vx, vy;       /* velocity */
    float hp;           /* current HP */
    float ore;          /* ore amount (for TIER_S) */
    float radius;       /* radius */
    float smelt_progress; /* 0.0-1.0, decoded from uint8 trailer */
    uint8_t grade;        /* mining_grade_t — 0 = common, set on tractor */
    uint8_t crystal_stage; /* crystal_stage_t */
    uint8_t phase;         /* asteroid_phase_t */
} NetAsteroidState;

typedef struct {
    uint16_t index;     /* asteroid slot 0-2047 */
    float x, y;         /* position */
    float vx, vy;       /* velocity */
} NetAsteroidMotionState;

typedef struct {
    uint16_t index;       /* asteroid slot 0-2047 */
    float hp;             /* current HP */
    float ore;            /* ore amount (for TIER_S) */
    float radius;         /* radius */
    float smelt_progress; /* 0.0-1.0, decoded from uint8 trailer */
    uint8_t grade;        /* mining_grade_t */
    uint8_t crystal_stage; /* crystal_stage_t */
    uint8_t phase;         /* asteroid_phase_t */
} NetAsteroidStateQ;

/* Packed NPC state for world sync. */
typedef struct {
    uint8_t index;      /* NPC slot 0-15 */
    uint8_t flags;      /* bit0=active, bits1-2=role, bits3-5=state, bit6=thrusting */
    float x, y;         /* position */
    float vx, vy;       /* velocity */
    float angle;        /* facing */
    int target_asteroid;    /* mining target (-1 for none) */
    int towed_fragment;     /* towed fragment (-1 for none) */
    uint8_t tint_r, tint_g, tint_b; /* manifest rarity display tint */
    uint8_t session_token[8];       /* worker identity for custody labels */
    uint8_t home_station;           /* 0xFF = unknown */
} NetNpcState;

typedef struct {
    uint8_t index;      /* NPC slot 0-15 */
    uint8_t flags;      /* bit0=active, bit6=thrusting */
    float x, y;         /* position */
    float vx, vy;       /* velocity */
    float angle;        /* facing */
} NetNpcMotionState;

typedef struct {
    uint8_t index;      /* NPC slot 0-15 */
    float x, y;         /* position */
} NetNpcPosState;

typedef struct {
    uint8_t index;      /* NPC slot 0-15 */
    float x, y;         /* position */
    float angle;        /* facing */
} NetNpcPoseState;

typedef struct {
    uint8_t index;      /* NPC slot 0-15 */
    float x, y;         /* position */
    float vx, vy;       /* velocity */
} NetNpcLinearState;

typedef struct {
    uint8_t index;      /* NPC slot 0-15 */
    uint8_t flags;      /* bit0=active, bits1-2=role, bits3-5=state */
    int target_asteroid;
    int towed_fragment;
} NetNpcStatusState;

/* Callbacks — set these before calling net_init(). */
typedef void (*net_on_player_join_fn)(uint8_t player_id);
typedef void (*net_on_player_leave_fn)(uint8_t player_id);
typedef void (*net_on_player_state_fn)(const NetPlayerState* state);
typedef void (*net_on_input_applied_fn)(uint16_t input_seq,
                                        uint32_t server_tick,
                                        uint32_t input_tick_ack,
                                        uint32_t client_sent_ms,
                                        uint32_t server_recv_ms,
                                        uint32_t server_send_ms);
typedef void (*net_on_asteroids_fn)(const NetAsteroidState* asteroids, int count);
typedef void (*net_on_asteroid_motion_fn)(
    const NetAsteroidMotionState* asteroids, int count);
typedef void (*net_on_asteroid_state_q_fn)(
    const NetAsteroidStateQ* asteroids, int count);
typedef void (*net_on_npcs_fn)(const NetNpcState* npcs, int count);
typedef void (*net_on_npc_motion_fn)(const NetNpcMotionState* npcs, int count);
typedef void (*net_on_npc_pos_fn)(const NetNpcPosState* npcs, int count);
typedef void (*net_on_npc_pose_fn)(const NetNpcPoseState* npcs, int count);
typedef void (*net_on_npc_linear_fn)(const NetNpcLinearState* npcs, int count);
typedef void (*net_on_npc_status_fn)(const NetNpcStatusState* npcs, int count);
/* Packed player ship state (from PLAYER_SHIP 0x15). */
typedef struct {
    uint8_t player_id;
    float hull;
    float station_balance;
    bool docked;
    uint8_t current_station;
    uint8_t mining_level;
    uint8_t hold_level;
    uint8_t tractor_level;
    uint8_t autopilot_mode; /* 0 = off, 1 = mining loop */
    float cargo[COMMODITY_COUNT];
    uint8_t nearby_fragments;
    uint8_t tractor_fragments;
    uint8_t towed_count;
    uint16_t towed_fragments[10]; /* asteroid indices, 0xFFFF = unused */
    uint8_t autopilot_target;    /* asteroid index, 0xFF = none */
    uint8_t path_count;          /* A* path waypoint count (0-12) */
    uint8_t path_current;        /* current waypoint index */
    float path_x[12];           /* waypoint X coords */
    float path_y[12];           /* waypoint Y coords */
} NetPlayerShipState;

typedef void (*net_on_player_ship_fn)(const NetPlayerShipState* state);

/* Station update callback: index + full inventory[COMMODITY_COUNT] + credit pool. */
typedef void (*net_on_stations_fn)(uint8_t index, const float* inventory, float credit_pool);

/* Contracts callback: full replacement of contract array. */
typedef void (*net_on_contracts_fn)(const contract_t* contracts, int count);

/* Per-player gossip-contract visibility mask. Bit i set iff compact
 * NET_MSG_CONTRACTS record i matches a summary in this player's
 * known_contracts. The dock UI uses this to hide contracts the player
 * hasn't heard about via dock contact. */
typedef void (*net_on_player_known_contracts_fn)(uint32_t mask);

typedef struct {
    uint8_t station;
    float balance;
} NetKnownLedgerEntry;

typedef void (*net_on_player_known_ledger_fn)(
    const NetKnownLedgerEntry *entries, int count);

typedef struct {
    market_memory_t memory;
    uint8_t hops;
} NetMarketMemoryEntry;

typedef void (*net_on_player_market_memories_fn)(
    const NetMarketMemoryEntry *entries, int count);

typedef struct {
    uint16_t shipment_id;
    uint8_t status;
    uint8_t origin_station;
    uint8_t destination_station;
    uint8_t contract_index;
    uint8_t commodity;
    uint16_t quantity_total;
    uint16_t quantity_delivered;
    uint16_t quantity_bound;
    float debt_principal;
    float destination_payout;
    float origin_completion_credit;
    uint32_t due_tick;
    uint16_t held_bound;
} NetDeliveryLedgerEntry;

typedef void (*net_on_delivery_ledger_fn)(
    const NetDeliveryLedgerEntry *entries, int count);

/* Packed station identity for network sync.
 * flags: bit0=scaffold, bit1=planned. */
typedef struct {
    uint8_t index;
    uint8_t flags;
    uint32_t services;
    float pos_x, pos_y;
    float radius, dock_radius, signal_range;
    char name[32];
    float base_price[COMMODITY_COUNT];
    float scaffold_progress;
    int module_count;
    station_module_t modules[MAX_MODULES_PER_STATION];
    int arm_count;
    float arm_speed[MAX_ARMS];
    float ring_offset[MAX_ARMS];
    float arm_rotation[MAX_ARMS];
    float arm_omega[MAX_ARMS];
    int plan_count;
    struct {
        module_type_t type;
        uint8_t ring;
        uint8_t slot;
        int8_t owner;
    } plans[STATION_PLAN_RECORD_COUNT];
    int pending_scaffold_count;
    struct {
        module_type_t type;
        int8_t owner;
    } pending_scaffolds[STATION_PENDING_SCAFFOLD_RECORD_COUNT];
    int pending_ship_build_count;
    struct {
        hull_class_t hull_class;
        int8_t owner;
        float build_progress;
    } pending_ship_builds[STATION_PENDING_SHIP_RECORD_COUNT];
    uint8_t stored_hull_count[HULL_CLASS_COUNT];
    char hail_message[STATION_IDENTITY_HAIL_MESSAGE_LEN];
    char miner_chatter[STATION_IDENTITY_CHATTER_LINES][STATION_IDENTITY_CHATTER_LINE_LEN];
    char hauler_chatter[STATION_IDENTITY_CHATTER_LINES][STATION_IDENTITY_CHATTER_LINE_LEN];
    char rati_hail_message[STATION_IDENTITY_RATI_HAIL_LEN];
    char currency_name[32];   /* station-local scrip label, empty = "credits" */
    uint8_t station_pubkey[32]; /* Ed25519 identity (#479 B). Matching secret
                                 * stays server-side and is never wired here. */
    uint8_t faction_id;
    uint8_t faction_allegiance;
    uint8_t faction_ideology;
    int8_t faction_relations[STATION_FACTION_COUNT];
    uint8_t policy_card_count;
    uint8_t policy_card_ids[STATION_IDENTITY_POLICY_CARD_COUNT];
} NetStationIdentity;

/* Station diagnostics callback: compact live flow state for modules. */
typedef void (*net_on_station_diag_fn)(uint8_t station_id,
                                       const uint8_t *diag,
                                       int module_count);
/* Packed scaffold state — server pushes the active scaffold pool.
 * Mirrors enough of scaffold_t for client rendering + tow logic. */
typedef struct {
    uint8_t index;
    uint8_t state;        /* scaffold_state_t enum */
    uint8_t module_type;
    int8_t  owner;        /* -1 for NPC-produced */
    float   pos_x, pos_y;
    float   vel_x, vel_y;
    float   radius;
    float   build_amount;
    int16_t built_at_station; /* decoded u8; -1 means no source station */
} NetScaffoldState;
typedef struct {
    uint8_t index;
    float pos_x, pos_y;
    float vel_x, vel_y;
} NetScaffoldMotionState;

/* Station identity callback: full static fields for a station slot. */
typedef void (*net_on_station_identity_fn)(const NetStationIdentity* station);
/* Scaffold pool snapshot callback. */
typedef void (*net_on_scaffolds_fn)(const NetScaffoldState* scaffolds, int count);
typedef void (*net_on_scaffold_remove_fn)(const uint8_t* indices, int count);
typedef void (*net_on_scaffold_motion_fn)(
    const NetScaffoldMotionState* scaffolds, int count);

typedef struct {
    uint8_t index;
    uint8_t kind;       /* cargo_pod_kind_t */
    uint8_t commodity;  /* commodity_t */
    int8_t tractor_player; /* legacy wire byte projected into a typed binding */
    float pos_x, pos_y;
    float vel_x, vel_y;
    float radius;
    float rotation;
    uint16_t quantity;
    uint16_t manifest_count;
    uint16_t shipment_id;
    uint8_t summary_flags;
    uint8_t summary_grade;
    uint8_t tractor_station;
    uint8_t tractor_module;
    uint8_t tow_hardpoint_tag;
} NetCargoPodState;
typedef struct {
    uint8_t index;
    float pos_x, pos_y;
    float vel_x, vel_y;
    float rotation;
} NetCargoPodMotionState;
typedef struct {
    uint8_t index;
    float pos_x, pos_y;
    float vel_x, vel_y;
} NetCargoPodLinearState;
typedef void (*net_on_cargo_pods_fn)(const NetCargoPodState* pods, int count);
typedef void (*net_on_cargo_pod_remove_fn)(const uint8_t* indices, int count);
typedef void (*net_on_cargo_pod_motion_fn)(const NetCargoPodMotionState* pods,
                                           int count);
typedef void (*net_on_cargo_pod_linear_fn)(const NetCargoPodLinearState* pods,
                                           int count);
typedef struct {
    uint8_t index;
    float source_x, source_y;
    float target_x, target_y;
    float range;
    float intensity;
} NetInteractionDriftState;
typedef void (*net_on_interactions_fn)(const sim_interaction_t *items, int count);
typedef void (*net_on_interaction_drift_fn)(const NetInteractionDriftState *items,
                                            int count);

typedef struct {
    uint32_t flags;
    float signal_quality;
    uint8_t candidate_count;
    uint8_t mode;
    uint64_t source_id;
} NetHailReason;

/* Hail response callback: server confirmed payout from a hail attempt. */
typedef void (*net_on_hail_response_fn)(uint8_t station,
                                        float credits,
                                        int contract_index,
                                        const NetHailReason *reason);

/* Signal-channel wire record (#316). audio_url isn't on the wire in V1;
 * agents reach it via the REST endpoint. entry_hash carries the SHA-256
 * chain link so clients can verify continuity locally. */
typedef struct {
    uint64_t id;
    uint32_t timestamp_ms;
    int16_t  sender_station;  /* -1 = system */
    char     text[SIGNAL_CHANNEL_TEXT_MAX];
    uint8_t  entry_hash[32];
} NetSignalChannelMsg;

typedef void (*net_on_signal_channel_fn)(const NetSignalChannelMsg *msgs, int count);

/* Compact manifest summary row. The same callback also receives canonical
 * cargo-unit details for individually addressable provenance rows. */
typedef struct {
    uint8_t  commodity;
    uint8_t  grade;
    uint16_t count;
} NetManifestSummaryEntry;
typedef void (*net_on_station_manifest_fn)(uint8_t station_id,
                                           const NetManifestSummaryEntry *summary,
                                           int summary_count,
                                           const cargo_unit_t *details,
                                           int detail_count);

/* Player manifest shape is the same, scoped to the implicit local pilot. */
typedef void (*net_on_player_manifest_fn)(const NetManifestSummaryEntry *summary,
                                          int summary_count,
                                          const cargo_unit_t *details,
                                          int detail_count);
typedef void (*net_on_cargo_receipt_bundle_fn)(const cargo_receipt_t *receipts,
                                               int count);

/* Laser/scan inspect snapshot. target_type mirrors the scan target
 * values in server_player_t: 0 none, 1 station/module, 2 NPC, 3 player.
 * For NPC/player ship targets, rows project the scanned manifest into
 * cargo-unit identity and portable receipt-chain heads. Quantity is
 * greater than 1 for grouped anonymous ingot and finished-good buckets. */
typedef struct {
    uint8_t  commodity;
    uint8_t  grade;
    uint8_t  chain_len;
    uint8_t  flags;
    uint64_t event_id;
    uint16_t quantity;
    uint8_t  cargo_pub[32];
    uint8_t  receipt_head[32];
    uint8_t  origin_station[32];
    uint8_t  latest_station[32];
} NetInspectSnapshotRow;

typedef struct {
    uint8_t  target_type;
    uint8_t  target_index;
    uint8_t  module_index;     /* 0xFF = none */
    uint8_t  role;             /* npc_role_t for NPC, hull_class_t for player */
    uint8_t  state;            /* npc_state_t for NPC, rounded hull for player */
    uint8_t  home_station;     /* 0xFF = unknown */
    uint8_t  dest_station;     /* 0xFF = unknown */
    uint16_t manifest_count;
    int      row_count;
    NetInspectSnapshotRow rows[INSPECT_SNAPSHOT_MAX_ROWS];
} NetInspectSnapshot;

typedef void (*net_on_inspect_snapshot_fn)(const NetInspectSnapshot *snapshot);

/* Global leaderboard — top-N death runs by credits earned. */
typedef struct {
    char     callsign[8];    /* not NUL-terminated if 8 chars */
    float    credits_earned;
    uint32_t world_id;       /* low 32 bits of belt_seed (or station-pub-derived for orphans) */
    uint32_t world_seq;      /* monotonic world id; higher = newer */
    uint32_t build_id;       /* low 32 bits of build SHA at run time */
    uint64_t epoch_tick;     /* sim tick at death */
    uint8_t  killed_by[8];   /* killer callsign, all-zero if unresolved */
} NetHighscoreEntry;
typedef void (*net_on_highscores_fn)(const NetHighscoreEntry *entries, int count);

typedef void (*net_on_players_begin_fn)(void);
typedef void (*net_on_action_ack_fn)(uint16_t action_id, uint16_t input_seq,
                                     uint8_t status, uint8_t action);
typedef void (*net_on_action_result_fn)(uint16_t action_id, uint16_t input_seq,
                                        uint8_t status, uint8_t action,
                                        uint32_t server_tick);
typedef void (*net_on_latency_sample_fn)(uint32_t seq, float rtt_ms,
                                         float server_turnaround_ms,
                                         uint32_t server_tick);
typedef void (*net_on_handoff_ticket_fn)(uint8_t status,
                                         uint8_t source_station,
                                         uint8_t dest_station,
                                         const handoff_ticket_t *ticket);
typedef void (*net_on_handoff_result_fn)(uint8_t status, uint8_t reason,
                                         uint8_t dest_station,
                                         const uint8_t ticket_hash[32]);

typedef bool (*net_loopback_send_fn)(const uint8_t *data, int len, void *user);

typedef struct {
    uint8_t msg_type;
    uint8_t stream_class;
    uint16_t flags;
    uint16_t header_size;
    uint16_t record_size;
    uint16_t max_records;
    uint16_t cadence_ms;
} NetProtocolStreamInfo;

typedef struct {
    uint16_t version;
    uint32_t capabilities;
    int stream_count;
    NetProtocolStreamInfo streams[PROTOCOL_INFO_STREAM_CAPACITY];
} NetProtocolInfo;

typedef void (*net_on_protocol_info_fn)(const NetProtocolInfo *info);

typedef struct {
    net_on_player_join_fn on_join;
    net_on_player_leave_fn on_leave;
    net_on_player_state_fn on_state;
    net_on_input_applied_fn on_input_applied;
    net_on_players_begin_fn on_players_begin;
    net_on_asteroids_fn on_asteroids;
    net_on_asteroid_motion_fn on_asteroid_motion;
    net_on_asteroid_state_q_fn on_asteroid_state_q;
    net_on_npcs_fn on_npcs;
    net_on_npc_motion_fn on_npc_motion;
    net_on_npc_pos_fn on_npc_pos;
    net_on_npc_pose_fn on_npc_pose;
    net_on_npc_linear_fn on_npc_linear;
    net_on_npc_status_fn on_npc_status;
    net_on_stations_fn on_stations;
    net_on_station_identity_fn on_station_identity;
    net_on_station_diag_fn on_station_diag;
    net_on_scaffolds_fn on_scaffolds;
    net_on_scaffold_remove_fn on_scaffold_remove;
    net_on_scaffold_motion_fn on_scaffold_motion;
    net_on_cargo_pods_fn on_cargo_pods;
    net_on_cargo_pod_remove_fn on_cargo_pod_remove;
    net_on_cargo_pod_motion_fn on_cargo_pod_motion;
    net_on_cargo_pod_linear_fn on_cargo_pod_linear;
    net_on_interactions_fn on_interactions;
    net_on_interaction_drift_fn on_interaction_drift;
    net_on_hail_response_fn on_hail_response;
    net_on_player_ship_fn on_player_ship;
    net_on_contracts_fn on_contracts;
    net_on_player_known_contracts_fn on_player_known_contracts;
    net_on_player_market_memories_fn on_player_market_memories;
    net_on_player_known_ledger_fn on_player_known_ledger;
    net_on_delivery_ledger_fn on_delivery_ledger;
    void (*on_death)(uint8_t player_id, float pos_x, float pos_y,
                     float vel_x, float vel_y, float angle,
                     float ore_mined, float credits_earned, float credits_spent,
                     int asteroids_fractured,
                     uint8_t respawn_station, float respawn_fee);
    void (*on_world_time)(float server_time);
    void (*on_events)(const sim_event_t *events, int count);
    net_on_signal_channel_fn on_signal_channel;
    net_on_station_manifest_fn on_station_manifest;
    net_on_player_manifest_fn  on_player_manifest;
    net_on_cargo_receipt_bundle_fn on_cargo_receipt_bundle;
    net_on_inspect_snapshot_fn on_inspect_snapshot;
    net_on_highscores_fn       on_highscores;
    net_on_action_ack_fn       on_action_ack;
    net_on_action_result_fn    on_action_result;
    net_on_latency_sample_fn   on_latency_sample;
    net_on_protocol_info_fn    on_protocol_info;
    net_on_handoff_ticket_fn   on_handoff_ticket;
    net_on_handoff_result_fn   on_handoff_result;
} NetCallbacks;

/* Initialize networking and connect to the relay server.
 * url: WebSocket URL, e.g. "ws://localhost:8080/ws"
 * Returns true if connection was initiated. */
bool net_init(const char* url, const NetCallbacks* callbacks);

/* Initialize the normal net decoder/callback stack without opening a socket.
 * Client-to-server packets are delivered to the callback registered with
 * net_set_loopback_send(); server-to-client packets should be fed back through
 * net_loopback_receive(). */
bool net_init_loopback(const NetCallbacks* callbacks, uint8_t local_id);
void net_set_loopback_send(net_loopback_send_fn send_fn, void *user);
bool net_is_loopback(void);
void net_loopback_receive(const uint8_t *data, int len);

/* Reconnect to the same server using stored URL + session token. */
bool net_reconnect(void);

/* Last protocol discovery packet received from the server, or NULL until a
 * server sends NET_MSG_PROTOCOL_INFO. */
const NetProtocolInfo *net_protocol_info(void);

/* Shut down the connection and free resources. */
void net_shutdown(void);

/* Layer A.2 of #479 — install the persistent Ed25519 pubkey to be
 * advertised to the server in NET_MSG_REGISTER_PUBKEY on every
 * (re)connect. Call this BEFORE net_init so the very first WS open
 * fires the registration message. Pass NULL to clear. */
void net_set_identity_pubkey(const uint8_t pubkey[32]);

/* Returns true if a local identity pubkey is available for registration. */
bool net_has_identity_pubkey(void);

/* Layer A.3 of #479 — install the player's Ed25519 secret key so the
 * client can sign state-changing actions before sending them on the
 * NET_MSG_SIGNED_ACTION channel. Pass NULL to clear (e.g. ephemeral
 * fallback identity, where signing is unavailable).
 *
 * The secret never leaves the client; the server only ever sees
 * signatures + pubkey. */
void net_set_identity_secret(const uint8_t secret[64]);

/* Clear the long-lived in-memory identity copy during final client teardown. */
void net_clear_identity(void);

/* Send a signed state-changing action.
 *
 * Returns true if the message was queued onto the wire; false if the
 * client lacks an installed secret or the payload exceeds
 * SIGNED_ACTION_MAX_PAYLOAD.
 *
 * Nonce is chosen internally — monotonic across the process lifetime.
 * The first signed action after process start uses the wall clock time
 * in microseconds; later actions strictly exceed every prior one. */
bool net_send_signed_action(uint8_t action_type,
                            const uint8_t *payload, uint16_t payload_len);

/* Returns true if a secret is installed and signed actions can be sent. */
bool net_has_identity_secret(void);

/* Layer A.4 of #479 — claim a legacy (token-keyed) save by signing the
 * domain-separated token name with the persistent identity. Returns true
 * if the message was queued. `token_basename` is the legacy save's base
 * name without the .sav suffix (as advertised in NET_MSG_LEGACY_SAVES_
 * AVAILABLE). The server verifies the signature, then renames the
 * legacy save to the pubkey-keyed path and loads it.
 *
 * UI integration is intentionally minimal for now — operators can
 * trigger this manually for stranded players; a docked-UI flow is a
 * follow-up issue. */
bool net_send_claim_legacy_save(const char *token_basename);

/* Send an app-level ping probe. The server immediately echoes it via
 * NET_MSG_LATENCY_PONG; the callback reports raw transport RTT, separate
 * from authoritative input-ack age. */
void net_send_latency_ping(void);

/* Periodically report client-observed network/runtime metrics to the relay.
 * The server writes structured analytics logs from this packet, using its
 * own pseudonymous user key; no raw token or public key is sent here. */
void net_send_client_metrics(uint32_t seq,
                             float ping_rtt_ms,
                             float ack_ms,
                             float ack_gap_ms,
                             float server_turnaround_ms,
                             float player_interval_ms,
                             uint16_t unacked_inputs,
                             uint16_t replay_depth,
                             uint8_t action_queue_depth,
                             uint8_t recovery_flags);

/* Send the local player's input state to the server.
 * flags: bitmask of NET_INPUT_* values.
 * action: station interaction (0=none, 1=dock, 2=launch, etc.)
 * mining_target: client's hover_asteroid index (0xFFFF=none)
 * input_tick: client-predicted sim tick this movement should affect */
/* `buy_grade` is the 5th byte of the input msg — only meaningful when
 * `action` is in the NET_ACTION_BUY_PRODUCT range. Pass MINING_GRADE_COUNT
 * (5) to mean "any grade, FIFO"; the server parser defaults to that when
 * the byte is missing (older clients). */
/* place_station/ring/slot ride along when action is
 * NET_ACTION_PLACE_OUTPOST. Pass -1 for "let the server auto-snap"
 * (the relay-founding path). For module scaffolds the client picks
 * a (station, ring, slot) via the placement reticle and the server
 * snaps to that explicit slot. Older clients only sent 5 bytes; the
 * server treats missing bytes as -1. */
uint32_t net_send_input(uint8_t flags, uint8_t action, uint16_t input_seq,
                        uint16_t mining_target,
                        uint8_t buy_grade, int8_t place_station,
                        int8_t place_ring, int8_t place_slot,
                        uint16_t action_id, uint32_t input_tick);

/* Present a carried cargo receipt chain to the current authority. The
 * multiplayer client sends these immediately before queued sell/deliver
 * actions for matching carried units. Handoff presentation now carries the
 * same receipt chains inside the signed ship snapshot. The server attaches the
 * chain to the matching carried cargo unit if it verifies. */
void net_send_present_receipt_chain(const uint8_t cargo_pub[32],
                                    const cargo_receipt_chain_t *chain);

void net_send_handoff_request(uint8_t source_station, uint8_t dest_station,
                              uint32_t ttl_ticks);
void net_send_handoff_present(const handoff_ticket_t *ticket,
                              const ship_t *ship);

/* Send a planning intent (outpost create / module slot / cancel).
 * Returns false when an identity-backed client cannot use the signed
 * planning channel and the request is blocked instead of downgraded. */
bool net_send_plan(uint8_t op, int8_t station, int8_t ring, int8_t slot,
                   uint8_t module_type, float px, float py);

/* Process incoming messages. Call once per frame. */
void net_poll(void);

/* Returns true if connected to the relay server. */
bool net_is_connected(void);

/* Returns the local player's assigned ID, or 0xFF if not assigned. */
uint8_t net_local_id(void);
const char* net_local_callsign(void);

/* Access remote player state array (NET_MAX_PLAYERS entries). */
const NetPlayerState* net_get_players(void);

/* Returns the server's git hash (empty string if not received). */
const char* net_server_hash(void);

#endif /* NET_H */
