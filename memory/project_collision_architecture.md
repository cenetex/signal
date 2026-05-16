---
name: Collision architecture status
description: Unified station geometry emitter is in place; keep collision/render consumers on it
type: project
---

The old collision architecture gap is substantially resolved. Station geometry now has a shared emitter in `shared/station_geom.h`: `station_build_geom` produces core circles, module circles, corridor arcs, cross-ring spokes, and dock positions, with `STATION_CORRIDOR_HW` and `STATION_MODULE_COL_RADIUS` as the shared constants.

**Why:** The prior failure mode was four drifting interpretations of station shape: player collision, NPC collision, asteroid collision, and rendering. That drift caused invisible walls, dock-skip inconsistencies, and render/collision mismatches. The shared emitter is now the contract those consumers should keep using.

**How to apply:** Treat this as a regression guard, not an open first-implementation backlog item. When station shape changes, update `station_build_geom` and the station geometry/collision tests first, then keep player/NPC/asteroid collision and rendering consuming the emitted shapes. Do not reintroduce local corridor constants or per-consumer station geometry rules.
