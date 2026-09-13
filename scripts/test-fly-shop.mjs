import test from 'node:test';
import vm from 'node:vm';
import assert from 'node:assert/strict';
import { generateKeyPairSync, sign } from 'node:crypto';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { createFlyShop } from './fly-shop-service.mjs';
import { encode58, decode58 } from '../web/fly-codec.mjs';
import { MAINNET_GENESIS, FLY_MINT, OFFERS, verifyFlyMint, buildBurnTransaction, matchesPreparedBurn } from './fly-shop-chain.mjs';
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
  const grants = new Map(), reservations = new Map();
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
    if (url.endsWith('/view')) return Response.json({ ready: true, remaining: 80 - reservations.size, workers: [...grants.values()].filter(g => g.wallet === data.wallet).map(g => ({ id: g.id, assetId: g.assetId })), stations: [] });
    if (url.endsWith('/reserve')) {
      if (!coreAvailable) return new Response('', { status: 503 });
      if (!reservations.has(data.id)) reservations.set(data.id, { ...data, assetId: reservations.size + 1 });
      return Response.json({ ok: true, assetId: reservations.get(data.id).assetId });
    }
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

test('saved finalized receipt restores ownership after world rollback', async t => {
  const f = await setup(t); await f.login(); const q = (await f.quote()).data;
  await f.request('submit', { id: q.id, transaction: f.signed(q) }); f.setFinal(true); await f.retry();
  assert.equal(f.grants.size, 1); f.grants.clear(); await f.restart(); await f.retry();
  assert.equal(f.grants.size, 1); await f.login();
  assert.equal((await f.request('me')).data.purchases[0].state, 'fulfilled');
});

test('a failed world reservation prevents payment broadcast', async t => {
  const f = await setup(t); await f.login(); f.setCore(false);
  assert.equal((await f.quote()).status, 503); assert.equal(f.sent, 0);
  f.setCore(true); const q = (await f.quote()).data; f.setCore(false);
  const submitted = await f.request('submit', { id: q.id, transaction: f.signed(q) });
  assert.equal(submitted.data.state, 'quoted'); assert.equal(f.sent, 0);
  f.setCore(true); await f.retry(); assert.equal(f.sent, 1);
});

// Build wallet variants independently of the production decoder.
function walletVariant(encoded, { versioned = false, fee = true, mutate = () => {} } = {}) {
  const wire = Buffer.from(encoded, 'base64');
  const original = Array.from({ length: 5 }, (_, i) => wire.subarray(69 + i * 32, 101 + i * 32));
  const keys = [original[0], original[2], original[1], original[4], original[3]];
  if (fee) keys.push(Buffer.from(decode58('ComputeBudget111111111111111111111111111111', 32)));
  const offset = 69 + 160 + 32 + 1;
  const instructions = [
    { program: 4, accounts: [2, 1, 0], data: Buffer.from(wire.subarray(offset + 6, offset + 16)) },
    { program: 3, accounts: [0], data: Buffer.from(wire.subarray(offset + 20)) },
  ];
  if (fee) {
    const limit = Buffer.alloc(5); limit[0] = 2; limit.writeUInt32LE(200000, 1);
    const price = Buffer.alloc(9); price[0] = 3; price.writeBigUInt64LE(10000n, 1);
    instructions.unshift({ program: 5, accounts: [], data: limit }, { program: 5, accounts: [], data: price });
  }
  const hash = Buffer.from(wire.subarray(229, 261));
  mutate({ keys, instructions, hash });
  return Buffer.concat([Buffer.from([1]), Buffer.alloc(64), ...(versioned ? [Buffer.from([128])] : []),
    Buffer.from([1, 0, keys.length - 3, keys.length]), ...keys, hash, Buffer.from([instructions.length]),
    ...instructions.map(ix => Buffer.concat([Buffer.from([ix.program, ix.accounts.length, ...ix.accounts, ix.data.length]), ix.data])),
    ...(versioned ? [Buffer.from([0])] : [])]);
}
for (const versioned of [false, true]) for (const fee of [false, true])
  test(`wallet reordered ${versioned ? 'v0' : 'legacy'} transaction with fees=${fee} is accepted`, async t => {
    const f = await setup(t); await f.login(); const q = (await f.quote()).data;
    const bytes = walletVariant(q.transaction, { versioned, fee });
    sign(null, bytes.subarray(65), f.buyer.privateKey).copy(bytes, 1);
    const result = await f.request('submit', { id: q.id, transaction: bytes.toString('base64') });
    assert.equal(result.status, 200); assert.equal(f.sent, 1);
    f.setFinal(true);
    assert.equal((await f.request('confirm', { id: q.id, signature: result.data.signature })).data.state, 'fulfilled');
  });
for (const [name, mutate] of Object.entries({
  amount: ({ instructions }) => { instructions[2].data[1] ^= 1; },
  decimals: ({ instructions }) => { instructions[2].data[9] = 8; },
  memo: ({ instructions }) => { instructions[3].data[20] ^= 1; },
  authority: ({ instructions }) => { instructions[2].accounts[2] = 1; },
  source: ({ keys }) => { keys[2] = Buffer.alloc(32, 44); },
  mint: ({ keys }) => { keys[1] = Buffer.alloc(32, 45); },
  blockhash: ({ hash }) => { hash[0] ^= 1; },
  extraBurn: ({ instructions }) => { instructions.push(instructions[2]); },
  feeAccounts: ({ instructions }) => { instructions[0].accounts = [0]; },
  duplicateFee: ({ instructions }) => { instructions.push(instructions[0]); },
  excessiveFee: ({ instructions }) => { instructions[1].data.writeBigUInt64LE(1000000000n, 1); },
  extraKey: ({ keys }) => { keys.push(Buffer.alloc(32, 46)); },
})) test(`signed wallet mutation rejects ${name} before broadcast`, async t => {
  const f = await setup(t); await f.login(); const q = (await f.quote()).data;
  const bytes = walletVariant(q.transaction, { mutate });
  sign(null, bytes.subarray(65), f.buyer.privateKey).copy(bytes, 1);
  assert.equal((await f.request('submit', { id: q.id, transaction: bytes.toString('base64') })).data.error, 'transaction_changed');
  assert.equal(f.sent, 0);
});
test('wire parser rejects truncation, trailing data and address lookups', () => {
  const q = { id: 'b'.repeat(64), wallet: key().address, mint: FLY_MINT, tokenProgram: TOKEN_2022_PROGRAM, decimals: 9, amount: '50000000000' };
  const prepared = buildBurnTransaction(q, key().address, key().address);
  const v0 = walletVariant(prepared.toString('base64'), { versioned: true });
  for (let n = 0; n < v0.length; n++) assert.equal(matchesPreparedBurn(v0.subarray(0, n), prepared), false);
  assert.equal(matchesPreparedBurn(Buffer.concat([v0, Buffer.from([0])]), prepared), false);
  v0[v0.length - 1] = 1; assert.equal(matchesPreparedBurn(v0, prepared), false);
});
async function recoveryHarness(api, entries) {
  const source = await readFile(new URL('../web/workers.mjs', import.meta.url), 'utf8');
  const cache = new Map(entries);
  const context = vm.createContext({ api, localStorage: { getItem: k => cache.get(k) || null, removeItem: k => cache.delete(k) },
    say() {}, refresh: async () => {}, setTimeout });
  vm.runInContext(source.slice(source.indexOf('const storageKey ='), source.indexOf('async function purchase(')), context);
  return { cache, submit: (id, tx) => context.submitSaved(id, tx), confirm: (id, sig) => context.confirm(id, sig) };
}
test('rejected local payment clears cache after the server confirms awaiting payment', async () => {
  const h = await recoveryHarness(async route => {
    if (route === 'submit') throw Error('invalid_signature');
    return { purchases: [{ id: 'q', state: 'quoted', signature: null }] };
  }, [['signal-fly:q', 'sig'], ['signal-fly:q:signed', 'tx']]);
  await assert.rejects(h.submit('q', 'tx'), /invalid_signature/); assert.equal(h.cache.size, 0);
});
test('ambiguous submission retains the exact signed transaction', async () => {
  const h = await recoveryHarness(async () => { throw Error('service_unavailable'); }, [['signal-fly:q:signed', 'tx']]);
  await assert.rejects(h.submit('q', 'tx'), /service_unavailable/); assert.equal(h.cache.get('signal-fly:q:signed'), 'tx');
});
test('accepted server receipt wins over stale local cache on recovery', async () => {
  const routes = [];
  const h = await recoveryHarness(async (route, data) => {
    routes.push(route);
    if (route === 'me') return { purchases: [{ id: 'q', state: 'paid', signature: 'accepted' }] };
    assert.equal(route, 'confirm'); assert.equal(data.signature, 'accepted'); return { state: 'fulfilled' };
  }, [['signal-fly:q:signed', 'old-tx']]);
  await h.confirm('q', 'old-sig'); assert.deepEqual(routes, ['me', 'confirm']); assert.equal(h.cache.size, 0);
});

const LIGHTHOUSE = 'L2TExMFKdjpN9kozasaurPirfHy9P8sbXoAN1qA3S95';
function guardedVariant(encoded, mutate = () => {}) {
  return walletVariant(encoded, { mutate: parts => {
    parts.keys.push(Buffer.from(decode58(LIGHTHOUSE, 32)));
    // Synthetic account-state checks with the same opcodes and placement as
    // Solflare's wallet guards. Keep user transactions out of the fixtures.
    const info = { program: 6, accounts: [2], data: Buffer.from('060401030100', 'hex') };
    parts.instructions.splice(2, 0, info, { ...info, accounts: [1] });
    parts.instructions.push({ ...info, accounts: [0] },
      { program: 6, accounts: [2], data: Buffer.from('0a04010000743ba40b00000000', 'hex') });
    mutate(parts);
  } });
}
test('signed wallet state guards around the burn retain a valid receipt', async t => {
  const f = await setup(t); await f.login(); const q = (await f.quote()).data;
  const bytes = guardedVariant(q.transaction);
  sign(null, bytes.subarray(65), f.buyer.privateKey).copy(bytes, 1);
  const result = await f.request('submit', { id: q.id, transaction: bytes.toString('base64') });
  assert.equal(result.status, 200); assert.equal(f.sent, 1);
  f.setFinal(true);
  assert.equal((await f.request('confirm', { id: q.id, signature: result.data.signature })).data.state, 'fulfilled');
});
for (const [name, mutate] of Object.entries({
  memoryWrite: ({ instructions }) => { instructions[2].data[0] = 0; },
  memoryClose: ({ instructions }) => { instructions[2].data[0] = 1; },
  unknownGuard: ({ instructions }) => { instructions[2].data[0] = 255; },
  extraAccount: ({ instructions }) => { instructions[2].accounts.push(0); },
  programAccount: ({ instructions }) => { instructions[2].accounts = [3]; },
  otherProgram: ({ keys }) => { keys[6] = Buffer.alloc(32, 55); },
  changedBurn: ({ instructions }) => { instructions[4].data[1] ^= 1; },
})) test(`wallet guard rejects ${name}`, () => {
  const q = { id: 'a'.repeat(64), wallet: key().address, mint: FLY_MINT, tokenProgram: TOKEN_2022_PROGRAM, decimals: 9, amount: '50000000000' };
  const prepared = buildBurnTransaction(q, key().address, key().address);
  assert.equal(matchesPreparedBurn(guardedVariant(prepared.toString('base64'), mutate), prepared), false);
});
test('valid transaction with a damaged signature reports signature failure', async t => {
  const f = await setup(t); await f.login(); const q = (await f.quote()).data;
  const bytes = Buffer.from(f.signed(q), 'base64'); bytes[1] ^= 1;
  assert.equal((await f.request('submit', { id: q.id, transaction: bytes.toString('base64') })).data.error, 'invalid_signature');
  assert.equal(f.sent, 0);
});
test('a changed transaction clears the rejected cache for a fresh quote', async () => {
  const h = await recoveryHarness(async route => {
    if (route === 'submit') throw Error('transaction_changed');
    return { purchases: [{ id: 'q', state: 'quoted', signature: null }] };
  }, [['signal-fly:q', 'sig'], ['signal-fly:q:signed', 'tx']]);
  await assert.rejects(h.submit('q', 'tx'), /transaction_changed/); assert.equal(h.cache.size, 0);
});
