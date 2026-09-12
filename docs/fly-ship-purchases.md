# FLY workers

The live shop at `/workers.html` sells one autonomous worker per finalized burn.
The wallet owner gets a live local map, launch state, hull condition and purchase
history. Workers join the existing station fleet and can be damaged or lost.

| Station | Worker | FLY burned |
| --- | --- | ---: |
| Prospect Refinery | Miner | 50 |
| Kepler Yard | Tug | 100 |
| Helios Works | Miner with level-two laser | 200 |

The fixed mainnet mint is `FLY3ytMF4wyGQcVPo2RZ5FTFsf7JEBj4DrtucnRqrFLY`.
It uses Token-2022 and nine decimals. The server checks the mainnet genesis,
mint identity, decimals, cleared mint and freeze authorities, and metadata-only
extension set before preparing or accepting a burn. Prices use integer base units.

## Purchase and recovery

Wallet Standard provides wallet discovery and Ed25519 sign-in. Each challenge
binds the site origin, wallet, nonce and five-minute expiry. The session uses a
Secure, HttpOnly, SameSite cookie. POST requests require the configured origin.

A saved quote binds a random purchase ID, wallet, station, mint and amount.
The wallet signs an exact Token-2022 `BurnChecked` transaction with a purchase
memo. The gateway checks the signature and full message. It saves the signed
transaction and signature before broadcasting. Retries broadcast the same bytes.
It fetches finalized transaction and signature status from its own RPC endpoint.
The verifier checks the signer, instruction, memo and exact token balance change.

The gateway stores quotes and receipts in `fly-shop.json` with atomic rename and
file and directory sync. Paid orders retry every 15 seconds. An expired signed
transaction can be replaced after finalized block height passes its validity
window and RPC confirms its receipt is absent or finalized with an error.

The world stores the purchase ID, wallet, burn signature and asset ID in save
version 85. The private grant endpoint acknowledges after a complete persistence
generation is saved. Receipt retries return the same asset, including after ship
loss. FLY assets retain their wallet owner and are reserved for autonomous use.
They carry explicit external purchase provenance. Stored hulls launch as fleet
slots become available.

The initial rollout has 80 lifetime worker spaces. Open quotes reserve spaces
and remain recoverable. A wallet can hold one open order. This small launch cap
also bounds saved receipts and retained lost ships. Raising it requires a save
format and capacity review. Workers use the deployed fly navigation circuit and
trained worker policy. The owner map reads the authoritative world every three
seconds while visible.

## Deployment

`SIGNAL_FLY_SHOP_ENABLED=1` enables the gateway. The entrypoint generates a fresh
private bridge key shared by the gateway and world process. The public gateway
blocks the private grant and view routes. `SIGNAL_ALLOWED_ORIGIN` pins the site.
`SIGNAL_SOLANA_RPC_URL` can select a trusted HTTPS mainnet RPC; the default is the
public mainnet endpoint. The persistent volume holds the shop and world saves.

## Checks

`node --test scripts/test-solana-ship-burn.mjs scripts/test-fly-shop.mjs` exercises
burn validation, Token-2022 identity, transaction bytes, wallet login, origin
binding, durable broadcast, repeated confirmation and recovery after restart.
The C `fly_purchase` regression covers ownership, launch, save/load, receipt
replay, ship loss and invalid ownership data. The complete C suite also checks
save migration and existing simulation behavior.

References: [Solana burns](https://solana.com/docs/tokens/basics/burn-tokens),
[transaction RPC](https://solana.com/docs/rpc/http/gettransaction),
[signature status RPC](https://solana.com/docs/rpc/http/getsignaturestatuses),
[account RPC](https://solana.com/docs/rpc/http/getaccountinfo).
