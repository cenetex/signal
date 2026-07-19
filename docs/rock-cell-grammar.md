# Rock-to-structure cell grammar

**Status:** accepted design authority for #609

**Canonical diagram:** [`rock-assembly-mockup.svg`](rock-assembly-mockup.svg)
**Executable authority:** `shared/cell_geometry.h` and
`shared/cell_geometry.c`

Signal uses one construction size, one edge length `L`, and one pointy-hex
60° lattice. The rule is: **hexes hold things, triangles do things, and struts
connect things.** A square is an ingot or packed payload mark, never a docking
cell. A circle is a field, signal, sensor, orbit, or tractor cue, never matter.

## Canonical transformation

```text
irregular rock
  -> fracture (2-4 irregular physical fragments)
  -> smelt one fragment into 4 named square ingots
  -> press one ingot into 4 named standard struts
  -> assemble struts into triangle / hex / reinforced-hex cells
  -> keep a cell detached as a carrier, or weld it into a ship/station graph
  -> shear or detach the same cell back into towable salvage
```

The manufactured boundary is therefore countable:

```text
1 fragment = 4 ingots = 16 struts
3 ingots = 12-strut handling batch = 4 triangles = 2 hex rims
```

Twelve struts are a convenient handling batch, not a recipe constraint. A
carrier rim still costs six struts and may leave two struts from a two-ingot
press batch for the next build.

## Lattice and legal joins

Volume cells use axial integer coordinates `(q, r)`. Their six neighbor steps,
clockwise from world `+X`, are:

| Orientation | `dq` | `dr` | Direction |
|---:|---:|---:|---|
| 0 | +1 | 0 | east |
| 1 | 0 | +1 | south-east |
| 2 | -1 | +1 | south-west |
| 3 | -1 | 0 | west |
| 4 | 0 | -1 | north-west |
| 5 | +1 | -1 | north-east |

For edge length `L`, a cell center is:

```text
x = sqrt(3) * L * (q + r/2)
y = 3/2 * L * r
```

Hex and reinforced-hex cells join only when their axial distance is exactly
one, which means complete rims meet along one edge. A triangle names the host
volume coordinate plus one orientation `0..5`; it occupies that complete edge
slot. Vertex hardpoints use the same six-step orientation vocabulary but do
not create hidden structural adjacency. There are no fractional coordinates,
half-edge joins, arbitrary rotations, T-junction adapters, or scaled cells.

`cell_graph_encode()` writes a padding-free little-endian representation of
the graph. Equivalent layouts therefore produce identical bytes on native and
wasm builds.

## Cell balance

Mass below is expressed in structural-strut mass units. Payload adds its own
commodity mass. Active output is counted in module units so ship tuning can
map it onto the current flight model without copying a silhouette's stats.

| Cell | Visible construction | Struts | Shell mass | Cargo capacity | Equipment capacity | Active role |
|---|---|---:|---:|---:|---:|---|
| Directional triangle | three-edge rim | 3 | 3 | 0 | 1 | engine, tow, weapon, sensor, or brace |
| Standard hex | six-edge rim | 6 | 6 | 24 units | 0 | cargo, control, habitat, or passive system volume |
| Reinforced hex | six-edge rim + six radial struts + center coupling | 12 | 12 | 12 units | 0 | station core or heavy load-transfer hub |

The center coupling is a standard strut-end fitting, like every vertex joint;
it does not mint an extra matter unit. Radial struts consume the six additional
units.

Twenty-four units per standard hex matches the starter ship hold, divides into
six four-unit occupancy rails, and makes a full carrier visibly heavier than
an empty six-strut shell. A reinforced hub gives up half its interior capacity
to the center joint and spokes.

## Weld and dismantle conservation

Every cell owns its complete rim. Welding two hexes aligns and locks two
existing edge struts; it consumes neither and does not replace them with one
anonymous shared edge. The seam is therefore a doubled, inspectable joint.
Detaching either cell unlocks the seam and returns the same complete cell.

This deliberately rejects the tempting `6N - shared_edges` discount. That
discount would require dismantling to fabricate a missing rim strut or mutate
both neighboring identities. Extra bracing is always an explicit triangle or
reinforced-hub spoke with an explicit cost.

## Authored silhouettes proven by the grammar

| Layout | Graph | Struts | Capacity | Active modules | Fragment-equivalent structure |
|---|---|---:|---:|---:|---:|
| Tug | 1 control hex + 1 aft engine triangle | 9 | 24 | 1 | 0.5625 |
| Light freighter | 3 joined hexes + 1 aft engine triangle | 21 | 72 | 1 | 1.3125 |
| Heavy freighter | reinforced control hex + six surrounding cargo hexes + 3 engine triangles | 57 | 156 | 3 | 3.5625 |
| Utility ship | 1 control hex + engine, tow, and sensor triangles | 15 | 24 | 3 | 0.9375 |
| Station hub cluster | reinforced center + six ordinary neighbor hexes | 48 | 156 | 0 | 3 |

The executable templates are `CELL_LAYOUT_TUG`,
`CELL_LAYOUT_LIGHT_FREIGHTER`, `CELL_LAYOUT_HEAVY_FREIGHTER`,
`CELL_LAYOUT_UTILITY`, and `CELL_LAYOUT_STATION_HUB_7`. The 19-hex station
section is the next axial ring around the same seven-cell hub, not a larger
primitive.

An engine triangle's orientation names the edge on which it is mounted. Its
functional thrust vector is the opposite orientation. Rotating the mount by
one step therefore rotates the force vector by exactly 60°.

## Throughput worksheet

The old live rate is `10 ingots/fragment` and `2 struts/ingot`, or 20 struts
per fragment. The accepted rate is `4 * 4 = 16`. Existing frame-denominated
station costs scale by `16/20` to preserve their rock requirement:

| Construction target | Before | Before rocks | Accepted struts | Accepted rocks |
|---|---:|---:|---:|---:|
| Dock | 20 frames | 1 | 16 | 1 |
| Hopper | 40 frames | 2 | 32 | 2 |
| Furnace | 60 frames | 3 | 48 | 3 |
| Repair bay | 30 frames | 1.5 | 24 | 1.5 |
| Signal relay | 40 frames | 2 | 32 | 2 |
| Frame press | 80 frames | 4 | 64 | 4 |
| Shipyard | 120 frames | 6 | 96 | 6 |
| Founding outpost contribution | 60 frames | 3 | 48 | 3 |

Laser and tractor fabrication sites currently ask for 80 specialty ingots.
Their retuned material amount is 32 specialty ingots, retaining eight source
fragments at the new four-ingot yield. #601 must show that specialty matter as
interior equipment/core treatment inside its structural cells; it must not
pretend the specialty ingots are rim struts.

The first carrier shell needs six struts (two pressed ingots with two struts
left over), and the tug structure needs nine. Both remain reachable within the
first fragment's 16-strut output once the required active core is available.
The first 48-strut station contribution remains exactly three fragments.

## One-size decision

The one-size grammar is **accepted**. Its gameplay reason is reversibility:
the object a player sees, tows, welds, targets, and later salvages keeps one
identity and one matter count across every transition. A second “mega” cell
would add a second capacity scale, collision family, tow rule, recipe ladder,
and hidden adapter problem without adding a new decision. Scale instead comes
from graph size and LOD groupings: one cell, seven-cell hub, and 19-cell
section.

Stations use the same cells at a second *assembly* scale without inventing a
second primitive size. A module remains one `L`-edge cell at its authored
world site. Longer hub/arm/ring distances are open megastructure trusses,
tessellated into repeated standard-length strut bays. Their circular path is
an orbit/layout convention, not a filled material ring. The compact axial
cell graph remains the deterministic construction and identity topology;
`station_build_geom()` projects each node onto its live module site so cells,
targeting, towing, production fields, and collision all name the same point.

## Implemented surfaces

- #600 renders every detached carrier as one hex shell with a separate
  interior payload treatment and six readable occupancy rails.
- #599 applies the `4 -> 4` yields and the throughput table above.
- #601 deterministically projects durable station module records through the
  axial construction graph onto authored module sites, beginning with a
  reinforced hub. Cells are joined by repeated open truss bays; material
  circles are never rendered.
- #610 maps live hull assets onto authored tug, utility, and freighter graphs;
  capacity, mass, center of mass, and active output are graph-derived.
- #611 uses named complete-edge hardpoints, shell-plus-payload mass, angular
  impulse, and rotated-hex narrow-phase collision. Hardpoint, angle, and spin
  survive save and wire snapshots.
- #603 shears the same graph nodes into identity- and momentum-preserving
  salvage, with bounded join stress, staged hub damage, canonical codecs, and
  reversible repair.
- #602 packages payload members under a domain-separated Merkle identity while
  keeping member, package, and carrier-shell identities separate. Package
  shape and member count never alter the structural graph.
