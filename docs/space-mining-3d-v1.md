# Signal 3D Mining V1 Design

**Status:** Draft v1 design
**Audience:** Signal client/gameplay implementers
**Scope:** Procedural 3D presentation and vertical slice for the existing Signal loop

## Thesis

Signal 3D v1 should feel like a cockpit-adjacent, physical mining game in a
fragile frontier economy, but it should not throw away the strongest thing the
project already has: the 2D deterministic simulation of rocks, stations,
signal, ledgers, construction, and NPC labor.

The v1 target is therefore:

> A real 3D view over the current Signal truth: the player flies on the sector
> plane, asteroids and stations render with hard procedural silhouettes and
> Gaussian-style material accents, mining and tractor tension are readable in
> space, and the economy loop remains identical.

This gives players the "space mining in 3D" fantasy without forcing an immediate
wire protocol, AI pathing, save, replay, and economy migration to full
volumetric simulation. The visual rule for v1 is:

> Hard silhouette, soft matter, hard rules.

The splat-primary prototype proved the failure mode: it becomes a glittering
point cloud before it becomes a readable game object. V1 should use procedural
mesh/implicit geometry for the main silhouette of ships, asteroids, stations,
and modules. Gaussian-style splats then add ore veins, heat, dust, crystal
growth, exhaust, smelter vapor, signal haze, and atmospheric richness. Dock
lanes, collision silhouettes, mining targets, tractor tension, throw vectors,
module slots, and UI-critical contacts stay crisp lines or simple meshes.

## Product Pillars

1. **The rock is still the verb.**
   Mining, towing, smelting, throwing, and construction all remain built from
   physical rock handling. Procedural geometry gives rocks weight and shape;
   splats add ore veins, fracture heat, dust, and crystal growth.

2. **Signal is the readable frontier.**
   The 3D scene should make signal coverage feel like breathable civil space:
   strong signal has color, clean instruments, visible routes, and station
   confidence. Weak signal loses saturation, contrast, contact certainty, and
   ship responsiveness.

3. **Stations are institutions, not backdrops.**
   Stations should read as large procedural machines with a dock gap, rotating
   rings, module silhouettes, smelter beams, shipyard scaffolds, and local
   identity. Hard station geometry carries the readable machine; splats give it
   glow, vapor, grit, and signal atmosphere.

4. **3D must serve control clarity.**
   This is not a six-degrees-of-freedom sim in v1. It is a readable 3D
   presentation of an arcade mining economy. The player should always understand
   velocity, target, tractor tension, mining hit, docking approach, and danger.

## V1 Player Experience

The first playable 3D slice should support a complete 12 to 20 minute session:

1. Spawn docked at Prospect Refinery.
2. Launch into a 3D oblique/chase view of the sector plane.
3. Fly to a nearby procedural asteroid cluster.
4. Mine a ferrite asteroid until it fractures into smaller chunks.
5. Tractor several fragments and feel the tow band stretch in 3D.
6. Return to Prospect and feed fragments into the smelter beam.
7. Dock, see local credits, buy one early upgrade, and launch again.
8. Optionally throw a fragment hard enough to damage an NPC/player hull.

If v1 does only this but feels physical, readable, and beautiful, it wins.

## Non-Goals

- Full 3D movement, vertical combat, or six-axis docking.
- A protocol/save migration from `vec2` to `vec3`.
- Asset-heavy ship/station art pipelines.
- New weapons. Rocks remain the only weapon.
- A separate 3D fork of the economy loop.
- A landing-page/demo shell. The first screen should be the playable game.

## Simulation Model

V1 keeps the authoritative world in the current sector plane:

- Position remains `vec2`.
- Velocity remains `vec2`.
- Collision remains circle/annular station geometry.
- Mining raycasts remain planar.
- Tractor physics remain the current spring/tension model.
- Networking and replay stay compatible.

The 3D layer adds deterministic visual depth:

- Each object gets a stable render-space `z` offset from seed, tier, role, or
  station structure.
- Gameplay-critical anchors stay on the plane.
- Procedural mesh thickness, parallax, rotation axes, and lighting sell volume
  without changing truth.
- Procedural splats sit on top as material/effects, not as the only object
  representation.
- Rule overlays are generated from the same authoritative planar data the
  existing 2D renderer uses.

This is best described as **2.5D authority, 3D presentation**.

## Camera And Controls

V1 should use a stable chase/inspection camera:

- Camera trails the player from above and behind at a fixed oblique angle.
- The ecliptic plane remains visible through a faint grid, route dust, or signal
  sheet.
- Ship heading and velocity get separate cues: nose direction, exhaust, and a
  subtle velocity vector.
- Scroll/zoom or existing zoom keys can adjust distance, not pitch.
- Hail/scan can briefly widen the camera and reveal contacts, preserving the
  current scan rhythm.

Keyboard controls should remain compatible:

- `W/S`: thrust/brake/reverse along ship heading/travel rules.
- `A/D`: turn on the plane.
- `M`: mining beam.
- `Space`: tractor/release.
- `E`: dock/place.
- `H`: hail/scan.
- `B`: plan mode.

The biggest v1 risk is depth ambiguity, so the game should render shadows or
plane-projection markers for rocks, ships, scaffolds, and towed fragments.

## Render Primitive Decision

V1 should use a **hybrid procedural geometry renderer**:

1. **Procedural geometry for silhouette.**
   Ships, asteroid hulls, station rings, module blocks, scaffolds, dock mouths,
   and towable fragments need hard silhouettes. Use low-poly meshes, implicit
   primitive meshes, or generated faceted shells.

2. **Procedural splats for material and atmosphere.**
   Ferrite/cuprite/crystal veins, fracture glow, mining dust, exhaust, smelter
   vapor, signal haze, distant station volume, and route glints are generated as
   Gaussian-style point clouds from deterministic seeds.

3. **Crisp overlays for rules.**
   The playable plane, collision proxies, dock lanes, station module sockets,
   mining hit reticles, tractor tension ribbons, throw vectors, target brackets,
   and selected-object outlines render as lines or simple low-poly geometry.

4. **Ray-traced-to-splat assets wait.**
   Offline ray-traced scenes converted to Gaussian splats are attractive for
   authored hangars, title scenes, station interiors, or far-background
   megastructures, but they are too baked for v1's mutable rocks and stations.

This keeps Signal deterministic and asset-light. The game stores seeds and sim
state; the client regenerates geometry and splats.

## Procedural Generation

V1 generation should be deterministic and layered:

1. **Sector graph:** current station positions, route intent, signal roots, and
   outpost slots.
2. **Asteroid fields:** clusters generated around existing belt/route zones.
   Field density, commodity mix, and tier distribution stay sim-authored.
3. **Rock core geometry:** each asteroid seed produces an irregular faceted
   shell with stable dents, seams, fracture planes, and spin axis.
4. **Rock splat fields:** the same seed produces ore vein, dust, heat, and
   crystal-growth splats layered over the core geometry.
5. **Fragment lineage:** child fragments inherit visual features from the parent
   seed so fracture reads as "this came from that body."
6. **Station geometry:** rings, dock gaps, smelter beams, module silhouettes,
   and faction palette are generated from station state. The visible atmosphere
   can splat; the interactive lane and slot geometry stays crisp.
7. **Signal atmosphere:** color, fog, contact legibility, and route glints derive
   from signal strength and scan freshness.

V1 should avoid storing visual meshes or splat clouds. Store or reuse seeds and
generate render buffers client-side.

## Visual Language

### Asteroids

- Ferrite: dense, dark faceted iron body, warm rust/ore splats, chunky fracture
  edges.
- Cuprite: sharper faceted body, green/copper oxidation splats.
- Crystal: dark host rock with hard shard silhouettes, translucent shard splats,
  and thin glow lines.
- Larger tiers should feel slow and massive through spin, shadow, and inertia.
- S fragments should be readable as towable objects, not screen noise.

### Mining

- Mining beam should strike a surface point, not the object center.
- Damage progress appears as heat splats, seam light, dust, and crisp fracture
  lines.
- When fracture triggers, child fragments separate along deterministic fault
  planes and inherit parent coloration.

### Tractor

- Tractor is a physical tension ribbon, not a magic selection line.
- Slack bands are faint and cool, with a subtle splat haze around the tow path.
- Dangerous stretch bands glow warmer and show crisp release vectors.
- The hottest throwable fragment gets the clearest arrow and target bracket.

### Stations

- Keep the current ring identity, but render rings as hard segmented machinery
  with splat glow, dust, and vapor.
- Module ports should silhouette their function: furnace, shipyard, relay,
  storage, dock, service.
- Smelter beams should visibly pull fragments into the industrial mouth.
- Docking should feel like entering a lane, not touching a menu trigger.

### Ray-Traced Splat Setpieces

Ray-traced-to-splat scenes are a later content lane, not the v1 object model:

- Good fit: title scene, hangar backdrop, station interior tableau, far
  megastructure, hand-authored story location.
- Bad fit: live asteroid fracture, moving fragments, towable scaffolds,
  player-built outpost modules, anything that must expose exact collision.
- Integration rule: every baked splat scene needs a separate hard proxy graph
  for gameplay and navigation.

### UI

Use the existing Signal clarity grammar:

- Crisp means witnessed/fresh/certain.
- Faint/desaturated means stale/rumored/low signal.
- HUD text stays compact and instrument-like.
- Do not explain the 3D controls with in-world instructional blocks.
- Preserve station panels and manifests as functional tools.

## Technical Architecture

Companion visual studies:

- `web/3d-render-prototype.html` sketches the v1 render language for a ship,
  asteroid, and station using dependency-free WebGL2 meshes.
- `web/gaussian-splat-prototype.html` is a stress test showing why splats should
  be accents/effects rather than the only object representation.

Recommended shape:

1. Add a 3D render path beside the current 2D world draw path.
2. Add hard procedural geometry for primary silhouettes.
3. Add a procedural splat layer for material/atmosphere.
4. Add a crisp overlay layer for gameplay rules.
5. Keep `client/hud.c` and `client/station_ui.c` as screen-space UI.
6. Keep current sim, protocol, save, and tests unchanged for the first pass.
7. Gate the view behind a compile/runtime switch until it is shippable.

Candidate file layout:

- `client/render3d.h`
- `client/render3d.c`
- `client/render_splat.h`
- `client/render_splat.c`
- `client/proc_splat.h`
- `client/proc_splat.c`
- `client/proc_mesh.h`
- `client/proc_mesh.c`
- `client/rule_overlay3d.h`
- `client/rule_overlay3d.c`
- `client/world_draw_3d.h`
- `client/world_draw_3d.c`

Key jobs:

- Build Sokol pipelines for splat points, simple meshes, and rule lines.
- Add small math helpers for `vec3`, `mat4`, camera, and projection.
- Generate asteroid mesh cores and splat accents from existing asteroid seeds.
- Generate simple ship/station/scaffold procedural meshes as the primary visual
  shape.
- Project gameplay plane positions into render-space.
- Render shadows/projections and collision proxies back onto the plane for
  clarity.
- Continue rendering HUD/station UI in the existing 2D pass.

## Milestones

### M0: Design Lock

- Agree that v1 is 2.5D authority with 3D presentation.
- Pick the initial camera angle and control promise.
- Define success as one complete mining/smelting session.

### M1: 3D Render Foundation

- Create the 3D camera, mesh pipeline, splat pipeline, and line overlay
  pipeline.
- Render a test ship, grid/signal plane, starfield, and hard projection shadow.
- Keep existing HUD visible over the 3D scene.
- Build native and web.

Acceptance: the player can launch and fly in a nonblank 3D scene with the
current controls.

### M2: Procedural Asteroids

- Generate stable faceted asteroid shells from asteroid seeds.
- Add ore/heat/dust/crystal splats over the shell.
- Render commodity/tier visual differences.
- Add crisp target/collision outlines on top of the splat volume.
- Preserve LOD and culling.
- Add plane shadows and target markers.

Acceptance: existing asteroid fields read as real clustered rocks, with no sim
changes.

### M3: Mining And Fracture Readability

- Render surface beam hits, heat splats, seams, and fracture burst.
- Make child fragments visually inherit parent material.
- Keep hard fracture/contact overlays readable over the splat cloud.
- Keep mining hit logic planar.

Acceptance: a new player can tell what is being mined, when it is close to
fracturing, and which fragments are collectible.

### M4: Tractor And Rock Combat

- Render tractor ribbons in 3D, with optional soft splat haze around the band.
- Render tension/hotness/release vectors.
- Preserve existing throw logic.

Acceptance: the player can deliberately create a dangerous rock throw because
the 3D cue makes tension legible.

### M5: Stations And Docking

- Render hard procedural station rings, dock gaps, module silhouettes, and
  atmospheric splats.
- Keep docking trigger logic unchanged.
- Make smelter beams visually obvious in 3D.

Acceptance: the player can return to Prospect, feed fragments into the smelter,
dock, buy one upgrade, and launch again.

### M6: Signal And Scan Atmosphere

- Map signal strength to saturation, fog, contact clarity, and route glints.
- Preserve hail/scan reveal behavior.
- Keep critical UI cues saturated enough to read in low signal.

Acceptance: low signal feels like leaving civilization without hiding gameplay
information.

### M7: Web Polish And Performance

- Verify desktop and browser builds.
- Check desktop and mobile-ish viewport screenshots.
- Tune mesh complexity, splat counts, sort cost, LOD, and draw calls.

Acceptance: the web build is playable for the v1 loop without visual overlap,
blank canvas frames, or unreadable HUD text.

## Risks

- **Depth ambiguity:** solved with plane shadows, projection markers, and a
  restrained camera.
- **Splat-primary objects looking noisy:** solved by using hard procedural mesh
  silhouettes as the primary representation and keeping splats as accents.
- **Splat fuzz hiding gameplay:** solved by drawing collision, dock, target,
  mining, tractor, and throw reads as crisp overlays after splats.
- **Splat sorting cost:** start with object-local splat ranges, coarse
  back-to-front object sorting, and low counts; only add per-splat sorting where
  visual overlap demands it.
- **Scope creep into true 3D physics:** defer until after v1 proves the feel.
- **Immediate-mode/UI coupling:** keep the current UI pass and add 3D only for
  world geometry.
- **Web performance:** start with bounded procedural splat counts, simple
  billboard shaders, and aggressive LOD.
- **Offline splat asset temptation:** ray-traced/baked splats are allowed only
  for non-mutable setpieces until the live gameplay layer is stable.
- **Player expectation mismatch:** be explicit in design language that v1 is an
  arcade space mining game in 3D, not a six-axis flight sim.

## Open Questions

1. Should the 3D view replace the 2D view, or ship first as `?view=3d`?
2. Should camera distance be player-controlled, or fixed for readability in v1?
3. What is the right mesh-to-splat ratio per object type: hard ship, hybrid
   asteroid, mostly-hard station?
4. Should the first v1 demo include construction/outposts, or stop at
   mining/smelting/upgrading?
5. Do we want cockpit-lite UI framing later, or keep the current instrument HUD?
6. What is the per-object splat budget target for web: 256, 512, or 1024 splats
   for nearby XL asteroids?

## Definition Of Done

V1 is done when a fresh player can open the game, launch from Prospect, mine a
procedural 3D asteroid, tractor fragments, smelt them, dock, buy an upgrade,
and understand signal, velocity, mining progress, and tractor tension without
needing a manual.

The game must still feel like Signal:

- no abstract global wallet
- no non-rock weapons
- no fake construction layer
- no hidden economy fork
- no loss of replay/protocol compatibility for the current world
