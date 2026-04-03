/*
 * signal.h — Unified signal quality model for Signal Space Miner.
 *
 * Signal quality (0.0–1.0) governs how effectively you can operate
 * in an area. All signal-dependent systems reference these shared
 * functions instead of ad-hoc thresholds.
 *
 * Signal bands:
 *   0.00–0.15  FRONTIER  No NPC support, heavy control penalty,
 *                        minimal mining efficiency, boundary push.
 *   0.15–0.50  FRINGE    Some mining viability, NPCs cautious,
 *                        moderate control penalty.
 *   0.50–0.80  OPERATIONAL  NPCs operate normally, reasonable mining,
 *                           logistics available.
 *   0.80–1.00  CORE      Full efficiency, NPC haulers active,
 *                        no penalties.
 */
#ifndef SIGNAL_MODEL_H
#define SIGNAL_MODEL_H

/* --- Band thresholds --- */
#define SIGNAL_BAND_FRONTIER    0.15f
#define SIGNAL_BAND_FRINGE      0.50f
#define SIGNAL_BAND_OPERATIONAL 0.80f

/* --- Modifier functions --- */

/* Mining efficiency: 0.2x at zero signal, 1.0x at full.
 * Smooth linear ramp — no cliff. */
static inline float signal_mining_efficiency(float quality) {
    return 0.2f + 0.8f * quality;
}

/* Control responsiveness: 30% minimum at zero signal.
 * Affects thrust and turn input scaling. */
static inline float signal_control_scale(float quality) {
    return 0.3f + 0.7f * quality;
}

/* NPC confidence: 0.0 below frontier, ramps to 1.0 at operational.
 * Governs NPC willingness to operate and mining target selection.
 * Below frontier: NPCs won't operate at all.
 * Frontier to operational: graduated confidence (slower, pickier). */
static inline float signal_npc_confidence(float quality) {
    if (quality < SIGNAL_BAND_FRONTIER) return 0.0f;
    if (quality >= SIGNAL_BAND_OPERATIONAL) return 1.0f;
    return (quality - SIGNAL_BAND_FRONTIER) / (SIGNAL_BAND_OPERATIONAL - SIGNAL_BAND_FRONTIER);
}

/* Boundary push strength: returns >0 when signal is in frontier band.
 * Used to gently push ships back toward coverage. */
static inline float signal_boundary_push(float quality) {
    if (quality >= SIGNAL_BAND_FRONTIER) return 0.0f;
    return (SIGNAL_BAND_FRONTIER - quality) / SIGNAL_BAND_FRONTIER;
}

/* Signal band name for UI display. */
static inline const char* signal_band_name(float quality) {
    if (quality < SIGNAL_BAND_FRONTIER)    return "FRONTIER";
    if (quality < SIGNAL_BAND_FRINGE)      return "FRINGE";
    if (quality < SIGNAL_BAND_OPERATIONAL) return "OPERATIONAL";
    return "CORE";
}

#endif /* SIGNAL_MODEL_H */
