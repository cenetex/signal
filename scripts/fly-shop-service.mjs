import { createPublicKey, randomBytes, verify } from 'node:crypto';
import path from 'node:path';
import { FlyShopStore } from './fly-shop-store.mjs';
import { decode58, encode58 } from '../web/fly-codec.mjs';
import { FLY_MINT, OFFERS, makeRpc, prepareFlyBurn, checkFlyBurn } from './fly-shop-chain.mjs';
import { TOKEN_2022_PROGRAM } from './solana-ship-burn.mjs';
const now = () => Date.now();
const fail = (code, status = 400) => Object.assign(new Error(code), { status });
const pubkey = wallet => createPublicKey({ format: 'der', type: 'spki',
  key: Buffer.concat([Buffer.from('302a300506032b6570032100', 'hex'), Buffer.from(decode58(wallet, 32))]) });

export async function createFlyShop({ dataDir, origin, coreUrl, coreKey, rpc,
  fetchImpl = fetch, clock = now, allowHttp = false }) {
  const cookieName = allowHttp ? 'signalFlyDev' : '__Host-signalFly';
  const site = new URL(origin);
  if ((!allowHttp && site.protocol !== 'https:') || site.origin !== origin || !/^[a-f0-9]{64}$/.test(coreKey))
    throw fail('invalid_shop_configuration');
  const store = await new FlyShopStore(path.join(dataDir, 'fly-shop.json')).init();
  const savedSignatures = new Set();
  for (const q of store.rows) {
    if (q.mint !== FLY_MINT || q.tokenProgram !== TOKEN_2022_PROGRAM || q.decimals !== 9 ||
        q.amount !== String(BigInt(OFFERS[q.station].tokens) * 1000000000n)) throw fail('invalid_shop_store');
    decode58(q.wallet, 32);
    if (q.signature || q.state !== 'quoted') {
      decode58(q.signature, 64);
      if (savedSignatures.has(q.signature)) throw fail('invalid_shop_store');
      savedSignatures.add(q.signature);
    }
  }
  const challenges = new Map(), sessions = new Map(), rates = new Map();
  const json = (res, status, data, extra = {}) => {
    res.writeHead(status, { 'content-type': 'application/json', 'cache-control': 'no-store',
      'x-content-type-options': 'nosniff', ...extra });
    res.end(JSON.stringify(data));
  };
  const commit = async fn => {
    const before = structuredClone(store.rows);
    try { const value = fn(); await store.save(); return value; }
    catch (error) { store.rows = before; throw error; }
  };
  const core = async (route, data) => {
    const response = await fetchImpl(`${coreUrl}/internal/v1/fly-shop/${route}`, {
      method: 'POST', redirect: 'error', signal: AbortSignal.timeout(30000),
      headers: { 'content-type': 'application/json', 'x-fly-shop-key': coreKey }, body: JSON.stringify(data),
    });
    if (!response.ok) throw fail('world_unavailable', 503);
    return response.json();
  };
  const worldView = wallet => core('view', { wallet: Buffer.from(decode58(wallet, 32)).toString('hex') });
  const publicView = () => core('view', { wallet: '00'.repeat(32) });
  const fulfill = async q => {
    const grant = await core('grant', { wallet: Buffer.from(decode58(q.wallet, 32)).toString('hex'),
      id: q.id, signature: Buffer.from(decode58(q.signature, 64)).toString('hex'), station: q.station });
    if (grant.ok !== true || !Number.isSafeInteger(grant.assetId) || grant.assetId <= 0)
      throw fail('world_unavailable', 503);
    await commit(() => { q.state = 'fulfilled'; q.assetId = grant.assetId; });
    return grant;
  };
  const body = async req => {
    if (!String(req.headers['content-type'] || '').startsWith('application/json')) throw fail('json_required');
    const parts = []; let size = 0;
    for await (const chunk of req) {
      size += chunk.length;
      if (size > 4096) throw fail('request_too_large', 413);
      parts.push(chunk);
    }
    try { return JSON.parse(Buffer.concat(parts).toString('utf8')); }
    catch { throw fail('invalid_json'); }
  };
  const limited = (key, limit, period) => {
    const time = clock();
    for (const [id, row] of rates) if (time >= row.until) rates.delete(id);
    let row = rates.get(key);
    if (!row) { if (rates.size >= 4096) throw fail('busy', 429); row = { count: 0, until: time + period }; rates.set(key, row); }
    if (++row.count > limit) throw fail('slow_down', 429);
  };
  const authenticate = req => {
    for (const [id, row] of sessions) if (row.expires <= clock()) sessions.delete(id);
    const cookie = String(req.headers.cookie || '').split(';').map(s => s.trim()).find(s => s.startsWith(`${cookieName}=`));
    const token = cookie?.slice(cookieName.length + 1);
    const session = sessions.get(token);
    if (!session) throw fail('connect_wallet', 401);
    return session.wallet;
  };
  const publicQuote = q => ({ id: q.id, station: q.station, tokens: OFFERS[q.station].tokens,
    createdAt: q.createdAt, state: q.state, signature: q.signature || null, assetId: q.assetId || null, expiredSignature: q.expiredSignature || null });
  const recoverExpired = async q => {
    if (q.state !== 'quoted' || !q.signature || !Number.isSafeInteger(q.lastValidBlockHeight)) return false;
    const height = await rpc('getBlockHeight', [{ commitment: 'finalized' }]);
    if (!Number.isSafeInteger(height) || height <= q.lastValidBlockHeight) return false;
    const [statuses, transaction] = await Promise.all([
      rpc('getSignatureStatuses', [[q.signature], { searchTransactionHistory: true }]),
      rpc('getTransaction', [q.signature, { commitment: 'finalized', encoding: 'jsonParsed', maxSupportedTransactionVersion: 0 }]),
    ]);
    const status = statuses?.value?.[0];
    if (transaction && transaction.meta?.err === null ||
        status && !(status.confirmationStatus === 'finalized' && status.err)) return false;
    if (transaction && !transaction.meta?.err) return false;
    await commit(() => { q.expiredSignature = q.signature; delete q.signature; delete q.signed; delete q.prepared; });
    return true;
  };
  let retrying = false;
  const retry = async () => {
    if (retrying) return;
    retrying = true;
    try {
      await store.exclusive(async () => {
        for (const q of store.rows.filter(q => q.state !== 'fulfilled' && q.signature)) {
          try {
            if (q.state === 'quoted') {
              await checkFlyBurn(rpc, q, q.signature);
              await commit(() => { q.state = 'paid'; });
            }
            await fulfill(q);
          } catch {
            try {
              if (!await recoverExpired(q) && q.state === 'quoted' && q.signed)
                await rpc('sendTransaction', [q.signed, { encoding: 'base64', skipPreflight: false, preflightCommitment: 'confirmed', maxRetries: 5 }]);
            } catch { /* Retry after the chain or world reconnects. */ }
          }
        }
      });
    } finally { retrying = false; }
  };
  const interval = setInterval(() => { retry().catch(() => {}); }, 15000);
  interval.unref();
  return {
    close() { clearInterval(interval); },
    retryPending: retry,
    async handle(req, res, url) {
      try {
        if (req.method === 'POST' && req.headers.origin !== origin) throw fail('origin_mismatch', 403);
        if (!['GET', 'POST'].includes(req.method)) throw fail('method_not_allowed', 405);
        const route = url.pathname.slice('/api/fly-shop/'.length);
        const data = req.method === 'POST' ? await body(req) : {};
        if (req.method === 'GET' && route === 'catalog') {
          limited(`catalog:${req.socket.remoteAddress}`, 240, 60000);
          const view = await publicView();
          json(res, 200, { offers: OFFERS, mint: FLY_MINT, decimals: 9, chain: 'solana:mainnet',
            ready: view.ready, remaining: view.remaining }); return;
        }
        if (req.method === 'POST' && route === 'challenge') {
          limited(`challenge:${req.socket.remoteAddress}`, 30, 600000);
          decode58(data.wallet, 32);
          for (const [id, c] of challenges) if (c.expires <= clock()) challenges.delete(id);
          if (challenges.size >= 1024) throw fail('busy', 429);
          const id = randomBytes(32).toString('hex'), expires = clock() + 300000;
          const message = `Signal FLY workers\nOrigin: ${origin}\nWallet: ${data.wallet}\nNonce: ${id}\nExpires: ${new Date(expires).toISOString()}\nSign in to view and purchase autonomous workers.`;
          challenges.set(id, { wallet: data.wallet, message, expires });
          json(res, 200, { id, message }); return;
        }
        if (req.method === 'POST' && route === 'session') {
          const c = challenges.get(data.id); challenges.delete(data.id);
          if (!c || c.expires <= clock() || !/^[a-f0-9]{128}$/.test(data.signature || '') ||
              !verify(null, Buffer.from(c.message), pubkey(c.wallet), Buffer.from(data.signature, 'hex')))
            throw fail('wallet_signature_failed', 401);
          for (const [id, row] of sessions) if (row.expires <= clock()) sessions.delete(id);
          if (sessions.size >= 1024) throw fail('busy', 429);
          const token = randomBytes(32).toString('hex');
          sessions.set(token, { wallet: c.wallet, expires: clock() + 86400000 });
          json(res, 200, { wallet: c.wallet }, { 'set-cookie': `${cookieName}=${token}; Path=/; HttpOnly; SameSite=Strict; Max-Age=86400${allowHttp ? '' : '; Secure'}` }); return;
        }
        const wallet = authenticate(req);
        limited(`wallet:${wallet}`, 90, 60000);
        if (req.method === 'GET' && route === 'me') {
          const view = await worldView(wallet);
          json(res, 200, { wallet, ...view, purchases: store.rows.filter(q => q.wallet === wallet).map(publicQuote) }); return;
        }
        if (req.method === 'POST' && route === 'quote') {
          limited(`quote:${wallet}`, 6, 600000);
          if (!Number.isInteger(data.station) || !OFFERS[data.station]) throw fail('invalid_station');
          const result = await store.exclusive(async () => {
            const view = await worldView(wallet);
            const open = store.rows.filter(q => q.state !== 'fulfilled');
            let q = open.find(q => q.wallet === wallet);
            if (!view.ready || (!q && view.remaining <= open.length) || store.rows.length >= 5000) throw fail('shop_full', 409);
            if (q?.state === 'paid' || q?.signature) throw fail('finish_existing_purchase', 409);
            if (q && q.station !== data.station) throw fail('finish_existing_purchase', 409);
            if (!q) {
              q = { id: randomBytes(32).toString('hex'), wallet, station: data.station, state: 'quoted',
                createdAt: clock(), mint: FLY_MINT, tokenProgram: TOKEN_2022_PROGRAM, decimals: 9,
                amount: String(BigInt(OFFERS[data.station].tokens) * 1000000000n) };
            }
            const prepared = await prepareFlyBurn(rpc, q);
            await commit(() => { q.prepared = prepared.transaction; q.lastValidBlockHeight = prepared.lastValidBlockHeight; if (!store.rows.includes(q)) store.rows.push(q); });
            return { ...publicQuote(q), transaction: prepared.transaction, mint: FLY_MINT };
          });
          json(res, 200, result); return;
        }
        if (req.method === 'POST' && route === 'submit') {
          const result = await store.exclusive(async () => {
            const q = store.rows.find(q => q.id === data.id && q.wallet === wallet);
            if (!q) throw fail('purchase_not_found', 404);
            if (typeof data.transaction !== 'string' || data.transaction.length > 2000) throw fail('invalid_signature');
            const signed = Buffer.from(data.transaction, 'base64');
            const prepared = Buffer.from(q.prepared || '', 'base64');
            if (signed.length !== prepared.length || signed[0] !== 1 ||
                !signed.subarray(65).equals(prepared.subarray(65)) ||
                !verify(null, signed.subarray(65), pubkey(wallet), signed.subarray(1, 65))) throw fail('invalid_signature');
            const signature = encode58(signed.subarray(1, 65));
            if (q.signature && q.signature !== signature) throw fail('purchase_already_paid', 409);
            // Persist the signed receipt before the first network broadcast.
            if (!q.signature) await commit(() => { q.signature = signature; q.signed = data.transaction; });
            if (q.state === 'quoted') {
              try { await rpc('sendTransaction', [q.signed, { encoding: 'base64', skipPreflight: false, preflightCommitment: 'confirmed', maxRetries: 5 }]); }
              catch { /* Confirmation and retry use the saved signature and identical bytes. */ }
            }
            return publicQuote(q);
          });
          json(res, 200, result); return;
        }
        if (req.method === 'POST' && route === 'confirm') {
          const result = await store.exclusive(async () => {
            const q = store.rows.find(q => q.id === data.id && q.wallet === wallet);
            if (!q) throw fail('purchase_not_found', 404);
            if (q.signature && q.signature !== data.signature) throw fail('purchase_already_paid', 409);
            if (q.state === 'fulfilled') return publicQuote(q);
            if (q.state === 'quoted') {
              await checkFlyBurn(rpc, q, data.signature);
              if (store.rows.some(other => other.id !== q.id && other.signature === data.signature)) throw fail('receipt_used', 409);
              await commit(() => { q.state = 'paid'; q.signature = data.signature; });
            }
            try { await fulfill(q); } catch { /* Saved paid purchases are retried after reconnect or restart. */ }
            return publicQuote(q);
          });
          json(res, 200, result); return;
        }
        throw fail('route_not_found', 404);
      } catch (error) {
        const known = new Set(['invalid_address', 'invalid_station', 'invalid_signature', 'burn_not_finalized',
          'signature_mismatch', 'burn_amount_mismatch', 'burn_identity_mismatch', 'purchase_memo_mismatch',
          'insufficient_fly', 'connect_wallet', 'wallet_signature_failed', 'purchase_not_found',
          'purchase_already_paid', 'receipt_used', 'finish_existing_purchase', 'shop_full', 'slow_down',
          'origin_mismatch', 'json_required', 'invalid_json', 'request_too_large', 'busy']);
        json(res, error.status || (known.has(error.message) ? 400 : 503),
          { error: known.has(error.message) ? error.message : 'service_unavailable' });
      }
    },
  };
}
export async function shopFromEnvironment(env = process.env) {
  if (env.SIGNAL_FLY_SHOP_ENABLED !== '1') return null;
  return createFlyShop({ dataDir: env.SIGNAL_DATA_DIR || '/app/data',
    origin: env.SIGNAL_ALLOWED_ORIGIN, coreUrl: `http://127.0.0.1:${env.SIGNAL_SERVER_PORT || 9091}`,
    coreKey: env.SIGNAL_FLY_SHOP_KEY,
    rpc: makeRpc(env.SIGNAL_SOLANA_RPC_URL || 'https://api.mainnet-beta.solana.com') });
}
