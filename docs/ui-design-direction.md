# Signal — UI Design Direction

**Date:** 2026-06-14 · **Status:** Direction / design report (v0, single-player-first) · **Audience:** engineers + agents implementing client UI (`client/hud.c`, `client/station_ui.c`, `client/world_draw.c`, `client/onboarding.c`, `client/input.c`).

This is a *design direction* report, not an implementation spec. It states the thesis, the one organizing principle, and the prioritized UI work with enough design detail to build against. It deliberately avoids code — the data hooks named here already exist in the sim.

---

## 1. Thesis

Signal's surface pitch is "it's about the rock." That's the hook. But the machinery the project has actually built deepest is **provenance**: every rock, fragment, ingot, route, kill, and now every *rumor* carries a verifiable history. The real game is:

> A single-player-and-up world where everything has a verifiable history, the NPC economy is a living society that reads and reacts to that history, and the player builds a legend that society can perceive.

The rock is the verb. Provenance is the noun. The neural-worker / holographic-gossip system isn't backend marvel for an empty server — in a single-player or low-population session **the NPC economy is the cast**, and the entire social signal of the game flows through whether the player can *perceive that world reacting to them*.

## 2. The core tension this report exists to resolve

The sim's intelligence lives where the player cannot see it. The first legibility pass closed several acute surfaces: thrown rocks now expose release tension and target locks, low signal now names control loss, construction activation names capability, and single-player station UI explains local credits. The remaining crisis is less about missing warning lights and more about synthesis: NPCs, stations, and rocks need to lead with the strongest state-grounded reason they matter.

This is **one legibility crisis, not three UI bugs.** Recent work raised the legibility floor; the direction below remains: *stop adding invisible intelligence; surface the intelligence already computed.*

## 3. The organizing principle — **clarity = certainty**

This medium is monospace text + primitive shapes over a starfield, with no widget toolkit. It rewards one rule that we should apply everywhere:

> **Information renders with a visual clarity that matches how certain and fresh it is.**

The gossip layer already computes confidence, salience, age, and hop count for every memory. Map those straight to rendering:

- Witnessed / high-confidence / fresh → crisp text, full color, solid brackets.
- Heard second-hand / low-confidence / stale → faint, desaturated, glyphs degrade toward `?`, more hops → more garble.

Why this is the unlock:
- It fits the procedural medium — it's brackets, alpha, and saturation, not chrome.
- It reuses the **signal** pillar (the game already desaturates the world by signal quality).
- It makes the provenance model legible **without a single explanatory sentence**. A rumor should *look* like a rumor. We never render a `confidence: 0.4` label; we render a contact that is literally harder to read.

This single rule unifies the throw, the NPC readout, the gossip strip, contract provenance, and the signal HUD into one visual language.

## 4. Design values / constraints

- **Diegetic over chrome.** Prefer instrument-readout / scan-result framing (brackets, reticles, scanlines, contact cards) over dashboard panels.
- **Honest to the sim.** Never narrate intelligence the agents don't have. The NPCs barely path today; UI must read real state and grow richer *as the sim does*, not write checks the sim can't cash.
- **Teach through the hands, not the manual.** The best cue is one the player learns by doing (see the throw). Reserve text for subtitles.
- **Single-player is the bar.** Every surface must make a solo session feel inhabited and reactive. Acceptance test for any UI change: *does this let the player perceive the world knowing they exist?*
- **Respect what already works** (Section 7).

## 5. Prioritized work

### Current shipped baseline

These items moved from direction to implemented baseline:

- Throw visualization: hot/cold release vectors and lethal target brackets.
- Low-signal control banner: `[ LOW SIGNAL -- CTRL n% ]` before near-zero loss,
  with `CTRL%` telemetry in the compact path.
- Station-local ledger legibility in single-player: docked ledger strip and
  first zero-balance local-credit notice.
- Construction consequence notices: activation text names relay, furnace,
  shipyard, dock, storage, or service consequences.
- Fragment usefulness first pass: tracked contract fit, direct station demand,
  and rare grade fallback in the target/tow HUD.
- HUD attention budget: death, station, inspect, scoreboard, message, and
  flight surfaces have deterministic precedence; scoreboard is modal, scan
  labels contract with viewport width, and healthy build/network telemetry is
  available on F3 instead of occupying the play surface.

The next work should not repaint those shipped cues. It should stabilize NPC
motive, summarize station memory, and deepen rock value beyond direct demand.

### P1 — Throw visualization (shipped baseline)

The hidden rule: a slack release does ~0 damage; the player must deep-stretch the tractor band toward its snap limit to clear the hull-damage threshold (release speed ≈40 base vs damage threshold ≈115). Per-fragment velocity/stretch is already computed server-side. Draw it as world-space vectors:

```
                          · ·          cold fragments → faint gray stubs
            ◇ ◇                        (harmless if released now)
     ◆—————————————◈  ship
            ◇ ◇
                  ╲▂▄▆██▶   most-stretched fragment: arrow length = release
                            speed, color ramps ORANGE→RED across the damage
                            threshold
   ╭─ lock ─╮
   ╎   ✦    ╎   a ship inside a HOT fragment's release cone gets a bracket
   ╰────────╯
```

- No tutorial text. The player swings wide, watches a stub turn red, and learns "make it glow, then release" through motion.
- The lock bracket appears **only when a lethal shot is actually pointed at a target** — so it doubles as "you can hurt that pirate now."
- Honest caveat: tap-release flings *all* towed fragments along their own axes, so "aim" is really "which fragment is hottest and where it points." The visualization works with that. If true aim is later wanted, the minimal sim change is *tap = throw hottest toward facing; hold = dump all* — the UI is identical either way. **Ship the visualization first; only change the sim if players still can't aim.**
- Hooks: `world_draw.c` now owns release preview, hotness, and lock rendering.
  Future throw work should be driven by playtest findings or by richer
  economic usefulness, not more combat chrome.

### P1 — NPC contact readout ("read their story")

Current selected-job scans use this shape: callsign/clarity, action +
destination, role/state + home, "because ..." motive, then route-memory/proof
source when available. Keep the contact card stable and legible before adding
more worker autonomy.

Replace the hash/diagnostic dump shown on scan/hail with a small contact card in the clarity grammar. Fresh, close contact:

```
┌─ CONTACT ───────────────────┐
│ GULL-7            ▮▮▮▮ clear │
│ hauling cuprite             │
│ → Helios · heard it pays    │   the "why," from the NPC's gossip memory
│ ran Prospect→Helios ×40     │   shown only if route-history exists
└─────────────────────────────┘
```

Stale / second-hand contact (heard about, not witnessed):

```
┌─ CONTACT ───────────────────┐
│ G?LL-?              ▮ faint  │
│ hauling ??? · rumor         │
│ → ? · 3 hops · old          │
└─────────────────────────────┘
```

- The card shows **only what the gossip layer actually knows**, at the clarity that layer assigns. A dumb beeline hauler reads "hauling cuprite → Helios" and nothing more — no faked personality.
- This converts the project's #1 stated legibility gap into a pure UI win on data that already exists, and it is the main thing that makes a solo world feel populated.
- Hooks: the existing inspect-snapshot wire data + gossip memory fields (subject, confidence, salience, age, hops, route reputation). Render through the existing callsign helper (never raw hex).

### P2 — Station memory and overheard gossip at dock

Docking already triggers physical gossip exchange, and station UI can already
show OVERHEARD/HISTORY rows. The next improvement is a first-read station memory
strip: what this place is known for, and what it remembers about the player.

```
  · a hauler's been working the Helios run
  · ⚠ rocks thrown near Kepler recently      (faint = old / unconfirmed)
```

This is the connective tissue of a solo session: stations should feel like
institutions with memory, not only menus with history tabs.

### P3 — Rock economic usefulness

The target/tow HUD now has a first-pass usefulness line: tracked work, direct
station demand, and rare grade. Deepen it only where the sim can prove the
reason:

```
  needed at Prospect
  smelts to FE ingot
  route remembers Prospect>Kepler
```

Rules:

- direct demand is crisp
- route/gossip memory uses clarity degradation
- tracked contract still wins over general usefulness
- rare grade remains the fallback when no stronger economic reason exists

### P4 — Multiplayer ledger snapshot

The sovereign-currency conceit is legible in single-player. Multiplayer still
needs a compact known-ledger snapshot before the client can safely show
cross-station balances:

```
  PROSPECT 240 · HELIOS 0 · KEPLER 88     (current station highlighted)
```

Do not imply a global wallet. Unknown rows should be absent or explicitly stale,
not silently zero.

### P5 — Low-signal control banner (shipped baseline)

Control collapses toward 0 at the FRONTIER band, and the HUD now fires a
`LOW SIGNAL -- CTRL n%` warning before total collapse. Future work here is
threshold/intensity tuning, not new conceptual UI.

## 6. Cleanup to fold into the UI pass (confirmed defects)

These came out of the UI review and should be swept while the surfaces are open:

- **Compact panels overflow with no clipping** — the compact TRADE panel runs ~100px past its frame; no scissor anywhere in `station_ui.c`. Clip to the panel rect or budget rows against remaining height (drop optional lineage lines first). This violates the narrow-window requirement.
- **Dead `[S] deliver` label on the SHIP panel** — the construction row advertises a key the SHIP input handler never reads. Either wire it or remove the label.
- **Native ESC quits the app at the event layer**, bypassing plan-mode / popup modal handling. Gate the quit behind a no-modal-active check and let ESC flow through the intent sampler.
- **Multiplayer ledger snapshot parity** — SP can read local world ledgers;
  multiplayer still needs explicit known-ledger rows before rendering the same
  cross-station strip honestly.
- **Always-on chain-audit lineage dump on trade rows** (`serial / parent / ep / seal`) — forensic detail with no player verb. Gate behind an explicit inspect toggle; keep serial + origin in the default view.

## 7. What already works — protect it

- A real **virtual canvas with DPI handling** (`ui_window_*`, `ui_scale`) — the HUD is not raw pixels; text scales and reflows.
- **Centralized, themeable palette** with near-zero scattered color literals, and **position-aware signal desaturation** in one place.
- **Shared row builders** (`build_trade_rows`, `build_work_slots`) feed both renderer and input picker — the key you press is provably the row you see.
- **Hashes render as callsigns/words**, never raw hex.
- **Touch is real on web** via a synthetic-keycode bridge reusing the intent pipeline.

Do not regress these while adding the above. (Note: critical HUD *text* currently bypasses the saturation floor that protects world cues — decide whether HUD text is intentionally exempt and document it, or route it through the same floor.)

## 8. Explicitly out of scope for this pass

- **Decentralization / trust UI** — Sector-X foundation work; the cryptographic guarantees are not a v0 player-facing surface.
- **Deep institution / route-history browser** — the compact HISTORY strip is enough for v0; a full browser waits until the reputation layer has more distinct evidence.
- **Changing the throw to true single-fragment aim** — shipped visualization
  should be playtested first.

## 9. North star

Two tests for everything the UI pass touches:

1. *Does this let the player perceive the world knowing they exist?*
2. *Clarity = certainty* — if a surface needs a "confidence" label, it's the wrong rendering; make the thing itself harder or easier to read.
