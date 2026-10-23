import { createHmac, createPrivateKey, createPublicKey, timingSafeEqual } from 'node:crypto';
import { appendFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

function publicKeyForSeed(seed) {
  const key = createPrivateKey({
    key: Buffer.concat([Buffer.from('302e020100300506032b657004220420', 'hex'), seed]),
    format: 'der', type: 'pkcs8',
  });
  return createPublicKey(key).export({ format: 'der', type: 'spki' }).subarray(-32);
}

export function validPlayCheckIdentity(value) {
  if (typeof value !== 'string') return false;
  const bytes = Buffer.from(value, 'base64');
  return bytes.length === 64 && bytes.toString('base64') === value &&
    timingSafeEqual(bytes.subarray(32), publicKeyForSeed(bytes.subarray(0, 32)));
}

// Separate app/device accounts stay stable across releases. Only the derived
// Ed25519 identity enters the browser; the station key stays in this process.
export function playCheckIdentity(secret, app, device) {
  if (!secret || !app || !['desktop', 'touch'].includes(device)) {
    throw new Error('Play checks require an authority secret, app and device.');
  }
  const seed = createHmac('sha256', secret)
    .update(JSON.stringify(['signal.release-play.v1', app, device])).digest();
  const publicKey = publicKeyForSeed(seed);
  return Buffer.concat([seed, publicKey]).toString('base64');
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  if (!process.env.GITHUB_ENV) throw new Error('GITHUB_ENV is required.');
  for (const device of ['desktop', 'touch']) {
    const identity = playCheckIdentity(process.env.SIGNAL_STATION_AUTH_SECRET, process.env.FLY_APP, device);
    console.log(`::add-mask::${identity}`);
    appendFileSync(process.env.GITHUB_ENV, `SIGNAL_PLAY_IDENTITY_${device.toUpperCase()}=${identity}\n`);
  }
}
