/*
 * flybrain.c -- Vendored into signal from the flybrain project
 * (connectome/flybrain in the develop tree), snapshot of 2026-09-11.
 * Local changes on top of that snapshot:
 *   - fb_sim_turn_raw() and fb_sim_calibrate() implemented here; they
 *     were declared in flybrain.h but unimplemented upstream, which
 *     broke the flybrain test build.
 * Re-sync with upstream before making further kernel changes.
 */
#include "flybrain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- portable exact integer helpers ------------------------------------- */

/* Q16 multiply that rounds toward zero symmetrically for both signs. Written
 * without relying on the sign behaviour of >> on negative operands so the
 * result is identical on every conforming compiler. */
static inline int32_t q16_mul(int32_t v, int32_t q)
{
    if (v >= 0) return (int32_t)(((uint64_t)(uint32_t)v * (uint32_t)q) >> 16);
    return -(int32_t)(((uint64_t)(uint32_t)(-v) * (uint32_t)q) >> 16);
}

/* exp(-dt/tau) in Q16, computed once at init by integer series so that no
 * libm call (and no float rounding mode) can perturb the state. */
static int32_t leak_q16(int32_t dt_us, int32_t tau_us)
{
    /* exp(-x) with x = dt/tau, evaluated as a Q32 Taylor series and truncated.
     * dt <= tau/2 in every sane configuration, so 8 terms is exact to Q16. */
    int64_t x = ((int64_t)dt_us << 32) / tau_us;   /* Q32 */
    int64_t term = (int64_t)1 << 32, sum = (int64_t)1 << 32;
    for (int k = 1; k <= 8; k++) {
        term = (term * x) >> 32;
        term /= k;
        sum += (k & 1) ? -term : term;
    }
    if (sum < 0) sum = 0;
    return (int32_t)(sum >> 16);
}

/* PCG-XSH-RR 64/32: small, deterministic, decent. One stream per agent, which
 * is what gives two NPCs flying the same connectome different personalities. */
static inline uint32_t pcg32(uint64_t *st)
{
    uint64_t old = *st;
    *st = old * 6364136223846793005ULL + 1442695040888963407ULL;
    uint32_t xorshifted = (uint32_t)(((old >> 18) ^ old) >> 27);
    uint32_t rot = (uint32_t)(old >> 59);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

/* ---- blob loading ------------------------------------------------------- */

int fb_circuit_from_memory(fb_circuit *out, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    if (len < 20) return -1;
    uint32_t magic; memcpy(&magic, p, 4);
    if (magic != FB_MAGIC) return -1;
    uint32_t hdr[4]; memcpy(hdr, p + 4, 16);
    memset(out, 0, sizeof(*out));
    out->n = hdr[0]; out->nnz = hdr[1]; out->n_in = hdr[2]; out->n_out = hdr[3];

    size_t need = 20
        + (size_t)(out->n + 1) * 4 + (size_t)out->nnz * 4 + (size_t)out->nnz * 2
        + (size_t)out->n * 2 + (size_t)out->n * 4
        + (size_t)out->n_in * 4 + (size_t)out->n_out * 4
        + (size_t)out->n * 8;
    if (len < need) return -1;

    const unsigned char *q = p + 20;
    out->row_ptr = (const uint32_t *)q; q += (size_t)(out->n + 1) * 4;
    out->col_idx = (const uint32_t *)q; q += (size_t)out->nnz * 4;
    out->weight  = (const uint16_t *)q; q += (size_t)out->nnz * 2;
    out->sign    = (const int8_t   *)q; q += out->n;
    out->side    = (const int8_t   *)q; q += out->n;
    out->bias    = (const int32_t  *)q; q += (size_t)out->n * 4;
    out->in_idx  = (const uint32_t *)q; q += (size_t)out->n_in * 4;
    out->out_idx = (const uint32_t *)q; q += (size_t)out->n_out * 4;
    out->root_id = (const uint64_t *)q; q += (size_t)out->n * 8;

    /* Population table: count, then {name[16], count, idx[count]} records. */
    if ((size_t)(q - p) + 4 <= len) {
        uint32_t np_; memcpy(&np_, q, 4); q += 4;
        if (np_ > 4096) return -2;
        out->n_pop = np_;
        if (np_) {
            out->pop = calloc(np_, sizeof(*out->pop));
            if (!out->pop) return -1;
            for (uint32_t i = 0; i < np_; i++) {
                if ((size_t)(q - p) + FB_POP_NAME_MAX + 4 > len) return -2;
                memcpy(out->pop[i].name, q, FB_POP_NAME_MAX);
                out->pop[i].name[FB_POP_NAME_MAX - 1] = 0;
                q += FB_POP_NAME_MAX;
                uint32_t cnt; memcpy(&cnt, q, 4); q += 4;
                if ((size_t)(q - p) + (size_t)cnt * 4 > len) return -2;
                out->pop[i].count = cnt;
                out->pop[i].idx = (const uint32_t *)q;
                for (uint32_t k = 0; k < cnt; k++)
                    if (out->pop[i].idx[k] >= out->n) return -2;
                q += (size_t)cnt * 4;
            }
        }
    }

    /* Reject a blob that would let a spike write outside the state array. */
    if (out->row_ptr[out->n] != out->nnz) return -2;
    for (uint32_t i = 0; i < out->nnz; i++)
        if (out->col_idx[i] >= out->n) return -2;
    for (uint32_t i = 0; i < out->n_in; i++)
        if (out->in_idx[i] >= out->n) return -2;
    for (uint32_t i = 0; i < out->n_out; i++)
        if (out->out_idx[i] >= out->n) return -2;
    return 0;
}

int fb_circuit_load(fb_circuit *out, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long len = ftell(f);
    if (len <= 0) { fclose(f); return -1; }
    rewind(f);
    void *buf = malloc((size_t)len);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return -1; }
    fclose(f);
    int rc = fb_circuit_from_memory(out, buf, (size_t)len);
    if (rc != 0) { free(buf); return rc; }
    out->_owned = buf;
    return 0;
}

void fb_circuit_free(fb_circuit *c)
{
    if (!c) return;
    free(c->pop);
    if (c->_owned) free(c->_owned);
    memset(c, 0, sizeof(*c));
}

uint32_t fb_population(const fb_circuit *c, const char *name, const uint32_t **idx)
{
    for (uint32_t i = 0; i < c->n_pop; i++) {
        if (strcmp(c->pop[i].name, name) == 0) {
            if (idx) *idx = c->pop[i].idx;
            return c->pop[i].count;
        }
    }
    if (idx) *idx = NULL;
    return 0;
}

/* ---- simulation -------------------------------------------------------- */

int fb_sim_init(fb_sim *s, const fb_circuit *c, uint32_t n_agents,
                int32_t dt_us, uint64_t seed)
{
    memset(s, 0, sizeof(*s));
    if (!c || !c->n || !n_agents || dt_us <= 0) return -1;
    s->c = c; s->n_agents = n_agents; s->dt_us = dt_us;
    s->leak_q16 = leak_q16(dt_us, FB_TAU_MEM_US);
    s->noise_uv = 0;

    size_t nn = (size_t)n_agents * c->n;
    s->v         = (int32_t  *)calloc(nn, sizeof(int32_t));
    s->refrac    = (uint16_t *)calloc(nn, sizeof(uint16_t));
    s->rate      = (uint32_t *)calloc(nn, sizeof(uint32_t));
    s->spike_buf = (uint32_t *)calloc(nn, sizeof(uint32_t));
    s->spikes    = (uint32_t *)calloc(c->n, sizeof(uint32_t));
    s->n_spikes  = (uint32_t *)calloc(n_agents, sizeof(uint32_t));
    s->rng       = (uint64_t *)calloc(n_agents, sizeof(uint64_t));
    if (!s->v || !s->refrac || !s->rate || !s->spike_buf || !s->spikes ||
        !s->n_spikes || !s->rng) { fb_sim_free(s); return -1; }

    for (uint32_t a = 0; a < n_agents; a++)
        s->rng[a] = seed ^ (0x9E3779B97F4A7C15ULL * (a + 1));
    return 0;
}

void fb_sim_free(fb_sim *s)
{
    if (!s) return;
    free(s->v); free(s->refrac); free(s->rate); free(s->spike_buf);
    free(s->spikes); free(s->n_spikes); free(s->rng);
    memset(s, 0, sizeof(*s));
}

void fb_sim_reset_agent(fb_sim *s, uint32_t agent, uint64_t seed)
{
    if (agent >= s->n_agents) return;
    size_t off = (size_t)agent * s->c->n;
    memset(s->v + off, 0, s->c->n * sizeof(int32_t));
    memset(s->refrac + off, 0, s->c->n * sizeof(uint16_t));
    memset(s->rate + off, 0, s->c->n * sizeof(uint32_t));
    s->n_spikes[agent] = 0;
    s->rng[agent] = seed ^ (0x9E3779B97F4A7C15ULL * (agent + 1));
}

void fb_sim_inject(fb_sim *s, uint32_t agent, const int32_t *uv)
{
    if (!uv || agent >= s->n_agents) return;
    const fb_circuit *c = s->c;
    int32_t *v = s->v + (size_t)agent * c->n;
    const uint16_t *rf = s->refrac + (size_t)agent * c->n;
    for (uint32_t k = 0; k < c->n_in; k++) {
        uint32_t i = c->in_idx[k];
        if (rf[i] == 0) v[i] += uv[k];   /* refractory neurons ignore input */
    }
}

void fb_sim_inject_pop(fb_sim *s, uint32_t agent, const char *name,
                       const int32_t *uv)
{
    const uint32_t *idx; uint32_t cnt = fb_population(s->c, name, &idx);
    if (!cnt || !uv || agent >= s->n_agents) return;
    int32_t *v = s->v + (size_t)agent * s->c->n;
    const uint16_t *rf = s->refrac + (size_t)agent * s->c->n;
    for (uint32_t k = 0; k < cnt; k++)
        if (rf[idx[k]] == 0) v[idx[k]] += uv[k];
}

/* Convert a Q16 decayed spike count into Q16 Hz. The estimator adds 1.0 per
 * spike and decays with a 100 ms time constant, so its steady-state value for a
 * neuron firing at f Hz is f * 0.1. */
static int32_t rate_to_hz_q16(uint64_t q16_sum, uint32_t count)
{
    if (!count) return 0;
    return (int32_t)((q16_sum * 10u) / count);
}

int32_t fb_sim_pop_rate(const fb_sim *s, uint32_t agent, const char *name)
{
    const uint32_t *idx; uint32_t cnt = fb_population(s->c, name, &idx);
    if (!cnt || agent >= s->n_agents) return 0;
    const uint32_t *rt = s->rate + (size_t)agent * s->c->n;
    uint64_t sum = 0;
    for (uint32_t k = 0; k < cnt; k++) sum += rt[idx[k]];
    return rate_to_hz_q16(sum, cnt);
}

/* Shared per-agent update. `a` indexes this sim's agent state. */
static void step_one(fb_sim *s, uint32_t a, int32_t leak, uint16_t dt,
                     int32_t rate_decay, int32_t amb)
{
    const fb_circuit *c = s->c;
    const uint32_t n = c->n;
    int32_t  *v  = s->v + (size_t)a * n;
    uint16_t *rf = s->refrac + (size_t)a * n;
    uint32_t *rt = s->rate + (size_t)a * n;
    uint32_t *sb = s->spike_buf + (size_t)a * n;
    uint64_t rng = s->rng[a];

    /* 1. propagate last step's spikes. Work is proportional to spikes. */
    uint32_t ns = s->n_spikes[a];
    for (uint32_t k = 0; k < ns; k++) {
        uint32_t src = sb[k];
        int32_t sgn = c->sign[src];
        if (sgn == 0) continue;            /* modulatory: no fast drive */
        int32_t gain = sgn * FB_SYN_UV;
        uint32_t e0 = c->row_ptr[src], e1 = c->row_ptr[src + 1];
        for (uint32_t e = e0; e < e1; e++)
            v[c->col_idx[e]] += gain * (int32_t)c->weight[e];
    }

    /* 2. leak, spontaneous drive, threshold. */
    uint32_t out = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (rf[i]) {                        /* clamped while refractory */
            rf[i] = (uint16_t)(rf[i] > dt ? rf[i] - dt : 0);
            v[i] = FB_V_RESET;
        } else {
            int32_t x = q16_mul(v[i], leak);
            if (amb && c->bias)
                x += (int32_t)(((int64_t)c->bias[i] * FB_SYN_UV * amb *
                                s->dt_us) / 1000000000);
            if (s->noise_uv)
                x += (int32_t)(pcg32(&rng) % (2u * s->noise_uv + 1u))
                     - (int32_t)s->noise_uv;
            if (x >= FB_V_THRESHOLD) {
                v[i] = FB_V_RESET;
                rf[i] = FB_REFRAC_US;
                sb[out++] = i;
            } else {
                v[i] = x;
            }
        }
        rt[i] = (uint32_t)q16_mul((int32_t)rt[i], rate_decay);
    }
    for (uint32_t k = 0; k < out; k++) rt[sb[k]] += 65536u;

    s->n_spikes[a] = out;
    s->rng[a] = rng;
}

void fb_sim_step_agent(fb_sim *s, uint32_t agent)
{
    if (agent >= s->n_agents) return;
    step_one(s, agent, s->leak_q16, (uint16_t)(s->dt_us > 65535 ? 65535 : s->dt_us),
             leak_q16(s->dt_us, 100000), s->ambient_mhz);
}

void fb_sim_step(fb_sim *s)
{
    const int32_t leak = s->leak_q16;
    const uint16_t dt = (uint16_t)(s->dt_us > 65535 ? 65535 : s->dt_us);
    const int32_t rate_decay = leak_q16(s->dt_us, 100000);
    const int32_t amb = s->ambient_mhz;
    for (uint32_t a = 0; a < s->n_agents; a++)
        step_one(s, a, leak, dt, rate_decay, amb);
    s->step_count++;
}

/* Raw, uncalibrated bilateral asymmetry of the descending bus, Q16 in
 * [-1,1]. Positive means the right side of the bus fires more, which is
 * the steer-right direction. Falls back to the side[] array when the blob
 * carries no dn_left/dn_right population table (same fallback as
 * fb_sim_command). */
int32_t fb_sim_turn_raw(const fb_sim *s, uint32_t agent)
{
    if (!s || agent >= s->n_agents) return 0;
    int32_t l = fb_sim_pop_rate(s, agent, "dn_left");
    int32_t r = fb_sim_pop_rate(s, agent, "dn_right");
    if (!l && !r) {
        const fb_circuit *c = s->c;
        const uint32_t *rt = s->rate + (size_t)agent * c->n;
        uint64_t ls = 0, rs = 0; uint32_t lc = 0, rc = 0;
        for (uint32_t k = 0; k < c->n_out; k++) {
            uint32_t i = c->out_idx[k];
            if (c->side[i] < 0) { ls += rt[i]; lc++; }
            else if (c->side[i] > 0) { rs += rt[i]; rc++; }
        }
        l = rate_to_hz_q16(ls, lc);
        r = rate_to_hz_q16(rs, rc);
    }
    int64_t tot = (int64_t)l + r;
    return tot ? (int32_t)((((int64_t)r - l) << 16) / tot) : 0;
}

/* Measure turn_bias and turn_span by driving agent 0 three ways:
 * symmetric (steer_uv/2 on both sides), left-only, right-only. Ring
 * neurons are GABAergic, so driving the LEFT ring suppresses the left
 * half of the central complex and the turn points RIGHT (positive raw
 * turn); driving the right ring points left. The half-span between the
 * two one-sided probes becomes turn_span, the symmetric probe becomes
 * turn_bias, and fb_sim_command then maps raw turns onto [-1,1].
 *
 * Agent 0 is used and left RESET at the end; callers own re-seeding.
 * Returns the measured raw half-span in Q16 (0 if the circuit has no
 * ring/columnar populations, i.e. nothing to calibrate). */
int32_t fb_sim_calibrate(fb_sim *s, int32_t tonic_uv, int32_t steer_uv, int steps)
{
    if (!s || !s->c || steps <= 0 || steer_uv <= 0) return 0;
    const uint32_t *ring; uint32_t nring = fb_population(s->c, "ring", &ring);
    uint32_t ncol = fb_population(s->c, "columnar", NULL);
    if (!nring || !ncol) return 0;

    int32_t *ton = (int32_t *)calloc(ncol, sizeof(int32_t));
    int32_t *inj = (int32_t *)calloc(nring, sizeof(int32_t));
    if (!ton || !inj) { free(ton); free(inj); return 0; }
    for (uint32_t k = 0; k < ncol; k++) ton[k] = tonic_uv;

    int32_t raw[3]; /* 0 = symmetric, 1 = drive LEFT ring, 2 = drive RIGHT ring */
    for (int mode = 0; mode < 3; mode++) {
        fb_sim_reset_agent(s, 0, 1);
        for (uint32_t k = 0; k < nring; k++) {
            int8_t sd = s->c->side[ring[k]];
            if (mode == 0) inj[k] = steer_uv / 2;
            else if (mode == 1) inj[k] = (sd < 0) ? steer_uv : 0;
            else inj[k] = (sd > 0) ? steer_uv : 0;
        }
        for (int t = 0; t < steps; t++) {
            fb_sim_inject_pop(s, 0, "columnar", ton);
            fb_sim_inject_pop(s, 0, "ring", inj);
            fb_sim_step_agent(s, 0);
        }
        raw[mode] = fb_sim_turn_raw(s, 0);
    }
    free(ton); free(inj);

    s->turn_bias = raw[0];
    int64_t span = ((int64_t)raw[1] - raw[2]) / 2;
    if (span < 0) span = -span;
    s->turn_span = (int32_t)span;
    fb_sim_reset_agent(s, 0, 1);
    return s->turn_span;
}

void fb_sim_command(const fb_sim *s, uint32_t agent, fb_command *out)
{
    memset(out, 0, sizeof(*out));
    if (agent >= s->n_agents) return;

    int32_t l = fb_sim_pop_rate(s, agent, "dn_left");
    int32_t r = fb_sim_pop_rate(s, agent, "dn_right");
    if (!l && !r) {  /* blob without population table: fall back to side[] */
        const fb_circuit *c = s->c;
        const uint32_t *rt = s->rate + (size_t)agent * c->n;
        uint64_t ls = 0, rs = 0; uint32_t lc = 0, rc = 0;
        for (uint32_t k = 0; k < c->n_out; k++) {
            uint32_t i = c->out_idx[k];
            if (c->side[i] < 0) { ls += rt[i]; lc++; }
            else if (c->side[i] > 0) { rs += rt[i]; rc++; }
        }
        l = rate_to_hz_q16(ls, lc);
        r = rate_to_hz_q16(rs, rc);
    }
    out->rate_left = l;
    out->rate_right = r;
    out->drive = (int32_t)(((int64_t)l + r) / 2);
    int64_t tot = (int64_t)l + r;
    /* Mean-normalised, so unequal population sizes cannot fake a turn. */
    int32_t raw = tot ? (int32_t)((((int64_t)r - l) << 16) / tot) : 0;
    if (s->turn_span > 0) {
        int64_t t = ((int64_t)(raw - s->turn_bias) << 16) / s->turn_span;
        if (t > 65536) t = 65536;
        if (t < -65536) t = -65536;
        out->turn = (int32_t)t;
    } else {
        out->turn = raw - s->turn_bias;
    }
}

int fb_sim_has_named_channels(const fb_circuit *c)
{
    return fb_population(c, "yaw_left", NULL) > 0 &&
           fb_population(c, "yaw_right", NULL) > 0;
}

/* Normalised right-minus-left asymmetry of a paired channel, Q16 in [-1,1]. */
static int32_t asym(const fb_sim *s, uint32_t agent, const char *l, const char *r)
{
    int32_t a = fb_sim_pop_rate(s, agent, l);
    int32_t b = fb_sim_pop_rate(s, agent, r);
    int64_t tot = (int64_t)a + b;
    return tot ? (int32_t)((((int64_t)b - a) << 16) / tot) : 0;
}

static int32_t pair_mean(const fb_sim *s, uint32_t agent, const char *l, const char *r)
{
    return (fb_sim_pop_rate(s, agent, l) + fb_sim_pop_rate(s, agent, r)) / 2;
}

void fb_sim_control(const fb_sim *s, uint32_t agent, fb_control *out)
{
    memset(out, 0, sizeof(*out));
    if (agent >= s->n_agents) return;
    out->yaw      = asym(s, agent, "yaw_left", "yaw_right");
    out->yaw_slow = asym(s, agent, "yaw_slow_left", "yaw_slow_right");
    out->forward  = pair_mean(s, agent, "forward_left", "forward_right");
    out->reverse  = pair_mean(s, agent, "reverse_left", "reverse_right");
    out->brake    = pair_mean(s, agent, "brake_left", "brake_right");
    out->escape   = (pair_mean(s, agent, "escape_left", "escape_right") +
                     pair_mean(s, agent, "escape2_left", "escape2_right")) / 2;
    out->bus      = pair_mean(s, agent, "dn_left", "dn_right");
}

uint32_t fb_sim_active_count(const fb_sim *s, uint32_t agent)
{
    return agent < s->n_agents ? s->n_spikes[agent] : 0;
}

uint64_t fb_sim_checksum(const fb_sim *s)
{
    /* FNV-1a over the full mutable state. Two runs that agree here agree
     * everywhere, which is the property lockstep replay needs. */
    uint64_t h = 1469598103934665603ULL;
    size_t nn = (size_t)s->n_agents * s->c->n;
    const unsigned char *p = (const unsigned char *)s->v;
    for (size_t i = 0; i < nn * sizeof(int32_t); i++) {
        h ^= p[i]; h *= 1099511628211ULL;
    }
    p = (const unsigned char *)s->refrac;
    for (size_t i = 0; i < nn * sizeof(uint16_t); i++) {
        h ^= p[i]; h *= 1099511628211ULL;
    }
    return h;
}
