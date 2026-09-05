# Release in progress: v0.6.0 — First Outpost

Review date: 2026-09-05. Code reviewed: `9337c7eeacd00e91d058172e98ba7d2f99ec2d6a`.
Status: implementation and acceptance checks are in progress.

The release promise is simple: earn your way from a starter ship to an active
relay, understand how your work changed the route, and return to that progress
after a restart. This gives the existing
[Playable Core milestone](https://github.com/cenetex/signal/issues/690) a clear
release outcome.

Use v0.6.0 because the 65 commits since v0.5.4 add a worker story, physical trade,
shared gameplay paths, and durable economy work. "First Outpost" is the proposed
release name. The release date follows the acceptance results.

## Build status

Updated September 5 after implementation began. The original review below
records the starting point; this table records the release work.

| Work | Current result | Next check |
| --- | --- | --- |
| Player story and guide | [#723](https://github.com/cenetex/signal/pull/723) merged. Progress is scoped to the player and local world or server. | Final route checkpoints. |
| Panel controls and receipts | [#724](https://github.com/cenetex/signal/pull/724) merged. Escape closes the active panel; receipt names and narrow trade rows are readable. | Final packaged play review. |
| Local world recovery | [#725](https://github.com/cenetex/signal/pull/725) passed every required CI check at `7209e146e455b5d55c145f3cf7342687fb656f43`. World save 85 and player save PLY8 preserve the exact borrowed hull and local progress. | A coherent production backup before the save upgrade. The brief service pause is awaiting approval. |
| Media packages | [#726](https://github.com/cenetex/signal/pull/726) adds pinned pack hashes, source records, and native/web/server archive layouts. Nine small-file tests pass. | The canonical music and portrait files, then checks of the real archives. |
| Ordinary-action route | [#727](https://github.com/cenetex/signal/pull/727) is a draft. Fresh runs reach fracture, tow, payment, both first upgrades, and 49 activation frames. Saved milestones restore exact crate content and held cargo. | Complete relay activation and prove each saved milestone. |
| Crate trade data | #727 includes the frame shell in the charge and sends the exact quote and unpack source in protocol 8. | Final CI and the complete route. |
| Physical play and final release | Pending the completed route baseline. | Named-hardware motion captures, new-player play, final memory checks, archives, and release notes. |

The merged changes are live at `eba8e77`. The public health check reports four
healthy station chains. The save-format and media branches remain separate
from that deployment while the required backup and source files are pending.

Current local checks on the route branch: 1,697 native tests, 1,697 sanitizer
tests, 9 functional soak tests, and 57 Chromium checks pass; 4 browser checks
require their dedicated live/adverse-network setup. The web build uses 885
initial WASM pages. These results cover the current changes; final candidate
qualification follows the remaining implementation.

Measured route milestones are controller results. A new-player session
will measure pacing. The fresh run fractures at 13.93 seconds, attaches ore at
14.64 seconds, and receives its first payment at 186.08 seconds. The physical route reaches the mining upgrade at 367.58 seconds and the hold
upgrade at 377.59 seconds. A second ore haul raises held frames to 49 at
918.93 seconds. Shipyard delivery and relay activation remain in progress.
The driver returns success after an active owned relay and successful
checkpoint checks.

The extracted macOS ARM64 server archive built from `540de4f` passes two
localhost starts at protocol 8. All four station chains are healthy. World
time continues from tick 121 to 125. Graceful shutdown takes 0.019 and 0.022
seconds, and each shutdown publishes a new complete generation. This result
covers an empty local server on ordinary disk; the slow-disk and occupied
server checks remain open.

Release copy is in [the draft notes](release-notes-v0.6.0-draft.md).

## Review result

Signal has a strong game identity: rocks become income, income becomes physical
cargo, and cargo becomes signal coverage. The station roles, local currencies,
and worker story support that loop. The next release should make one full
session feel clear, responsive, and durable.

The engineering baseline is substantial. Current main includes the tow fixes,
background save writer, durable station payouts, HUD attention work, and worker
journey. The largest remaining product task is to prove these pieces together
through ordinary play.

| Area | Current evidence | Release decision |
| --- | --- | --- |
| Live service | Both public health endpoints returned `9337c7e`, status `ok`, and four healthy station chains. The browser rendered the station and flight views. | Use current main as the starting point. |
| Packaged release | [v0.5.4](https://github.com/cenetex/signal/releases/tag/v0.5.4), published July 25, remains the latest release. Its assets are three native clients and one Linux server. | Qualify a complete new package set. |
| Local checks | 1,684 native tests and 9 functional soak tests passed on the reviewed code. The native client also compiled. | Preserve these checks throughout the release. |
| Hosted checks | Current main passed [deployment verification](https://github.com/cenetex/signal/actions/runs/33950374180) and [cross-runner replay](https://github.com/cenetex/signal/actions/runs/33950374137). The September 5 Valgrind run was cancelled. | Require complete evidence for the final candidate, including a completed memory check. |
| Story | The event-based worker journey is merged in [#721](https://github.com/cenetex/signal/pull/721). | Finish progress storage and verify the journey through normal actions. |
| HNN pilot | The [mixed-mode report](liblecore-pilot/mixed-evaluation.md) records 162 passing checks and zero HNN selections across 8,631 decisions. | Keep shadow mode as the default. Further AI promotion belongs to the later evaluation work. |

## Findings that shape the scope

### 1. Story progress needs a durable home

[`client/story_runtime.c:22–42`](https://github.com/cenetex/signal/blob/9337c7eeacd00e91d058172e98ba7d2f99ec2d6a/client/story_runtime.c#L22-L42)
stores progress inside the browser-only compilation branch. Native progress
therefore lasts for the current process. Browser progress uses the single
`signal_story_loop_v1` key, which shares the same flags across identities and
worlds on that origin.

Persist story progress for native and browser clients. Key progress by the
player and the chosen world or server. Define migration of the existing browser
key. Keep accepted world events as the source of progress, including valid
existing player-owned relays. Test process restart, browser reload, reconnect,
identity change, and switching between local and shared worlds.

Evidence level: source review. The current story tests exercise the state
machine; storage and restart behavior need runtime coverage.

### 2. Escape can quit while a player is reading a panel

[`client/main.c:5484–5491`](https://github.com/cenetex/signal/blob/9337c7eeacd00e91d058172e98ba7d2f99ec2d6a/client/main.c#L5484-L5491)
requests quit while the inspect pane or scoreboard can be active. This confirms
the control-flow finding in [#720](https://github.com/cenetex/signal/issues/720).

Make Escape close the active surface. Use the same attention choice for input,
rendering, and browser inspection. Include readable receipt labels and a narrow
TRADE panel check in this focused UI pass.

Evidence level: source review of the quit path. Native interaction and small
viewport rendering remain acceptance work.

### 3. Release media needs a complete delivery path

The checked production URLs returned HTTP 404:

- `/anime/ep0-first-light.mpg`
- `/music/nebula_drift_protocol.mp3`
- `/stations/prospect/portrait.png`

The current `make assets` target exits with an explanation that the download
command is retired. Native packaging copies the executable into each archive.
Web packaging includes four client files. The container copies anime and music
when present. These paths need one verified media source and complete package
contents. See [#656](https://github.com/cenetex/signal/issues/656),
[`assets/manifest.txt`](../assets/manifest.txt),
[`release.yml`](../.github/workflows/release.yml), and
[`server/Dockerfile`](../server/Dockerfile).

Restore media provisioning before build-time feature detection. Record hashes
and source rights with the asset manifest. Package the assets for each runtime.
Check representative responses, content types, and hashes after deployment.

Evidence level: current source and public HTTP responses. The three requests
sample one asset from each media class.

### 4. First-hour completion remains an acceptance gap

[#688](https://github.com/cenetex/signal/issues/688) remains open. Its required
path is `launch → fracture → tow → smelt → trade → upgrade → acquire frames →
found an outpost`. Browser smoke covers startup, controls, network behavior,
and selected HUD states. The story test advances through direct event calls.

Add a scenario that reaches an active outpost through normal player actions in
a fresh test world. Then have a new player follow the same route from visible
cues. Exercise the handoff from the economy guide to the story, including the
Helios and Kepler hails, weak signal, Blackglass, relay activation, delivery,
and return to Prospect. Check that the story's route language matches the
events that actually advance it.

Evidence level: source and issue review. The live browser inspection covered
entry and launch; full-session pacing remains to be measured.

## Work order

Each row is a focused PR in its own worktree. Keep the plan current as measured
results narrow the remaining work.

| Order | Work | Completion check | Depends on |
| --- | --- | --- | --- |
| 1 | Persist the worker journey | Native restart, browser reload, reconnect, and player/world separation preserve the correct beat; existing browser progress has a defined migration. | Current main |
| 2 | Finish panel controls and labels, #720 | Escape closes each active surface; input and rendering agree; inspect labels are readable; TRADE fits desktop and narrow views. | Current main |
| 3 | Restore and package media, #656 | A clean build obtains verified assets; all five release archives carry their required content; production samples return valid media. | Access to the canonical media source |
| 4 | Prove the full route, #688 | The automated ordinary-action route and a new-player session reach an active relay; each economic milestone survives reconnect and reload. | 1 and 2; use 3 for final presentation review |
| 5 | Tune the measured physical-play gaps, #684 and a focused part of #689 | Acquire, tow, swing, release, impact, and recovery feel clear; two existing upgrade choices have visible value; a 15-minute haul/combat session has readable outcomes. | Baseline captures from 4 |
| 6 | Qualify saves and release artifacts; refresh milestone status | Supported save upgrades and failure recovery pass; candidate checks complete; archives launch; release notes match the delivered behavior. | 1–5 |

Planning estimate: about 8–12 focused engineering days for one developer,
followed by the time needed for candidate checks and play review. This assumes
the media source is available and save qualification needs only focused repairs.
Confirm the estimate after the first ordinary-action progression baseline.

## Acceptance targets

These pacing values are proposed targets. Record actual results before tuning.

| Milestone | Target for a new-player session |
| --- | --- |
| First fracture and successful tow | Within 5 minutes |
| First visible station payout | Within 10 minutes |
| First meaningful upgrade | Within 25 minutes |
| First active player relay | Within 60 minutes |
| Progress stalls | The player can name a next action; any unexplained stall over 2 minutes becomes a tracked defect. |

Record stage times, rejection reasons, station balances, cargo custody, and the
current story beat. Keep test records local or in the PR. Use fresh isolated
worlds for destructive recovery and combat scenarios.

Run the route in local mode and against a test multiplayer server. Reconnect or
restart at the payout, purchase, frame handoff, outpost, and story milestones.
Verify exact cargo quantities, owner identity, and station balances at each
boundary. Add a second player for slot reuse, shared construction, and combat
attribution checks.

Capture ten-minute native and browser motion traces on named test hardware.
Use the existing [jank report](gameplay-jank-observability.md) for frame p50,
p95, p99, tick loss, correction, and save timing. Start with the current 16.6 ms
frame budget on the reference desktop. Record local, loopback, and adverse
network results separately. The final candidate must pass the existing numeric
correction limits and a visual review of tow, collision, and camera motion.

## Save and backlog review

Several open reports describe earlier code. Update them against their remaining
acceptance criteria:

- **#690:** six of its ten listed child issues are closed (#686, #685, #687,
  #617, #674, and #619). Refresh the checkboxes from issue state and attach the
  final session evidence.
- **#666:** complete save generations and a background writer are implemented.
  `docs/save-persistence-generations.md` still describes the older synchronous
  phase. Qualify chain/snapshot recovery, slow disk behavior, and shutdown; list
  the remaining work precisely.
- **#668:** current code contains stable actor principals and ownership
  quarantine. Review remaining owner fields and adversarial cases before
  closing the issue.
- **#672 and #658:** authenticated recovery exists. The recovery document
  describes the remaining historical-world blocker: unresolved ownership
  quarantine can reject a claim. Verify the supported recovery path and its
  player message. Define operator reconciliation separately.
- **#676:** save version is now 84. Map the old v78 request to current cargo
  classification and quarantine behavior. Qualify every supported migration
  path used by this release, including repeat load/save and custody checks.

Release qualification must demonstrate recovery of a consistent world and
player generation. Include payout retry, interrupted save, chain history ahead
of the selected snapshot, and restart with a different player in the old slot.
Any discovered loss, duplicate credit, or ownership transfer becomes a release
blocker. Keep an explicit recovery procedure for a blocked station chain.

Keep the broader AI promotion, federation, settlement, Sector X, and scale work
in their existing later milestones. This release uses the current shadow-mode
AI decision policy and existing world capacity.

## Candidate and release procedure

1. Select the final commit after the scope PRs pass their relevant checks.
   Verify full native, sanitizer, soak, static, replay, browser, container,
   macOS, and Windows results for that candidate. Complete a memory-check run.
2. Run the first-hour, save recovery, motion, media, and narrow-layout checks
   above. Inspect the actual packaged native clients and release WebAssembly.
   Enforce the existing memory ceilings: 17,000,000 bytes for `world_t`,
   22,250,000 bytes for `game_t`, and 896 initial WASM pages.
3. Record the previous live image, a coherent backup of saves and station
   chains, the asset revision, and the tested restore procedure. Save version
   compatibility determines which binary and backup can be restored together.
4. Tag the tested commit and use the existing **Stage Release Artifacts**
   workflow. Inspect its populated draft: Linux, macOS, Windows, Linux server,
   and web archives, with hashes and the reviewed release notes.
5. Obtain the release owner's approval immediately before public publication.
   Publish the reviewed draft. Verify the canonical play page, health version,
   station chains, media routes, and a reconnect against the chosen release.
   The service normally deploys from main; record the live commit separately
   from the release tag.
6. If a gate fails after rollout, apply the tested restore procedure and check
   the restored version, chains, and representative player state.

## Review evidence and limits

Completed locally on the reviewed commit:

- `make test`: 1,684 / 1,684 passed.
- `make test-soak`: 9 / 9 passed.
- `make cargo-trust-audit banned-apis deterministic-libm doc-freshness
  soak-automation vendor-drift`: passed.
- Native client build and memory probe: `world_t=16,917,576`,
  `game_t=22,199,672`; both are within the current ceilings.
- Read-only source review, current GitHub issue/release/workflow review, public
  health and media requests, and live station/flight visual inspection.

Sanitizer and browser-suite evidence in this review comes from the successful
current-main deployment workflow. Full first-hour play, artifact installation,
and new failure-injection scenarios remain the release work defined above.
