# Fly ship purchases

The planned shop serves Prospect Refinery, Kepler Yard, and Helios Works. The
requested price points are 50, 100, and 200. The token mint, price unit, and ship
ownership mode are awaiting the owner's choice.

## Player flow

1. Choose a ship at a station.
2. Connect a Solana wallet and sign the link to the authenticated game identity.
3. Review the token amount and ship details. The wallet approves one burn with
   a memo tied to the saved purchase.
4. The server checks finalization and saves the ship grant.
5. Show the ship and receipt. The player can recover a purchase after reconnecting.

## Implemented receipt check

`scripts/solana-ship-burn.mjs` checks a parsed transaction and signature status
against an immutable server-owned purchase. It requires finalized success, matching
slot and transaction signature, a buyer signature, one purchase memo, one SPL Token
BurnChecked instruction, the expected mint and authority, exact integer amount and
decimals, and matching token-account owner and balance reduction.

The first implementation targets the original SPL Token program. Token-2022
support requires a separate review once the payment mint is chosen. All token
amounts are strings in raw token units and use integer arithmetic.

Run the receipt tests with:

```sh
node --test scripts/test-solana-ship-burn.mjs
```

## Integration work

- Save quotes bound to a random 32-byte purchase ID, authenticated player, wallet,
  mint, amount, token decimals, station, ship product, and expected Solana cluster.
- Authenticate wallet linking using both the game identity and wallet signatures,
  with an expiring, single-use challenge tied to the site origin.
- Fetch transaction and status from a configured server-side RPC. Check the cluster
  genesis hash. Request `getTransaction` with finalized commitment and jsonParsed
  encoding, and `getSignatureStatuses` with transaction-history lookup.
- Pass only those server-fetched results to the receipt checker.
- Save purchase consumption and the resulting ship asset in one durable operation.
  Use unique purchase and transaction keys across all stations and restarts. A
  retry returns the same grant. Recovery completes a saved pending fulfillment.
- Build wallet checkout and the station UI once token pricing and ownership mode
  are selected. Reserve available ship capacity before asking the buyer to burn.
- Test reconnect, duplicate submission, simultaneous claims, RPC failure,
  finalization delay, restart, and the purchase-to-ship flow on a test cluster.
- Verify the selected mint and decimals on the chosen cluster before enabling
  production checkout.

This draft contains receipt validation and tests. The live checkout and ship grant
remain integration work. Current deployments continue to use the existing game UI.

## Sources

- [Solana token burns](https://solana.com/docs/tokens/basics/burn-tokens)
- [Transaction lookup](https://solana.com/docs/rpc/http/gettransaction)
- [Signature status lookup](https://solana.com/docs/rpc/http/getsignaturestatuses)
- [RPC transaction structures](https://solana.com/docs/rpc/json-structures)
