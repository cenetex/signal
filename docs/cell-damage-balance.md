# Cell damage, repair, and provenance

Structural damage uses bounded join stress. A collision contributes physical
impulse (`fragment mass * closing velocity`) at the impacted cell. Stress walks
only the joins present in `cell_stress_state_t` and halves at each graph hop;
there is no hidden hitpoint adjacency or soft-body solve.

V1 thresholds are expressed in the same deterministic impulse units:

| Join | Failure behavior | Threshold |
|---|---|---:|
| Directional triangle mount | shears as one active cell | 80 |
| Standard complete-edge weld | standard cell/component can detach | 120 |
| Reinforced hub spoke/weld | visible stages at 120 and 240, fails at 360 | 360 |

A thrown fragment with mass 3 and closing velocity 40 therefore reaches the
standard threshold exactly. Casual low-speed contact remains below it. A
determined siege must repeatedly place high-momentum rock impacts, and a hub
flower retains alternate explicit side joins after its first spoke crack.

Repair reattaches an intact salvage graph without creating or consuming
matter: both complete rims already exist. Replacing a lost cell instead costs
that cell's ordinary 3/6/12 conserved-strut recipe. Field repair should clear
one standard failed join per repair cycle; the intended pacing target is one
cycle faster than a casual attacker can mine, aim, and land another threshold
rock, while coordinated repeated impacts can outrun a lone repair bay.

An intact detach preserves cell identity, payload count, motion, rotation, and
the supplied shell/payload manifest roots. `cell_salvage_t` carries those roots
through its canonical codec and uses the ordinary tractor-body path. Dismantle
returns the cell's complete owned struts and separately releases payload.
Re-smelting creates normal recipe outputs whose parent points to the recovered
matter. If an old save lacks a root, salvage is explicitly
`CELL_PROVENANCE_UNKNOWN` with zero roots; no synthetic chain is invented.
