# Ledger Balances Are float32 — Design Note

Status: accepted interim design · written 2026-07-18 · supersedes nothing,
documents what was previously only implicit in code.
Migration tracked as issue #615.

Station ledger balances — the per-station sovereign credits that are the
game's only money — are stored and transmitted as `float`. This note records
why that is currently survivable, where the real error bounds are, and what
a migration to exact arithmetic would have to touch. It exists so that the
choice stays *conscious* rather than accidental, and so reviewers stop
re-litigating it without new information.

## Where the float lives

- **Storage:** `station_ledger_entry_t.balance` (`shared/types.h`), plus
  `lifetime_supply` and per-ship stat counters.
- **Live mutation surface:** ordinary simulation credits and debits use
  `ledger_earn_by_pubkey`, `ledger_spend_by_pubkey`,
  `ledger_force_debit_by_pubkey`, and
  `ledger_credit_supply_amount_by_pubkey` in `server/game_sim.c`. All four
  clamp through `ledger_sanitize_float()` (NaN → 0,
  ±`LEDGER_FLOAT_LIMIT` = 16,000,000, `server/game_sim.h`). Save migration,
  ledger-slot initialization, and `shared/settlement_engine.c`'s isolated
  replay state write fields directly and must preserve the same bounds at
  their trust boundaries.
- **Wire:** little-endian f32 (`write_f32_le`) in hail/balance messages and
  station snapshots (`server/net_protocol.h`).
- **Save:** raw float field in `world.sav` / player saves
  (`server/sim_save.c`, `WRITE_FIELD(f, s->ledger[i].balance)`).

## Why float has not bitten yet

1. **The cap is below the integer-exact ceiling.** `LEDGER_FLOAT_LIMIT`
   (16,000,000) sits just under 2^24 (16,777,216), the largest consecutive
   integer a float32 represents exactly. That looks deliberate: below the
   cap, *integer-valued* balances never round. It does nothing for
   fractional amounts, which the economy produces constantly (65% smelter
   payouts, 0.5×–2.0× dynamic price scaling).
2. **Live mutations are mostly centralized.** Gameplay credits and debits
   flow through a handful of helpers that sanitize inputs and outputs.
   Reconstruction code is a deliberate exception: the settlement engine
   applies signed event deltas to its private replay ledger, while save-load
   sanitization clamps persisted balances before they re-enter live state.
3. **The spend check has an explicit rounding tolerance.**
   `ledger_spend_by_pubkey` rejects only when
   `balance + 0.01f < amount` — an admission that the low bits are noise,
   priced at one cent.
4. **Conservation is structural, not stored.** `station_credit_pool()` is
   derived as `-Σ(balance)` from the ledger — there is no second counter
   that could disagree with the sum of accounts.
5. **The per-operation error is bounded at gameplay scale.** One ULP at a
   balance of 1,000 credits is ~0.00006; at 1,000,000 it is ~0.0625; at
   the cap it is 1.0 credit. Per-operation relative error is ≤ 2^-24.
   Cumulative drift and deliberately split transactions still need empirical
   tests; the clamp and spend tolerance limit damage but do not prove that an
   economic exploit is impossible.
6. **Determinism is preserved.** All arithmetic is strict IEEE with
   `-ffp-contract=off -fno-fast-math`, executed in a fixed tick order on
   authoritative state. Native↔WASM replay gates (`make
   replay-native-wasm`) would catch platform drift in these paths.
7. **Audit records are independent of the cached float.** Trade and supply
   callers can emit station-signed chain-log events with their own payloads,
   and `signal_verify` can re-walk those records. The low-level ledger helpers
   do not emit events themselves, so audit completeness remains a caller
   invariant; float drift in a cached balance cannot rewrite events that were
   recorded.

## Where it genuinely hurts

- **Exact-conservation audits are fuzzy.** "Sum of events == sum of
  balances" can only be checked to a tolerance. Settlement work
  (#354/#355/#356) that wants canonical, checkpointable financial state
  will hit this first.
- **Cross-station wrapping (#480)** turns balances into something another
  system settles against; exchange-settled money should not carry ULP
  noise.
- **The cap interacts badly with fractions near the top of the range:**
  near 16M, a fractional payout rounds to whole credits — a player
  holding a near-cap balance would see +0 or +1 where +0.65 was due.

## Migration path (when triggered)

Preferred target: **integer micro-credits** (int64, 1 credit = 1,000,000
micro) rather than q32.32 — money wants decimal-exact semantics and
saturation rules more than it wants bit-compat with sim fixed point.
Touch points:

1. `shared/types.h` — field type change.
2. The four `ledger_*` mutation functions + `ledger_sanitize_float`
   (becomes an int64 clamp).
3. `station_buy_price` / `station_sell_price` / payout shares — price
   math currently produces floats; quantize at the credit boundary
   (round-half-up on payout, floor on charge, documented).
4. Wire: f32 fields → u64 LE (protocol bump; clients must reject mixed
   versions).
5. Save: `SAVE_VERSION` bump + migration reading old floats and
   quantizing (round-to-nearest micro; drift absorbed into a documented
   one-time migration event in the chain log).
6. Tests: `test_sovereign_ledger.c`, `test_economy.c`,
   `test_dynamic_ore_price_*` tolerance assertions become exact.

## Trigger conditions

Do this work when **any** of these lands, not before:

- settlement events (#354–356) start treating balances as canonical
  financial state rather than gameplay state,
- cross-station wrapping (#480) ships,
- a demonstrated exploit or audit discrepancy exceeding the 0.01-credit
  spend tolerance.

Until then the float stays, with the sanitizers and the chain log as the
control. Changing it earlier buys no player-visible correctness and costs
a protocol + save migration.
