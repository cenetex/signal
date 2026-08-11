# Perception acceptance review

The normal browser smoke lane includes one deterministic acceptance test for
the six visibility/perception player questions. Each scenario uses live game
state and the same production reason/label builder as the rendered surface.
The deploy workflow uploads the successful desktop and narrow canvas captures
as the `perception-review-<commit>` artifact.

| Player question | Fixture | Semantic source | Review attachments |
| --- | --- | --- | --- |
| Why is this rock useful? | Targeted ferrite fragment beside Prospect | Flight HUD strongest-use classifier | `perception-rock-value-{desktop,narrow}` |
| Why is civilization/control thinning here? | Ship outside signal coverage | Signal warning and saturation model | `perception-signal-loss-{desktop,narrow}` |
| Why can I spend credits here but not there? | 80 Prospect vouchers, docked at Kepler with zero local balance | Station ledger strip and cargo-bridge hint | `perception-local-money-{desktop,narrow}` |
| Why did that worker choose that route? | Hauler choosing a receipt-backed Prospect-to-Kepler route | NPC contact-card reason and clarity builders | `perception-npc-motive-{desktop,narrow}` |
| Who remembers what I did? | Station-signed Prospect-to-Kepler delivery history | HISTORY signed-proof labels | `perception-remembered-work-{desktop,narrow}` |
| What changed because I built this? | Newly commissioned signal relay | Module activation consequence label | `perception-construction-consequence-{desktop,narrow}` |

The automated gate checks semantic copy, crisp/degraded NPC and route
knowledge, canvas bounds, and render presence. During a release review, inspect
the artifact for the effects that are intentionally not reduced to brittle
pixel assertions:

- Motion: the commission pulse and signal-resaturation pulse begin at the
  relevant object and do not look like unrelated ambient animation.
- Salience: the answer to the player question is readable before secondary
  metadata, without covering the ship, target, or station interaction.
- Hierarchy: degraded knowledge looks visibly less certain than witnessed
  knowledge; raw confidence numbers are not required to understand that
  distinction.
- Narrow layout: all six cues remain inside the canvas/panel, retain a usable
  type size, and do not collide with touch controls.
- Honesty: station-local money is never summed into a global wallet, missing
  provenance is a gap, and NPC motive copy comes from observed job evidence.

If a semantic assertion fails, the browser error names the player question it
violated. If a visual review fails, record the scenario, viewport, and artifact
name on the sprint tracker before changing presentation.

## HUD attention-budget review

The same browser lane captures a deterministic attention pass:

| State | Expected primary surface | Review attachment |
| --- | --- | --- |
| Docked, desktop | Station terminal; healthy build/network diagnostics absent | `perception-hud-attention-station-desktop` |
| Onboarding message, narrow | One tutorial/notice instruction; competing flight action absent | `perception-hud-attention-message-narrow` |
| Undocked scoreboard, narrow | Scoreboard only; no flight panel or tutorial subtitle beneath it | `perception-hud-attention-scoreboard-narrow` |
| Selected NPC scan, narrow | Inspect/contact card; stale scoreboard and tutorial subtitle suppressed | `perception-hud-attention-inspect-narrow` |

The gate also asserts that F3 is the explicit diagnostic toggle and that scan
budgets contract from eight asteroid/four NPC labels on desktop to four
asteroid/two NPC labels on narrow screens. Critical connection states remain
visible without F3; healthy version, latency, queue, and replay telemetry does
not.
