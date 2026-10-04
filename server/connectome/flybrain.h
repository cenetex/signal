/*
 * flybrain.h -- deterministic integer leaky-integrate-and-fire kernel over a
 * FlyWire connectome subgraph, batched across many independent agents.
 *
 * Design commitments, in order of importance:
 *
 *  1. NO FLOATING POINT anywhere in the step path. Membrane voltages are int32
 *     microvolts above rest; the leak is a Q16 integer multiply. The same inputs
 *     therefore produce bit-identical output on every machine, which is what makes
 *     this safe to run inside a lockstep-replayed multiplayer sim.
 *
 *  2. ONE connectome, N agent states. The weight matrix is shared and read-only;
 *     each agent owns only a voltage/refractory/rate vector. Per-agent cost is
 *     16 KiB for the CX circuit, so a hundred agents live in L2.
 *
 *  3. EVENT-DRIVEN. Synaptic work is proportional to spikes, not to synapses.
 *     Fly neurons are quiet -- a few percent fire per millisecond -- so pushing
 *     from spikers beats gathering into everyone by one to two orders of magnitude.
 *
 *  4. NO TRAINING. There is no loss, no gradient, no checkpoint. The wiring is
 *     the program. Behaviour is set by the connectome plus per-agent seed.
 *
 * Cell parameters follow Shiu et al., Nature 2024 (whole-brain fly LIF): all
 * neurons identical, 20 ms membrane time constant, 7 mV to threshold from rest,
 * 2.2 ms refractory, 0.275 mV per synapse. Uniform and literature-derived rather
 * than fitted -- that is the point, not a simplification.
 */
#ifndef FLYBRAIN_H
#define FLYBRAIN_H

#include <stddef.h>
#include <stdint.h>

#define FB_MAGIC 0x34584E43u /* "CNX4" little-endian */
#define FB_POP_NAME_MAX 16

/* --- cell parameters, microvolts and microseconds --- */
#define FB_V_THRESHOLD   7000   /* uV above rest: -45 mV vs -52 mV rest */
#define FB_V_RESET          0
#define FB_TAU_MEM_US   20000   /* 20 ms */
#define FB_REFRAC_US     2200   /* 2.2 ms */
#define FB_SYN_UV         275   /* 0.275 mV per synapse */

typedef struct {
    uint32_t n;          /* neurons */
    uint32_t nnz;        /* synapse pairs */
    uint32_t n_in;
    uint32_t n_out;
    const uint32_t *row_ptr;  /* n+1, CSR over PREsynaptic neuron */
    const uint32_t *col_idx;  /* nnz, postsynaptic targets */
    const uint16_t *weight;   /* nnz, raw synapse counts */
    const int8_t   *sign;     /* n, Dale's law: one transmitter per neuron */
    const int8_t   *side;     /* n, -1 left / +1 right / 0 centre */
    const int32_t  *bias;     /* n, signed synapse count deleted by extraction */
    const uint32_t *in_idx;   /* n_in, injection sites */
    const uint32_t *out_idx;  /* n_out, descending bus */
    const uint64_t *root_id;  /* n, FlyWire ids for debugging */
    /* Named populations, so callers address "ring" or "dn_left" rather than
     * hardcoding indices that shift with every release. */
    uint32_t n_pop;
    struct { char name[FB_POP_NAME_MAX]; uint32_t count; const uint32_t *idx; } *pop;
    void *_owned;             /* backing allocation, NULL if borrowed */
} fb_circuit;

/* Per-agent mutable state. Laid out agent-major: each agent's voltage vector is
 * contiguous so a step touches one cache-resident block. */
typedef struct {
    const fb_circuit *c;
    uint32_t n_agents;
    int32_t  *v;          /* n_agents * n, microvolts above rest */
    uint16_t *refrac;     /* n_agents * n, microseconds remaining */
    uint32_t *rate;       /* n_agents * n, Q16 decayed spike rate estimate */
    uint32_t *spikes;     /* scratch: spiking indices for the current agent */
    uint32_t *n_spikes;   /* n_agents, count from the previous step */
    uint32_t *spike_buf;  /* n_agents * n, per-agent carry-over spike lists */
    uint64_t *rng;        /* n_agents, per-agent deterministic stream */
    int32_t  dt_us;
    int32_t  leak_q16;    /* round(exp(-dt/tau) * 65536) */
    uint32_t noise_uv;    /* mean spontaneous drive per step, microvolts */
    /* Assumed mean firing rate of the rest of the brain, in milli-Hz. This is
     * the ONE free parameter of the model: it converts each neuron's deleted
     * input into a per-neuron bias current. Everything else comes from the
     * connectome or from literature cell parameters. */
    int32_t ambient_mhz;
    uint64_t step_count;
    /* Sensor calibration. The descending bus has a fixed anatomical bias (616
     * left vs 621 right neurons, different wiring), and the steering signal is
     * a small push-pull excursion around it -- as it is in a real fly. These
     * constants are measured once per circuit so `turn` comes out as a usable
     * [-1,1] control axis instead of a raw offset. */
    int32_t turn_bias;    /* Q16 turn under symmetric drive */
    int32_t turn_span;    /* Q16 half-range between full-left and full-right */
} fb_sim;

/* Descending-bus readout: what the game actually consumes. */
typedef struct {
    /* Q16 in [-1,1]: normalised bilateral asymmetry of the descending bus.
     * Negative steers left. Per-side MEAN rates, so the 611-vs-618 difference
     * in neuron counts cannot masquerade as a turn command. */
    int32_t turn;
    int32_t drive;       /* Q16 Hz, mean descending firing rate */
    int32_t rate_left;   /* Q16 Hz */
    int32_t rate_right;  /* Q16 Hz */
} fb_command;

/* Look up a named population. Returns count, 0 if absent. */
uint32_t fb_population(const fb_circuit *c, const char *name, const uint32_t **idx);

/* Inject into one named population; `uv` is length = that population's count. */
void fb_sim_inject_pop(fb_sim *s, uint32_t agent, const char *name,
                       const int32_t *uv);
/* Mean firing rate of a named population, Q16 Hz. */
int32_t fb_sim_pop_rate(const fb_sim *s, uint32_t agent, const char *name);

/* Named behavioural channels read off individually identified descending
 * neurons, rather than a sum over the anonymous bus. Each is Q16. `yaw` is a
 * normalised [-1,1] asymmetry; the rest are firing rates in Hz. */
typedef struct {
    int32_t yaw;       /* DNa02 right minus left -- the fly's steering command */
    int32_t yaw_slow;  /* DNa01, slower steering channel */
    int32_t forward;   /* DNb01 */
    int32_t reverse;   /* MDN, the moonwalker neurons */
    int32_t brake;     /* DNp09, freezing */
    int32_t escape;    /* DNp07 + DNp10 */
    int32_t bus;       /* whole descending bus, a general arousal level */
} fb_control;

/* Zero-filled if the blob carries no annotation-derived channels. */
void fb_sim_control(const fb_sim *s, uint32_t agent, fb_control *out);
int  fb_sim_has_named_channels(const fb_circuit *c);

int  fb_circuit_load(fb_circuit *out, const char *path);
int  fb_circuit_from_memory(fb_circuit *out, const void *buf, size_t len);
void fb_circuit_free(fb_circuit *c);

int  fb_sim_init(fb_sim *s, const fb_circuit *c, uint32_t n_agents,
                 int32_t dt_us, uint64_t seed);
void fb_sim_free(fb_sim *s);
void fb_sim_reset_agent(fb_sim *s, uint32_t agent, uint64_t seed);

/* Inject external drive into agent's input sites. `uv` is length c->n_in and is
 * added to membrane voltage this step. Pass NULL for no sensory drive. */
void fb_sim_inject(fb_sim *s, uint32_t agent, const int32_t *uv);

/* Advance every agent by one dt. Bit-exact and order-independent across agents. */
void fb_sim_step(fb_sim *s);

/* Advance ONE agent by one dt. Agents share the read-only connectome and touch
 * only their own state, so stepping them unequal numbers of times is safe --
 * which is what lets compute be rationed per agent. */
void fb_sim_step_agent(fb_sim *s, uint32_t agent);

/* Read the descending bus for one agent. `turn` is bias-corrected and scaled by
 * the calibration if fb_sim_calibrate() has run, raw otherwise. */
void fb_sim_command(const fb_sim *s, uint32_t agent, fb_command *out);

/* Measure turn_bias and turn_span by driving the circuit symmetrically, then
 * fully to each side. Uses agent 0 and leaves it reset. Returns the measured
 * raw half-span in Q16 so a caller can see how much signal there actually is. */
int32_t fb_sim_calibrate(fb_sim *s, int32_t tonic_uv, int32_t steer_uv, int steps);

/* Raw, uncalibrated turn -- for diagnostics and for the calibration itself. */
int32_t fb_sim_turn_raw(const fb_sim *s, uint32_t agent);

uint32_t fb_sim_active_count(const fb_sim *s, uint32_t agent);
uint64_t fb_sim_checksum(const fb_sim *s); /* determinism gate */

#endif /* FLYBRAIN_H */
