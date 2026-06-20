import { readFileSync, writeFileSync, existsSync, readdirSync, statSync } from 'fs';
import { resolve, join } from 'path';
import { createHash } from 'crypto';
import Irys from '@irys/sdk';
import { Keypair } from '@solana/web3.js';
import { homedir } from 'os';

const SITE_DIR = resolve('_site');
const HOST = process.env.ARWEAVE_HOST || 'arweave.net';
const PORT = parseInt(process.env.ARWEAVE_PORT || '443');
const PROTOCOL = process.env.ARWEAVE_PROTOCOL || 'https';

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
  const t = { html: 'text/html', js: 'application/javascript', wasm: 'application/wasm', css: 'text/css', mp3: 'audio/mpeg' };
  return t[f.split('.').pop().toLowerCase()] || 'application/octet-stream';
}

async function main() {
  const irys = new Irys({
    network: 'mainnet',
    token: 'solana',
    key: keypair.secretKey,
    config: { providerUrl: 'https://api.mainnet-beta.solana.com' },
  });

  const addr = irys.address;
  const bal = await irys.getLoadedBalance();
  console.log(`Wallet: ${addr}  Irys credit: ${bal}`);

  const files = collectFiles(SITE_DIR, '');
  const htmlFiles = [];
  const manifestEntries = {};
  let totalBytes = 0;

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

    // Inject wasm locateFile
    const wasmUrl = `/signal.wasm?v=${deployHash}`;
    if (f.name === 'signal.html' || f.name === 'play.html') {
      const moduleInject = `<script>\nif (!Module) var Module = {};\nModule.locateFile = function(p) { return p === 'signal.wasm' ? '${wasmUrl}' : p; };\n</script>\n`;
      content = content.replace('<script>', moduleInject + '<script>');
    }

    const data = Buffer.from(content);
    const hash = sha256(data);
    const cached = assetCache[f.name];
    if (cached && cached.hash === hash) {
      manifestEntries[f.name] = cached.tx;
      if (f.name.endsWith('.html')) manifestEntries[f.name.replace('.html', '')] = cached.tx;
      console.log(`  ${f.name} (unchanged, reuse ${cached.tx.slice(0, 8)}...)`);
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
