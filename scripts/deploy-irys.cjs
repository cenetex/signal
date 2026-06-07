const Irys = require('@irys/sdk');
const { readFileSync, writeFileSync } = require('fs');
const { Keypair } = require('@solana/web3.js');
const { homedir } = require('os');

async function main() {
  // Load Solana keypair: CI uses SOLANA_KEYPAIR env var, local uses ~/.config/solana/id.json
  let keyBytes;
  if (process.env.SOLANA_KEYPAIR) {
    keyBytes = Uint8Array.from(JSON.parse(process.env.SOLANA_KEYPAIR));
  } else {
    keyBytes = Uint8Array.from(JSON.parse(readFileSync(homedir() + '/.config/solana/id.json', 'utf-8')));
  }
  const keypair = Keypair.fromSecretKey(keyBytes);

  const irys = new Irys({
    network: 'devnet',
    token: 'solana',
    key: keypair.secretKey,
    config: { providerUrl: 'https://api.devnet.solana.com' },
  });

  const files = ['index.html', 'mine.html', 'play.html', 'signal.html', 'signal.js', 'signal.wasm', 'signal-touch-controls.js'];
  const txIds = {};

  for (const name of files) {
    const path = `_site/${name}`;
    let data;
    try { data = readFileSync(path); } catch { console.log(`  SKIP ${name} (not found)`); continue; }

    const price = await irys.getPrice(data.length);
    const bal = await irys.getLoadedBalance();
    if (bal < price) {
      const need = price - bal;
      console.log(`  Funding ${need} winston for ${name}...`);
      await irys.fund(need);
    }

    const tx = await irys.upload(data, {
      tags: [
        { name: 'Content-Type', value: name.endsWith('.html') ? 'text/html' :
          name.endsWith('.js') ? 'application/javascript' : name.endsWith('.wasm') ? 'application/wasm' : 'application/octet-stream' },
        { name: 'App-Name', value: 'Signal' },
        { name: 'Path', value: name },
      ],
    });
    txIds[name] = tx.id;
    console.log(`  ${name} → ${tx.id}`);
  }

  // Write primary tx ID for Cloudflare KV update
  const primary = txIds['index.html'] || txIds['mine.html'] || Object.values(txIds)[0];
  if (primary) {
    writeFileSync('.irys-manifest-tx', primary);
    console.log(`\nManifest tx: ${primary}`);
    console.log(`https://arweave.net/${primary}`);
  }
}
main().catch(e => { console.error(e.message); process.exit(1); });
