import test from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, sign } from 'node:crypto';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { createFlyShop } from './fly-shop-service.mjs';
import { encode58, decode58 } from '../web/fly-codec.mjs';
import { MAINNET_GENESIS, FLY_MINT, OFFERS, verifyFlyMint, buildBurnTransaction } from './fly-shop-chain.mjs';
import { TOKEN_2022_PROGRAM, MEMO_PROGRAM, purchaseMemo } from './solana-ship-burn.mjs';
const key = () => { const pair = generateKeyPairSync('ed25519'); return { ...pair, address: encode58(pair.publicKey.export({ format: 'der', type: 'spki' }).subarray(-32)) }; };
const mintAccount = () => ({ value: { owner: TOKEN_2022_PROGRAM, executable: false, data: { parsed: { type: 'mint', info: {
  decimals: 9, isInitialized: true, mintAuthority: null, freezeAuthority: null, extensions: [{ extension: 'metadataPointer' }, { extension: 'tokenMetadata' }],
} } } } });
function finalized(q, signature) {
  const balance = amount => ({ accountIndex: 1, mint: FLY_MINT, owner: q.wallet, programId: TOKEN_2022_PROGRAM, uiTokenAmount: { amount, decimals: 9 } });
  return { slot: 100, meta: { err: null, preTokenBalances: [balance(q.amount)], postTokenBalances: [balance('0')] },
    transaction: { signatures: [signature], message: { accountKeys: [{ pubkey: q.wallet, signer: true }, { pubkey: 'token', signer: false }], instructions: [
      { programId: TOKEN_2022_PROGRAM, parsed: { type: 'burnChecked', info: { mint: FLY_MINT, authority: q.wallet, account: 'token', tokenAmount: { amount: q.amount, decimals: 9 } } } },
      { programId: MEMO_PROGRAM, parsed: purchaseMemo(q.id) },
    ] } } };
}
async function setup(t) {
  const dir = await mkdtemp(path.join(os.tmpdir(), 'signal-fly-test-'));
  const buyer = key(), token = key().address, hash = key().address;
  let shop, origin, grantCalls = 0, coreAvailable = true, chainFinal = false, currentQuote, lastSignature, sent = 0, height = 500, chainError = false;
  const grants = new Map();
  const rpc = async (method, params) => {
    switch (method) {
      case 'getGenesisHash': return MAINNET_GENESIS;
      case 'getAccountInfo': return mintAccount();
      case 'getTokenAccountsByOwner': return { value: [{ pubkey: token, account: { owner: TOKEN_2022_PROGRAM, data: { parsed: { info: { owner: params[0], mint: FLY_MINT, state: 'initialized', tokenAmount: { amount: '1000000000000', decimals: 9 } } } } } }] };
      case 'getLatestBlockhash': return { value: { blockhash: hash, lastValidBlockHeight: 999 } };
      case 'getBlockHeight': return height;
      case 'sendTransaction': {
        const saved = JSON.parse(await readFile(path.join(dir, 'fly-shop.json'), 'utf8')).quotes;
        lastSignature = encode58(Buffer.from(params[0], 'base64').subarray(1, 65));
        assert.ok(saved.some(q => q.signature === lastSignature && q.signed === params[0]), 'signed receipt is durable before broadcast');
        sent++; return lastSignature;
      }
      case 'getSignatureStatuses': return { value: [chainFinal ? { slot: 100, err: chainError ? 'failed' : null, confirmationStatus: 'finalized' } : null] };
      case 'getTransaction': { const tx = chainFinal ? finalized(currentQuote, params[0]) : null; if (tx && chainError) tx.meta.err = 'failed'; return tx; }
      default: throw Error(method);
    }
  };
  const core = async (url, options) => {
    const data = JSON.parse(options.body);
    assert.equal(options.headers['x-fly-shop-key'], 'a'.repeat(64));
    if (url.endsWith('/view')) return Response.json({ ready: true, remaining: 80 - grants.size, workers: [], stations: [] });
    grantCalls++;
    if (!coreAvailable) return new Response('', { status: 503 });
    if (!grants.has(data.id)) grants.set(data.id, { ...data, assetId: grants.size + 1 });
    return Response.json({ ok: true, assetId: grants.get(data.id).assetId });
  };
  const server = http.createServer((req, res) => shop.handle(req, res, new URL(req.url, origin)));
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve)); origin = `http://127.0.0.1:${server.address().port}`;
  const start = () => createFlyShop({ dataDir: dir, origin, coreUrl: 'http://core', coreKey: 'a'.repeat(64), rpc, fetchImpl: core, allowHttp: true });
  shop = await start();
  t.after(async () => { shop.close(); server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); await rm(dir, { recursive: true, force: true }); });
  let cookie = '';
  async function request(route, body, override = {}) {
    const response = await fetch(`${origin}/api/fly-shop/${route}`, { method: body ? 'POST' : 'GET', headers: { origin, cookie, 'content-type': 'application/json', ...override }, body: body ? JSON.stringify(body) : undefined });
    if (response.headers.get('set-cookie')) cookie = response.headers.get('set-cookie').split(';')[0];
    return { status: response.status, data: await response.json() };
  }
  async function login(who = buyer) {
    const c = (await request('challenge', { wallet: who.address })).data;
    return request('session', { id: c.id, signature: sign(null, Buffer.from(c.message), who.privateKey).toString('hex') });
  }
  async function quote(station = 0) {
    const result = await request('quote', { station });
    if (result.status === 200) currentQuote = JSON.parse(await readFile(path.join(dir, 'fly-shop.json'), 'utf8')).quotes.find(q => q.id === result.data.id);
    return result;
  }
  function signed(q) {
    const bytes = Buffer.from(q.transaction, 'base64'); sign(null, bytes.subarray(65), buyer.privateKey).copy(bytes, 1); return bytes.toString('base64');
  }
  return { request, login, quote, signed, buyer, rpc, dir, grants,
    setFinal(value) { chainFinal = value; }, setHeight(value) { height = value; }, setChainError(value) { chainError = value; },
    retry() { return shop.retryPending(); }, setCore(value) { coreAvailable = value; },
    async restart() { shop.close(); shop = await start(); cookie = ''; },
    get sent() { return sent; }, get grantCalls() { return grantCalls; } };
}
test('codec preserves leading zeros and rejects invalid addresses', () => {
  for (const n of [32, 64]) { const data = new Uint8Array(n); data[n - 1] = 255; assert.deepEqual(decode58(encode58(data), n), data); }
  assert.throws(() => decode58('0abc', 32));
});
test('transaction has the exact Token-2022 burn, wallet signer, memo and raw price', () => {
  const q = { id: 'b'.repeat(64), wallet: key().address, mint: FLY_MINT, tokenProgram: TOKEN_2022_PROGRAM, decimals: 9, amount: '200000000000' };
  const bytes = buildBurnTransaction(q, key().address, key().address);
  assert.equal(bytes[0], 1); assert.deepEqual([...bytes.subarray(65, 69)], [1, 0, 2, 5]);
  const burnOffset = 65 + 4 + 160 + 32 + 1;
  assert.deepEqual([...bytes.subarray(burnOffset, burnOffset + 6)], [3, 3, 1, 2, 0, 10]);
  assert.equal(bytes[burnOffset + 6], 15); assert.equal(bytes.readBigUInt64LE(burnOffset + 7), 200000000000n); assert.equal(bytes[burnOffset + 15], 9);
  assert.ok(bytes.includes(Buffer.from(purchaseMemo(q.id))));
});
test('mint gate pins mainnet, Token-2022, decimals and extension set', async () => {
  for (const mutate of [m => { m.value.owner = 'other'; }, m => { m.value.data.parsed.info.decimals = 6; }, m => { m.value.data.parsed.info.extensions.push({ extension: 'transferFeeConfig' }); }]) {
    const m = mintAccount(); mutate(m); await assert.rejects(verifyFlyMint(async method => method === 'getGenesisHash' ? MAINNET_GENESIS : m));
  }
  await assert.rejects(verifyFlyMint(async method => method === 'getGenesisHash' ? 'devnet' : mintAccount()));
});
test('wallet login binds origin and challenge; replay fails', async t => {
  const f = await setup(t);
  assert.equal((await f.request('me')).status, 401);
  assert.equal((await f.request('challenge', { wallet: f.buyer.address }, { origin: 'https://evil.example' })).status, 403);
  const c = (await f.request('challenge', { wallet: f.buyer.address })).data;
  const body = { id: c.id, signature: sign(null, Buffer.from(c.message), f.buyer.privateKey).toString('hex') };
  assert.equal((await f.request('session', body)).status, 200);
  assert.equal((await f.request('session', body)).status, 401);
});
test('signed burn is saved before broadcast and grants once after finality', async t => {
  const f = await setup(t); await f.login();
  const quote = await f.quote(); assert.equal(quote.status, 200); assert.equal(quote.data.tokens, 50);
  const encoded = f.signed(quote.data);
  const submit = await f.request('submit', { id: quote.data.id, transaction: encoded }); assert.equal(submit.status, 200); assert.equal(f.sent, 1);
  const confirm = { id: quote.data.id, signature: submit.data.signature };
  assert.equal((await f.request('confirm', confirm)).status, 400); assert.equal(f.grants.size, 0);
  f.setFinal(true);
  const results = await Promise.all([f.request('confirm', confirm), f.request('confirm', confirm)]);
  results.forEach(r => assert.equal(r.data.state, 'fulfilled')); assert.equal(f.grants.size, 1); assert.equal(f.grantCalls, 1);
  await f.restart(); await f.login(); assert.equal((await f.request('confirm', confirm)).data.state, 'fulfilled'); assert.equal(f.grantCalls, 1);
});
test('paid purchase recovers after a world failure and service restart', async t => {
  const f = await setup(t); await f.login(); const q = (await f.quote(2)).data; assert.equal(q.tokens, 200);
  const sent = (await f.request('submit', { id: q.id, transaction: f.signed(q) })).data;
  f.setFinal(true); f.setCore(false);
  const receipt = { id: q.id, signature: sent.signature };
  assert.equal((await f.request('confirm', receipt)).data.state, 'paid');
  await f.restart(); await f.login(); f.setCore(true);
  assert.equal((await f.request('confirm', receipt)).data.state, 'fulfilled'); assert.equal(f.grants.size, 1);
});
test('altered transaction and another wallet cannot redeem a purchase', async t => {
  const f = await setup(t); await f.login(); const q = (await f.quote(1)).data; assert.equal(q.tokens, 100);
  const bytes = Buffer.from(f.signed(q), 'base64'); bytes[100] ^= 1;
  assert.equal((await f.request('submit', { id: q.id, transaction: bytes.toString('base64') })).status, 400);
  await f.login(key());
  assert.equal((await f.request('submit', { id: q.id, transaction: f.signed(q) })).status, 404);
  assert.equal(f.sent, 0);
});

for (const failed of [false, true]) test(`expired ${failed ? 'failed' : 'absent'} signature allows a fresh wallet attempt`, async t => {
  const f = await setup(t); await f.login(); const q = (await f.quote()).data;
  const sent = (await f.request('submit', { id: q.id, transaction: f.signed(q) })).data;
  assert.equal((await f.quote()).status, 409);
  f.setHeight(1000); f.setFinal(failed); f.setChainError(failed);
  await f.retry();
  const me = (await f.request('me')).data;
  assert.equal(me.purchases[0].signature, null);
  assert.equal(me.purchases[0].expiredSignature, sent.signature);
  assert.equal((await f.quote()).status, 200); assert.equal(f.grants.size, 0);
});
test('background recovery completes a saved finalized signature after restart', async t => {
  const f = await setup(t); await f.login(); const q = (await f.quote()).data;
  await f.request('submit', { id: q.id, transaction: f.signed(q) });
  await f.restart(); f.setFinal(true); f.setHeight(1000); await f.retry(); await f.login();
  assert.equal((await f.request('me')).data.purchases[0].state, 'fulfilled'); assert.equal(f.grants.size, 1);
});
