# Station Tokens: Three Currencies, Three Nations

## The remap

| Station | Token | Role | Economy |
|---------|-------|------|---------|
| **Prospect Refinery** | RUBY | Starter refinery, new player onboarding | Entry-level. Common ore, common cards, common everything. The door. |
| **Kepler Yard** | KYRO | Shipyard, frame press, manufacturing | Industry. Ships, frames, modules, construction. The factory. |
| **Helios Works** | RATi | Advanced processing, coordination | Sovereignty. Signal studies, yield coordination, operator network. The capital. |

Each station is a sovereign currency issuer. Each has its own furnace that
accepts memecoins and mints that station's token. Each has its own bonding
curve, its own boost rates, its own economic policy.

## The three-token economy

```
                    ┌──────────────────┐
                    │     MEMECOINS    │
                    │  (external fuel) │
                    └────────┬─────────┘
                             │
            ┌────────────────┼────────────────┐
            │                │                │
            ▼                ▼                ▼
     ┌────────────┐  ┌────────────┐  ┌────────────┐
     │  PROSPECT  │  │  KEPLER    │  │  HELIOS    │
     │  Furnace   │  │  Furnace   │  │  Furnace   │
     │            │  │            │  │            │
     │  burns     │  │  burns     │  │  burns     │
     │  memecoins │  │  memecoins │  │  memecoins │
     │     ↓      │  │     ↓      │  │     ↓      │
     │   RUBY    │  │   KYRO     │  │   RATi     │
     └─────┬──────┘  └─────┬──────┘  └─────┬──────┘
           │               │               │
           │               │               │
           ▼               ▼               ▼
     ┌────────────┐  ┌────────────┐  ┌────────────┐
     │  RUBY      │  │  KYRO      │  │  RATi      │
     │  Economy   │  │  Economy   │  │  Economy   │
     │            │  │            │  │            │
     │ • Packs    │  │ • Ships    │  │ • Yield    │
     │ • Cards    │  │ • Frames   │  │ • Ingots   │
     │ • School   │  │ • Modules  │  │ • Operator │
     │ • Starter  │  │ • Industry │  │ • Signal   │
     └────────────┘  └────────────┘  └────────────┘
```

## What each token does

### RUBY (Prospect Refinery)

The entry token. You start here. Prospect is where new players learn to mine,
smelt, and trade. The RUBY token buys Ruby High card packs — the collection
game. It's the most liquid, the most widely held, the "retail" token.

- **Furnace:** Accepts common memecoins, generous conversion rates
- **Economy:** Card packs, starter ships, basic modules, Hall Passes
- **Bonding curve:** Flat, accessible — designed for volume
- **Yield:** Lower per-token, higher total volume

### KYRO (Kepler Yard)

The industrial token. You graduate here. Kepler is where ships are built,
frames are pressed, modules are fabricated. The KYRO token buys ships, frames,
lasers, tractors — the means of production.

- **Furnace:** Accepts mid-tier memecoins, moderate conversion rates
- **Economy:** Ships, frames, modules, station construction kits
- **Bonding curve:** Steeper than RUBY — less volume, more value per unit
- **Yield:** Medium per-token, from manufacturing fees and ship sales

### RATi (Helios Works)

The sovereignty token. You ascend here. Helios is where operators coordinate,
where signal studies unlock Sector X, where yield-split ingots earn LP fees
from the RATi/SOL pool. RATi is the governance and coordination layer.

- **Furnace:** Accepts any registered memecoin (the universal converter)
- **Economy:** Yield-split ingots, operator network access, governance,
  Signal studies passes, Sector X gate keys
- **Bonding curve:** Steepest — scarce, valuable, designed for operators
- **Yield:** Highest per-token, from LP fees on RATi/SOL pool

## Cross-station value flow

```
  RUBY holder                KYRO holder                RATi holder
  ───────────                ───────────                ───────────

  "I want a ship."           "I want packs."            "I want yield."

  Path A: Trade              Path B: Haul               Path C: Operate
  ─────────────              ────────────               ──────────────

  Sell RUBY → buy KYRO       Earn KYRO credits          Burn RATi → ingot
  on Jupiter                 by hauling frames          → earn LP fees
                              from Kepler to             → compound yield
  Buy ship at Kepler         Prospect                   → build station
  with KYRO                                             → earn more RATi
```

The three tokens trade against each other on Jupiter. A RUBY holder who wants
a ship sells RUBY for KYRO. A KYRO holder who wants packs sells KYRO for RUBY.
A RATi holder who wants both sells RATi for either.

But there's also the hauling path: you don't need to trade tokens. You can
haul goods between stations and earn credits in the destination station's
currency. A KYRO-rich player hauls frames to Prospect, earns RUBY credits,
buys packs. No token swap needed — the labor IS the conversion.

## Per-station furnaces

Each station's furnace has its own configuration:

```json
{
  "stations": {
    "prospect": {
      "token": "RUBY",
      "furnace_tier": 1,
      "boost_multiplier": 1.5,
      "accepted_mints": ["MOON", "BONK", "WIF", "POPCAT"],
      "conversion_bonus_bps": 0
    },
    "kepler": {
      "token": "KYRO",
      "furnace_tier": 2,
      "boost_multiplier": 2.0,
      "accepted_mints": ["MOON", "BONK", "POPCAT", "MYRO"],
      "conversion_bonus_bps": 100
    },
    "helios": {
      "token": "RATI",
      "furnace_tier": 3,
      "boost_multiplier": 3.0,
      "accepted_mints": ["*"],
      "conversion_bonus_bps": 250
    }
  }
}
```

Helios accepts any registered memecoin (`"*"`). It's the universal converter.
Prospect and Kepler are more selective — they accept popular memecoins but
not everything. This creates a natural hierarchy: common memecoins go to
Prospect, rarer ones go to Kepler, anything goes to Helios.

## Economic tiers

```
  ┌─────────────────────────────────────────────┐
  │              RATi — Helios                  │
  │  Sovereignty, yield, governance, operators  │
  │  ──────────────────────────────────────     │
  │    ↑  ascend through gameplay               │
  │  ┌───────────────────────────────────────┐  │
  │  │        KYRO — Kepler                  │  │
  │  │  Industry, ships, frames, modules     │  │
  │  │  ────────────────────────────────     │  │
  │  │    ↑  build and trade                 │  │
  │  │  ┌───────────────────────────────┐    │  │
  │  │  │     RUBY — Prospect           │    │  │
  │  │  │  Entry, packs, cards, start   │    │  │
  │  │  │  ────────────────────────     │    │  │
  │  │  │    ↑  everyone starts here    │    │  │
  │  │  └───────────────────────────────┘    │  │
  │  └───────────────────────────────────────┘  │
  └─────────────────────────────────────────────┘
```

This isn't a hierarchy of "better" and "worse." It's a progression of
complexity and sovereignty. You start with RUBY — packs, cards, simple mining.
You build up to KYRO — ships, manufacturing, trade routes. You ascend to RATi —
yield coordination, station operation, governance.

Most players stay in RUBY/KYRO. That's fine. The economy needs volume at the
entry tier. RATi is for operators — the people running stations, coordinating
mining fleets, managing yield distribution. It's not "the best token." It's
the token for a specific role in the economy.

## Station credit = station token

With this model, station credits and station tokens merge. You don't have
"Prospect credits" AND "RUBY tokens." The station's ledger IS denominated in
its token. When you earn credits at Prospect for delivering ore, you earn
RUBY. When you spend credits at Kepler for a ship, you spend KYRO.

The per-station ledger (`station_t.ledger[]`) holds balances in that station's
token. The ledger is an off-chain accounting system; the token is the on-chain
SPL token. They can be the same thing if the station operator manages a reserve
and issues credits against it. Or they can be separate — credits are IOUs for
the token, redeemable at the station.

For simplicity, the initial implementation should make them the same: station
ledger balances ARE SPL token balances. The player's linked Solana wallet IS
their station ledger. No separate accounting system. Docking at Prospect shows
your RUBY balance (read from Solana). Earning RUBY for ore delivery means the
station sends RUBY to your wallet.

### The station as a bank

Under this model, the station operator holds a treasury of tokens. When a
player delivers ore, the station sends tokens from its treasury to the
player's wallet. When a player buys a ship, the player sends tokens to the
station's treasury. The station's treasury is funded by:
- Furnace conversion fees (the spread between memecoin value and token minted)
- Ship and module sales
- Docking and service fees
- LP yield from the station's own locked liquidity

The station operates like a business. Revenue comes from services. Expenses
are ore bounties and NPC wages. Profit is reinvested into the station (more
modules, better furnaces, higher bounties).

## Updated furnace flow

```
PLAYER at Kepler             KEPLER SERVER              SOLANA
────────────────             ─────────────              ──────

1. Tab to Furnace
2. Select MOON
3. Enter amount
   └────────────────────────► 4. Build burn tx:
                                 burn MOON → mint KYRO
                                 (Kepler's bonding curve)

   ◄──────────────────────── 5. Show preview:
                                 "Burn 100,000 MOON
                                  Receive ~500 KYRO"

6. Sign with Phantom
   └─────────────────────────────────────────────────► 7. MOON burned ✓
                                                       8. KYRO minted to
                                                          player's wallet

                            9. KYRO now in wallet
                               → station UI shows
                               increased KYRO balance
                               → player can buy ships
                               at Kepler shipyard
```

The furnace doesn't need a separate "convert" and "boost" mode. Converting
memecoins to the station's token IS the furnace's job. The boost mechanic
(powering the smelter) is a station upgrade that costs the station's token —
you buy a furnace upgrade with KYRO, and then the furnace runs faster. But the
core operation is simple: memecoin in, station token out.

## What changes in the codebase

| What | Change |
|------|--------|
| Station identity | Prospect → RUBY, Kepler → KYRO, Helios → RATi |
| `station_t.currency_name` | "ruby", "kyro", "rati" (lowercase) |
| Station ledger | Merged with SPL token balances (read from Solana) |
| Furnace module | Added to all three stations, with per-station config |
| Bonding curves | Three separate curves, one per token |
| Player identity | Already has `linked_solana_wallets[]` — same for all stations |
| Ruby High packs | Purchased with RUBY at Prospect |
| Shipyard | Purchased with KYRO at Kepler |
| Yield-split | RATi-only at Helios |
EOF
echo "Done"