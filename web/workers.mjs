import { encode58 } from './fly-codec.mjs';
const $ = id => document.getElementById(id);
const wallets = new Set(); let wallet, account, catalog, busy = false, connected = false;
const say = message => { $('status').textContent = message; };
const messages = { connect_wallet: 'Connect your wallet to continue.', insufficient_fly: 'This wallet needs more FLY for this worker.',
  burn_not_finalized: 'Waiting for Solana to finalize your burn…', shop_full: 'Worker spaces are reserved. Please check back later.',
  finish_existing_purchase: 'Finish your open purchase in the history below.', service_unavailable: 'The world is reconnecting. Your saved purchase will resume here.',
  transaction_changed: 'Please choose your worker again for a fresh purchase request.',
  invalid_signature: 'Please choose your worker again for a fresh wallet request.',
  wallet_signature_failed: 'The wallet signature could not be checked. Please connect again.' };
async function api(route, data) {
  const response = await fetch(`/api/fly-shop/${route}`, { method: data ? 'POST' : 'GET', credentials: 'same-origin',
    headers: data ? { 'content-type': 'application/json' } : {}, body: data ? JSON.stringify(data) : undefined,
    signal: AbortSignal.timeout(45000) });
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || 'service_unavailable');
  return result;
}
function showError(error) { say(messages[error.message] || 'Please check your wallet and try again. Your purchase history stays here.'); }
function listWallets() {
  const current = $('wallets').value;
  $('wallets').replaceChildren();
  for (const w of wallets) {
    if (!w.features['standard:connect'] || !w.features['solana:signMessage'] || !w.features['solana:signTransaction']) continue;
    const option = document.createElement('option'); option.value = w.name; option.textContent = w.name; $('wallets').append(option);
  }
  if (!$('wallets').children.length) { const option = document.createElement('option'); option.textContent = 'Open in a Solana wallet browser'; option.value = ''; $('wallets').append(option); }
  else if ([...$('wallets').options].some(o => o.value === current)) $('wallets').value = current;
}
const registry = Object.freeze({ register(...items) { items.forEach(w => wallets.add(w)); listWallets(); return () => { items.forEach(w => wallets.delete(w)); listWallets(); }; } });
window.addEventListener('wallet-standard:register-wallet', event => { if (typeof event.detail === 'function') event.detail(registry); });
window.dispatchEvent(new CustomEvent('wallet-standard:app-ready', { detail: registry }));
listWallets();
function element(tag, text, className) { const e = document.createElement(tag); e.textContent = text; if (className) e.className = className; return e; }
function drawOffers() {
  $('offers').replaceChildren();
  for (const offer of catalog.offers) {
    const card = element('article', '', 'card');
    card.append(element('div', offer.name, 'eyebrow'), element('h3', offer.ship), element('div', `${offer.tokens} FLY`, 'price'), element('p', offer.detail, 'muted'));
    const button = element('button', `Get ${offer.ship.toLowerCase()}`, 'buy'); button.disabled = !catalog.ready || busy;
    button.onclick = () => purchase(offer.station); card.append(button); $('offers').append(card);
  }
}
async function connect() {
  wallet = [...wallets].find(w => w.name === $('wallets').value);
  if (!wallet) { say('Open this page in your Solana wallet browser, or enable a wallet extension.'); return; }
  const result = await wallet.features['standard:connect'].connect();
  account = result.accounts.find(a => a.chains.includes('solana:mainnet'));
  if (!account) throw new Error('connect_wallet');
  const challenge = await api('challenge', { wallet: account.address });
  const bytes = new TextEncoder().encode(challenge.message);
  const [signed] = await wallet.features['solana:signMessage'].signMessage({ account, message: bytes });
  if (signed.signedMessage.length !== bytes.length || !bytes.every((v, i) => signed.signedMessage[i] === v)) throw new Error('wallet_signature_failed');
  await api('session', { id: challenge.id, signature: [...signed.signature].map(n => n.toString(16).padStart(2, '0')).join('') });
  connected = true; $('connect').textContent = `${account.address.slice(0, 4)}…${account.address.slice(-4)}`;
  say('Wallet connected. Choose a worker below.'); await refresh();
}
$('connect').onclick = () => connect().catch(showError);
const storageKey = id => `signal-fly:${id}`;
async function submitSaved(id, transaction) {
  try { return await api('submit', { id, transaction }); }
  catch (error) {
    if (['invalid_signature', 'transaction_changed'].includes(error.message)) {
      // A validation rejection happens before broadcast. Only clear this local
      // attempt after checking that the server has no accepted payment.
      const me = await api('me');
      const q = me.purchases.find(q => q.id === id);
      if (q?.signature) return q;
      if (q?.state === 'quoted' && localStorage.getItem(`${storageKey(id)}:signed`) === transaction) {
        localStorage.removeItem(storageKey(id)); localStorage.removeItem(`${storageKey(id)}:signed`);
      }
    }
    throw error;
  }
}
async function confirm(id, signature) {
  say('Waiting for Solana to finalize your burn…');
  const me = await api('me');
  const q = me.purchases.find(q => q.id === id);
  if (q?.signature) signature = q.signature;
  else {
    const signed = localStorage.getItem(`${storageKey(id)}:signed`);
    if (signed) signature = (await submitSaved(id, signed)).signature;
  }
  for (let n = 0; n < 24; n++) {
    try {
      const result = await api('confirm', { id, signature });
      if (result.state === 'fulfilled') { localStorage.removeItem(storageKey(id)); localStorage.removeItem(`${storageKey(id)}:signed`); say('Your worker is ready. Its live window is below.'); await refresh(); return; }
      say('Payment saved. The world is preparing your worker.'); await refresh(); return;
    } catch (error) {
      if (error.message !== 'burn_not_finalized' && error.message !== 'service_unavailable') throw error;
      await new Promise(resolve => setTimeout(resolve, 2500));
    }
  }
  say('Solana is still confirming. Use “Check purchase” below to resume.'); await refresh();
}
async function purchase(station) {
  if (busy) return;
  busy = true; drawOffers();
  try {
    if (!connected || !account) await connect();
    if (!connected || !account) return;
    // Resume any saved signature before requesting another wallet payment.
    const me = await api('me');
    const open = me.purchases.find(q => q.state !== 'fulfilled');
    const saved = open && (open.signature || localStorage.getItem(storageKey(open.id)));
    if (saved) { await confirm(open.id, saved); return; }
    say('Preparing your worker purchase…');
    const quote = await api('quote', { station });
    await refresh();
    const transaction = Uint8Array.from(atob(quote.transaction), c => c.charCodeAt(0));
    say(`Approve the ${quote.tokens} FLY burn in your wallet.`);
    const [result] = await wallet.features['solana:signTransaction'].signTransaction({
      account, chain: 'solana:mainnet', transaction });
    const signed = result.signedTransaction;
    const signature = encode58(signed.slice(1, 65));
    const encoded = btoa(String.fromCharCode(...signed));
    localStorage.setItem(storageKey(quote.id), signature);
    localStorage.setItem(`${storageKey(quote.id)}:signed`, encoded);
    await submitSaved(quote.id, encoded);
    await confirm(quote.id, signature);
  } catch (error) { showError(error); }
  finally { busy = false; drawOffers(); }
}
function drawMap(canvas, worker, stations) {
  const ctx = canvas.getContext('2d'); canvas.width = 600; canvas.height = 360;
  ctx.fillStyle = '#0a1411'; ctx.fillRect(0, 0, 600, 360);
  ctx.strokeStyle = '#1e3028'; ctx.lineWidth = 1;
  for (let x = 0; x < 600; x += 60) { ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, 360); ctx.stroke(); }
  for (let y = 0; y < 360; y += 60) { ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(600, y); ctx.stroke(); }
  const point = (x, y) => [300 + (x - worker.x) * .035, 180 + (y - worker.y) * .035];
  for (const [x, y, r] of worker.rocks) { const p = point(x, y); ctx.beginPath(); ctx.arc(...p, Math.max(2, r * .035), 0, Math.PI * 2); ctx.fillStyle = '#78887b'; ctx.fill(); }
  for (const station of stations) { const [x, y] = point(station.x, station.y); ctx.strokeStyle = '#e1bf77'; ctx.strokeRect(x - 6, y - 6, 12, 12); }
  ctx.save(); ctx.translate(300, 180); ctx.rotate(worker.angle); ctx.beginPath(); ctx.moveTo(12, 0); ctx.lineTo(-8, -7); ctx.lineTo(-4, 0); ctx.lineTo(-8, 7); ctx.closePath(); ctx.fillStyle = worker.lost ? '#a56b5b' : '#e1bf77'; ctx.fill(); ctx.restore();
  ctx.fillStyle = '#a5aea9'; ctx.font = '15px monospace'; ctx.fillText('LOCAL VIEW · 5,000 m radius', 16, 338);
}
let refreshing = false;
async function refresh() {
  if (refreshing || document.hidden) return;
  refreshing = true;
  try {
    const me = await api('me'); connected = true;
    $('view-note').textContent = me.workers.length ? `Live world · updated ${new Date().toLocaleTimeString()}` : 'Your workers will appear here after purchase.';
    $('workers').replaceChildren();
    for (const worker of me.workers) {
      const card = element('article', '', 'card'), offer = catalog?.offers[worker.station];
      card.append(element('div', offer?.name || 'Sector One', 'eyebrow'), element('h3', `${offer?.ship || 'Worker'} #${worker.assetId}`));
      const canvas = document.createElement('canvas'); canvas.setAttribute('aria-label', 'Live map centered on your worker'); card.append(canvas);
      const job = worker.lost ? 'Ship lost' : !worker.launched ? 'Launch queued' : worker.towing ? 'Towing cargo' : ['Choosing a job', 'Flying to the rock field', 'Mining', 'Returning to station', 'Docked', 'Flying to destination', 'Unloading'][worker.state] || 'Working autonomously';
      card.append(element('div', job, 'job'), element('small', `Hull ${Math.max(0, Math.round(worker.hull))} / ${Math.round(worker.maxHull)}`));
      $('workers').append(card); drawMap(canvas, worker, me.stations);
    }
    // Keep recovery fields stable while the owner enters a receipt.
    if (!$('purchases').contains(document.activeElement)) {
      $('purchases').replaceChildren(); $('purchase-area').classList.toggle('hidden', !me.purchases.length);
      for (const q of me.purchases) {
        if (q.expiredSignature && localStorage.getItem(storageKey(q.id)) === q.expiredSignature) {
          localStorage.removeItem(storageKey(q.id)); localStorage.removeItem(`${storageKey(q.id)}:signed`);
        }
        const row = element('div', `${q.tokens} FLY · ${q.state === 'fulfilled' ? 'Worker granted' : q.state === 'paid' ? 'Payment saved · preparing worker' : 'Awaiting payment'}`);
        if (q.signature) { const link = element('a', ' View burn'); link.href = `https://explorer.solana.com/tx/${q.signature}`; link.target = '_blank'; link.rel = 'noopener'; row.append(link); }
        if (q.state !== 'fulfilled') {
          const input = document.createElement('input'); input.placeholder = 'Burn signature from your wallet, if already sent'; input.setAttribute('aria-label', 'Burn transaction signature'); input.value = q.signature || localStorage.getItem(storageKey(q.id)) || '';
          const button = element('button', 'Check purchase'); button.onclick = () => confirm(q.id, input.value.trim()).catch(showError); row.append(input, button);
        }
        $('purchases').append(row);
      }
    }
  } catch (error) { if (error.message === 'connect_wallet') connected = false; else $('view-note').textContent = 'Reconnecting to the live world…'; }
  finally { refreshing = false; }
}
try { catalog = await api('catalog'); drawOffers(); say(catalog.ready ? 'Three stations. Choose where your worker begins.' : 'The world is preparing worker launches.'); await refresh(); } catch (error) { showError(error); }
setInterval(() => { if (connected) void refresh(); }, 3000);
