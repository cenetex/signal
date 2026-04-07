---
name: Collision architecture gap
description: Station collision is fragmented across 4 code paths that have drifted; needs unified geometry emitter
type: project
---

Collision is 4 separate implementations that have drifted: player vs station, NPC vs station, asteroid vs station, and rendered geometry. Core problem: corridor geometry is implicit and copied.

**Why:** Each collision fix touches one path but not the others, causing recurring invisible walls, dock-skip inconsistencies, and render/collision mismatches. CORRIDOR_HW is defined separately in game_sim.c and hardcoded in world_draw.c.

**How to apply:** The fix is architectural — one shared station-geometry emitter that produces core circles, module circles, corridor arcs (solid/open), and dock openings. All four consumers (player/NPC/asteroid collision + rendering) read from this. Don't keep treating #238 as individual bugs. Full analysis in the conversation from 2026-04-05.
