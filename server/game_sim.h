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

/* ------------------------------------------------------------------ */
/* Constants (server-only)                                            */
/* ------------------------------------------------------------------ */

enum {
    MAX_PLAYERS = 32,
};

static const float WORLD_RADIUS = 50000.0f;  /* safety net; gameplay bounded by station signal_range */
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
    uint32_t h = (uint32_t)((cx * 73856093) ^ (cy * 19349663));
    for (uint32_t i = h & g->mask; ; i = (i + 1) & g->mask) {
        const sparse_cell_entry_t *e = &g->entries[i];
        if (e->key_x == INT32_MIN) return NULL;      /* empty slot */
        if (e->key_x == cx && e->key_y == cy) return &e->cell;
    }
}

/* ------------------------------------------------------------------ */
/* Cached signal strength grid — O(1) lookups instead of O(N_stations)*/
/* ------------------------------------------------------------------ */

#define SIGNAL_GRID_DIM  256
#define SIGNAL_CELL_SIZE 200.0f  /* covers ±25,600 units from origin */

typedef struct {
    float *strength;           /* heap-allocated SIGNAL_GRID_DIM² floats */
    float offset_x, offset_y;  /* world offset to center grid */
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

/* ------------------------------------------------------------------ */
/* Server-specific types                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    float turn;
    float thrust;
    bool mine;
    bool interact;
    bool service_sell;
    /* Selective delivery filter for service_sell. COMMODITY_COUNT means
     * "deliver everything that fits a contract or the primary buy slot"
     * (the default). Setting this to a specific commodity restricts the
     * delivery to that one commodity, so the player can keep e.g. their
     * crystal cargo while still delivering ferrite. */
    commodity_t service_sell_only;
    bool service_repair;
    bool upgrade_mining;
    bool upgrade_hold;
    bool upgrade_tractor;
    bool place_outpost;
    /* Optional explicit target for tow placement. If place_target_station >= 0,
     * the server places the towed scaffold at that ring/slot; otherwise it
     * auto-snaps to the closest valid slot or founds a new outpost. */
    int8_t place_target_station;
    int8_t place_target_ring;
    int8_t place_target_slot;
    /* Planning mode: add a placement plan to a station. */
    bool add_plan;
    int8_t plan_station;
    int8_t plan_ring;
    int8_t plan_slot;
    module_type_t plan_type;
    /* Create a new planned outpost (server-side ghost). */
    bool create_planned_outpost;
    vec2 planned_outpost_pos;
    /* Cancel a planned outpost (only the owner can). */
    bool cancel_planned_outpost;
    int8_t cancel_planned_station;
    /* Cancel a single placement plan on a station slot. */
    bool cancel_plan_slot;
    int8_t cancel_plan_st;
    int8_t cancel_plan_ring;
    int8_t cancel_plan_sl;
    bool buy_scaffold_kit;
    module_type_t scaffold_kit_module; /* what module type the kit builds */
    bool buy_product;
    commodity_t buy_commodity;
    /* Optional grade hint for manifest-first buys. MINING_GRADE_COUNT =
     * "any grade available, FIFO"; a specific grade means "only transfer
     * a unit of this grade — if none exist, the float path still runs
     * as a legacy common row". */
    mining_grade_t buy_grade;
    int mining_target_hint;  /* client's hover_asteroid, -1 = none */
    bool hail;               /* collect pending credits from nearby station */
    bool tractor_hold;       /* R held — tractor active this frame */
    bool release_tow;        /* R tapped — drop all towed fragments */
    bool reset;
    bool toggle_autopilot;   /* one-shot: flip autopilot_mode on/off */
    bool boost;              /* Shift held — thrust multiplier + hull drain */
} input_intent_t;

typedef struct {
    bool connected;
    uint8_t id;
    void *conn;
    uint8_t session_token[8]; /* stable identity for save persistence */
    bool session_ready;       /* true once client sends SESSION message */
    bool grace_period;        /* true while waiting for reconnect after disconnect */
    float grace_timer;        /* seconds remaining in grace window */
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
    int scan_target_type;  /* 0=none, 1=station_module, 2=npc, 3=player */
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
    int autopilot_state;        /* internal state machine cursor */
    float autopilot_timer;
    vec2 autopilot_last_pos;    /* position snapshot for stuck detection */
    float autopilot_stuck_timer;/* seconds since meaningful movement */
    /* Per-player relevance: tracks which asteroids this player has received */
    bool asteroid_sent[MAX_ASTEROIDS];
} server_player_t;

typedef struct {
    station_t stations[MAX_STATIONS];
    int station_count;              /* highest active slot + 1 (3 at reset, grows with outposts) */
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
    npc_ship_t npc_ships[MAX_NPC_SHIPS];
    /* #294 Slice 1: empty controller pool. Nothing populates this yet —
     * MINER migration (Slice 3a) will be the first writer. Sized to the
     * NPC + player union so later slices don't need a flag-day resize. */
    character_t characters[MAX_PLAYERS + MAX_NPC_SHIPS];
    scaffold_t scaffolds[MAX_SCAFFOLDS];
    server_player_t players[MAX_PLAYERS];
    uint32_t rng;
    float time;
    float field_spawn_timer;
    float gravity_accumulator;  /* runs gravity at reduced rate */
    sim_events_t events;
    contract_t contracts[MAX_CONTRACTS];
    bool player_only_mode;
    uint32_t next_fracture_id;
    belt_field_t belt;
    spatial_grid_t asteroid_grid;
    signal_grid_t signal_cache;
    signal_channel_t signal_channel;  /* station broadcast log (#316) */
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
void world_cleanup(world_t *w);
void world_sim_step(world_t *w, float dt);
void world_sim_step_player_only(world_t *w, int player_idx, float dt);
void player_init_ship(server_player_t *sp, world_t *w);
float signal_strength_at(const world_t *w, vec2 pos);
void spatial_grid_build(world_t *w);
void ledger_credit_supply(station_t *st, const uint8_t *token, float ore_value);

/* Nav API — canonical declarations in sim_nav.h.
 * Repeated here because sim_nav.h includes game_sim.h (circular).
 * Client code (src/) includes game_sim.h but not sim_nav.h. */
int nav_get_player_path(int player_id, vec2 *out_waypoints, int max_count, int *out_current);
int nav_compute_path(const world_t *w, vec2 start, vec2 goal, float clearance,
                     vec2 *out_waypoints, int max_count);
bool nav_segment_clear(const world_t *w, vec2 start, vec2 goal, float clearance);
void station_rebuild_all_nav(const world_t *w);
void rebuild_signal_chain(world_t *w);
bool can_place_outpost(const world_t *w, vec2 pos);
void begin_module_construction(world_t *w, station_t *st, int station_idx, module_type_t type);
void begin_module_construction_at(world_t *w, station_t *st, int station_idx, module_type_t type, int ring, int slot);
/* Deliver build material from `ship` (player or NPC) into modules at
 * this station awaiting supply. `filter` restricts which commodity may
 * be consumed; pass COMMODITY_COUNT to allow any. The filter prevents
 * "deliver ingots only" from also draining frames into a half-built
 * module behind the player's back. */
void step_module_delivery(world_t *w, station_t *st, int station_idx,
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
bool world_save(const world_t *w, const char *path);
bool world_load(world_t *w, const char *path);
/* Station catalog — per-station identity persistence (sim_catalog.c) */
int  station_catalog_load_all(station_t *stations, int max, const char *dir);
bool station_catalog_save_all(const station_t *stations, int count, const char *dir);
bool player_save(const server_player_t *sp, const char *dir, int slot);
bool player_load(server_player_t *sp, world_t *w, const char *dir, int slot);
bool player_load_by_token(server_player_t *sp, world_t *w, const char *dir,
                          const uint8_t token[8]);

/* Cross-module sim helpers — defined in game_sim.c, used by sim_*.c. */
void anchor_ship_in_station(server_player_t *sp, world_t *w);
asteroid_tier_t max_mineable_tier(int mining_level);
vec2 station_approach_target(const station_t *st, vec2 from);
void emit_event(world_t *w, sim_event_t ev);
/* Station-local ledger economy */
float ledger_balance(const station_t *st, const uint8_t *token);
void ledger_earn(station_t *st, const uint8_t *token, float amount);
void ledger_credit_supply(station_t *st, const uint8_t *token, float ore_value);
/* Returns false if the player can't afford `amount` at this station;
 * otherwise debits the ledger, refunds the credit_pool, and bumps the
 * ship's stat_credits_spent. */
bool ledger_spend(station_t *st, const uint8_t *token, float amount, ship_t *ship);
/* Always-succeeds debit for unrefusable services (spawn, repair).
 * Allows the balance to go negative (debt). */
void ledger_force_debit(station_t *st, const uint8_t *token, float amount, ship_t *ship);
/* Signal channel — station broadcast log (#316). */
uint64_t signal_channel_post(world_t *w, int sender_station, const char *text, const char *audio_url);
const signal_channel_msg_t *signal_channel_at(const world_t *w, int i);

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
#define HOPPER_PULL_ACCEL 500.0f    /* pull strength */

#endif /* GAME_SIM_H */
