# RATi Burn-To-Mint On-Chain Account Order

Status: draft v0.1
Date: 2026-05-21
Scope: fixed account order for the burn-to-mint SBF executor.

The dependency-free `native/` crate owns byte layouts and accounting. This
`onchain/` reference crate owns the readable account-order contract. The active
C candidate mirrors it through `../onchain-c/vectors/account-rules.v1.json`,
and the disposable migration planner reads that vector for account-order labels.

No instruction accepts a protocol-fee account, fee-recipient account, treasury
rake account, or fee-switch account. Extra accounts are rejected.

## InitializeConfig

1. payer signer
2. config PDA writable
3. admin authority
4. pause authority
5. system program

## RegisterDestinationMint

1. admin or governance signer writable
2. config PDA writable
3. destination token config PDA writable
4. destination mint PDA writable
5. mint authority PDA
6. destination token program
7. system program

## RegisterSourceMint

1. admin or governance signer writable
2. config PDA writable
3. destination token config PDA
4. source mint config PDA writable
5. source mint
6. destination mint
7. source token program
8. system program

## SetSourceEnabled

1. admin or governance signer
2. config PDA
3. source mint config PDA writable
4. destination token config PDA

## Migrate

Base path:

1. user signer
2. config PDA
3. source mint config PDA writable
4. destination token config PDA writable
5. user source token account writable
6. user destination token account writable
7. source mint writable
8. destination mint PDA writable
9. mint authority PDA
10. source token program
11. destination token program

Receipt path adds:

12. receipt PDA writable
13. system program

## Pause

1. pause authority signer
2. config PDA writable
3. source or destination config PDA writable

## FinalizeSourceMint

1. admin or governance signer
2. config PDA
3. source mint config PDA writable
4. destination token config PDA

## TransferAuthorityBegin

1. current admin or governance signer
2. config PDA writable
3. pending authority

## TransferAuthorityAccept

1. pending authority signer
2. config PDA writable
3. current admin or governance authority

## RetireAuthority

1. current admin or governance signer
2. config PDA writable
3. current pause authority signer
