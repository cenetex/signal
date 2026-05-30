import { readFileSync, writeFileSync, existsSync, readdirSync, statSync } from 'fs';
import { resolve, join } from 'path';
import Arweave from 'arweave';
import { createHash } from 'crypto';

const SITE_DIR = resolve('_site');
const HOST = process.env.ARWEAVE_HOST || 'arweave.net';
const PORT = parseInt(process.env.ARWEAVE_PORT || '443');
const PROTOCOL = process.env.ARWEAVE_PROTOCOL || 'https';

let wallet;
if (process.env.ARWEAVE_WALLET_JSON) {
  wallet = JSON.parse(process.env.ARWEAVE_WALLET_JSON);
} else {
  let wp = null;
  for (let i = 0; i < process.argv.length; i++) {
    if (process.argv[i] === '--wallet' && process.argv[i + 1]) wp = resolve(process.argv[i + 1]);
  }
  if (!wp && process.env.ARWEAVE_WALLET) wp = resolve(process.env.ARWEAVE_WALLET);
  if (!wp) wp = resolve('arweave-wallet.json');
  if (!existsSync(wp)) { console.error(`No wallet at: ${wp}`); process.exit(1); }
  wallet = JSON.parse(readFileSync(wp, 'utf-8'));
}

// Content-hash cache to avoid re-uploading unchanged files
let assetCache = {};
try { assetCache = JSON.parse(readFileSync('.arweave-cache.json', 'utf-8')); } catch (_) {}

const arweave = Arweave.init({ host: HOST, port: PORT, protocol: PROTOCOL });

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

function ct(f) { const t={html:'text/html',js:'application/javascript',wasm:'application/wasm',css:'text/css'}; return t[f.split('.').pop().toLowerCase()]||'application/octet-stream'; }

async function upload(data, tags) {
  const tx = await arweave.createTransaction({ data }, wallet);
  for (const [n,v] of Object.entries(tags)) tx.addTag(n, v);
  await arweave.transactions.sign(tx, wallet);
  const up = await arweave.transactions.getUploader(tx);
  while (!up.isComplete) await up.uploadChunk();
  return tx.id;
}

function rawUrl(name, assets) { return `https://arweave.net/raw/${assets[name]}`; }

async function main() {
  const addr = await arweave.wallets.jwkToAddress(wallet);
  const bal = arweave.ar.winstonToAr(await arweave.wallets.getBalance(addr));
  console.log(`Wallet: ${addr}  Balance: ${bal} AR`);
  if (parseFloat(bal) < 0.0001) { console.error('Balance too low.'); process.exit(1); }

  const files = collectFiles(SITE_DIR, '');
  const assets = {};
  const htmls = [];
  let total = 0;

  // Phase 1: non-HTML
  console.log('Phase 1: assets...');
  for (const f of files) {
    if (f.name.endsWith('.html')) { htmls.push(f); continue; }
    const d = readFileSync(f.path);
    const hash = sha256(d);
    const cached = assetCache[f.name];
    if (cached && cached.hash === hash) {
      assets[f.name] = cached.tx;
      console.log(`  ${f.name} (unchanged, reuse ${cached.tx})`);
      continue;
    }
    total += d.length;
    console.log(`  ${f.name} (${(d.length/1024).toFixed(1)} KB)...`);
    const tx = await upload(d, {'Content-Type':ct(f.name),'App-Name':'Signal'});
    assets[f.name] = tx;
    assetCache[f.name] = { hash, tx };
    console.log(`    -> ${assets[f.name]}`);
  }

  // Phase 2: HTML with rewritten URLs
  console.log('\nPhase 2: HTML...');
  const manifest = {};
  htmls.sort((a,b) => ["signal.html","play.html","index.html"].indexOf(a.name) - ["signal.html","play.html","index.html"].indexOf(b.name));
  for (const f of htmls) {
    let c = readFileSync(f.path, 'utf-8');

    // Replace asset paths with absolute Arweave raw URLs
    if (f.name === 'signal.html') {
      c = c.replace(/src=["'](\.\/)?signal-touch-controls\.js["']/, `src="${rawUrl('signal-touch-controls.js', assets)}"`);
      c = c.replace(/src=["']signal\.js["']/, `src="${rawUrl('signal.js', assets)}"`);
    }
    if (f.name === 'play.html') {
      c = c.replace(/src=["'](\.\/)?signal-touch-controls\.js["']/, `src="${rawUrl('signal-touch-controls.js', assets)}"`);
    }
    if (f.name === 'index.html') {
      c = c.replace(/href=["']\/play["']/g, `href="${rawUrl('play.html', assets)}"`);
      c = c.replace(/href=["']\/ost["']/g, 'href="https://signal.ratimics.com/ost"');
    }

    // Inject wasm locateFile into signal.html and play.html
    const wasmUrl = rawUrl('signal.wasm', assets);
    const wasmLines =
`<script>
if(!Module)var Module={};
Module.locateFile=function(p){if(p==='signal.wasm')return'${wasmUrl}';return p};
</script>`;

    if (f.name === 'signal.html') {
      c = c.replace('<script>var Module=', wasmLines + '\n<script>var Module=');
    }
    if (f.name === 'play.html') {
      c = c.replace(/<script>\s*var canvas/, wasmLines + '\n<script>var canvas');
    }

    const d = Buffer.from(c);
    const hash = sha256(d);
    const cached = assetCache[f.name];
    if (cached && cached.hash === hash) {
      assets[f.name] = cached.tx;
      manifest[f.name] = cached.tx;
      console.log(`  ${f.name} (unchanged, reuse ${cached.tx})`);
    } else {
      total += d.length;
      console.log(`  ${f.name} (${(d.length/1024).toFixed(1)} KB)...`);
      const tx = await upload(d, {'Content-Type':'text/html','App-Name':'Signal'});
      assets[f.name] = tx;
      manifest[f.name] = tx;
      assetCache[f.name] = { hash, tx };
    }
    if (f.name === 'index.html') manifest['/'] = assets[f.name];
    console.log(`    -> ${assets[f.name]}`);
  }

  // Phase 3: manifest
  console.log('\nPhase 3: manifest...');
  for (const [n, id] of Object.entries(assets)) { if (!manifest[n]) manifest[n] = id; }
  const mtx = await upload(
    Buffer.from(JSON.stringify({manifest:'arweave/paths',version:'0.2.0',paths:manifest})),
    {'Content-Type':'application/x.arweave-manifest+json','App-Name':'Signal'}
  );
  // Persist content-hash cache
  writeFileSync('.arweave-cache.json', JSON.stringify(assetCache, null, 2));
  console.log(`\nDeployed ${(total/1024).toFixed(1)} KB (${Object.keys(assetCache).length} files cached)`);
  console.log(`URL: https://arweave.net/${mtx}`);
  writeFileSync('.arweave-manifest-tx', mtx);
}

main().catch(e => { console.error(e); process.exit(1); });
