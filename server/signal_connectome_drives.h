/* Internal connectome drive state and posture effects. */
#ifndef SIGNAL_CONNECTOME_DRIVES_H
#define SIGNAL_CONNECTOME_DRIVES_H

#include <stdbool.h>
#include <stdint.h>

#define Q16_ONE 65536

/* Strategic postures for the hybrid brain. The sampler's weight array is
 * indexed by this, so order is load-bearing. */
typedef enum {
    CB_STRAT_FORAGE = 0,   /* mine, hunger-led */
    CB_STRAT_PROSPECT,     /* bolder, farther ore */
    CB_STRAT_CAUTION,      /* threat-averse */
    CB_STRAT_HAUL,         /* cargo-led */
    CB_STRAT_REGROUP,      /* hurt or idle: go home */
    CB_STRAT_COUNT
} cb_strategy_t;

/* ---------------- per-agent drive state (runtime only) ---------------- */

typedef struct {
    int32_t  sensed_lust;   /* Q16 cargo drive retained between ticks */
    int32_t  sensed_hunger; /* Q16 foraging drive retained between ticks */
    int32_t  lust;      /* Q16: courtship arousal -- towing ore */
    int32_t  hunger;    /* Q16: foraging arousal -- empty-handed */
    int32_t  fear;      /* Q16: closing-rock threat + low hull */
    int32_t  pain;      /* Q16: recent hull damage, decays ~1 s */
    float    last_hull;
    int32_t  pending_u; /* Q16 steering intent recorded by flight_cmd */
    int32_t  turn_ema;  /* Q16 smoothed descending turn, see flight_cmd */
} cb_agent_state_t;

/* Advance sensed drives, then start a fresh effective drive for this tick. */
static inline void cb_update_cargo_drives(cb_agent_state_t *st,
                                           int carrying, bool docked)
{
    /* LUST: the fly is in love with its ore. Rises while towing,
     * glows and fades after delivery. */
    if (carrying > 0)
        st->sensed_lust = st->sensed_lust < Q16_ONE - 4096
            ? st->sensed_lust + 4096 : Q16_ONE;
    else
        st->sensed_lust -= st->sensed_lust >> 6;

    /* HUNGER: foraging drive. Empty-handed working flies get hungry. */
    if (carrying == 0 && !docked)
        st->sensed_hunger = st->sensed_hunger < Q16_ONE - 2048
            ? st->sensed_hunger + 2048 : Q16_ONE;
    else
        st->sensed_hunger = 0;

    st->lust = st->sensed_lust;
    st->hunger = st->sensed_hunger;
}

static inline int32_t cb_strategy_blend(int32_t v, float delta, float authority)
{
    float nv = (float)v + (float)v * delta * authority;
    if (nv < 0.0f) nv = 0.0f;
    if (nv > (float)Q16_ONE) nv = (float)Q16_ONE;
    return (int32_t)nv;
}

/* Apply the station's posture, scaled by how much of its signal the fly
 * can actually hear. Out of range the leash goes slack and the fly falls
 * back to its own connectome drives. */
static inline void cb_strategy_modulate(cb_agent_state_t *st, int strat,
                                         float authority)
{
    float dh = 0.0f, dl = 0.0f, df = 0.0f;
    switch (strat) {
    case CB_STRAT_FORAGE:   dh =  0.25f;  break;
    case CB_STRAT_PROSPECT: dh =  0.125f; break;
    case CB_STRAT_CAUTION:  df =  0.5f;   break;
    case CB_STRAT_HAUL:     dl =  0.25f;  break;
    case CB_STRAT_REGROUP:  dh = -0.25f;  break;
    default: return;
    }
    st->hunger = cb_strategy_blend(st->hunger, dh, authority);
    st->lust   = cb_strategy_blend(st->lust,   dl, authority);
    st->fear   = cb_strategy_blend(st->fear,   df, authority);
}

#endif
