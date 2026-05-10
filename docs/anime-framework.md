# Milestone Videos

**Status:** cut down to shipped milestone videos only.

Signal no longer carries a broader "anime content" roadmap. The videos are
milestone rewards for important progression beats. They should support the
core loop, not become a parallel narrative system.

## Cut Line

Keep:

- The existing milestone videos.
- The in-engine MPEG playback path documented in
  [`anime-integration-plan.md`](./anime-integration-plan.md).
- Lightweight trigger logic tied to already-important game events.

Cut:

- Signal Ghost discovery nodes.
- Signal Archive modules.
- Audio-fragment collection.
- Future episode seasons.
- New lore-delivery systems that compete with mining, hauling, construction,
  station ops, or rock combat.

## Current Milestones

| Episode | Title | Intended beat |
| --- | --- | --- |
| 0 | First Light | First launch. |
| 1 | Kepler's Law | First proof that the three-station economy matters. |
| 2 | Furnace | First meaningful smelt/sell moment. |
| 3 | Scaffold | First scaffold tow. |
| 4 | Naming | First outpost activation. |
| 5 | Drones | First visible automation at an outpost. |
| 6 | Hauler | First completed supply/tractor contract. |
| 7 | Dark Sector | First real signal-loss moment. |
| 8 | Every AI Dreams | Network reaches critical mass. |
| 9 | Death | Player death. |

If trigger details drift, the implementation doc is the source of truth. This
file records product intent.

## Product Rule

A milestone video earns its place only if the player just did something the
game already wants them to care about.

Do not add a video trigger to make a weak milestone feel important. Fix the
milestone first.

## Tone

Keep the presentation restrained:

- Short, diegetic, signal-borne.
- Industrial rather than expository.
- Focused on the player's action and the station network's response.
- No new required reading or collection layer.

The strongest version is: the player acts, the network notices, and the video
arrives as a brief artifact of that change.
