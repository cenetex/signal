// Deploy the Signal website to Arweave.
//
// Wallet resolution order:
//   1. ARWEAVE_WALLET_JSON env var (raw JSON keyfile, for CI secrets)
//   2. --wallet <path> CLI flag
//   3. ARWEAVE_WALLET env var (path to keyfile)
//   4. ./arweave-wallet.json

import { readFileSync, existsSync, readdirSync, statSync, writeFileSync, unlinkSync } from 'fs';
import { resolve, join } from 'path';
import { tmpdir } from 'os';
import Arweave from 'arweave';

const SITE_DIR = resolve('_site');
const HOST = process.env.ARWEAVE_HOST || 'arweave.net';
const PORT = parseInt(process.env.ARWEAVE_PORT || '443');
const PROTOCOL = process.env.ARWEAVE_PROTOCOL || 'https';

let wallet;

// 1. Raw JSON from env (CI)
if (process.env.ARWEAVE_WALLET_JSON) {
  wallet = JSON.parse(process.env.ARWEAVE_WALLET_JSON);
} else {
  // 2-4. File-based
  let walletPath = null;
  for (let i = 0; i < process.argv.length; i++) {
    if (process.argv[i] === '--wallet' && process.argv[i + 1]) {
      walletPath = resolve(process.argv[i + 1]);
    }
  }
  if (!walletPath && process.env.ARWEAVE_WALLET) walletPath = resolve(process.env.ARWEAVE_WALLET);
  if (!walletPath) walletPath = resolve('arweave-wallet.json');

  if (!existsSync(walletPath)) {
    console.error(`No wallet at: ${walletPath}`);
    console.error('Set ARWEAVE_WALLET_JSON (CI), ARWEAVE_WALLET, or use --wallet');
    process.exit(1);
  }
  wallet = JSON.parse(readFileSync(walletPath, 'utf-8'));
}

const arweave = Arweave.init({ host: HOST, port: PORT, protocol: PROTOCOL });

function collectFiles(dir, base) {
  const files = [];
  for (const entry of readdirSync(dir)) {
    const full = join(dir, entry);
    const rel = base ? base + '/' + entry : entry;
    if (statSync(full).isDirectory()) {
      files.push(...collectFiles(full, rel));
    } else {
      files.push({ path: full, name: rel });
    }
  }
  return files;
}

function contentType(f) {
  const t = { html: 'text/html', js: 'application/javascript', wasm: 'application/wasm', css: 'text/css', svg: 'image/svg+xml', png: 'image/png', jpg: 'image/jpeg', json: 'application/json' };
  return t[f.split('.').pop().toLowerCase()] || 'application/octet-stream';
}

async function main() {
  const addr = await arweave.wallets.jwkToAddress(wallet);
  const bal = arweave.ar.winstonToAr(await arweave.wallets.getBalance(addr));
  console.log(`Wallet: ${addr}  Balance: ${bal} AR`);

  if (parseFloat(bal) < 0.0001) {
    console.error('Balance too low. Fund this wallet first.');
    process.exit(1);
  }

  const files = collectFiles(SITE_DIR, '');
  console.log(`Uploading ${files.length} files...`);
  const manifest = {};
  let total = 0;

  for (const f of files) {
    const data = readFileSync(f.path);
    total += data.length;
    console.log(`  ${f.name} (${(data.length / 1024).toFixed(1)} KB)...`);
    const tx = await arweave.createTransaction({ data }, wallet);
    tx.addTag('Content-Type', contentType(f.name));
    tx.addTag('App-Name', 'Signal');
    await arweave.transactions.sign(tx, wallet);
    const up = await arweave.transactions.getUploader(tx);
    while (!up.isComplete) await up.uploadChunk();
    manifest[f.name] = tx.id;
    console.log(`    -> ${tx.id}`);
  }

  const mtx = await arweave.createTransaction({
    data: Buffer.from(JSON.stringify({ manifest: 'arweave/paths', version: '0.2.0', paths: manifest })),
  }, wallet);
  mtx.addTag('Content-Type', 'application/x.arweave-manifest+json');
  mtx.addTag('App-Name', 'Signal');
  await arweave.transactions.sign(mtx, wallet);
  const mup = await arweave.transactions.getUploader(mtx);
  while (!mup.isComplete) await mup.uploadChunk();

  console.log(`\nDeployed ${(total / 1024).toFixed(1)} KB`);
  console.log(`URL: https://arweave.net/${mtx.id}`);
  console.log(`::notice title=Arweave Deploy::https://arweave.net/${mtx.id}`);
}

main().catch(e => { console.error(e); process.exit(1); });
