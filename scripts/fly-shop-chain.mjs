import { decode58, encode58 } from '../web/fly-codec.mjs';
import { MEMO_PROGRAM, TOKEN_2022_PROGRAM, purchaseMemo, verifyShipBurn } from './solana-ship-burn.mjs';
export const FLY_MINT = 'FLY3ytMF4wyGQcVPo2RZ5FTFsf7JEBj4DrtucnRqrFLY';
export const MAINNET_GENESIS = '5eykt4UsFv8P8NJdTREpY1vzqKqZKvdpKuc147dw2N9d';
export const OFFERS = Object.freeze([
  { station: 0, name: 'Prospect Refinery', ship: 'Miner', tokens: 50, detail: 'A fly-brain miner for the nearby rock fields.' },
  { station: 1, name: 'Kepler Yard', ship: 'Tug', tokens: 100, detail: 'A fly-brain tug that brings loose rock home.' },
  { station: 2, name: 'Helios Works', ship: 'Upgraded miner', tokens: 200, detail: 'A fly-brain miner with a level-two laser.' },
]);
const compact = n => {
  const bytes = [];
  do { const value = n & 127; n >>>= 7; bytes.push(value | (n ? 128 : 0)); } while (n);
  return Buffer.from(bytes);
};
export function buildBurnTransaction(purchase, tokenAccount, blockhash) {
  const keys = [purchase.wallet, tokenAccount, purchase.mint, purchase.tokenProgram, MEMO_PROGRAM];
  const burn = Buffer.alloc(10); burn[0] = 15;
  burn.writeBigUInt64LE(BigInt(purchase.amount), 1); burn[9] = purchase.decimals;
  const memo = Buffer.from(purchaseMemo(purchase.id));
  const ix = (program, accounts, data) => Buffer.concat([
    Buffer.from([program]), compact(accounts.length), Buffer.from(accounts), compact(data.length), data]);
  const message = Buffer.concat([Buffer.from([1, 0, 2]), compact(keys.length),
    ...keys.map(key => Buffer.from(decode58(key, 32))), Buffer.from(decode58(blockhash, 32)),
    compact(2), ix(3, [1, 2, 0], burn), ix(4, [0], memo)]);
  return Buffer.concat([Buffer.from([1]), Buffer.alloc(64), message]);
}
// Compare the signed instructions, allowing wallets to reorder keys and add
// compute fees. Keep the quoted blockhash so expiry recovery stays exact.
export function matchesPreparedBurn(signed, prepared) {
  const budget = 'ComputeBudget111111111111111111111111111111';
  const lighthouse = 'L2TExMFKdjpN9kozasaurPirfHy9P8sbXoAN1qA3S95';
  function parse(bytes) {
    let offset = 0;
    const take = n => {
      if (offset + n > bytes.length) throw Error('truncated_transaction');
      const value = bytes.subarray(offset, offset + n); offset += n; return value;
    };
    const byte = () => take(1)[0];
    const count = () => {
      let n = 0;
      for (let i = 0; i < 3; i++) {
        const b = byte(); n |= (b & 127) << (7 * i);
        if (!(b & 128)) {
          if (n > 65535 || (i && b === 0)) throw Error('invalid_length');
          return n;
        }
      }
      throw Error('invalid_length');
    };
    if (bytes.length > 1232 || count() !== 1) throw Error('invalid_signers');
    take(64);
    let required = byte(), versioned = false;
    if (required === 128) { versioned = true; required = byte(); }
    const readonlySigned = byte(), readonlyUnsigned = byte(), keyCount = count();
    if (required !== 1 || readonlySigned !== 0 || keyCount < 1 || readonlyUnsigned >= keyCount) throw Error('invalid_header');
    const keys = Array.from({ length: keyCount }, (_, i) => ({
      address: encode58(take(32)), signer: i === 0, writable: i < keyCount - readonlyUnsigned,
    }));
    if (new Set(keys.map(k => k.address)).size !== keyCount) throw Error('duplicate_key');
    const lookup = i => { if (!keys[i]) throw Error('invalid_key'); return keys[i]; };
    const blockhash = take(32).toString('hex');
    const instructions = Array.from({ length: count() }, () => {
      const program = lookup(byte());
      const accounts = Array.from(take(count()), lookup);
      const data = take(count()).toString('hex');
      return { program, accounts, data };
    });
    if (versioned && count() !== 0) throw Error('lookup_tables_unsupported');
    if (offset !== bytes.length) throw Error('trailing_bytes');
    return { keys, blockhash, instructions };
  }
  try {
    const actual = parse(signed), expected = parse(prepared);
    if (actual.blockhash !== expected.blockhash) return false;
    let limit = 1400000n, price = 0n;
    const seen = new Set();
    actual.instructions = actual.instructions.filter(ix => {
      if (ix.program.address === lighthouse) {
        // Lighthouse 4c579479, instruction.rs: 6 = AssertAccountInfoMulti,
        // 10 = AssertTokenAccountMulti. Both only evaluate account state.
        // https://github.com/Jac0xb/lighthouse/tree/4c579479c98635e419b1b167f08be02a71604a71/programs/lighthouse/src
        const op = Buffer.from(ix.data, 'hex')[0];
        if (![6, 10].includes(op) || ix.program.signer || ix.program.writable ||
            ix.accounts.length !== 1 || !expected.keys.slice(0, 3).some(k =>
              JSON.stringify(k) === JSON.stringify(ix.accounts[0]))) throw Error('invalid_wallet_guard');
        return false;
      }
      if (ix.program.address !== budget) return true;
      const data = Buffer.from(ix.data, 'hex'), op = data[0];
      if (ix.accounts.length || ix.program.signer || ix.program.writable || seen.has(op)) throw Error('invalid_fee');
      seen.add(op);
      if (op === 2 && data.length === 5) {
        limit = BigInt(data.readUInt32LE(1));
        if (limit < 1n || limit > 1400000n) throw Error('invalid_fee');
      } else if (op === 3 && data.length === 9) price = data.readBigUInt64LE(1);
      else throw Error('invalid_fee');
      return false;
    });
    // Bound wallet-added priority fees to 0.001 SOL.
    if ((limit * price + 999999n) / 1000000n > 1000000n) return false;
    const keys = value => value.keys.filter(k => k.address !== budget && k.address !== lighthouse).sort((a, b) => a.address.localeCompare(b.address));
    return JSON.stringify(keys(actual)) === JSON.stringify(keys(expected)) &&
      JSON.stringify(actual.instructions) === JSON.stringify(expected.instructions);
  } catch { return false; }
}
export function makeRpc(url, fetchImpl = fetch) {
  const parsed = new URL(url);
  if (parsed.protocol !== 'https:') throw new Error('rpc_requires_https');
  return async (method, params = []) => {
    const response = await fetchImpl(url, { method: 'POST', redirect: 'error',
      headers: { 'content-type': 'application/json' }, signal: AbortSignal.timeout(12000),
      body: JSON.stringify({ jsonrpc: '2.0', id: 1, method, params }) });
    if (!response.ok) throw new Error('rpc_unavailable');
    const text = await response.text();
    if (text.length > 2_000_000) throw new Error('rpc_response_too_large');
    const result = JSON.parse(text);
    if (result.error || result.jsonrpc !== '2.0' || result.id !== 1) throw new Error('rpc_unavailable');
    return result.result;
  };
}
export async function verifyFlyMint(rpc) {
  const [genesis, account] = await Promise.all([
    rpc('getGenesisHash'), rpc('getAccountInfo', [FLY_MINT, { encoding: 'jsonParsed', commitment: 'finalized' }]),
  ]);
  if (genesis !== MAINNET_GENESIS) throw new Error('wrong_solana_cluster');
  const value = account?.value;
  const info = value?.data?.parsed?.info;
  if (value?.owner !== TOKEN_2022_PROGRAM || value.executable || value.data.parsed.type !== 'mint' ||
      info?.decimals !== 9 || info.isInitialized !== true || info.mintAuthority !== null || info.freezeAuthority !== null)
    throw new Error('fly_mint_mismatch');
  if (!Array.isArray(info.extensions) || info.extensions.some(ext =>
    !['metadataPointer', 'tokenMetadata'].includes(ext.extension))) throw new Error('unsupported_mint_extension');
  return { mint: FLY_MINT, decimals: 9, tokenProgram: TOKEN_2022_PROGRAM };
}
export async function prepareFlyBurn(rpc, quote) {
  const mint = await verifyFlyMint(rpc);
  const accounts = await rpc('getTokenAccountsByOwner', [quote.wallet, { mint: FLY_MINT },
    { encoding: 'jsonParsed', commitment: 'finalized' }]);
  const tokenAccount = accounts?.value?.find(row => {
    const info = row.account?.data?.parsed?.info;
    return row.account?.owner === TOKEN_2022_PROGRAM && info?.owner === quote.wallet &&
      info.mint === FLY_MINT && info.state === 'initialized' && info.tokenAmount?.decimals === 9 &&
      /^\d+$/.test(info.tokenAmount.amount) && BigInt(info.tokenAmount.amount) >= BigInt(quote.amount);
  });
  if (!tokenAccount) throw new Error('insufficient_fly');
  const blockhash = await rpc('getLatestBlockhash', [{ commitment: 'finalized' }]);
  return { ...mint, lastValidBlockHeight: blockhash.value.lastValidBlockHeight, transaction: buildBurnTransaction(quote, tokenAccount.pubkey,
    blockhash.value.blockhash).toString('base64') };
}
export async function checkFlyBurn(rpc, quote, signature) {
  decode58(signature, 64);
  await verifyFlyMint(rpc);
  const [statuses, transaction] = await Promise.all([
    rpc('getSignatureStatuses', [[signature], { searchTransactionHistory: true }]),
    rpc('getTransaction', [signature, { commitment: 'finalized', encoding: 'jsonParsed', maxSupportedTransactionVersion: 0 }]),
  ]);
  return verifyShipBurn({ purchase: quote, signature, status: statuses?.value?.[0], transaction });
}
