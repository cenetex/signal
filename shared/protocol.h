/*
 * shared/net_protocol.h — Single source of truth for the Signal Space Miner
 * binary wire protocol.  Included by both the client (client/net.h) and the
 * authoritative server (server/net_protocol.h).
 *
 * Packet layouts (little-endian):
 *   JOIN            (0x01): [type:1][player_id:1]
 *   LEAVE           (0x02): [type:1][player_id:1]
 *   STATE           (0x03): [type:1][id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1][tractor_lvl:1][towed_count:1][towed_frags:20]
 *                         optional authoritative tail:
 *                         [input_ack:u16][server_tick:u32][input_tick_ack:u32]
 *                         [client_sent_ms:u32][server_recv_ms:u32][server_send_ms:u32]
 *   INPUT           (0x04): legacy 4-18 bytes, current 22 bytes with seq + uint16 target + action id + input tick + client_sent_ms
 *   LATENCY_PING    (0x3C): [type:1][seq:u32][client_sent_ms:u32]
 *   LATENCY_PONG    (0x3D): [type:1][seq:u32][client_sent_ms:u32][server_recv_ms:u32][server_send_ms:u32][server_tick:u32]
 *   ACTION_ACK      (0x3A): [type:1][action_id:u16][input_seq:u16][status:1][action:1]
 *   ACTION_RESULT   (0x3B): [type:1][action_id:u16][input_seq:u16][status:1][action:1][server_tick:u32]
 *   INPUT_APPLIED   (0x48): [type:1][input_seq:u16][server_tick:u32][input_tick_ack:u32][client_sent_ms:u32][server_recv_ms:u32][server_send_ms:u32]
 *   WORLD_ASTEROIDS (0x10): [type:1][count:u16] + count * ASTEROID_RECORD_SIZE records
 *   WORLD_ASTEROIDS_Q (0x68): [type:1][count:u16] + count * ASTEROID_Q_RECORD_SIZE fixed-point records
 *   WORLD_ASTEROIDS8_Q (0x69): [type:1][count:1] + count * ASTEROID8_Q_RECORD_SIZE fixed-point records
 *   WORLD_ASTEROID_MOTION (0x4B): [type:1][count:u16] + count * ASTEROID_MOTION_RECORD_SIZE records
 *   WORLD_ASTEROID_MOTION_Q (0x4C): [type:1][count:u16] + count * ASTEROID_MOTION_Q_RECORD_SIZE records
 *   WORLD_ASTEROID_POS_Q (0x55): [type:1][count:u16] + count * ASTEROID_POS_Q_RECORD_SIZE records
 *   WORLD_ASTEROID_POS8_Q (0x5C): [type:1][count:1] + count * ASTEROID_POS8_Q_RECORD_SIZE records
 *   WORLD_ASTEROID_POSD_Q (0x66): [type:1][count:u16] + count * ASTEROID_POSD_Q_RECORD_SIZE records
 *   WORLD_ASTEROID_POSD8_Q (0x67): [type:1][count:1] + count * ASTEROID_POSD8_Q_RECORD_SIZE records
 *   WORLD_ASTEROID_STATE_Q (0x51): [type:1][count:u16] + count * ASTEROID_STATE_Q_RECORD_SIZE records
 *   WORLD_ASTEROID_REMOVE (0x56): [type:1][count:u16] + count * ASTEROID_REMOVE_RECORD_SIZE records
 *   WORLD_NPCS      (0x11): [type:1][count:1] + count * NPC_RECORD_SIZE records
 *   WORLD_NPC_MOTION (0x4E): [type:1][count:1] + count * NPC_MOTION_RECORD_SIZE records
 *   WORLD_NPC_MOTION_Q (0x52): [type:1][count:1] + count * NPC_MOTION_Q_RECORD_SIZE records
 *   WORLD_NPC_POS_Q (0x5A): [type:1][count:1] + count * NPC_POS_Q_RECORD_SIZE records
 *   WORLD_NPC_POSE_Q (0x5B): [type:1][count:1] + count * NPC_POSE_Q_RECORD_SIZE records
 *   WORLD_NPC_STATUS (0x53): [type:1][count:1] + count * NPC_STATUS_RECORD_SIZE records
 *   WORLD_NPC_STATUS8_Q (0x5D): [type:1][count:1] + count * NPC_STATUS8_RECORD_SIZE records
 *   WORLD_NPC_LINEAR_Q (0x5E): [type:1][count:1] + count * NPC_LINEAR_Q_RECORD_SIZE records
 *   WORLD_NPC_MOTION8_Q (0x60): [type:1][count:1] + count * NPC_MOTION8_Q_RECORD_SIZE records
 *   WORLD_CARGO_POD_MOTION (0x4F): [type:1][count:1] + count * CARGO_POD_MOTION_RECORD_SIZE records
 *   WORLD_CARGO_POD_MOTION_Q (0x54): [type:1][count:1] + count * CARGO_POD_MOTION_Q_RECORD_SIZE records
 *   WORLD_CARGO_POD_REMOVE (0x57): [type:1][count:1] + count * CARGO_POD_REMOVE_RECORD_SIZE records
 *   WORLD_CARGO_POD_LINEAR_Q (0x5F): [type:1][count:1] + count * CARGO_POD_LINEAR_Q_RECORD_SIZE records
 *   WORLD_CARGO_PODS_Q (0x62): [type:1][count:1] + count * CARGO_POD_Q_RECORD_SIZE records
 *   STATION_IDENTITY_Q (0x63): compact variable-length station identity
 *   WORLD_SCAFFOLD_MOTION_Q (0x59): [type:1][count:1] + count * SCAFFOLD_MOTION_Q_RECORD_SIZE records
 *   WORLD_INTERACTION_DRIFT (0x50): [type:1][count:1] + count * INTERACTION_DRIFT_RECORD_SIZE records
 *   WORLD_INTERACTIONS_Q (0x61): [type:1][count:1] + count * INTERACTION_Q_RECORD_SIZE records
 *   WORLD_STATIONS  (0x12): [type:1][count:1] + count * STATION_RECORD_SIZE records
 *   PLAYER_SHIP     (0x15): [type:1][id:1] + ship cargo/hull/credits/levels
 *   SERVER_INFO     (0x16): [type:1][hash:up to 11]
 *   PROTOCOL_INFO   (0x41): stream capability + record-size discovery
 *   HANDOFF_REQUEST (0x42): [type:1][source_station:1][dest_station:1][ttl_ticks:u32]
 *   HANDOFF_TICKET  (0x43): [type:1][status:1][source_station:1][dest_station:1][handoff_ticket_t]
 *   HANDOFF_PRESENT (0x44): [type:1][handoff_ticket_t][snapshot_len:u32][snapshot]
 *   HANDOFF_RESULT  (0x45): [type:1][status:1][reason:1][dest_station:1][ticket_hash:32]
 *   STATION_IDENTITY(0x17): [type:1][index:1][reserved:1][services:4][pos:2xf32][radius:f32][dock_radius:f32][signal_range:f32][name:32] + fixed structural trailers
 *   WORLD_PLAYERS   (0x18): [type:1][count:1] + count * PLAYER_RECORD_SIZE records
 *                         per-recipient batches may omit the recipient's own
 *                         record; local authoritative baselines ride STATE
 *   WORLD_PLAYER_MOTION (0x4D): [type:1][count:1] + count * PLAYER_MOTION_RECORD_SIZE records
 *   WORLD_PLAYER_MOTION_Q (0x6A): [type:1][count:1] + count * PLAYER_MOTION_Q_RECORD_SIZE records
 *   WORLD_PLAYER_DOCK_Q (0x6B): [type:1][count:1] + count * PLAYER_DOCK_RECORD_SIZE records
 *   WORLD_PLAYER_MOTIOND_Q (0x6C): [type:1][count:1] + count * PLAYER_MOTIOND_Q_RECORD_SIZE records
 *   WORLD_PLAYER_POSED_Q (0x6D): [type:1][count:1] + count * PLAYER_POSED_Q_RECORD_SIZE records
 *   WORLD_PLAYER_MOTIONM_Q (0x6E): [type:1][count:1] + mixed player delta records
 *   WORLD_TOW_LINKS (0x71): atomic revisioned replacement of live tow relations
 *   STATION_DIAG    (0x40): [type:1][index:1][module_count:1][diag:MAX_MODULES_PER_STATION×u8]
 *   HAIL_RESPONSE   (0x25): [type:1][station:1][credits:f32][contract:1]
 *                         optional reason tail:
 *                         [flags:u32][signal_quality:f32][candidate_count:1]
 *                         [mode:1][source_id:u64]
 */
#ifndef SHARED_PROTOCOL_H
#define SHARED_PROTOCOL_H

#include "types.h"  /* MODULE_COUNT, COMMODITY_COUNT, MAX_ASTEROIDS, etc. */

/* ------------------------------------------------------------------ */
/* Message types                                                      */
/* ------------------------------------------------------------------ */

enum {
    NET_MSG_JOIN            = 0x01,
    NET_MSG_LEAVE           = 0x02,
    NET_MSG_STATE           = 0x03,
    NET_MSG_INPUT           = 0x04,
    NET_MSG_WORLD_ASTEROIDS = 0x10,
    NET_MSG_WORLD_NPCS      = 0x11,
    NET_MSG_WORLD_STATIONS  = 0x12,
    NET_MSG_WORLD_STATIONS_Q = 0x65, /* server -> client. Sparse station
                                      * economy summary; same semantics as
                                      * WORLD_STATIONS without zero inventory
                                      * padding. */
    NET_MSG_MINING_ACTION   = 0x13,
    NET_MSG_HOST_ASSIGN     = 0x14,
    NET_MSG_PLAYER_SHIP     = 0x15,
    NET_MSG_SERVER_INFO     = 0x16,
    NET_MSG_STATION_IDENTITY= 0x17,
    NET_MSG_STATION_IDENTITY_Q = 0x63, /* server -> client. Compact variable
                                        * station identity. Same semantic
                                        * fields as STATION_IDENTITY, but
                                        * fixed-capacity string/list trailers
                                        * are length-prefixed and sparse. */
    NET_MSG_WORLD_PLAYERS   = 0x18,
    NET_MSG_CONTRACTS       = 0x19,
    NET_MSG_CONTRACTS_Q     = 0x64, /* server -> client. Sparse compact
                                     * contract snapshot. Same semantic
                                     * fields/order as CONTRACTS, with the
                                     * provenance tails present only when
                                     * nonzero. */
    NET_MSG_SESSION         = 0x20, /* client -> server:
                                    * [type:1][token:8][callsign:7]
                                    * [protocol_version:u16] */
    NET_MSG_DEATH           = 0x21, /* server -> client: [type:1][player_id:1] */
    NET_MSG_WORLD_TIME      = 0x22, /* server -> client: [type:1][time:f32].
                                     * Low-rate reconciliation; clients advance
                                     * world time locally between samples. */
    NET_MSG_PLAN            = 0x23, /* client -> server: outpost planning intents */
    NET_MSG_WORLD_SCAFFOLDS = 0x24, /* server -> client: scaffold identity/build upserts */
    NET_MSG_HAIL_RESPONSE   = 0x25, /* server -> client: station hail/balance response */
    NET_MSG_EVENTS          = 0x26, /* server -> client: sim event batch */
    NET_MSG_SIGNAL_CHANNEL  = 0x27, /* server -> client: broadcast-log snapshot / append (#316) */
    /* 0x28 and 0x29 were the transition-era STATION_INGOTS/HOLD_INGOTS
     * detail streams. Protocol v2 folds their provenance rows into the
     * canonical manifest packets and deliberately leaves the ids retired. */
    NET_MSG_BUY_INGOT       = 0x2A, /* client -> server: [type:1][pubkey:32] purchase from current docked station */
    NET_MSG_DELIVER_INGOT   = 0x2B, /* client -> server: [type:1][hold_index:1] deposit to current docked station */
    NET_MSG_FRACTURE_CHALLENGE = 0x2C, /* server -> nearby clients: [type:1][fracture_id:u32][seed:32][deadline_ms:u32][burst_cap:u16] */
    NET_MSG_FRACTURE_CLAIM     = 0x2D, /* client -> server: [type:1][fracture_id:u32][burst_nonce:u32][claimed_grade:u8] */
    NET_MSG_FRACTURE_RESOLVED  = 0x2E, /* server -> nearby clients: [type:1][fracture_id:u32][fragment_pub:32][winner_pub:32][grade:u8] */
    NET_MSG_STATION_MANIFEST   = 0x2F, /* server -> client: atomic per-station manifest summary + provenance detail. */
    NET_MSG_HIGHSCORES         = 0x30, /* server -> client: top-N leaderboard.
                                        * [type:1][count:1] + count × [callsign:8][credits_earned:f32]
                                        * [world_id:u32][world_seq:u32][build_id:u32][epoch_tick:u64]
                                        * [killed_by:8] (40 bytes/entry) */
    NET_MSG_PLAYER_MANIFEST    = 0x31, /* server -> client: atomic local-player manifest summary + provenance detail. */
    NET_MSG_REGISTER_PUBKEY    = 0x32, /* client -> server: [type:1][pubkey:32]. Layer A.2 of #479 — sent once per
                                        * connection BEFORE NET_MSG_SESSION so the server can remember the pubkey
                                        * assertion. Pubkey persistence/registry rebinding stays pending until
                                        * NET_MSG_PROVE_PUBKEY proves possession of the matching private key. */
    NET_MSG_SIGNED_ACTION      = 0x33, /* Layer A.3 of #479 — Ed25519-signed state-changing action.
                                        *
                                        * Wire layout (little-endian):
                                        *   [type:1=0x33]
                                        *   [nonce:8]                 u64, monotonic per player
                                        *   [action_type:1]           signed_action_type_t
                                        *   [payload_len:2]
                                        *   [payload:payload_len]     action-specific bytes
                                        *   [signature:64]            Ed25519 over
                                        *                             (nonce || action_type ||
                                        *                              payload_len || payload)
                                        *
                                        * The pubkey is implicit: the server resolves it via the
                                        * connection's session_token using the registry from A.2. Only
                                        * connections that have completed REGISTER_PUBKEY can submit
                                        * signed actions; everything else is silently dropped.
                                        *
                                        * Transient ship inputs (movement, mining beam) stay on the
                                        * unsigned NET_MSG_INPUT channel — signing every 60Hz frame
                                        * would torch the server (see PR description). Only events
                                        * that mutate persistent state need signatures.
                                        */
    NET_MSG_LEGACY_SAVES_AVAILABLE = 0x34, /* RETIRED server -> client value.
                                            * Current authorities never emit
                                            * legacy names or token prefixes;
                                            * current clients ignore this
                                            * message from stale peers. */
    NET_MSG_CARGO_RECEIPT_BUNDLE   = 0x36, /* server -> client. Layer D of #479.
                                            *
                                            * Sent immediately after a BUY_INGOT or
                                            * BUY_PRODUCT response that resulted in a
                                            * cargo transfer to the player. The client
                                            * appends each receipt to ship.receipts so
                                            * subsequent sells/deliveries can present
                                            * the chain to destination stations.
                                            *
                                            *   [type:1=0x36][count:u16]
                                            *     count × cargo_receipt_t (208 bytes each)
                                            *
                                            * If the receipts all name the same cargo_pub
                                            * and link chronologically, the bundle is the
                                            * full carried chain for that cargo. Older
                                            * servers may send independent singleton
                                            * receipts; clients accept both shapes. */
    NET_MSG_PRESENT_RECEIPT_CHAIN  = 0x37, /* client -> server. Layer D of #479.
                                            *
                                            *   [type:1=0x37][cargo_pub:32][chain_len:u16]
                                            *     chain_len × cargo_receipt_t
                                            *
                                            * Sent before a sell/deliver action
                                            * for cargo carried by the peer. The
                                            * authority verifies signature/linkage,
                                            * verifies the chain head names the
                                            * player's pubkey, then attaches the
                                            * chain to the matching carried manifest
                                            * unit. Handoff presentation carries
                                            * receipt chains inside the signed ship
                                            * snapshot. */
    NET_MSG_PLAYER_KNOWN_CONTRACTS = 0x39, /* server -> client. Per-player gossip-contract visibility
                                            * mask. Wire shape: [type:1][mask:u32].
                                            * Bit i set iff compact NET_MSG_CONTRACTS record i matches
                                            * a summary in this player's ship known_contracts pool. UI
                                            * uses this mask to hide contracts the player hasn't heard
                                            * about via dock gossip — keeps NET_MSG_CONTRACTS as the
                                            * global authoritative snapshot but filters the player-facing
                                            * dock UI to gossip-legal entries only. Sent per-tick,
                                            * per-player. */
    NET_MSG_ACTION_ACK             = 0x3A, /* server -> client. Immediate delivery ack for NET_MSG_INPUT.
                                            *
                                            *   [type:1=0x3A][action_id:u16][input_seq:u16]
                                            *   [status:1][action:1]
                                            *
                                            * Movement-only input is acknowledged by private
                                            * INPUT_APPLIED/STATE receipts, with WORLD_PLAYERS
                                            * mirroring the latest input_ack on semantic
                                            * heartbeats for compatibility. This is a transport/dedupe
                                            * ack for one-shot actions, not a semantic success response.
                                            * The normal authoritative snapshots still decide whether an action
                                            * visibly succeeded. */
    NET_MSG_ACTION_RESULT          = 0x3B, /* server -> client. Semantic result for an accepted
                                            * one-shot action after the server sim tick has consumed it.
                                            *
                                            *   [type:1=0x3B][action_id:u16][input_seq:u16]
                                            *   [status:1][action:1][server_tick:u32]
                                            *
                                            * This is paired with ACTION_ACK: ACK says the packet arrived;
                                            * RESULT says the authoritative sim accepted, rejected, or no-op'd
                                            * the requested action. */
    NET_MSG_LATENCY_PING           = 0x3C, /* client -> server. App-level transport RTT probe.
                                            *
                                            *   [type:1=0x3C][seq:u32][client_sent_ms:u32]
                                            *
                                            * This measures the wire/transport path only. It is intentionally
                                            * separate from input_seq acknowledgements, which are gated by
                                            * server sim and authoritative receipt cadence. */
    NET_MSG_LATENCY_PONG           = 0x3D, /* server -> client. Immediate echo for LATENCY_PING.
                                            *
                                            *   [type:1=0x3D][seq:u32][client_sent_ms:u32]
                                            *   [server_recv_ms:u32][server_send_ms:u32]
                                            *   [server_tick:u32]
                                            *
                                            * The client computes ping RTT from its own echoed timestamp.
                                            * The two server timestamps are same-clock server turnaround
                                            * telemetry only; clients must not compare them with client time.
                                            * The server tick is a cheap prediction anchor so full world
                                            * snapshots do not need to carry the tick heartbeat alone. */
    NET_MSG_CLIENT_METRICS         = 0x3E, /* client -> server. Periodic end-user telemetry report.
                                            *
                                            *   [type:1=0x3E][seq:u32]
                                            *   [ping_rtt_ms:u16][ack_ms:u16][ack_gap_ms:u16]
                                            *   [server_turnaround_ms:u16][player_interval_ms:u16]
                                            *   [unacked_inputs:u16][replay_depth:u16]
                                            *   [action_queue_depth:u8][recovery_flags:u8]
                                            *
                                            * The relay logs this as structured analytics. It never carries
                                            * raw session tokens, pubkeys, or client IPs. */
    NET_MSG_PROVE_PUBKEY           = 0x3F, /* client -> server: proof-of-possession for the registered pubkey.
                                            *
                                            *   [type:1=0x3F][pubkey:32][session_token:8][signature:64]
                                            *
                                            * Protocol v3+ signatures are Ed25519 over
                                            *   PUBKEY_PROOF_DOMAIN || pubkey || session_token ||
                                            *   server_challenge
                                            *
                                            * The one-time challenge binds the proof to this transport and is
                                            * consumed on successful verification. A v3+ server never accepts
                                            * the legacy unchallenged v1 signature. A newer client may use that
                                            * legacy signature only after PROTOCOL_INFO explicitly advertises
                                            * protocol <= 2 and no challenge has been received. The server
                                            * only rebinds the pubkey registry or restores
                                            * pubkey-keyed saves after this verifies. Legacy
                                            * save enumeration is retired. */
    NET_MSG_PUBKEY_CHALLENGE       = 0x70, /* server -> client:
                                            * [type:1][nonce:32].
                                            * Fresh for every transport and
                                            * consumed by one valid proof. */
    NET_MSG_WORLD_TOW_LINKS        = 0x71, /* server -> client. Atomic replacement
                                            * snapshot of canonical live towing
                                            * relationships.
                                            *
                                            *   [type:1][count:u16]
                                            *   [revision:u32][revision_tick:u32]
                                            *     count x TOW_LINK_RECORD_SIZE
                                            *
                                            * Legacy ship/target tow fields remain
                                            * compatibility projections. Clients that
                                            * understand this stream rebuild those
                                            * projections only from this snapshot. */
    NET_MSG_STATION_DIAG           = 0x40, /* server -> client: live per-module station diagnostics.
                                            *
                                            *   [type:1=0x40][station:1][module_count:1]
                                            *   [diag:MAX_MODULES_PER_STATION]
                                            *
                                            * This is live telemetry, intentionally split from
                                            * NET_MSG_STATION_IDENTITY so flow changes do not
                                            * resend static text, prices, pubkeys, or layout. */
    NET_MSG_PROTOCOL_INFO          = 0x41, /* server -> client/tool: protocol discovery.
                                            *
                                            *   [type:1=0x41][version:u16][capabilities:u32]
                                            *   [stream_count:1]
                                            *     stream_count ×
                                            *       [msg:1][class:1][flags:u16]
                                            *       [header_size:u16][record_size:u16]
                                            *       [max_records:u16][cadence_ms:u16]
                                            *
                                            * This is sent on connect before the large world
                                            * snapshots. External consumers should use it to
                                            * validate stream sizes/cadences instead of
                                            * hardcoding protocol constants. */
    NET_MSG_HANDOFF_REQUEST        = 0x42, /* client -> server. Ask the current/source
                                            * authority to issue a signed zone handoff
                                            * ticket for this player's current ship.
                                            *
                                            *   [type:1=0x42][source_station:1]
                                            *   [dest_station:1][ttl_ticks:u32]
                                            *
                                            * source_station may be 0xFF to mean "the
                                            * player's current docked station". */
    NET_MSG_HANDOFF_TICKET         = 0x43, /* server -> client. Response to HANDOFF_REQUEST.
                                            *
                                            *   [type:1=0x43][status:1]
                                            *   [source_station:1][dest_station:1]
                                            *   [handoff_ticket_t:252]
                                            *
                                            * status is NET_HANDOFF_STATUS_*. The ticket is
                                            * zero-filled on rejection. */
    NET_MSG_HANDOFF_PRESENT        = 0x44, /* client -> server. Present a source-issued
                                            * handoff ticket plus the ship snapshot it
                                            * binds. The destination verifies the source
                                            * signature, hashes, expiry, and replay cache
                                            * before hydrating the ship.
                                            *
                                            *   [type:1=0x44][handoff_ticket_t:252]
                                            *   [snapshot_len:u32][snapshot bytes] */
    NET_MSG_HANDOFF_RESULT         = 0x45, /* server -> client. Semantic accept/reject for
                                            * HANDOFF_PRESENT.
                                            *
                                            *   [type:1=0x45][status:1][reason:1]
                                            *   [dest_station:1][ticket_hash:32] */
    NET_MSG_WORLD_CARGO_PODS       = 0x46, /* server -> client: active towable cargo pod identity upserts */
    NET_MSG_DELIVERY_LEDGER        = 0x47, /* server -> client. Per-player recourse
                                            * shipment/debt ledger.
                                            *
                                            *   [type:1=0x47][count:1]
                                            *     count × DELIVERY_LEDGER_RECORD_SIZE */
    NET_MSG_INPUT_APPLIED          = 0x48, /* server -> client. Fixed-size applied-input
                                            * receipt for movement sync fallback/compatibility.
                                            *
                                            *   [type:1=0x48][input_seq:u16]
                                            *   [server_tick:u32][input_tick_ack:u32]
                                            *   [client_sent_ms:u32][server_recv_ms:u32]
                                            *   [server_send_ms:u32]
                                            *
                                            * Modern movement ack/correction uses private
                                            * INPUT_APPLIED/STATE receipts; WORLD_PLAYERS still
                                            * mirrors the latest ack on semantic heartbeats for
                                            * compatibility. */
    NET_MSG_WORLD_INTERACTIONS     = 0x49, /* server -> client: transient authored
                                            * interaction visuals such as module
                                            * tractor beams. */
    NET_MSG_PLAYER_KNOWN_LEDGER    = 0x4A, /* server -> client. Per-player station
                                            * ledger balances known to the authority.
                                            *
                                            *   [type:1=0x4A][count:1]
                                            *     count × [station:1][balance:f32]
                                            *
                                            * PLAYER_SHIP continues to carry the
                                            * current/nearby station balance. This
                                            * packet lets remote multiplayer render
                                            * cross-station local-credit rows without
                                            * exposing full station ledger tables. */
    NET_MSG_PLAYER_MARKET_MEMORIES = 0x6F, /* server -> client. Per-player bounded
                                            * carried market/gossip evidence.
                                            *
                                            *   [type:1=0x6F][count:1]
                                            *     count x PLAYER_MARKET_MEMORY_RECORD_SIZE
                                            *
                                            * This is the private wire mirror of
                                            * KNOW_MARKET items only; authoritative
                                            * contracts retain their visibility mask. */
    NET_MSG_WORLD_ASTEROID_MOTION  = 0x4B, /* server -> client. Compact live asteroid
                                            * motion correction for already-known clean
                                            * rocks.
                                            *
                                            *   [type:1=0x4B][count:u16]
                                            *     count × [index:u16][x:f32][y:f32]
                                            *              [vx:f32][vy:f32]
                                            *
                                            * Full NET_MSG_WORLD_ASTEROIDS records still
                                            * carry first-seen and structural
                                            * identity updates; removals ride
                                            * NET_MSG_WORLD_ASTEROID_REMOVE. */
    NET_MSG_WORLD_ASTEROID_MOTION_Q= 0x4C, /* server -> client. Quantized live asteroid
                                            * motion correction for far already-known
                                            * clean rocks.
                                            *
                                            *   [type:1=0x4C][count:u16]
                                            *     count × [index:u16][x:i16][y:i16]
                                            *              [vx:i16][vy:i16]
                                            *
                                            * Position uses
                                            * ASTEROID_MOTION_Q_POS_SCALE pixels per
                                            * step; velocity uses
                                            * ASTEROID_MOTION_Q_VEL_SCALE px/s per step. */
    NET_MSG_WORLD_ASTEROID_POS_Q   = 0x55, /* server -> client. Quantized asteroid
                                            * position correction for already-known
                                            * clean rocks whose quantized velocity
                                            * has not changed since the previous
                                            * motion sample.
                                            *
                                            *   [type:1=0x55][count:u16]
                                            *     count x [index:u16][x:i16][y:i16]
                                            *
                                            * Position uses
                                            * ASTEROID_MOTION_Q_POS_SCALE pixels per
                                            * step; clients retain the last known
                                            * asteroid velocity. */
    NET_MSG_WORLD_ASTEROID_POS8_Q  = 0x5C, /* server -> client. Byte-index variant
                                            * of WORLD_ASTEROID_POS_Q for
                                            * already-known asteroid slots < 256.
                                            *
                                            *   [type:1=0x5C][count:u8]
                                            *     count x [index:u8][x:i16][y:i16]
                                            *
                                            * Slots >= 256 remain on
                                            * WORLD_ASTEROID_POS_Q. */
    NET_MSG_WORLD_ASTEROID_POSD_Q  = 0x66, /* server -> client. Quantized asteroid
                                            * position delta from the previous
                                            * absolute quantized asteroid position.
                                            *
                                            *   [type:1=0x66][count:u16]
                                            *     count x [index:u16][dx:i8][dy:i8]
                                            *
                                            * Deltas are in
                                            * ASTEROID_MOTION_Q_POS_SCALE steps;
                                            * clients add them to the last
                                            * absolute asteroid position sample. */
    NET_MSG_WORLD_ASTEROID_POSD8_Q = 0x67, /* server -> client. Byte-index variant
                                            * of WORLD_ASTEROID_POSD_Q for
                                            * already-known asteroid slots < 256.
                                            *
                                            *   [type:1=0x67][count:u8]
                                            *     count x [index:u8][dx:i8][dy:i8]
                                            *
                                            * Slots >= 256 remain on
                                            * WORLD_ASTEROID_POSD_Q or the absolute
                                            * WORLD_ASTEROID_POS_Q fallback. */
    NET_MSG_WORLD_ASTEROIDS_Q      = 0x68, /* server -> client. Compact active
                                            * asteroid identity/upsert for
                                            * already relevant slots.
                                            *
                                            *   [type:1=0x68][count:u16]
                                            *     count x [index:u16][flags:1]
                                            *             [x:i16][y:i16]
                                            *             [vx:i16][vy:i16]
                                            *             [hp:u16][ore:u16]
                                            *             [radius:u16]
                                            *             [smelt:u8][detail:u8]
                                            *
                                            * Position/velocity use the same
                                            * quantized scales as asteroid
                                            * motion streams. Numeric identity
                                            * fields use
                                            * ASTEROID_IDENTITY_Q_VALUE_SCALE;
                                            * detail packs grade, crystal, and
                                            * phase. */
    NET_MSG_WORLD_ASTEROIDS8_Q     = 0x69, /* server -> client. Byte-index
                                            * compact active asteroid upsert for
                                            * slots < 256.
                                            *
                                            *   [type:1=0x69][count:u8]
                                            *     count x [index:u8][flags:1]
                                            *             [x:i16][y:i16]
                                            *             [vx:i16][vy:i16]
                                            *             [hp:u16][ore:u16]
                                            *             [radius:u16]
                                            *             [smelt:u8][detail:u8]
                                            *
                                            * Numeric identity fields use
                                            * ASTEROID_IDENTITY_Q_VALUE_SCALE;
                                            * detail packs grade, crystal, and
                                            * phase. Slots >= 256 remain on
                                            * WORLD_ASTEROIDS_Q or the legacy
                                            * WORLD_ASTEROIDS fallback. */
    NET_MSG_WORLD_ASTEROID_REMOVE  = 0x56, /* server -> client. Compact inactive
                                            * asteroid removals for already-known
                                            * relevant slots that left view or were
                                            * destroyed.
                                            *
                                            *   [type:1=0x56][count:u16]
                                            *     count x [index:u16]
                                            *
                                            * Active first-seen/identity records
                                            * remain on NET_MSG_WORLD_ASTEROIDS. */
    NET_MSG_WORLD_PLAYER_MOTION    = 0x4D, /* server -> client. Compact live player
                                            * pose correction for remote undocked
                                            * ships.
                                            *
                                            *   [type:1=0x4D][count:1]
                                            *     count × [id:1][x:f32][y:f32]
                                            *              [vx:f32][vy:f32][angle:f32]
                                            *
                                            * Full NET_MSG_WORLD_PLAYERS records still
                                            * carry identity, flags, tow state, beam
                                            * endpoints, and input-ack metadata. */
    NET_MSG_WORLD_PLAYER_MOTION_Q  = 0x6A, /* server -> client. Quantized live
                                            * remote player pose correction.
                                            *
                                            *   [type:1=0x6A][count:1]
                                            *     count x [id:1][x:i16][y:i16]
                                            *             [vx:i16][vy:i16]
                                            *             [angle:u8]
                                            *
                                            * x/y use 4 px units; vx/vy use
                                            * 0.25 px/s units; angle maps a
                                            * full turn to 256 steps. */
    NET_MSG_WORLD_PLAYER_DOCK_Q    = 0x6B, /* server -> client. Compact remote
                                            * dock/thrust status update.
                                            *
                                            *   [type:1=0x6B][count:1]
                                            *     count x [id:1][status_flags:1]
                                            *
                                            * status_flags uses
                                            * PLAYER_DOCK_STATUS_FLAGS_MASK
                                            * from the WORLD_PLAYERS flags byte.
                                            *
                                            * Pose continues on STATE /
                                            * WORLD_PLAYER_MOTION_Q. */
    NET_MSG_WORLD_PLAYER_MOTIOND_Q = 0x6C, /* server -> client. Delta-compressed
                                            * remote player motion after an
                                            * absolute WORLD_PLAYER_MOTION_Q
                                            * baseline has been delivered.
                                            *
                                            *   [type:1=0x6C][count:1]
                                            *     count x [id:1][dx:i8][dy:i8]
                                            *             [vx:i8][vy:i8]
                                            *             [angle:u8]
                                            *
                                            * dx/dy are signed deltas in
                                            * PLAYER_MOTION_Q_POS_SCALE units
                                            * from the last sent absolute/delta
                                            * player motion sample for that id.
                                            * vx/vy use
                                            * PLAYER_MOTIOND_Q_VEL_SCALE. */
    NET_MSG_WORLD_PLAYER_POSED_Q   = 0x6D, /* server -> client. Position plus
                                            * angle remote player delta after
                                            * an absolute or delta motion
                                            * baseline.
                                            *
                                            *   [type:1=0x6D][count:1]
                                            *     count x [id:1][dx:i8][dy:i8]
                                            *             [angle:u8]
                                            *
                                            * Retains the client's previous
                                            * velocity for that id. */
    NET_MSG_WORLD_PLAYER_MOTIONM_Q = 0x6E, /* server -> client. Mixed remote
                                            * player delta packet after an
                                            * absolute WORLD_PLAYER_MOTION_Q
                                            * baseline.
                                            *
                                            *   [type:1=0x6E][count:1]
                                            *     count x pose:
                                            *       [id:1][dx:i8][dy:i8]
                                            *       [angle:u8]
                                            *     or count x motion:
                                            *       [id|0x80:1][dx:i8][dy:i8]
                                            *       [vx:i8][vy:i8][angle:u8]
                                            *
                                            * id uses the low 5 bits
                                            * (MAX_PLAYERS=32). The 0x80 bit
                                            * means the record includes
                                            * velocity; otherwise the client
                                            * retains the previous velocity. */
    NET_MSG_WORLD_NPC_MOTION       = 0x4E, /* server -> client. Compact live NPC
                                            * pose correction for relevant NPCs.
                                            *
                                            *   [type:1=0x4E][count:1]
                                            *     count × [index:1][flags:1]
                                            *              [x:f32][y:f32]
                                            *              [vx:f32][vy:f32][angle:f32]
                                            *
                                            * Full NET_MSG_WORLD_NPCS records still
                                            * carry visibility, role, tint, a
                                            * reserved-zero legacy identity tail,
                                            * and home station; status churn rides
                                            * NET_MSG_WORLD_NPC_STATUS. */
    NET_MSG_WORLD_CARGO_POD_MOTION = 0x4F, /* server -> client. Compact live cargo
                                            * pod pose correction for already-known
                                            * relevant pods.
                                            *
                                            *   [type:1=0x4F][count:1]
                                            *     count × [index:1][x:f32][y:f32]
                                            *              [vx:f32][vy:f32]
                                            *              [rotation:f32]
                                            *
                                            * Full NET_MSG_WORLD_CARGO_PODS records
                                            * still carry kind, commodity, radius,
                                            * quantity, shipment, tow owner, tractor
                                            * custody, and visible-set changes. */
    NET_MSG_WORLD_INTERACTION_DRIFT = 0x50, /* server -> client. Quantized visual
                                            * drift for the current
                                            * WORLD_INTERACTIONS identity list.
                                            *
                                            *   [type:1=0x50][count:1]
                                            *     count × [index:1]
                                            *              [source_x:i16][source_y:i16]
                                            *              [target_x:i16][target_y:i16]
                                            *              [range:u16][intensity:u8]
                                            *
                                            * Full NET_MSG_WORLD_INTERACTIONS records
                                            * still carry type, visual, commodity,
                                            * flags, and source/target identity. */
    NET_MSG_WORLD_ASTEROID_STATE_Q  = 0x51, /* server -> client. Compact dirty
                                            * state for already-known active
                                            * asteroids.
                                            *
                                            *   [type:1=0x51][count:u16]
                                            *     count × [index:u16][hp:f32]
                                            *              [ore:f32][radius:f32]
                                            *              [smelt:u8][grade:u8]
                                            *              [crystal_stage:u8]
                                            *              [phase:u8]
                                            *
                                            * First-seen and structural identity
                                            * changes remain on
                                            * NET_MSG_WORLD_ASTEROIDS; removals
                                            * ride NET_MSG_WORLD_ASTEROID_REMOVE. */
    NET_MSG_WORLD_NPC_MOTION_Q     = 0x52, /* server -> client. Quantized live
                                            * NPC pose correction for relevant
                                            * NPCs.
                                            *
                                            *   [type:1=0x52][count:1]
                                            *     count × [index:1][flags:1]
                                            *              [x:i16][y:i16]
                                            *              [vx:i16][vy:i16]
                                            *              [angle:u16]
                                            *
                                            * Position uses
                                            * NPC_MOTION_Q_POS_SCALE pixels per
                                            * step; velocity uses
                                            * NPC_MOTION_Q_VEL_SCALE px/s per
                                            * step; angle covers one turn. Full
                                            * NET_MSG_WORLD_NPCS records still
                                            * carry visibility, role, tint, a
                                            * reserved-zero legacy identity tail,
                                            * and home station;
                                            * status churn rides
                                            * NET_MSG_WORLD_NPC_STATUS. */
    NET_MSG_WORLD_NPC_STATUS       = 0x53, /* server -> client. Compact live
                                            * NPC status for role/state/target/tow
                                            * churn. Thrust-only visual changes
                                            * are owned by NPC_MOTION(_Q).
                                            *
                                            *   [type:1=0x53][count:1]
                                            *     count × [index:1][flags:1]
                                            *              [target:u16]
                                            *              [towed_fragment:u16]
                                            *
                                            * Full NET_MSG_WORLD_NPCS records
                                            * carry visibility, role, tint, a
                                            * reserved-zero legacy identity tail,
                                            * and home station. */
    NET_MSG_WORLD_CARGO_POD_MOTION_Q = 0x54, /* server -> client. Quantized live
                                            * cargo pod pose correction for
                                            * already-known relevant pods.
                                            *
                                            *   [type:1=0x54][count:1]
                                            *     count × [index:1]
                                            *              [x:i16][y:i16]
                                            *              [vx:i16][vy:i16]
                                            *              [rotation:u16]
                                            *
                                            * Full NET_MSG_WORLD_CARGO_PODS
                                            * records still carry cargo identity,
                                            * tow owner, tractor custody, and
                                            * visible-set changes. */
    NET_MSG_WORLD_CARGO_POD_REMOVE = 0x57, /* server -> client. Compact cargo
                                            * pod relevance/removal update for
                                            * already-known pod slots.
                                            *
                                            *   [type:1=0x57][count:1]
                                            *     count × [index:1]
                                            *
                                            * With
                                            * SIGNAL_PROTOCOL_CAP_CARGO_POD_REMOVE,
                                            * WORLD_CARGO_PODS is an upsert lane;
                                            * this packet owns disappearances. */
    NET_MSG_WORLD_SCAFFOLD_REMOVE  = 0x58, /* server -> client. Compact scaffold
                                            * relevance/removal update for
                                            * already-known scaffold slots.
                                            *
                                            *   [type:1=0x58][count:1]
                                            *     count × [index:1]
                                            *
                                            * With
                                            * SIGNAL_PROTOCOL_CAP_SCAFFOLD_REMOVE,
                                            * WORLD_SCAFFOLDS is an upsert lane;
                                            * this packet owns disappearances. */
    NET_MSG_WORLD_SCAFFOLD_MOTION_Q = 0x59, /* server -> client. Quantized live
                                             * scaffold pose/velocity update.
                                             *
                                             *   [type:1=0x59][count:1]
                                             *     count × [index:1][x:i16][y:i16]
                                             *              [vx:i16][vy:i16]
                                             *
                                             * Full NET_MSG_WORLD_SCAFFOLDS
                                             * records still carry scaffold
                                             * state/module/owner/radius/build
                                             * identity and visible-set changes. */
    NET_MSG_WORLD_NPC_POS_Q       = 0x5A, /* server -> client. Quantized
                                           * position-only correction for
                                           * already-known NPC slots whose
                                           * velocity/angle/thrust visual state
                                           * still matches the previous motion
                                           * baseline.
                                           *
                                           *   [type:1=0x5A][count:1]
                                           *     count × [index:1][x:i16][y:i16]
                                           *
                                           * Velocity, angle, thrust, role, and
                                           * target changes still use
                                           * NPC_MOTION(_Q) or NPC_STATUS. */
    NET_MSG_WORLD_NPC_POSE_Q      = 0x5B, /* server -> client. Quantized pose
                                           * correction for already-known NPC
                                           * slots whose velocity/thrust visual
                                           * state still matches the previous
                                           * motion baseline, but facing changed
                                           * enough to reconcile.
                                           *
                                           *   [type:1=0x5B][count:1]
                                           *     count × [index:1][x:i16][y:i16]
                                           *              [angle:u16]
                                           *
                                           * Velocity, thrust, role, and target
                                           * changes still use NPC_MOTION(_Q) or
                                           * NPC_STATUS. */
    NET_MSG_WORLD_NPC_STATUS8_Q   = 0x5D, /* server -> client. Compact status
                                           * batch for visible NPCs whose
                                           * target/towed asteroid references
                                           * are either none or fit in one byte.
                                           *
                                           *   [type:1=0x5D][count:1]
                                           *     count x [index:1][flags:1]
                                           *             [target:u8][towed:u8]
                                           *
                                           * 0xFF means none. Batches needing a
                                           * 16-bit asteroid reference use
                                           * WORLD_NPC_STATUS. */
    NET_MSG_WORLD_NPC_LINEAR_Q    = 0x5E, /* server -> client. Quantized
                                           * position+velocity correction for
                                           * already-known NPC slots whose
                                           * angle/thrust visual state still
                                           * matches the previous motion
                                           * baseline.
                                           *
                                           *   [type:1=0x5E][count:1]
                                           *     count x [index:1][x:i16][y:i16]
                                           *             [vx:i16][vy:i16]
                                           *
                                           * Angle, thrust, role, and target
                                           * changes still use NPC_MOTION(_Q) or
                                           * NPC_STATUS. */
    NET_MSG_WORLD_CARGO_POD_LINEAR_Q = 0x5F, /* server -> client. Quantized
                                            * position+velocity correction for
                                            * already-known cargo pods whose
                                            * rotation still matches the
                                            * previous motion baseline.
                                            *
                                            *   [type:1=0x5F][count:1]
                                            *     count x [index:1][x:i16][y:i16]
                                            *             [vx:i16][vy:i16]
                                            *
                                            * Rotation changes still use
                                            * CARGO_POD_MOTION(_Q). */
    NET_MSG_WORLD_NPC_MOTION8_Q   = 0x60, /* server -> client. Compact
                                           * quantized NPC pose correction for
                                           * relevant NPCs.
                                           *
                                           *   [type:1=0x60][count:1]
                                           *     count x [index:1][flags:1]
                                           *             [x:i16][y:i16]
                                           *             [vx:i8][vy:i8]
                                           *             [angle:u8]
                                           *
                                           * This is the preferred live NPC
                                           * visual stream; WORLD_NPC_MOTION_Q
                                           * remains as the higher-precision
                                           * compatibility fallback. */
    NET_MSG_WORLD_INTERACTIONS_Q   = 0x61, /* server -> client. Compact
                                            * interaction identity upsert.
                                            *
                                            *   [type:1=0x61][count:1]
                                            *     count x [type:1][visual:1]
                                            *             [commodity:1][flags:1]
                                            *             [source_type:1]
                                            *             [source_index:i16]
                                            *             [source_aux:i16]
                                            *             [target_type:1]
                                            *             [target_index:i16]
                                            *             [target_aux:i16]
                                            *             [source_x:i16]
                                            *             [source_y:i16]
                                            *             [target_x:i16]
                                            *             [target_y:i16]
                                            *             [range:u16]
                                            *             [intensity:u8]
                                            *
                                            * Source/target identity is exact;
                                            * visual fields use the same scales as
                                            * WORLD_INTERACTION_DRIFT. */
    NET_MSG_WORLD_CARGO_PODS_Q     = 0x62, /* server -> client. Compact cargo
                                            * pod identity upsert.
                                            *
                                            *   [type:1=0x62][count:1]
                                            *     count x [index:1][kind:1]
                                            *             [commodity:1]
                                            *             [towed_by:1]
                                            *             [x:i16][y:i16]
                                            *             [vx:i16][vy:i16]
                                            *             [radius:f32]
                                            *             [rotation:u16]
                                            *             [quantity:u16]
                                            *             [manifest_count:u16]
                                            *             [shipment_id:u16]
                                            *             [flags:1][best_grade:1]
                                            *             [tractor_station:1]
                                            *             [tractor_module:1]
                                            *
                                            * Semantic fields stay exact; visual
                                            * pose uses CARGO_POD_MOTION_Q scales. */
    NET_MSG_INSPECT_SNAPSHOT       = 0x38, /* server -> client. Laser/scan inspection snapshot.
                                            *
                                            *   [type:1=0x38][target_type:1][target_index:1]
                                            *   [module_index:1][role:1][state:1]
                                            *   [home_station:1][dest_station:1]
                                            *   [row_count:1][manifest_count:u16]
                                            *     row_count × INSPECT_SNAPSHOT_ROW bytes
                                            *
                                            * Rows project a scanned ship's current
                                            * manifest into cargo hashes + portable
                                            * receipt-chain heads. This is display
                                            * telemetry, not authority. */
    NET_MSG_CLAIM_LEGACY_SAVE      = 0x35, /* RETIRED client -> server value.
                                            * Its claimant-chosen basename
                                            * payload could not prove legacy
                                            * save ownership. Current local
                                            * and dedicated authorities reject
                                            * every packet with this type
                                            * without inspecting its payload
                                            * or mutating state. */
    NET_MSG_LEGACY_RECOVERY_OFFER  = 0x72, /* server -> client, protocol v5+.
                                            * Opaque, connection-bound offer:
                                            * [type:1][offer_id:16]
                                            * [expires_in_seconds:u16].
                                            * No path, token, basename, or
                                            * reconnect material is exposed. */
    NET_MSG_LEGACY_RECOVERY_RESULT = 0x73, /* server -> client, protocol v5+.
                                            * [type:1][status:1]. Statuses are
                                            * intentionally bounded and never
                                            * disclose unrelated saves. */
    NET_MSG_EVENTS_V2              = 0x74, /* server -> client: typed public
                                            * actor event batch; never carries
                                            * reconnect/session bearers */
};

enum {
    SESSION_MSG_TOKEN_OFFSET = 1,
    SESSION_MSG_CALLSIGN_OFFSET = 9,
    SESSION_MSG_PROTOCOL_OFFSET = 16,
    SESSION_MSG_SIZE = 18,
};

enum {
    NET_HAIL_RESPONSE_BASE_SIZE = 7,
    NET_HAIL_RESPONSE_REASON_SIZE = 25,
};

/* Protocol discovery. Increment SIGNAL_PROTOCOL_VERSION only when a
 * compatibility decision is needed by external consumers; adding a new stream
 * to PROTOCOL_INFO is normally discoverable via stream_count/capabilities.
 *
 * Version 3 makes the server-issued challenge mandatory for pubkey proofs.
 * Version 4 extends cargo-pod identity records with custody and an opaque,
 * generation-bound PRESENT/UNPACK selection token.
 * Version 5 replaces arbitrary-basename legacy claims with one opaque,
 * connection-bound offer confirmed through the signed-action nonce stream.
 * Version 6 replaces bearer-backed combat attribution with typed public
 * actor IDs and marks legacy event attribution explicitly unknown.
 * Version 7 adds Engine cargo/module identities and expands station module
 * records for the first Engine Fab. */
#define SIGNAL_PROTOCOL_VERSION 7u
#define SIGNAL_PROTOCOL_CHALLENGE_PUBKEY_PROOF_VERSION 3u

enum {
    SIGNAL_PROTOCOL_CAP_PROTOCOL_INFO   = 1u << 0,
    SIGNAL_PROTOCOL_CAP_STATION_DIAG    = 1u << 1,
    SIGNAL_PROTOCOL_CAP_MANIFEST_STREAMS= 1u << 2,
    SIGNAL_PROTOCOL_CAP_LATENCY_METRICS = 1u << 3,
    SIGNAL_PROTOCOL_CAP_RECEIPT_CHAINS  = 1u << 4,
    SIGNAL_PROTOCOL_CAP_INSPECT_SNAPSHOT= 1u << 5,
    SIGNAL_PROTOCOL_CAP_HANDOFF_TICKETS = 1u << 6,
    SIGNAL_PROTOCOL_CAP_DELIVERY_SHIPMENTS = 1u << 7,
    SIGNAL_PROTOCOL_CAP_INPUT_APPLIED_ACK = 1u << 8,
    SIGNAL_PROTOCOL_CAP_PLAYER_KNOWN_LEDGER = 1u << 9,
    SIGNAL_PROTOCOL_CAP_ASTEROID_MOTION = 1u << 10,
    SIGNAL_PROTOCOL_CAP_ASTEROID_MOTION_Q = 1u << 11,
    SIGNAL_PROTOCOL_CAP_PLAYER_MOTION = 1u << 12,
    SIGNAL_PROTOCOL_CAP_NPC_MOTION = 1u << 13,
    SIGNAL_PROTOCOL_CAP_CARGO_POD_MOTION = 1u << 14,
    SIGNAL_PROTOCOL_CAP_INTERACTION_DRIFT = 1u << 15,
    SIGNAL_PROTOCOL_CAP_ASTEROID_STATE_Q = 1u << 16,
    SIGNAL_PROTOCOL_CAP_NPC_MOTION_Q = 1u << 17,
    SIGNAL_PROTOCOL_CAP_NPC_STATUS = 1u << 18,
    SIGNAL_PROTOCOL_CAP_CARGO_POD_MOTION_Q = 1u << 19,
    SIGNAL_PROTOCOL_CAP_LATENCY_PONG_TICK = 1u << 20,
    SIGNAL_PROTOCOL_CAP_ASTEROID_POS_Q = 1u << 21,
    SIGNAL_PROTOCOL_CAP_ASTEROID_REMOVE = 1u << 22,
    SIGNAL_PROTOCOL_CAP_CARGO_POD_REMOVE = 1u << 23,
    SIGNAL_PROTOCOL_CAP_SCAFFOLD_REMOVE = 1u << 24,
    SIGNAL_PROTOCOL_CAP_SCAFFOLD_MOTION_Q = 1u << 25,
    SIGNAL_PROTOCOL_CAP_NPC_POS_Q = 1u << 26,
    SIGNAL_PROTOCOL_CAP_NPC_POSE_Q = 1u << 27,
    SIGNAL_PROTOCOL_CAP_ASTEROID_POS8_Q = 1u << 28,
    SIGNAL_PROTOCOL_CAP_NPC_STATUS8_Q = 1u << 29,
    SIGNAL_PROTOCOL_CAP_NPC_LINEAR_Q = 1u << 30,
};

#define SIGNAL_PROTOCOL_CAP_ASTEROID_POSD_Q (1u << 31)

enum {
    PROTOCOL_STREAM_CLASS_STATIC   = 1, /* identity/config snapshots */
    PROTOCOL_STREAM_CLASS_LIVE     = 2, /* live diagnostics/pose telemetry */
    PROTOCOL_STREAM_CLASS_ECON     = 3, /* inventory, manifest, contracts */
    PROTOCOL_STREAM_CLASS_PLAYER   = 4, /* per-player private state */
    PROTOCOL_STREAM_CLASS_EVENT    = 5, /* event/log append or snapshot */
    PROTOCOL_STREAM_CLASS_AUTH     = 6, /* authority/provenance protocol */
};

enum {
    PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT = 1u << 0,
    PROTOCOL_STREAM_FLAG_CLIENT_TO_SERVER = 1u << 1,
    PROTOCOL_STREAM_FLAG_DIRTY_ONLY       = 1u << 2,
    PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER = 1u << 3,
    PROTOCOL_STREAM_FLAG_PER_PLAYER       = 1u << 4,
    PROTOCOL_STREAM_FLAG_FIXED_SIZE       = 1u << 5,
};

enum {
    PROTOCOL_INFO_HEADER_SIZE        = 8,
    PROTOCOL_INFO_STREAM_RECORD_SIZE = 12,
    PROTOCOL_INFO_STREAM_CAPACITY    = 72,
    PROTOCOL_INFO_SIZE = PROTOCOL_INFO_HEADER_SIZE +
                         PROTOCOL_INFO_STREAM_CAPACITY * PROTOCOL_INFO_STREAM_RECORD_SIZE,
};

#define SIGNAL_PROTOCOL_CAPABILITIES \
    (SIGNAL_PROTOCOL_CAP_PROTOCOL_INFO | \
     SIGNAL_PROTOCOL_CAP_STATION_DIAG | \
     SIGNAL_PROTOCOL_CAP_MANIFEST_STREAMS | \
     SIGNAL_PROTOCOL_CAP_LATENCY_METRICS | \
     SIGNAL_PROTOCOL_CAP_RECEIPT_CHAINS | \
     SIGNAL_PROTOCOL_CAP_INSPECT_SNAPSHOT | \
     SIGNAL_PROTOCOL_CAP_HANDOFF_TICKETS | \
     SIGNAL_PROTOCOL_CAP_DELIVERY_SHIPMENTS | \
     SIGNAL_PROTOCOL_CAP_INPUT_APPLIED_ACK | \
     SIGNAL_PROTOCOL_CAP_PLAYER_KNOWN_LEDGER | \
     SIGNAL_PROTOCOL_CAP_ASTEROID_MOTION | \
     SIGNAL_PROTOCOL_CAP_ASTEROID_MOTION_Q | \
     SIGNAL_PROTOCOL_CAP_PLAYER_MOTION | \
     SIGNAL_PROTOCOL_CAP_NPC_MOTION | \
     SIGNAL_PROTOCOL_CAP_CARGO_POD_MOTION | \
     SIGNAL_PROTOCOL_CAP_INTERACTION_DRIFT | \
     SIGNAL_PROTOCOL_CAP_ASTEROID_STATE_Q | \
     SIGNAL_PROTOCOL_CAP_NPC_MOTION_Q | \
     SIGNAL_PROTOCOL_CAP_NPC_STATUS | \
     SIGNAL_PROTOCOL_CAP_CARGO_POD_MOTION_Q | \
     SIGNAL_PROTOCOL_CAP_LATENCY_PONG_TICK | \
     SIGNAL_PROTOCOL_CAP_ASTEROID_POS_Q | \
     SIGNAL_PROTOCOL_CAP_ASTEROID_REMOVE | \
     SIGNAL_PROTOCOL_CAP_CARGO_POD_REMOVE | \
     SIGNAL_PROTOCOL_CAP_SCAFFOLD_REMOVE | \
     SIGNAL_PROTOCOL_CAP_SCAFFOLD_MOTION_Q | \
     SIGNAL_PROTOCOL_CAP_NPC_POS_Q | \
     SIGNAL_PROTOCOL_CAP_NPC_POSE_Q | \
     SIGNAL_PROTOCOL_CAP_ASTEROID_POS8_Q | \
     SIGNAL_PROTOCOL_CAP_NPC_STATUS8_Q | \
     SIGNAL_PROTOCOL_CAP_NPC_LINEAR_Q | \
     SIGNAL_PROTOCOL_CAP_ASTEROID_POSD_Q)

/* NET_MSG_REGISTER_PUBKEY wire size: 1 + 32 = 33 bytes. */
#define REGISTER_PUBKEY_MSG_SIZE 33

/* NET_MSG_PROVE_PUBKEY wire constants. The wire packet is unchanged between
 * the legacy and challenge-bound schemes; only the signed message differs. */
#define PUBKEY_PROOF_V1_DOMAIN    "prove-pubkey-v1"
#define PUBKEY_PROOF_V1_DOMAIN_LEN 15
#define PUBKEY_PROOF_V1_MESSAGE_SIZE \
    (PUBKEY_PROOF_V1_DOMAIN_LEN + 32 + 8)
#define PUBKEY_PROOF_DOMAIN       "prove-pubkey-v2"
#define PUBKEY_PROOF_DOMAIN_LEN   15
#define PUBKEY_PROOF_CHALLENGE_SIZE 32
#define PUBKEY_PROOF_MESSAGE_SIZE \
    (PUBKEY_PROOF_DOMAIN_LEN + 32 + 8 + PUBKEY_PROOF_CHALLENGE_SIZE)
#define PUBKEY_CHALLENGE_MSG_SIZE (1 + PUBKEY_PROOF_CHALLENGE_SIZE)
#define PROVE_PUBKEY_PUBKEY_OFFSET 1
#define PROVE_PUBKEY_TOKEN_OFFSET  33
#define PROVE_PUBKEY_SIG_OFFSET    41
#define PROVE_PUBKEY_MSG_SIZE      (1 + 32 + 8 + 64)

/* Opaque authenticated legacy-save recovery (protocol v5+). */
enum {
    LEGACY_RECOVERY_OFFER_ID_SIZE = 16,
    NET_LEGACY_RECOVERY_OFFER_SIZE =
        1 + LEGACY_RECOVERY_OFFER_ID_SIZE + 2,
    NET_LEGACY_RECOVERY_RESULT_SIZE = 2,
};

typedef enum {
    LEGACY_RECOVERY_RESULT_NO_MATCH = 1,
    LEGACY_RECOVERY_RESULT_STALE_OFFER,
    LEGACY_RECOVERY_RESULT_REPLAY,
    LEGACY_RECOVERY_RESULT_INVALID_SOURCE,
    LEGACY_RECOVERY_RESULT_DESTINATION_CONFLICT,
    LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE,
    LEGACY_RECOVERY_RESULT_SUCCESS,
} legacy_recovery_result_status_t;

/* ------------------------------------------------------------------ */
/* Signed-action types (#479 Layer A.3)                                */
/* ------------------------------------------------------------------ */

typedef enum {
    SIGNED_ACTION_BUY_PRODUCT     = 1, /* payload: [commodity:1][grade:1]
                                        *          [action_id:u16 optional] */
    SIGNED_ACTION_BUY_INGOT       = 2, /* payload: [pubkey:32]
                                        *          [action_id:u16 optional] */
    SIGNED_ACTION_SELL_CARGO      = 3, /* payload: [commodity:1][grade:1]
                                        *   commodity=COMMODITY_COUNT, grade=MINING_GRADE_COUNT
                                        *   means "sell all" (legacy bulk),
                                        *   then [action_id:u16 optional]. */
    SIGNED_ACTION_DELIVER         = 4, /* payload: [hold_index:1]
                                        *          [action_id:u16 optional]
                                        * matches NET_MSG_DELIVER_INGOT. */
    SIGNED_ACTION_PLACE_OUTPOST   = 5, /* payload: [station:1][ring:1][slot:1]
                                        *          [action_id:u16 optional] */
    SIGNED_ACTION_FRACTURE_CLAIM  = 6, /* payload: [fracture_id:4][burst_nonce:4][grade:1] */
    SIGNED_ACTION_CLAIM_CONTRACT  = 7, /* payload: [contract_id:1] */
    SIGNED_ACTION_CANCEL_CONTRACT = 8, /* payload: [contract_id:1] */
    SIGNED_ACTION_INPUT_ACTION    = 9, /* payload: [net_action:1][buy_grade:1][station:1][ring:1][slot:1][action_id:u16 optional] */
    SIGNED_ACTION_PLAN            = 10, /* payload: NET_MSG_PLAN bytes after the type byte */
    SIGNED_ACTION_PRESENT_POD     = 11, /* payload: [pod_index:1]
                                        *          [selection_digest:32]
                                        *          [action_id:u16 optional] */
    SIGNED_ACTION_RECOVER_LEGACY_SAVE = 12, /* payload: [offer_id:16].
                                             * The server derives the sole
                                             * eligible source from the
                                             * authenticated session token. */
    SIGNED_ACTION_COUNT
} signed_action_type_t;

/* Header size on the wire, excluding payload + signature:
 *   type(1) + nonce(8) + action_type(1) + payload_len(2) = 12 bytes
 * Signature is appended after the payload, 64 bytes. */
#define SIGNED_ACTION_HEADER_SIZE 12
#define SIGNED_ACTION_SIG_SIZE    64
/* Cap payload at 256 bytes; today's largest fixed payload is PRESENT_POD. */
#define SIGNED_ACTION_MAX_PAYLOAD 256

/* Top-N global leaderboard persisted server-side, broadcast on join and
 * after every death. */
enum {
    HIGHSCORE_TOP_N      = 10,
    /* 8-byte callsign + f32 credits + u32 world_id + u32 world_seq +
     * u32 build_id + u64 epoch_tick + 8-byte killed_by callsign
     * = 40 bytes/entry. */
    HIGHSCORE_ENTRY_SIZE = 40,
    HIGHSCORE_HEADER     = 2,    /* type + count */
};

/* Canonical manifest snapshot. Summary rows keep the market/HUD payload
 * compact; detail rows carry complete field-packed cargo_unit_t identity for
 * concrete units, bounded to MANIFEST_DETAIL_MAX. Both sections are in one
 * packet so a client never combines different ticks. Summary counts remain
 * authoritative for any overflow beyond the detail bound.
 *
 * NET_MSG_STATION_MANIFEST:
 *   [type:1][station_idx:1][summary_count:u16][detail_count:u16]
 *   summary_count × [commodity:1][grade:1][count:u16]
 *   detail_count × cargo_unit_t wire record
 *
 * NET_MSG_PLAYER_MANIFEST:
 *   [type:1][summary_count:u16][detail_count:u16]
 *   summary_count × [commodity:1][grade:1][count:u16]
 *   detail_count × cargo_unit_t wire record
 *
 * Detail encoding is the canonical 80-byte field order declared by
 * cargo_unit_wire_pack(), not an in-memory struct dump. */
enum {
    STATION_MANIFEST_HEADER = 6,
    PLAYER_MANIFEST_HEADER = 5,
    MANIFEST_SUMMARY_ENTRY = 4,
    MANIFEST_DETAIL_ENTRY = 80,
    MANIFEST_DETAIL_MAX = 256,
};

#define STATION_MANIFEST_MAX_SIZE \
    (STATION_MANIFEST_HEADER + \
     COMMODITY_COUNT * MINING_GRADE_COUNT * MANIFEST_SUMMARY_ENTRY + \
     MANIFEST_DETAIL_MAX * MANIFEST_DETAIL_ENTRY)
#define PLAYER_MANIFEST_MAX_SIZE \
    (PLAYER_MANIFEST_HEADER + \
     COMMODITY_COUNT * MINING_GRADE_COUNT * MANIFEST_SUMMARY_ENTRY + \
     MANIFEST_DETAIL_MAX * MANIFEST_DETAIL_ENTRY)

/* Per-class buy price at any station's stockpile. RATi/commissioned
 * are an order of magnitude scarcer so they cost proportionally more.
 * Indexed by ingot_prefix_t. Anonymous = 0 (not purchasable). */
#define INGOT_PRICE_M             1500
#define INGOT_PRICE_H             1500
#define INGOT_PRICE_T             1500
#define INGOT_PRICE_S             1500
#define INGOT_PRICE_F             1500
#define INGOT_PRICE_K             1500
#define INGOT_PRICE_RATI          35000
#define INGOT_PRICE_COMMISSIONED  100000
/* Delivery credit paid to the player when they deposit a named ingot
 * at a station's stockpile — small flat reward for transit. */
#define INGOT_DELIVERY_CREDIT     100

/* NET_MSG_INSPECT_SNAPSHOT wire layout:
 *   header:
 *     [type:1][target_type:1][target_index:1][module_index:1]
 *     [role:1][state:1][home_station:1][dest_station:1]
 *     [row_count:1][manifest_count:u16]
 *   row:
 *     [commodity:1][grade:1][chain_len:1][flags:1][event_id:u64]
 *     [quantity:u16][cargo_pub:32][receipt_head:32][origin_station_pub:32]
 *     [latest_station_pub:32]
 *
 * Diagnostic rows (flags bit2) reuse the row payload for NPC scan context:
 * commodity = inspect_diag_kind_t. For market rows, grade = confidence,
 * chain_len = salience, event_id bytes [station_a, station_b, action,
 * commodity], quantity = value/quantity hint. For HNN trace rows, grade =
 * compact capacity load, chain_len = compact fidelity, event_id = action
 * vocabulary hash, quantity = stored count, and cargo_pub bytes use the
 * INSPECT_HNN_TRACE_* offsets below. For job rows, grade = compact
 * score, chain_len = selected/candidate marker, event_id bytes
 * [source_station, dest_station, job_kind, commodity], quantity = job hint,
 * cargo_pub bytes [0..6] = compact factor scores for
 * value/demand/supply/route/freshness/capability/proof; byte [7] =
 * inspect_job_reason_t provenance/reason code; bytes [8..11] =
 * market-memory kind, hop count, observed-age seconds bucket, source station,
 * proof/hash kind, and a four-byte proof/hash prefix for compact provenance.
 * Job rows also place the full 32-byte provenance hash, when known, in the
 * receipt_head field so clients can expand the compact prefix without widening
 * the row. Market diagnostic rows place full source-chain hashes in the normal
 * row hash fields: cargo_pub = subject_hash, receipt_head = chain_anchor,
 * origin_station_pub = source_hash, latest_station_pub = witness_hash.
 *
 * target_type mirrors server_player_t.scan_target_type:
 *   0 none, 1 station/module, 2 NPC, 3 player, 4 cargo pod.
 * For NPC targets, role/state are npc_role_t/npc_state_t. For player
 * targets, role is hull_class_t and state is rounded hull, clamped to
 * one byte. target_index/module_index/home_station/dest_station use 0xFF as
 * unknown/none. flags bit0 = row has at least one receipt link;
 * bit1 = row is a grouped bulk/finished-good row with no individual cargo
 * identity; bit2 = row is a diagnostic row, not cargo; bit3 = receipt chain
 * was retrieved from local station storage rather than carried cargo; bit4 =
 * receipt chain was retrieved from another local relay/custodian ship. */
enum {
    INSPECT_TARGET_NONE    = 0,
    INSPECT_TARGET_STATION = 1,
    INSPECT_TARGET_NPC     = 2,
    INSPECT_TARGET_PLAYER  = 3,
    INSPECT_TARGET_CARGO_POD = 4,

    INSPECT_SNAPSHOT_MAX_ROWS = 8,
    INSPECT_SNAPSHOT_HEADER   = 11,
    INSPECT_SNAPSHOT_ROW      = 142,
    INSPECT_ROW_HAS_RECEIPT   = 1 << 0,
    INSPECT_ROW_GROUPED       = 1 << 1,
    INSPECT_ROW_DIAGNOSTIC    = 1 << 2,
    INSPECT_ROW_STATION_RECEIPT = 1 << 3,
    INSPECT_ROW_RELAY_RECEIPT   = 1 << 4,
};

typedef enum {
    INSPECT_DIAG_NONE = 0,
    INSPECT_DIAG_MARKET_DEMAND,
    INSPECT_DIAG_MARKET_SUPPLY,
    INSPECT_DIAG_ROUTE_DANGER,
    INSPECT_DIAG_ROUTE_SUCCESS,
    INSPECT_DIAG_DELIVERY_RECEIPT,
    INSPECT_DIAG_RECEIPT_LINK,
    INSPECT_DIAG_ROUTE_REPUTATION,
    INSPECT_DIAG_ROUTE_RISK,
    INSPECT_DIAG_STATION_TRUST,
    INSPECT_DIAG_STATION_RISK,
    INSPECT_DIAG_JOB_MINE,
    INSPECT_DIAG_JOB_HAUL,
    INSPECT_DIAG_JOB_TOW,
    INSPECT_DIAG_JOB_DELIVER_PROOF,
    INSPECT_DIAG_JOB_SCOUT,
    INSPECT_DIAG_JOB_REPAIR,
    INSPECT_DIAG_HNN_TRACE,
} inspect_diag_kind_t;

enum {
    INSPECT_JOB_FACTOR_VALUE = 0,
    INSPECT_JOB_FACTOR_DEMAND,
    INSPECT_JOB_FACTOR_SUPPLY,
    INSPECT_JOB_FACTOR_ROUTE,
    INSPECT_JOB_FACTOR_FRESHNESS,
    INSPECT_JOB_FACTOR_CAPABILITY,
    INSPECT_JOB_FACTOR_PROOF,
    INSPECT_JOB_FACTOR_HOLOGRAM,
    INSPECT_JOB_FACTOR_COUNT,
};

enum {
    INSPECT_JOB_META_REASON = INSPECT_JOB_FACTOR_COUNT,
    INSPECT_JOB_META_MEMORY_KIND,
    INSPECT_JOB_META_HOPS,
    INSPECT_JOB_META_AGE,
    INSPECT_JOB_META_SOURCE_STATION,
    INSPECT_JOB_META_PROOF_KIND,
    INSPECT_JOB_META_PROOF0,
    INSPECT_JOB_META_PROOF1,
    INSPECT_JOB_META_PROOF2,
    INSPECT_JOB_META_PROOF3,
};

enum {
    INSPECT_HNN_TRACE_LOAD = 0,
    INSPECT_HNN_TRACE_FIDELITY,
    INSPECT_HNN_TRACE_MARGIN,
    INSPECT_HNN_TRACE_SNR,
    INSPECT_HNN_TRACE_FLAGS,
    INSPECT_HNN_TRACE_KEYGEN_VERSION,
    INSPECT_HNN_TRACE_ENCODER_VERSION,
    INSPECT_HNN_TRACE_FORMAT_VERSION,
    INSPECT_HNN_TRACE_CAPACITY_LO,
    INSPECT_HNN_TRACE_CAPACITY_HI,
    INSPECT_HNN_TRACE_DIM_LO,
    INSPECT_HNN_TRACE_DIM_HI,
};

enum {
    INSPECT_HNN_TRACE_WARN_NOISY = 1 << 0,
    INSPECT_HNN_TRACE_WARN_LOW_MARGIN = 1 << 1,
    INSPECT_HNN_TRACE_WARN_UNTRAINED = 1 << 2,
};

typedef enum {
    INSPECT_JOB_REASON_NONE = 0,
    INSPECT_JOB_REASON_LOCAL_CONTRACT,
    INSPECT_JOB_REASON_MARKET_DEMAND,
    INSPECT_JOB_REASON_REMOTE_SUPPLY,
    INSPECT_JOB_REASON_RECEIPT_PROOF,
    INSPECT_JOB_REASON_STATION_TRUST,
    INSPECT_JOB_REASON_STATION_RISK,
    INSPECT_JOB_REASON_HNN_RESONANCE,
    INSPECT_JOB_REASON_ORE_PRESSURE,
    INSPECT_JOB_REASON_CONSTRUCTION_PLAN,
    INSPECT_JOB_REASON_DELIVERY_PROOF,
    INSPECT_JOB_REASON_DISTRESS_SIGNAL,
    INSPECT_JOB_REASON_REPAIR_NEED,
    INSPECT_JOB_REASON_ROUTE_MEMORY,
    INSPECT_JOB_REASON_ROUTE_RISK,
    INSPECT_JOB_REASON_GOSSIP_COURIER,
} inspect_job_reason_t;

typedef enum {
    INSPECT_JOB_PROOF_NONE = 0,
    INSPECT_JOB_PROOF_SUBJECT_HASH,
    INSPECT_JOB_PROOF_CHAIN_ANCHOR,
    INSPECT_JOB_PROOF_WITNESS_HASH,
} inspect_job_proof_t;

#define INSPECT_SNAPSHOT_MAX_SIZE \
    (INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_MAX_ROWS * INSPECT_SNAPSHOT_ROW)

static inline bool inspect_snapshot_unit_is_groupable(const cargo_unit_t *u) {
    if (!u) return false;
    if (u->commodity >= COMMODITY_COUNT) return false;
    if (u->grade >= MINING_GRADE_COUNT) return false;
    switch ((cargo_kind_t)u->kind) {
    case CARGO_KIND_INGOT:
        return (ingot_prefix_t)u->prefix_class == INGOT_PREFIX_ANONYMOUS;
    case CARGO_KIND_FRAME:
        return u->commodity == COMMODITY_FRAME;
    case CARGO_KIND_LASER:
        return u->commodity == COMMODITY_LASER_MODULE;
    case CARGO_KIND_TRACTOR:
        return u->commodity == COMMODITY_TRACTOR_MODULE;
    default:
        return false;
    }
}

/* Client-hashed fracture window */
#define FRACTURE_CHALLENGE_BURST_CAP 50
#define FRACTURE_CLAIM_WINDOW_MS     1500u
#define FRACTURE_CHALLENGE_SIZE      (1 + 4 + 32 + 4 + 2)
#define FRACTURE_CLAIM_SIZE          (1 + 4 + 4 + 1)
#define FRACTURE_RESOLVED_SIZE       (1 + 4 + 32 + 32 + 1)

/* Transient interaction visuals (NET_MSG_WORLD_INTERACTIONS):
 * [type:1][count:1] + count * INTERACTION_RECORD_SIZE
 * record:
 * [type:1][visual:1][commodity:1][flags:1]
 * [source_type:1][source_index:i16][source_aux:i16]
 * [target_type:1][target_index:i16][target_aux:i16]
 * [source_x:f32][source_y:f32][target_x:f32][target_y:f32]
 * [range:f32][intensity:f32]
 */
#define INTERACTION_RECORD_SIZE 38
#define INTERACTION_Q_RECORD_SIZE 25
#define INTERACTION_DRIFT_MSG_HEADER 2
#define INTERACTION_DRIFT_RECORD_SIZE 12
#define INTERACTION_DRIFT_POS_SCALE 4.0f
#define INTERACTION_DRIFT_RANGE_SCALE 4.0f

/* Signal channel wire record:
 *   [id:u64][ts_ms:u32][sender:i8][text_len:u8][text:200][entry_hash:32] = 246 bytes
 * audio_url is server-side only for V1; agents read it via REST.
 * entry_hash is the SHA-256 chain link — clients can recompute and
 * verify against this value to detect tampering / desync. */
#define SIGNAL_CHANNEL_RECORD_SIZE (8 + 4 + 1 + 1 + 200 + 32)

/* ------------------------------------------------------------------ */
/* Plan operations (NET_MSG_PLAN payload byte 1)                      */
/* Layout: [type:1][op:1][station:1][ring:1][slot:1][module_type:1]   */
/*         [px:f32][py:f32]  = 14 bytes                               */
/* ------------------------------------------------------------------ */

enum {
    NET_PLAN_OP_NONE              = 0,
    NET_PLAN_OP_CREATE_OUTPOST    = 1, /* uses px,py */
    NET_PLAN_OP_ADD_SLOT          = 2, /* uses station,ring,slot,module_type */
    NET_PLAN_OP_CANCEL_OUTPOST    = 3, /* uses station */
    /* Atomic create + first plan: outpost is born at (px,py) AND the
     * first slot plan is added in the same input pulse. The client uses
     * this when leaving "ghost preview" mode so the lock-in position is
     * the player's current ship pos at the moment they pressed E. */
    NET_PLAN_OP_CREATE_AND_ADD    = 4, /* uses px,py + ring,slot,module_type */
    /* Cancel a single plan slot (red/clear state). */
    NET_PLAN_OP_CANCEL_PLAN_SLOT  = 5, /* uses station,ring,slot */
};

#define NET_PLAN_MSG_SIZE 14

/* ------------------------------------------------------------------ */
/* Input flags (client -> server), packed into one byte                */
/* ------------------------------------------------------------------ */

enum {
    NET_INPUT_THRUST = 1 << 0,
    NET_INPUT_LEFT   = 1 << 1,
    NET_INPUT_RIGHT  = 1 << 2,
    NET_INPUT_FIRE   = 1 << 3,
    NET_INPUT_BRAKE   = 1 << 4,
    NET_INPUT_TRACTOR = 1 << 5,
    NET_INPUT_BOOST   = 1 << 6,
    NET_INPUT_REVERSE = 1 << 7,
};

/* ------------------------------------------------------------------ */
/* Station action byte values (sent inside INPUT packets)             */
/* ------------------------------------------------------------------ */

enum {
    NET_ACTION_NONE           = 0,
    NET_ACTION_DOCK           = 1,
    NET_ACTION_LAUNCH         = 2,
    NET_ACTION_SELL_CARGO     = 3,
    NET_ACTION_REPAIR         = 4,
    NET_ACTION_UPGRADE_MINING = 5,
    NET_ACTION_UPGRADE_HOLD   = 6,
    NET_ACTION_UPGRADE_TRACTOR= 7,
    NET_ACTION_PLACE_OUTPOST  = 8,
    NET_ACTION_BUILD_MODULE   = 9,  /* DEPRECATED #259 — legacy build menu, no-op on server */
    NET_ACTION_BUY_SCAFFOLD   = 25,
    NET_ACTION_HAIL           = 26,  /* signal hail/contact scan */
    NET_ACTION_RELEASE_TOW    = 27,  /* tap Space: release towed bodies */
    NET_ACTION_RESET          = 28,  /* self-destruct — respawn at nearest station */
    NET_ACTION_BUY_INGOT      = 29,  /* direct named-ingot purchase; result/telemetry only */
    NET_ACTION_BUY_PRODUCT    = 30, /* +commodity offset, range [30..30+COMMODITY_COUNT) */
    NET_ACTION_PLACE_MODULE   = 49, /* DEPRECATED #259 — legacy placement, no-op on server (range sentinel) */
    NET_ACTION_BUY_SCAFFOLD_TYPED = 50, /* +module_type offset, range [50..50+MODULE_COUNT) */
    NET_ACTION_DELIVER_COMMODITY  = 70, /* +commodity offset, range [70..70+COMMODITY_COUNT) */
    NET_ACTION_AUTOPILOT_TOGGLE   = 90, /* toggle player mining autopilot on/off */
    NET_ACTION_COMMISSION_SHIP     = 91, /* +hull_class offset, range [91..91+HULL_CLASS_COUNT) */
    NET_ACTION_PRESENT_POD        = 100, /* source-local receipt-backed POD unpack */
};

enum {
    NET_ACTION_ACK_RECEIVED  = 1,
    NET_ACTION_ACK_DUPLICATE = 2,
    NET_ACTION_ACK_REJECTED  = 3,
};

#define NET_ACTION_ACK_SIZE 7

enum {
    NET_ACTION_RESULT_OK       = 1,
    NET_ACTION_RESULT_REJECTED = 2,
    NET_ACTION_RESULT_NOOP     = 3,
};

#define NET_ACTION_RESULT_SIZE 11
#define NET_HANDOFF_REQUEST_SIZE 7
#define NET_HANDOFF_RESULT_SIZE 36

enum {
    NET_HANDOFF_STATUS_OK       = 1,
    NET_HANDOFF_STATUS_REJECTED = 2,
};

#define NET_INPUT_LEGACY_SIZE 18
#define NET_INPUT_MSG_SIZE 22
#define NET_STATE_MSG_SIZE 45
#define NET_STATE_AUTH_LEGACY_SIZE 55
#define NET_STATE_AUTH_SIZE 67
#define NET_STATE_AUTH_INPUT_ACK_OFFSET NET_STATE_MSG_SIZE
#define NET_STATE_AUTH_SERVER_TICK_OFFSET (NET_STATE_MSG_SIZE + 2)
#define NET_STATE_AUTH_INPUT_TICK_OFFSET (NET_STATE_MSG_SIZE + 6)
#define NET_STATE_AUTH_CLIENT_SENT_MS_OFFSET NET_STATE_AUTH_LEGACY_SIZE
#define NET_STATE_AUTH_SERVER_RECV_MS_OFFSET (NET_STATE_AUTH_LEGACY_SIZE + 4)
#define NET_STATE_AUTH_SERVER_SEND_MS_OFFSET (NET_STATE_AUTH_LEGACY_SIZE + 8)
#define NET_INPUT_APPLIED_LEGACY_SIZE 11
#define NET_INPUT_APPLIED_SIZE 23
#define NET_LATENCY_PING_SIZE 9
#define NET_LATENCY_PONG_LEGACY_SIZE 17
#define NET_LATENCY_PONG_SIZE 21
#define NET_CLIENT_METRICS_SIZE 21
#define NET_CLIENT_METRICS_ACK_TIER_MASK 0x03u
#define NET_CLIENT_METRICS_PING_FRESH 0x04u
#define NET_CLIENT_METRICS_ACK_FRESH 0x08u
#define NET_CLIENT_METRICS_PING_MISSED 0x10u
#define NET_CLIENT_METRICS_ACK_MISSED 0x20u
#define NET_DEATH_MSG_SIZE 43

#define NET_INPUT_ACTIVE_HEARTBEAT_MS 100u
#define NET_INPUT_ACTIVE_ACK_HEARTBEAT_MS 100u
#define NET_INPUT_IDLE_HEARTBEAT_MS 1000u

/* Input prediction horizon, in fixed SIM_DT ticks. The server only accepts
 * future-dated movement this far ahead; the client should rebase before replay
 * history grows into visible rollback territory. */
#define NET_INPUT_LEAD_MIN_TICKS 1u
#define NET_INPUT_LEAD_MAX_TICKS 12u
#define NET_INPUT_APPLY_FUTURE_MAX_TICKS 12u
#define NET_REPLAY_REBASE_SKEW_TICKS (NET_INPUT_APPLY_FUTURE_MAX_TICKS * 2u)

/* NET_MSG_CONTRACTS record: action, station, commodity, grade, provenance
 * requirements, origin bans, quantity, price, age, target position, target
 * index, and stable target pubkey. Kept shared so client decoders and
 * external tools do not hardcode server-local constants. */
#define CONTRACT_RECORD_SIZE 104
#define CONTRACT_Q_HEADER_SIZE 2
#define CONTRACT_Q_BASE_SIZE 32
#define CONTRACT_Q_FLAG_PARENT 0x01u
#define CONTRACT_Q_FLAG_ORIGIN_MASK 0x02u
#define CONTRACT_Q_FLAG_TARGET_PUB 0x04u
#define CONTRACT_Q_FLAG_MASK 0x07u
#define CONTRACT_Q_MAX_RECORD_SIZE \
    (1 + CONTRACT_Q_BASE_SIZE + 32 + 8 + 32)
#define CONTRACT_Q_MAX_SIZE \
    (CONTRACT_Q_HEADER_SIZE + MAX_CONTRACTS * CONTRACT_Q_MAX_RECORD_SIZE)

/* NET_MSG_DELIVERY_LEDGER record:
 * shipment_id:u16, status:u8, origin:u8, destination:u8, contract_index:u8,
 * commodity:u8, quantity_total:u16, quantity_delivered:u16,
 * quantity_bound:u16, debt_principal:f32, destination_payout:f32,
 * origin_credit:f32, due_tick:u32, held_bound:u16, reserved:1. */
#define DELIVERY_LEDGER_HEADER 2
#define DELIVERY_LEDGER_RECORD_SIZE 32
#define DELIVERY_LEDGER_MAX_RECORDS 24

/* NET_MSG_PLAYER_KNOWN_LEDGER record:
 * station:u8, balance:f32. */
#define PLAYER_KNOWN_LEDGER_HEADER 2
#define PLAYER_KNOWN_LEDGER_RECORD_SIZE 5
#define PLAYER_KNOWN_LEDGER_MAX_RECORDS MAX_STATIONS

/* NET_MSG_PLAYER_MARKET_MEMORIES record:
 * kind:u8, station_a:u8, station_b:u8, commodity:u8, action:u8,
 * confidence:u8, salience:u8, hops:u8, quantity_hint:u16,
 * value_hint:u16, observed_tick:u32, subject_nonce:u64. */
#define PLAYER_MARKET_MEMORIES_HEADER 2
#define PLAYER_MARKET_MEMORY_RECORD_SIZE 24
#define PLAYER_MARKET_MEMORY_MAX_RECORDS SHIP_KNOWN_ITEM_CAP

/* ------------------------------------------------------------------ */
/* Event broadcast (NET_MSG_EVENTS)                                   */
/* ------------------------------------------------------------------ */

/* Fixed-size legacy event record:
 * [type:1][player_id:1][payload:16] = 18 bytes.
 * Protocol-v5 payload meaning depends on type. Unused bytes and the bytes
 * formerly used for bearer attribution are reserved and zero. */
enum {
    NET_EVENT_RECORD_SIZE   = 18,
};

/*
 * Protocol-v6 public event record:
 *   [type:1][presentation_player_id:1]
 *   [subject_public_actor:33][source_public_actor:33][payload:16]
 *
 * player_id remains a presentation hint only. The typed public actor IDs are
 * the attribution keys; callsigns are never part of identity.
 */
enum {
    NET_EVENT_V2_HEADER_SIZE          = 2,
    NET_EVENT_V2_SUBJECT_OFFSET       = 2,
    NET_EVENT_V2_SOURCE_OFFSET        =
        NET_EVENT_V2_SUBJECT_OFFSET + PUBLIC_ACTOR_ID_WIRE_SIZE,
    NET_EVENT_V2_PAYLOAD_OFFSET       =
        NET_EVENT_V2_SOURCE_OFFSET + PUBLIC_ACTOR_ID_WIRE_SIZE,
    NET_EVENT_V2_PAYLOAD_SIZE         = 16,
    NET_EVENT_V2_RECORD_SIZE          =
        NET_EVENT_V2_PAYLOAD_OFFSET + NET_EVENT_V2_PAYLOAD_SIZE,
};

_Static_assert(NET_EVENT_V2_RECORD_SIZE == 84,
               "public event v2 record layout drifted");

/*
 * Attribution-only decoders are shared so tests can pin the trust boundary
 * without linking the platform network client. Legacy reserved bytes are
 * intentionally ignored; there is no compatibility path from bearer-shaped
 * data to public identity.
 */
static inline void net_event_decode_legacy_actor_fields(
    const uint8_t record[NET_EVENT_RECORD_SIZE],
    sim_event_t *event) {
    (void)record;
    if (!event ||
        (event->type != SIM_EVENT_DEATH &&
         event->type != SIM_EVENT_NPC_KILL)) {
        return;
    }
    event->subject_actor =
        public_actor_id_legacy_unattributed();
    event->source_actor =
        public_actor_id_legacy_unattributed();
}

static inline bool net_event_v2_decode_actor_fields(
    const uint8_t record[NET_EVENT_V2_RECORD_SIZE],
    sim_event_t *event) {
    if (!record || !event ||
        !public_actor_id_unpack(
            &record[NET_EVENT_V2_SUBJECT_OFFSET],
            &event->subject_actor) ||
        !public_actor_id_unpack(
            &record[NET_EVENT_V2_SOURCE_OFFSET],
            &event->source_actor) ||
        event->subject_actor.kind ==
            (uint8_t)PUBLIC_ACTOR_ID_NONE ||
        event->source_actor.kind ==
            (uint8_t)PUBLIC_ACTOR_ID_NONE) {
        if (event) {
            event->subject_actor = public_actor_id_none();
            event->source_actor = public_actor_id_none();
        }
        return false;
    }
    return true;
}

/* Compile-time check: action ranges must not overlap.
 * BUILD_MODULE is deprecated and no-op, so its range collapses to a
 * single byte; new module types can grow MODULE_COUNT freely. */
_Static_assert(NET_ACTION_BUY_SCAFFOLD < NET_ACTION_BUY_PRODUCT,
               "BUY_SCAFFOLD overlaps BUY_PRODUCT range");
_Static_assert(NET_ACTION_BUY_PRODUCT + COMMODITY_COUNT <= NET_ACTION_PLACE_MODULE,
               "BUY_PRODUCT range overlaps PLACE_MODULE");
_Static_assert(NET_ACTION_BUY_SCAFFOLD_TYPED + MODULE_COUNT <= NET_ACTION_DELIVER_COMMODITY,
               "BUY_SCAFFOLD_TYPED overlaps DELIVER_COMMODITY range");
_Static_assert(NET_ACTION_DELIVER_COMMODITY + COMMODITY_COUNT <= 256,
               "DELIVER_COMMODITY range overflows uint8_t");
_Static_assert(NET_ACTION_COMMISSION_SHIP + HULL_CLASS_COUNT <= 256,
               "COMMISSION_SHIP range overflows uint8_t");

/* ------------------------------------------------------------------ */
/* Record sizes                                                       */
/* ------------------------------------------------------------------ */

/* Station economic snapshot: [index:1][inventory:COMMODITY_COUNT×f32] */
#define STATION_RECORD_SIZE (1 + COMMODITY_COUNT * 4 + 4)  /* 41 bytes: index + inventory + credit_pool */
#define STATION_Q_HEADER_SIZE 2
#define STATION_Q_CREDIT_POOL_MASK 0x8000u
#define STATION_Q_COMMODITY_MASK \
    ((uint16_t)((1u << COMMODITY_COUNT) - 1u))
#define STATION_Q_MAX_RECORD_SIZE (1 + 2 + COMMODITY_COUNT * 4 + 4)
#define STATION_Q_MAX_SIZE \
    (STATION_Q_HEADER_SIZE + MAX_STATIONS * STATION_Q_MAX_RECORD_SIZE)

/* Player state record: [id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1][tractor_lvl:1][towed_count:1][towed_frags:20][callsign:7]
 * [beam_start_x:f32][beam_start_y:f32][beam_end_x:f32][beam_end_y:f32]
 * [input_ack:u16][server_tick:u32][input_tick_ack:u32]
 * towed_frags: 10 × uint16_t asteroid index, 0xFFFF = unused. Widened
 * from uint8_t in #285 Phase 3 so slots 255-2047 survive the wire.
 * flags bits: 1=thrust 2=beam_active 4=docked 8=scan 16=tractor 32=beam_ineffective 64=beam_hit
 * Beam coords are server-authoritative — fixes autopilot mining visuals
 * and (eventually) combat hit prediction. input_ack/server_tick let the
 * client reconcile against the server state age instead of blindly pulling
 * predicted controls back to a stale packet. input_tick_ack is the sim tick
 * where that input was actually applied. */
#define PLAYER_RECORD_SIZE 77  /* 51 + 16 beam coords + 2 ack + 4 pose tick + 4 input tick */
#define PLAYER_MOTION_MSG_HEADER 2  /* type + count */
#define PLAYER_MOTION_RECORD_SIZE 21 /* id + pos/vel/angle floats */
#define PLAYER_MOTION_Q_MSG_HEADER 2  /* type + count */
#define PLAYER_MOTION_Q_RECORD_SIZE 10 /* id + pos/vel i16 + angle u8 */
#define PLAYER_MOTION_Q_POS_SCALE 4.0f
#define PLAYER_MOTION_Q_VEL_SCALE 0.25f
#define PLAYER_MOTIOND_Q_MSG_HEADER 2  /* type + count */
#define PLAYER_MOTIOND_Q_RECORD_SIZE 6 /* id + pos delta i8 + vel i8 + angle u8 */
#define PLAYER_MOTIOND_Q_VEL_SCALE 4.0f
#define PLAYER_POSED_Q_MSG_HEADER 2  /* type + count */
#define PLAYER_POSED_Q_RECORD_SIZE 4 /* id + pos delta i8 + angle u8 */
#define PLAYER_MOTIONM_Q_MSG_HEADER 2  /* type + count */
#define PLAYER_MOTIONM_Q_ID_MASK 0x1Fu
#define PLAYER_MOTIONM_Q_FLAG_VEL 0x80u
#define PLAYER_MOTIONM_Q_RESERVED_MASK 0x60u
#define PLAYER_MOTIONM_Q_POSE_RECORD_SIZE 4 /* id + pos delta i8 + angle u8 */
#define PLAYER_MOTIONM_Q_VEL_RECORD_SIZE 6 /* id|flag + pos delta i8 + vel i8 + angle u8 */
#define PLAYER_MOTIONM_Q_MAX_RECORD_SIZE PLAYER_MOTIONM_Q_VEL_RECORD_SIZE
#define PLAYER_DOCK_MSG_HEADER 2  /* type + count */
#define PLAYER_DOCK_RECORD_SIZE 2 /* id + compact status flags byte */
#define PLAYER_DOCK_STATUS_FLAGS_MASK 0x05u /* thrusting + docked */

/* Canonical tow relationship snapshot. Each entity reference is encoded as
 * [kind:u8][index:i16][part:i16][generation:u16]. The record then carries
 * [profile:u8][slot:u8][state:u8][reserved:u8]
 * [attached_tick:u32][revision:u32]. */
#define TOW_LINKS_MSG_HEADER_SIZE 11
#define TOW_LINK_RECORD_SIZE 26
#define TOW_LINKS_MAX_SIZE \
    (TOW_LINKS_MSG_HEADER_SIZE + MAX_TOW_LINKS * TOW_LINK_RECORD_SIZE)

/* Asteroid record: [index:2][flags:1][pos:2xf32][vel:2xf32][hp:f32][ore:f32][radius:f32]
 * [smelt:u8][grade:u8][crystal_stage:u8][phase:u8] */
#define ASTEROID_MSG_HEADER 3  /* type + uint16 count */
#define ASTEROID_RECORD_SIZE 35  /* uint16 index + flags + 7 floats + smelt:u8 + grade:u8 + crystal_stage:u8 + phase:u8 */
#define ASTEROID_Q_MSG_HEADER 3  /* type + uint16 count */
#define ASTEROID_Q_RECORD_SIZE 19
#define ASTEROID8_Q_MSG_HEADER 2  /* type + uint8 count */
#define ASTEROID8_Q_RECORD_SIZE 18

/* Compact asteroid motion record: [index:2][pos:2xf32][vel:2xf32].
 * Used only for clean already-known moving rocks; static/dirty state remains
 * on NET_MSG_WORLD_ASTEROIDS and removals use NET_MSG_WORLD_ASTEROID_REMOVE. */
#define ASTEROID_MOTION_MSG_HEADER 3  /* type + uint16 count */
#define ASTEROID_MOTION_RECORD_SIZE 18
#define ASTEROID_MOTION_Q_MSG_HEADER 3  /* type + uint16 count */
#define ASTEROID_MOTION_Q_RECORD_SIZE 10
#define ASTEROID_POS_Q_MSG_HEADER 3  /* type + uint16 count */
#define ASTEROID_POS_Q_RECORD_SIZE 6
#define ASTEROID_POS8_Q_MSG_HEADER 2  /* type + uint8 count */
#define ASTEROID_POS8_Q_RECORD_SIZE 5
#define ASTEROID_POSD_Q_MSG_HEADER 3  /* type + uint16 count */
#define ASTEROID_POSD_Q_RECORD_SIZE 4
#define ASTEROID_POSD8_Q_MSG_HEADER 2  /* type + uint8 count */
#define ASTEROID_POSD8_Q_RECORD_SIZE 3
#define ASTEROID_REMOVE_MSG_HEADER 3  /* type + uint16 count */
#define ASTEROID_REMOVE_RECORD_SIZE 2
#define ASTEROID_MOTION_Q_POS_SCALE 4.0f
#define ASTEROID_MOTION_Q_VEL_SCALE 0.25f
#define ASTEROID_IDENTITY_Q_VALUE_SCALE 0.125f
#define ASTEROID_STATE_Q_MSG_HEADER 3  /* type + uint16 count */
#define ASTEROID_STATE_Q_RECORD_SIZE 18

/* Cargo pod record: [index:1][kind:1][commodity:1][towed_by:1][pos:2xf32]
 * [vel:2xf32][radius:f32][rotation:f32][quantity:u16]
 * [manifest_count:u16][shipment_id:u16][flags:1][best_grade:1]
 * [tractor_station_tag:1][tractor_module_tag:1][tow_hardpoint_tag:1]
 * [custody_station_tag:1][selection_token:32].
 * Manifest unit rows are not present on this live stream; the flags carry
 * server-derived summary truth for UI gates, while the opaque token lets a
 * client bind a signed PRESENT action without learning private manifest rows. */
#define CARGO_POD_RECORD_SIZE 72
#define CARGO_POD_Q_RECORD_SIZE 62
_Static_assert(CARGO_POD_Q_RECORD_SIZE ==
               (4 + 4 * 2 + 4 + 2 + 2 + 2 + 2 + 1 + 1 + 2 + 1 + 1 + 32),
               "compact cargo pod identity record size drifted");
#define CARGO_POD_MOTION_MSG_HEADER 2
#define CARGO_POD_MOTION_RECORD_SIZE 21
#define CARGO_POD_MOTION_Q_MSG_HEADER 2
#define CARGO_POD_MOTION_Q_RECORD_SIZE 11
#define CARGO_POD_LINEAR_Q_MSG_HEADER 2
#define CARGO_POD_LINEAR_Q_RECORD_SIZE 9
#define CARGO_POD_REMOVE_MSG_HEADER 2
#define CARGO_POD_REMOVE_RECORD_SIZE 1
#define CARGO_POD_MOTION_Q_POS_SCALE 4.0f
#define CARGO_POD_MOTION_Q_VEL_SCALE 0.25f
enum {
    CARGO_POD_SUMMARY_EXACT_MATERIAL = 1u << 0, /* exact, unbound manifest matching commodity */
    CARGO_POD_SUMMARY_SHIPMENT_BOUND = 1u << 1,
};

/* NPC record: [id:1][flags:1][pos:2xf32][vel:2xf32][angle:f32]
 * [target:u16][towed_fragment:u16][rarity_tint:3][reserved_zero:8]
 * [home_station:1], 0xFFFF = none.
 *
 * Protocol v6 retires the token-derived NPC identity tail. Bytes 29..36 are
 * pinned to zero and must be ignored by clients; slot/role/home station are
 * presentation and routing metadata, never public identity. */
#define NPC_RECORD_SIZE 38
#define NPC_RECORD_RESERVED_IDENTITY_OFFSET 29
#define NPC_RECORD_RESERVED_IDENTITY_SIZE 8
#define NPC_RECORD_HOME_STATION_OFFSET 37
_Static_assert(NPC_RECORD_RESERVED_IDENTITY_OFFSET +
                   NPC_RECORD_RESERVED_IDENTITY_SIZE ==
               NPC_RECORD_HOME_STATION_OFFSET,
               "NPC reserved-zero field layout drifted");
_Static_assert(NPC_RECORD_HOME_STATION_OFFSET + 1 == NPC_RECORD_SIZE,
               "NPC record tail layout drifted");
#define NPC_MOTION_MSG_HEADER 2  /* type + count */
#define NPC_MOTION_RECORD_SIZE 22 /* index + flags + pos/vel/angle floats */
#define NPC_MOTION_Q_MSG_HEADER 2  /* type + count */
#define NPC_MOTION_Q_RECORD_SIZE 12 /* index + flags + pos/vel/angle quantized */
#define NPC_MOTION8_Q_MSG_HEADER 2
#define NPC_MOTION8_Q_RECORD_SIZE 9 /* index + flags + pos, i8 vel, u8 angle */
#define NPC_POS_Q_MSG_HEADER 2
#define NPC_POS_Q_RECORD_SIZE 5
#define NPC_POSE_Q_MSG_HEADER 2
#define NPC_POSE_Q_RECORD_SIZE 7
#define NPC_LINEAR_Q_MSG_HEADER 2
#define NPC_LINEAR_Q_RECORD_SIZE 9
#define NPC_MOTION_Q_POS_SCALE 4.0f
#define NPC_MOTION_Q_VEL_SCALE 0.25f
#define NPC_MOTION_Q_ANGLE_SCALE (6.28318530717958647692f / 65536.0f)
#define NPC_MOTION8_Q_VEL_SCALE 2.0f
#define NPC_MOTION8_Q_ANGLE_SCALE (6.28318530717958647692f / 256.0f)
#define NPC_STATUS_MSG_HEADER 2
#define NPC_STATUS_RECORD_SIZE 6 /* index + flags + target + towed_fragment */
#define NPC_STATUS8_MSG_HEADER 2
#define NPC_STATUS8_RECORD_SIZE 4 /* index + flags + target8 + towed8 */

/* Station identity: [index:1][flags:1][services:4][pos:2xf32][radius:f32][dock_radius:f32][signal_range:f32][name:32]
 * [base_price:COMMODITY_COUNT×f32][scaffold_progress:f32][module_count:1][modules:MAX_MODULES×9]
 * [arm_count:1][arm_speed:MAX_ARMS×f32][ring_offset:MAX_ARMS×f32]
 * [plan_count:1][plans:8 × (type:1, ring:1, slot:1, owner:1)]
 * [pending_count:1][pending:4 × (type:1, owner:1)]
 * [pending_ship_count:1][pending_ship:4 × (hull:1, reserved:1, progress:f32)]
 * [...text trailers...][station_pubkey:32]
 * [stored_hull_count:HULL_CLASS_COUNT×u8] -- appended so v1 trailer offsets stay stable.
 * flags: bit0=scaffold, bit1=planned */
#define STATION_MODULE_RECORD_SIZE 9  /* type:1 + scaffold:1 + ring:1 + slot:1 + build_progress:f32 + commodity:1 */
#define STATION_PLAN_RECORD_SIZE 4    /* type:1 + ring:1 + slot:1 + owner:1 */
#define STATION_PLAN_RECORD_COUNT 8
#define STATION_PENDING_SCAFFOLD_RECORD_SIZE 2  /* type:1 + owner:1 */
#define STATION_PENDING_SCAFFOLD_RECORD_COUNT 4
#define STATION_PENDING_SHIP_RECORD_SIZE 6      /* hull:1 + reserved(0xFF):1 + progress:f32 */
#define STATION_PENDING_SHIP_RECORD_COUNT 4
#define STATION_IDENTITY_HAIL_MESSAGE_LEN 256  /* trailer: station MOTD/hail copy */
#define STATION_IDENTITY_CHATTER_LINES 8
#define STATION_IDENTITY_CHATTER_LINE_LEN 64
#define STATION_IDENTITY_RATI_HAIL_LEN 256
#define STATION_IDENTITY_CURRENCY_NAME_LEN 32  /* trailer: per-station scrip label */
#define STATION_IDENTITY_PUBKEY_LEN 32         /* Ed25519 station identity (#479 B) */
#define STATION_IDENTITY_FACTION_SIZE (3 + STATION_FACTION_COUNT)
#define STATION_IDENTITY_POLICY_CARD_COUNT 8
#define STATION_IDENTITY_POLICY_SIZE (1 + STATION_IDENTITY_POLICY_CARD_COUNT)
#define STATION_IDENTITY_V1_SIZE (59 + COMMODITY_COUNT * 4 + 4 \
    + 1 + MAX_MODULES_PER_STATION * STATION_MODULE_RECORD_SIZE \
    + 1 + MAX_ARMS * 4 + MAX_ARMS * 4 + MAX_ARMS * 4 + MAX_ARMS * 4 \
    + 1 + STATION_PLAN_RECORD_COUNT * STATION_PLAN_RECORD_SIZE \
    + 1 + STATION_PENDING_SCAFFOLD_RECORD_COUNT * STATION_PENDING_SCAFFOLD_RECORD_SIZE \
    + 1 + STATION_PENDING_SHIP_RECORD_COUNT * STATION_PENDING_SHIP_RECORD_SIZE \
    + STATION_IDENTITY_HAIL_MESSAGE_LEN \
    + STATION_IDENTITY_CHATTER_LINES * STATION_IDENTITY_CHATTER_LINE_LEN \
    + STATION_IDENTITY_CHATTER_LINES * STATION_IDENTITY_CHATTER_LINE_LEN \
    + STATION_IDENTITY_RATI_HAIL_LEN \
    + STATION_IDENTITY_CURRENCY_NAME_LEN \
    + STATION_IDENTITY_PUBKEY_LEN)
#define STATION_IDENTITY_HULL_SIZE (STATION_IDENTITY_V1_SIZE + HULL_CLASS_COUNT)
#define STATION_IDENTITY_FACTION_TRAILER_SIZE (STATION_IDENTITY_HULL_SIZE + STATION_IDENTITY_FACTION_SIZE)
#define STATION_IDENTITY_SIZE (STATION_IDENTITY_FACTION_TRAILER_SIZE + STATION_IDENTITY_POLICY_SIZE)
#define STATION_IDENTITY_Q_HEADER_SIZE 3
#define STATION_IDENTITY_Q_MAX_SIZE STATION_IDENTITY_SIZE
#define STATION_DIAG_SIZE (3 + MAX_MODULES_PER_STATION)
/* The four "MAX_ARMS * 4" terms above are arm_speed[], ring_offset[],
 * arm_rotation[], and arm_omega[]. arm_rotation/arm_omega seed client-side
 * ring prediction on station snapshot; steady-state identity cache checks
 * treat ordinary live drift as non-semantic so it does not compete with the
 * ping/input-ack lane. */

/* Scaffold record: [id:1][state:1][module_type:1][owner:1]
 *                  [pos:2xf32][vel:2xf32][radius:f32][build_amount:f32]
 *                  [built_at_station:i8] = 29 bytes */
#define SCAFFOLD_RECORD_SIZE 29
#define SCAFFOLD_REMOVE_MSG_HEADER 2
#define SCAFFOLD_REMOVE_RECORD_SIZE 1
#define SCAFFOLD_MOTION_Q_MSG_HEADER 2
#define SCAFFOLD_MOTION_Q_RECORD_SIZE 9
#define SCAFFOLD_MOTION_Q_POS_SCALE 4.0f
#define SCAFFOLD_MOTION_Q_VEL_SCALE 0.25f

/* Player ship state: [type:1][id:1][hull:f32][credits:f32][docked:1][station:1]
 * [mining:1][hold:1][tractor:1][scaffold_kit:1][cargo:COMMODITY_COUNT×f32]
 * [nearby_frags:1][tractor_frags:1][towed_count:1][towed_frags:20]
 * [autopilot_target:1][path_count:1][path_current:1][waypoints: count×(x:f32,y:f32)]
 * towed_frags: 10 × uint16_t asteroid index, 0xFFFF = unused. */
#define PLAYER_SHIP_SIZE (42 + COMMODITY_COUNT * 4 + 12 * 8)  /* 174 bytes max */

#endif /* SHARED_PROTOCOL_H */
