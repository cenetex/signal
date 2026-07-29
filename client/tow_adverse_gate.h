#ifndef SIGNAL_TOW_ADVERSE_GATE_H
#define SIGNAL_TOW_ADVERSE_GATE_H

/*
 * Deterministic browser diagnostic for one bounded relationship path:
 * a live player ship towing one cargo-pod slot. It drives the real cargo
 * interpolation and atomic tow-snapshot apply functions through synthetic
 * delivery schedules; it is never part of the production transport.
 */
int signal_smoke_adverse_towable_gate(void);
const char *signal_smoke_adverse_towable_report(void);

#endif /* SIGNAL_TOW_ADVERSE_GATE_H */
