---
name: Milestone video integration status
description: Anime scope is narrowed to shipped milestone-video playback, not a broader narrative system
type: project
---

The old broad anime roadmap is cut from MVP scope. Current product scope is narrow: play existing milestone videos at meaningful progression beats through the in-engine episode popup. There is no Signal Ghost, Signal Archive module, audio-fragment collection, visual-echo discovery layer, or future-season roadmap in current scope.

**Why:** The current docs intentionally keep videos secondary to play. Milestone videos should reward real progress without adding a parallel narrative collection system or delaying mining, hauling, construction, station economy, and rock-combat readability.

**How to apply:** Use `docs/anime-framework.md` and `docs/anime-integration-plan.md` as the current source of truth. Episodes are MPEG-1 `.mpg` assets decoded in `client/episode.c`, rendered as bottom-right milestone popups, and triggered from gameplay events. Remaining work is asset-production workflow, optional native watched-state persistence, browser smoke coverage, and revisiting the episode 2 trigger if the stricter "all three ore types" milestone still matters.
