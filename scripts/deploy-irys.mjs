const Irys = require('@irys/sdk');
const { readFileSync } = require('fs');
const { Keypair } = require('@solana/web3.js');
const { homedir } = require('os');

async function main() {
  const keyBytes = Uint8Array.from(JSON.parse(readFileSync(homedir() + '/.config/solana/id.json', 'utf-8')));
  const keypair = Keypair.fromSecretKey(keyBytes);

  const irys = new Irys({
    network: 'devnet',
    token: 'solana',
    key: keypair.secretKey,
    config: { providerUrl: 'https://api.devnet.solana.com' },
  });

  const files = ['mine.html'];
  for (const name of files) {
    const path = `_site/${name}`;
    let data;
    try { data = readFileSync(path); } catch { console.log(`  SKIP ${name} (not found)`); continue; }

    const price = await irys.getPrice(data.length);
    const bal = await irys.getLoadedBalance();
    if (bal < price) {
      console.log(`  Funding ${price - bal} winston...`);
      await irys.fund(price - bal);
    }

    const tx = await irys.upload(data, {
      tags: [
        { name: 'Content-Type', value: 'text/html' },
        { name: 'App-Name', value: 'Signal' },
        { name: 'Path', value: name },
      ],
    });
    console.log(`${name} → ${tx.id}`);
    console.log(`https://arweave.net/${tx.id}`);
  }
}
main().catch(e => { console.error(e.message); process.exit(1); });
