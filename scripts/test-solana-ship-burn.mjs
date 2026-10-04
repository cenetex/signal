import test from 'node:test';
import assert from 'node:assert/strict';
import { TOKEN_PROGRAM, MEMO_PROGRAM, purchaseMemo, verifyShipBurn } from './solana-ship-burn.mjs';

function fixture(amount = '50000000') {
  const purchase = { id: 'a'.repeat(64), wallet: 'buyer', mint: 'configured-mint',
    amount, decimals: 6, tokenProgram: TOKEN_PROGRAM };
  const balance = value => ({ accountIndex: 1, mint: purchase.mint, owner: purchase.wallet,
    programId: TOKEN_PROGRAM, uiTokenAmount: { amount: value, decimals: 6 } });
  return { purchase, signature: 'signature', status: { slot: 100, err: null, confirmationStatus: 'finalized' },
    transaction: { slot: 100, meta: { err: null,
      preTokenBalances: [balance(amount)], postTokenBalances: [balance('0')] },
    transaction: { signatures: ['signature'], message: {
      accountKeys: [{ pubkey: 'buyer', signer: true }, { pubkey: 'token-account', signer: false }],
      instructions: [{ programId: MEMO_PROGRAM, parsed: purchaseMemo(purchase.id) },
        { programId: TOKEN_PROGRAM, parsed: { type: 'burnChecked', info: {
          mint: purchase.mint, authority: purchase.wallet, account: 'token-account',
          tokenAmount: { amount, decimals: 6 } } } }] } } } };
}
const info = f => f.transaction.transaction.message.instructions[1].parsed.info;
const cases = {
  'pending confirmation': f => { f.status.confirmationStatus = 'confirmed'; },
  'failed status': f => { f.status.err = { InstructionError: [0, 'InvalidArgument'] }; },
  'failed execution': f => { f.transaction.meta.err = 'failed'; },
  'different signature': f => { f.signature = 'another'; },
  'different slot': f => { f.transaction.slot++; },
  'unsigned buyer': f => { f.transaction.transaction.message.accountKeys[0].signer = false; },
  'different purchase': f => { f.purchase.id = 'b'.repeat(64); },
  'wrong mint': f => { info(f).mint = 'another-mint'; },
  'wrong authority': f => { info(f).authority = 'delegate'; },
  'underpayment': f => { info(f).tokenAmount.amount = '49999999'; },
  'wrong decimals': f => { info(f).tokenAmount.decimals = 9; },
  'numeric amount': f => { info(f).tokenAmount.amount = 50000000; },
  'amount overflow': f => { f.purchase.amount = '18446744073709551616'; },
  'zero price': f => { f.purchase.amount = '0'; },
  'unsupported program': f => { f.purchase.tokenProgram = 'other-program'; },
  'spoofed burn program': f => { f.transaction.transaction.message.instructions[1].programId = 'other-program'; },
  'unchecked burn': f => { f.transaction.transaction.message.instructions[1].parsed.type = 'burn'; },
  'transfer instead of burn': f => { f.transaction.transaction.message.instructions[1].parsed.type = 'transferChecked'; },
  'extra burn': f => { f.transaction.transaction.message.instructions.push(f.transaction.transaction.message.instructions[1]); },
  'extra memo': f => { f.transaction.transaction.message.instructions.push(f.transaction.transaction.message.instructions[0]); },
  'wrong account owner': f => { f.transaction.meta.preTokenBalances[0].owner = 'someone-else'; },
  'wrong balance mint': f => { f.transaction.meta.postTokenBalances[0].mint = 'someone-else'; },
  'inconsistent balance': f => { f.transaction.meta.postTokenBalances[0].uiTokenAmount.amount = '1'; },
  'missing balance': f => { f.transaction.meta.postTokenBalances = []; },
  'duplicate balance': f => { f.transaction.meta.postTokenBalances.push(f.transaction.meta.postTokenBalances[0]); },
  'missing metadata': f => { f.transaction.meta = null; },
};
for (const [name, mutate] of Object.entries(cases)) {
  test(`rejects ${name}`, () => {
    const f = fixture(); mutate(f); assert.throws(() => verifyShipBurn(f));
  });
}
test('accepts an exact finalized burn bound to the purchase', () => {
  const f = fixture(); const receipt = verifyShipBurn(f);
  assert.equal(receipt.purchaseId, f.purchase.id);
  assert.equal(receipt.amount, '50000000');
  assert.ok(Object.isFrozen(receipt));
});
test('preserves integer precision above the safe Number limit', () => {
  assert.equal(verifyShipBurn(fixture('9007199254740993')).amount, '9007199254740993');
});

test('accepts Token-2022 with its exact program on the instruction and balances', async () => {
  const { TOKEN_2022_PROGRAM } = await import('./solana-ship-burn.mjs');
  const f = fixture(); f.purchase.tokenProgram = TOKEN_2022_PROGRAM;
  f.transaction.transaction.message.instructions[1].programId = TOKEN_2022_PROGRAM;
  f.transaction.meta.preTokenBalances[0].programId = TOKEN_2022_PROGRAM;
  f.transaction.meta.postTokenBalances[0].programId = TOKEN_2022_PROGRAM;
  assert.equal(verifyShipBurn(f).purchaseId, f.purchase.id);
  f.transaction.meta.postTokenBalances[0].programId = TOKEN_PROGRAM;
  assert.throws(() => verifyShipBurn(f));
});
