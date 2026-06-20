import { readFileSync, writeFileSync, existsSync, readdirSync, statSync } from 'fs';
import { resolve, join } from 'path';
import { createHash } from 'crypto';
import Irys from '@irys/sdk';
import { Keypair } from '@solana/web3.js';
import BigNumber from 'bignumber.js';
import { homedir } from 'os';

const SITE_DIR = resolve('_site');
const HOST = process.env.ARWEAVE_HOST || 'arweave.net';
const PORT = parseInt(process.env.ARWEAVE_PORT || '443');
const PROTOCOL = process.env.ARWEAVE_PROTOCOL || 'https';
const AUTO_FUND = process.env.IRYS_AUTO_FUND !== '0';
const FUNDING_BUFFER = parseFloat(process.env.IRYS_FUNDING_BUFFER || '2');
const OST_MANIFEST_FILE = resolve('web/ost-manifest.json');
const PREVIOUS_PATHS_FILE = resolve('.arweave-previous-paths.json');

// Load Solana keypair for Irys
let keypair;
if (process.env.SOLANA_KEYPAIR) {
  keypair = Keypair.fromSecretKey(Uint8Array.from(JSON.parse(process.env.SOLANA_KEYPAIR)));
} else {
  const keyPath = process.env.SOLANA_KEY_PATH || homedir() + '/.config/solana/id.json';
  if (!existsSync(keyPath)) { console.error(`No Solana key at: ${keyPath}`); process.exit(1); }
  keypair = Keypair.fromSecretKey(Uint8Array.from(JSON.parse(readFileSync(keyPath, 'utf-8'))));
}

// Content-hash cache to avoid re-uploading unchanged files
let assetCache = {};
try { assetCache = JSON.parse(readFileSync('.arweave-cache.json', 'utf-8')); } catch (_) {}

let previousPathEntries = {};
try { previousPathEntries = JSON.parse(readFileSync(PREVIOUS_PATHS_FILE, 'utf-8')); } catch (_) {}
const remoteCacheHits = new Map();

function collectFiles(dir, base) {
  const files = [];
  for (const entry of readdirSync(dir)) {
    const full = join(dir, entry);
    const rel = base ? base + '/' + entry : entry;
    if (statSync(full).isDirectory()) files.push(...collectFiles(full, rel));
    else files.push({ path: full, name: rel });
  }
  return files;
}

function sha256(data) { return createHash('sha256').update(data).digest('hex'); }

function ct(f) {
  const t = {
    html: 'text/html',
    js: 'application/javascript',
    wasm: 'application/wasm',
    css: 'text/css',
    json: 'application/json',
    mp3: 'audio/mpeg',
    jpg: 'image/jpeg',
    jpeg: 'image/jpeg',
    png: 'image/png',
    mpg: 'video/mpeg',
    mpeg: 'video/mpeg',
  };
  return t[f.split('.').pop().toLowerCase()] || 'application/octet-stream';
}

function loadStaticPathEntries() {
  if (!existsSync(OST_MANIFEST_FILE)) return {};

  const manifest = JSON.parse(readFileSync(OST_MANIFEST_FILE, 'utf-8'));
  if (!Array.isArray(manifest.tracks)) {
    throw new Error(`Invalid OST manifest: missing tracks array in ${OST_MANIFEST_FILE}`);
  }
  if (manifest.runtimeTracks && !Array.isArray(manifest.runtimeTracks)) {
    throw new Error(`Invalid OST manifest: runtimeTracks must be an array in ${OST_MANIFEST_FILE}`);
  }

  const entries = {};
  const staticTracks = manifest.tracks.concat(manifest.runtimeTracks || []);
  for (const track of staticTracks) {
    const audio = track && track.audio ? track.audio : {};
    const name = audio.path;
    const txId = audio.tx;
    if (typeof name !== 'string' || typeof txId !== 'string' || !name || !txId) {
      throw new Error(`Invalid OST track audio entry in ${OST_MANIFEST_FILE}`);
    }
    if (name.startsWith('/')) {
      throw new Error(`OST track paths must be relative: ${name}`);
    }
    if (entries[name] && entries[name] !== txId) {
      throw new Error(`Conflicting OST track tx for path ${name}`);
    }
    entries[name] = txId;
  }

  return entries;
}

async function fetchGatewayBytes(txId) {
  for (const gw of [`${PROTOCOL}://${HOST}/raw/`, 'https://gateway.irys.xyz/', 'https://node2.irys.xyz/']) {
    try {
      const res = await fetch(gw + txId);
      if (res.ok) return Buffer.from(await res.arrayBuffer());
    } catch (_) {}
  }
  return null;
}

async function remoteCacheTxForFile(name, data) {
  if (remoteCacheHits.has(name)) return remoteCacheHits.get(name);

  const txId = previousPathEntries[name];
  if (typeof txId !== 'string' || !txId) {
    remoteCacheHits.set(name, null);
    return null;
  }

  const remote = await fetchGatewayBytes(txId);
  const hit = remote && sha256(remote) === sha256(data) ? txId : null;
  remoteCacheHits.set(name, hit);
  return hit;
}

async function estimateUploadPlan(irys, files) {
  let payloadBytes = 0;
  let price = new BigNumber(0);

  for (const f of files) {
    const data = readFileSync(f.path);
    const cached = assetCache[f.name];
    const rewritesPerDeploy = f.name === 'play.html' || f.name === 'signal.html';
    if (cached && cached.hash === sha256(data) && !rewritesPerDeploy) continue;
    if (!rewritesPerDeploy && await remoteCacheTxForFile(f.name, data)) continue;

    const itemBytes = data.length;
    payloadBytes += itemBytes;
    price = price.plus(await irys.getPrice(itemBytes + 4096));
  }

  // Irys charges for signed data item overhead too. Keep a small per-file
  // allowance plus manifest/HTML rewrite slack so deploys fund once up front.
  const estimatedBytes = payloadBytes + files.length * 4096 + 65536;
  price = price.plus(await irys.getPrice(65536));
  return { bytes: estimatedBytes, price };
}

async function ensureIrysBalance(irys, uploadPlan) {
  const balance = await irys.getLoadedBalance();
  const { bytes: estimatedBytes, price } = uploadPlan;
  const target = price.multipliedBy(FUNDING_BUFFER).integerValue(BigNumber.ROUND_CEIL);
  console.log(`Estimated upload: ${(estimatedBytes / 1024).toFixed(1)} KB  Estimated cost: ${price}  Target balance: ${target}`);

  if (balance.isGreaterThanOrEqualTo(target)) return balance;
  if (balance.isGreaterThanOrEqualTo(price)) {
    console.log(`Irys credit ${balance} is below buffered target ${target}, but covers estimated cost ${price}; continuing.`);
    return balance;
  }
  if (!AUTO_FUND) {
    throw new Error(`Irys credit ${balance} is below estimated target ${target}; fund the wallet or enable IRYS_AUTO_FUND.`);
  }

  const amount = target.minus(balance).integerValue(BigNumber.ROUND_CEIL);
  console.log(`Funding Irys shortfall: ${amount}...`);
  try {
    const receipt = await irys.fund(amount);
    console.log(`Funded Irys: ${receipt.id || 'submitted'} quantity=${receipt.quantity}`);
  } catch (e) {
    throw new Error(`Unable to fund Irys shortfall ${amount}. Check the Solana wallet balance and funding permissions. ${e.message || e}`);
  }

  const updated = await irys.getLoadedBalance();
  console.log(`Irys credit after funding: ${updated}`);
  if (updated.isLessThan(price)) {
    throw new Error(`Irys credit ${updated} is still below estimated upload cost ${price}.`);
  }
  return updated;
}

async function main() {
  const irys = new Irys({
    network: 'mainnet',
    token: 'solana',
    key: keypair.secretKey,
    config: { providerUrl: 'https://api.mainnet-beta.solana.com' },
  });

  const addr = irys.address;
  let bal = await irys.getLoadedBalance();
  console.log(`Wallet: ${addr}  Irys credit: ${bal}`);

  const files = collectFiles(SITE_DIR, '');
  bal = await ensureIrysBalance(irys, await estimateUploadPlan(irys, files));
  console.log(`Wallet: ${addr}  Deployable Irys credit: ${bal}`);

  const htmlFiles = [];
  const manifestEntries = {};
  let totalBytes = 0;

  const staticPathEntries = loadStaticPathEntries();
  Object.assign(manifestEntries, staticPathEntries);
  if (Object.keys(staticPathEntries).length) {
    console.log(`Static Arweave paths: ${Object.keys(staticPathEntries).length}`);
  }

  // Phase 1: upload non-HTML (assets, js, wasm, mp3)
  console.log('\nPhase 1: assets...');
  for (const f of files) {
    if (f.name.endsWith('.html')) { htmlFiles.push(f); continue; }
    const data = readFileSync(f.path);
    const hash = sha256(data);
    const cached = assetCache[f.name];
    if (cached && cached.hash === hash) {
      manifestEntries[f.name] = cached.tx;
      console.log(`  ${f.name} (unchanged, reuse ${cached.tx.slice(0, 8)}...)`);
      continue;
    }
    const remoteTx = await remoteCacheTxForFile(f.name, data);
    if (remoteTx) {
      manifestEntries[f.name] = remoteTx;
      assetCache[f.name] = { hash, tx: remoteTx };
      console.log(`  ${f.name} (remote unchanged, reuse ${remoteTx.slice(0, 8)}...)`);
      continue;
    }
    totalBytes += data.length;
    console.log(`  ${f.name} (${(data.length / 1024).toFixed(1)} KB)...`);
    const receipt = await irys.upload(data, {
      tags: [{ name: 'Content-Type', value: ct(f.name) }, { name: 'App-Name', value: 'Signal' }],
    });
    manifestEntries[f.name] = receipt.id;
    assetCache[f.name] = { hash, tx: receipt.id };
    console.log(`    -> ${receipt.id}`);
  }

  // Phase 2: upload HTML with rewritten asset URLs
  console.log('\nPhase 2: HTML...');
  const deployHash = Date.now().toString(36);
  for (const f of htmlFiles) {
    let content = readFileSync(f.path, 'utf-8');

    // Keep asset references on the Signal worker so it can apply gateway
    // fallback for recently uploaded Irys files.
    for (const [name, txId] of Object.entries(manifestEntries)) {
      if (!name.includes('/') && (name.endsWith('.js') || name.endsWith('.wasm') || name.endsWith('.css'))) {
        content = content.replaceAll(`./${name}`, `/${name}`);
        content = content.replaceAll(`"${name}"`, `"/${name}"`);
      }
      if (name.startsWith('music/')) {
        content = content.replaceAll(`"./music/${name.slice(6)}"`, `"/music/${name.slice(6)}"`);
      }
    }

    // Keep JS/WASM as a cache-busted pair. The locateFile assignment must
    // happen after the page creates Module, otherwise the Module object reset
    // wipes it out before Emscripten starts.
    const wasmUrl = `/signal.wasm?v=${deployHash}`;
    const jsUrl = `/signal.js?v=${deployHash}`;
    if (f.name === 'signal.html' || f.name === 'play.html') {
      const locateFileLine = `Module.locateFile = function(p) { return p === 'signal.wasm' ? '${wasmUrl}' : p; };`;
      content = content.replace('window.SignalGameModule = Module;', `window.SignalGameModule = Module;\n      ${locateFileLine}`);
      content = content.replace('window.SignalGameModule=Module</script>', `window.SignalGameModule=Module;${locateFileLine}</script>`);
      content = content.replaceAll("loadGame('/signal.js')", `loadGame('${jsUrl}')`);
      content = content.replaceAll('loadGame("/signal.js")', `loadGame("${jsUrl}")`);
      content = content.replaceAll('src="/signal.js"', `src="${jsUrl}"`);
      content = content.replaceAll('src=/signal.js', `src="${jsUrl}"`);
      content = content.replaceAll('src="signal.js"', `src="${jsUrl}"`);
      content = content.replaceAll('src=signal.js', `src="${jsUrl}"`);
    }
    writeFileSync(f.path, content);

    const data = Buffer.from(content);
    const hash = sha256(data);
    const cached = assetCache[f.name];
    if (cached && cached.hash === hash) {
      manifestEntries[f.name] = cached.tx;
      if (f.name.endsWith('.html')) manifestEntries[f.name.replace('.html', '')] = cached.tx;
      console.log(`  ${f.name} (unchanged, reuse ${cached.tx.slice(0, 8)}...)`);
      continue;
    }
    const remoteTx = await remoteCacheTxForFile(f.name, data);
    if (remoteTx) {
      manifestEntries[f.name] = remoteTx;
      if (f.name.endsWith('.html')) manifestEntries[f.name.replace('.html', '')] = remoteTx;
      assetCache[f.name] = { hash, tx: remoteTx };
      console.log(`  ${f.name} (remote unchanged, reuse ${remoteTx.slice(0, 8)}...)`);
      continue;
    }

    totalBytes += data.length;
    console.log(`  ${f.name} (${(data.length / 1024).toFixed(1)} KB)...`);
    const receipt = await irys.upload(data, {
      tags: [{ name: 'Content-Type', value: 'text/html' }, { name: 'App-Name', value: 'Signal' }],
    });
    manifestEntries[f.name] = receipt.id;
    if (f.name.endsWith('.html')) manifestEntries[f.name.replace('.html', '')] = receipt.id;
    assetCache[f.name] = { hash, tx: receipt.id };
    console.log(`    -> ${receipt.id}`);
  }

  // Persist cache
  writeFileSync('.arweave-cache.json', JSON.stringify(assetCache, null, 2));

  // Upload manifest JSON to Irys (settles to Arweave)
  console.log('\nPhase 3: manifest...');
  const manifest = { manifest: 'arweave/paths', version: '0.2.0', paths: manifestEntries };
  const manifestData = Buffer.from(JSON.stringify(manifest));
  const manifestReceipt = await irys.upload(manifestData, {
    tags: [
      { name: 'Content-Type', value: 'application/x.arweave-manifest+json' },
      { name: 'App-Name', value: 'Signal' },
    ],
  });
  console.log(`Manifest TX: ${manifestReceipt.id}`);
  writeFileSync('.arweave-manifest-tx', manifestReceipt.id);

  // Also write the paths map for KV-accelerated worker
  writeFileSync('.arweave-paths.json', JSON.stringify(manifestEntries, null, 2));

  console.log(`\nDeployed ${(totalBytes / 1024).toFixed(1)} KB (${Object.keys(assetCache).length} files cached)`);
  console.log(`Manifest URL: https://arweave.net/${manifestReceipt.id}`);
  console.log('Copy .arweave-paths.json into Cloudflare KV key "manifest" to update worker');
}

main().catch(e => { console.error(e); process.exit(1); });
