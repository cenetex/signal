# Furnace Design: Memecoin → RATi Converter

## The model

Players bring memecoins to Signal furnaces. The furnace burns them. RATi comes
out. The furnace is the bridge between the speculative meme economy and the
productive RATi economy — it converts dead capital into yield-bearing assets.

Two models for how the furnace produces RATi:

### Model A: Direct conversion (furnace IS burn-to-mint)

The furnace IS the on-chain burn-to-mint program. It accepts registered source
mints (any memecoin the RATi registry has approved) and mints RATi at a bonding
curve rate. The furnace UI is a frontend for the burn-to-mint program.

```
Player brings $MOON to furnace
  → Furnace burns $MOON on Solana
  → Burn-to-mint program mints RATi to player's wallet
  → Player receives RATi at bonding curve price
  → Memecoin supply decreases, RATi supply increases
```

**Simple, direct, pure DeFi.** The furnace doesn't involve Signal gameplay at
all. It's a token converter with a station-themed UI. Anyone can use it —
Signal player or not.

### Model B: Furnace as powered smelter (furnace accelerates mining)

The furnace burns memecoins as fuel. Fuel powers the smelter. A powered
smelter processes Signal ore into ingots faster. Ingots earn RATi yield from
LP fees. The memecoin is consumed; the RATi is earned through yield, not
directly minted.

```
Player brings $MOON to furnace
  → Furnace burns $MOON on Solana (fuel)
  → Furnace runs hotter for N sim ticks
  → Player's ore smelts faster → more ingots
  → Ingots earn RATi yield from LP fees
  → Memecoin burned, RATi earned through yield
```

**Gameplay-integrated.** The furnace is part of the mining loop. Burning
memecoins accelerates your mining operation. You still need to mine ore
in Signal — the furnace just processes it faster.

### Model C: Hybrid (player chooses conversion path)

The furnace offers two operations:

1. **Convert:** Burn memecoin → mint RATi directly (Model A, instant,
   bonding curve price). No Signal gameplay required. Pure DeFi.

2. **Boost:** Burn memecoin → power furnace → smelt ore faster → more ingots
   → earn RATi yield (Model B, takes time, produces more RATi per memecoin
   burned if you actually mine). Better returns but requires gameplay.

The player chooses: fast conversion or boosted mining. The furnace UI shows
both options with estimated RATi output for each.

## Where the UI lives

The furnace UI lives in Signal's game client as a station module panel.

```
Signal client UI (docked at station):

┌──────────────────────────────────────────────┐
│  PROSPECT REFINERY — Furnace Module          │
│                                              │
│  [TRADE] [SHIP] [CONTRACTS] [YARD] [FURNACE] │  ← Tab to cycle
│                                              │
│  ┌────────────────────────────────────────┐  │
│  │  Linked Wallet: 7xK...a3B              │  │
│  │  $MOON balance:  1,000,000 MOON        │  │
│  │  RATi balance:   500 RATi              │  │
│  └────────────────────────────────────────┘  │
│                                              │
│  ┌─ Convert ──────────────────────────────┐  │
│  │  Burn MOON → mint RATi                 │  │
│  │  Amount: [  100,000  ] MOON            │  │
│  │  Est. RATi: 250 RATi (curve price)     │  │
│  │  [CONVERT]                              │  │
│  └────────────────────────────────────────┘  │
│                                              │
│  ┌─ Boost ────────────────────────────────┐  │
│  │  Burn MOON → power furnace             │  │
│  │  Amount: [  100,000  ] MOON            │  │
│  │  Duration: 60 minutes of 2× smelt rate │  │
│  │  Current ore in hopper: 47 fragments   │  │
│  │  Est. ingots: 12-15                    │  │
│  │  Est. RATi yield: 300-400 (over time)  │  │
│  │  [BOOST]                                │  │
│  └────────────────────────────────────────┘  │
│                                              │
│  ┌─ Active Boosts ────────────────────────┐  │
│  │  2× smelt rate · 42 min remaining      │  │
│  │  Power: ████████░░ 78%                  │  │
│  └────────────────────────────────────────┘  │
│                                              │
│  [Link Wallet] [Refresh Balances]            │
└──────────────────────────────────────────────┘
```

### Why Signal's game client

The furnace is a physical object in the game world — a station module. You fly
to it. You dock at it. You watch ingots come out of it. The UI belongs where
the furnace lives.

Trebuchet could also have a standalone "Furnace" tab for people who don't play
Signal but want to convert memecoins to RATi. But the canonical experience is
in-game. The furnace is part of the world.

## Code location

```
signal/
├── client/
│   └── furnace_ui.c          # Furnace panel UI (new)
│       ├── render_furnace_panel()
│       ├── furnace_handle_input()
│       ├── furnace_convert()      # Calls packnft CLI
│       └── furnace_boost()        # Calls packnft CLI + server
│
├── server/
│   ├── solana_bridge.c        # Solana RPC client (new)
│   │   ├── solana_check_balance()
│   │   ├── solana_verify_burn()
│   │   └── solana_find_token_accounts()
│   │
│   ├── sim_production.c       # Existing — add furnace smelt logic
│   │   └── sim_furnace_smelt()    # Creates ingots from boosted ore
│   │
│   └── game_sim.c             # Existing — add boost timer
│       └── station_t.furnace_boost_remaining
│
├── tools/packnft/
│   └── cli.c                  # Add operations:
│       ├── build-furnace-convert   # Burn memecoin → mint RATi tx
│       └── build-furnace-boost     # Burn memecoin → record boost tx
│
└── shared/
    └── types.h                # Add:
        ├── player_t.linked_solana_wallets[]
        ├── station_t.furnace_boost_multiplier
        └── station_t.furnace_boost_remaining
```

## The convert flow (Model A)

```
PLAYER (in Signal)              SIGNAL SERVER              SOLANA
──────────────────              ─────────────              ──────

1. Tab to Furnace panel
2. Select "Convert" tab
3. Enter MOON amount
   └──────────────────────────► 4. Check MOON balance via
                                   solana_bridge ✓

                                5. Call packnft CLI:
                                   build-furnace-convert
                                   → returns unsigned tx

   ◄────────────────────────── 6. Show tx preview:
                                   "Burn 100,000 MOON
                                    Receive ~250 RATi"

7. Click [CONVERT]
   → Phantom popup appears
   → Player signs tx
   └─────────────────────────────────────────────────────► 8. Submit to Solana
                                                           9. Burn MOON ✓
                                                           10. Mint RATi ✓
                                                           11. RATi appears in
                                                               wallet

                                12. Poll Solana for
                                    confirmation

   ◄────────────────────────── 13. Update UI:
                                   "Converted! +250 RATi"
                                   Refresh RATi balance
```

## The boost flow (Model B)

```
PLAYER (in Signal)              SIGNAL SERVER              SOLANA
──────────────────              ─────────────              ──────

1. Tab to Furnace panel
2. Select "Boost" tab
3. Enter MOON amount
   └──────────────────────────► 4. Calculate boost:
                                   duration = MOON_amount
                                   × BOOST_RATE

                                5. Call packnft CLI:
                                   build-furnace-boost
                                   → returns unsigned tx

   ◄────────────────────────── 6. Show boost preview:
                                   "Burn 100,000 MOON
                                    60 min of 2× smelt rate
                                    Current hopper: 47 ore"

7. Click [BOOST]
   → Phantom popup appears
   → Player signs tx
   └─────────────────────────────────────────────────────► 8. Submit to Solana
                                                           9. Burn MOON ✓
                                                           10. Boost record on
                                                               chain (optional)

                                11. Poll Solana for
                                    confirmation

                                12. Activate boost:
                                    station.furnace_boost
                                    _multiplier = 2.0
                                    station.furnace_boost
                                    _remaining = 3600s

   ◄────────────────────────── 13. UI shows:
                                   "Boost active! 2× smelt
                                    rate for 60 min"
                                   Progress bar appears

14. Mine ore, tow to station
    → Ore smelts at 2× speed
    → Ingots created faster
    → More yield per hour

15. Boost expires
    → furnace_boost_multiplier
      resets to 1.0
```

## Furnace station module

The furnace is a station module, like the shipyard or smelter. It occupies a
module slot on the station ring.

```c
// shared/module_schema.h — new module type
MODULE_FURNACE = 8

// shared/types.h — station module config
station_module_t {
    ...
    uint8_t  furnace_tier;       // 1-3, affects boost multiplier and
                                 // conversion rate
    uint64_t furnace_boost_multiplier;  // 1.0 = normal, 2.0 = boosted
    uint64_t furnace_boost_remaining;   // sim ticks remaining
    uint8_t  furnace_accepted_mints[8][32];  // which memecoins accepted
}
```

A Tier 1 furnace (basic) might offer 1.5× boost. Tier 2: 2×. Tier 3: 3×.
Higher-tier furnaces also get better conversion rates (more RATi per memecoin).

Furnace modules are built from RATi-ingots (the economic loop closes: burn
memecoins → get RATi → compose ingots → build furnace → burn more memecoins).

## What Trebuchet provides

Trebuchet doesn't host the furnace UI, but it provides the launch configuration
for furnaces:

```json
// In .launch file: yield.json
{
  "furnace": {
    "accepted_mints": [
      { "mint": "MOON_mint_address...", "symbol": "MOON", "boost_rate": 1.0 },
      { "mint": "BONK_mint_address...", "symbol": "BONK", "boost_rate": 0.8 }
    ],
    "tiers": [
      { "tier": 1, "boost_multiplier": 1.5, "conversion_bonus_bps": 0 },
      { "tier": 2, "boost_multiplier": 2.0, "conversion_bonus_bps": 100 },
      { "tier": 3, "boost_multiplier": 3.0, "conversion_bonus_bps": 250 }
    ]
  }
}
```

Trebuchet deploys the furnace configuration as part of the launch. Signal reads
the `.launch` file to know which memecoins the furnace accepts and what the
boost rates are.

## The economic loop

```
MEMECOIN HOLDER
      │
      │  brings $MOON to furnace
      ▼
┌──────────────┐     burn MOON      ┌──────────────┐
│   FURNACE    │ ─────────────────► │   SOLANA     │
│  (Signal)    │                    │  MOON supply │
│              │ ◄───────────────── │  decreases   │
│  Converts or │    mint RATi       │  RATi supply │
│  boosts      │                    │  increases   │
└──────┬───────┘                    └──────────────┘
       │
       │  RATi received
       ▼
┌──────────────┐
│  RATi HOLDER │
│              │
│  Options:    │
│  ├─ Trade on pools (LP fees accrue)
│  ├─ Burn at furnace for ingots (yield)
│  ├─ Buy Ruby High packs (cards)
│  └─ Compose into ships/stations (mining)
└──────────────┘
```

The furnace is the entry point. Memecoins go in. RATi comes out. Everything
downstream — liquidity, yield, ingots, packs, ships, stations — is powered by
the RATi that came through the furnace. The furnace doesn't judge which
memecoin you bring. If the registry has approved the mint, the furnace accepts
it.
