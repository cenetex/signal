/** Validate server-fetched Solana RPC results against an immutable purchase.
 * The caller owns authenticated quotes, RPC trust, and atomic redemption.
 */
export const TOKEN_PROGRAM = 'TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA';
export const MEMO_PROGRAM = 'MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr';

function requireFact(ok, code) {
  if (!ok) throw new Error(code);
}

function rawAmount(value) {
  requireFact(typeof value === 'string' && /^(0|[1-9][0-9]{0,19})$/.test(value), 'invalid_amount');
  const amount = BigInt(value);
  requireFact(amount <= 18446744073709551615n, 'amount_overflow');
  return amount;
}

/** The random purchase ID maps to a saved quote containing station, ship,
 * player identity, wallet, mint, amount, and cluster. Keep that quote immutable.
 */
export function purchaseMemo(purchaseId) {
  requireFact(typeof purchaseId === 'string' && /^[a-f0-9]{64}$/.test(purchaseId), 'invalid_purchase_id');
  return `signal:fly-ship:v1:${purchaseId}`;
}

export function verifyShipBurn({ purchase, signature, status, transaction }) {
  requireFact(purchase && typeof purchase.wallet === 'string' && purchase.wallet.length > 0 &&
    typeof purchase.mint === 'string' && purchase.mint.length > 0, 'invalid_purchase');
  requireFact(purchase.tokenProgram === TOKEN_PROGRAM, 'unsupported_token_program');
  requireFact(Number.isInteger(purchase.decimals) && purchase.decimals >= 0 && purchase.decimals <= 255,
    'invalid_decimals');
  const amount = rawAmount(purchase.amount);
  requireFact(amount > 0n, 'zero_amount');
  const memo = purchaseMemo(purchase.id);
  requireFact(typeof signature === 'string' && signature.length > 0, 'invalid_signature');
  requireFact(status?.confirmationStatus === 'finalized' && status.err === null, 'burn_not_finalized');
  requireFact(Number.isSafeInteger(status.slot) && status.slot >= 0 && transaction?.slot === status.slot,
    'slot_mismatch');
  requireFact(transaction?.meta?.err === null, 'transaction_failed');
  requireFact(transaction?.transaction?.signatures?.[0] === signature, 'signature_mismatch');
  const message = transaction.transaction.message;
  requireFact(Array.isArray(message?.accountKeys) && Array.isArray(message.instructions), 'invalid_message');
  requireFact(message.accountKeys.some(key => key.pubkey === purchase.wallet && key.signer === true),
    'buyer_not_signer');
  const memos = message.instructions.filter(ix => ix.programId === MEMO_PROGRAM);
  requireFact(memos.length === 1 && memos[0].parsed === memo, 'purchase_memo_mismatch');
  const burns = message.instructions.filter(ix => ix.programId === TOKEN_PROGRAM &&
    (ix.parsed?.type === 'burn' || ix.parsed?.type === 'burnChecked'));
  requireFact(burns.length === 1 && burns[0].parsed.type === 'burnChecked', 'expected_one_checked_burn');
  const info = burns[0].parsed.info;
  requireFact(info?.mint === purchase.mint && info.authority === purchase.wallet, 'burn_identity_mismatch');
  requireFact(info.tokenAmount?.decimals === purchase.decimals &&
    rawAmount(info.tokenAmount.amount) === amount, 'burn_amount_mismatch');
  const accountIndex = message.accountKeys.findIndex(key => key.pubkey === info.account);
  requireFact(accountIndex >= 0, 'burn_account_missing');
  const balance = rows => {
    requireFact(Array.isArray(rows), 'token_balances_missing');
    const matches = rows.filter(row => row.accountIndex === accountIndex);
    requireFact(matches.length === 1, 'token_balance_missing_or_duplicate');
    const row = matches[0];
    requireFact(row.mint === purchase.mint && row.owner === purchase.wallet && row.programId === TOKEN_PROGRAM,
      'token_balance_identity_mismatch');
    requireFact(row.uiTokenAmount?.decimals === purchase.decimals, 'token_balance_decimals_mismatch');
    return rawAmount(row.uiTokenAmount.amount);
  };
  const before = balance(transaction.meta.preTokenBalances);
  const after = balance(transaction.meta.postTokenBalances);
  requireFact(before - after === amount, 'token_balance_delta_mismatch');
  return Object.freeze({ purchaseId: purchase.id, signature, slot: status.slot,
    mint: purchase.mint, wallet: purchase.wallet, amount: purchase.amount });
}
