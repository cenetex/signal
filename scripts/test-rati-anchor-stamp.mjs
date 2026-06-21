#!/usr/bin/env node
import {
  chmodSync,
  mkdtempSync,
  readFileSync,
  writeFileSync,
} from 'fs';
import { tmpdir } from 'os';
import { join } from 'path';
import { spawnSync } from 'child_process';

const ROOT = new URL('..', import.meta.url).pathname.replace(/\/$/, '');
const BUILD_SCRIPT = join(ROOT, 'scripts/build-rati-anchor-batch.mjs');
const STAMP_SCRIPT = join(ROOT, 'scripts/stamp-rati-anchor.mjs');

function hex(ch) {
  return ch.repeat(64);
}

function receipt() {
  return {
    version: 'rati_mining_receipt_v1',
    receipt_hash: hex('1'),
    station_pubkey: hex('a'),
    station_pubkey_b58: 'Station111111111111111111111111111111111',
    world: { world_id: 1, world_seq: 2, build_id: 'test' },
    event: {
      kind: 'CHAIN_EVT_SMELT',
      event_id: 1,
      segment_id: 0,
      epoch: 101,
      event_hash: hex('b'),
      payload_hash: hex('d'),
      prev_hash: hex('0'),
      signature: 'e'.repeat(128),
    },
    mining: {
      fragment_pub: hex('2'),
      cargo_pub: hex('4'),
      parent_merkle: hex('2'),
      grade: 'RATi',
      grade_verified: true,
      grade_note: 'verified from CHAIN_EVT_CLAIM_FRAGMENT',
      prefix_class: 'RATi',
      prefix_class_id: 7,
      mined_tick: 201,
    },
    claim: {
      kind: 'CHAIN_EVT_CLAIM_FRAGMENT',
      event_id: 11,
      segment_id: 0,
      epoch: 91,
      event_hash: hex('6'),
      fracture_seed: hex('8'),
      claimant_pubkey: hex('9'),
      fracture_id: 1,
      burst_nonce: 2,
      burst_cap: 50,
      asteroid_slot: 3,
      callsign: 'RJRAJZ7',
      claimed_grade: 'RATi',
      computed_grade: 'RATi',
      computed_fragment_pub: hex('2'),
      fragment_verified: true,
      grade_verified: true,
    },
    arweave: { segment_tx: null, checkpoint_tx: null, manifest_tx: null },
    bitcoin: { batch_root: null, anchor_txid: null, block_height: null, block_hash: null },
  };
}

function run(script, args, opts = {}) {
  return spawnSync(process.execPath, [script, ...args], {
    cwd: ROOT,
    encoding: 'utf8',
    ...opts,
  });
}

function assert(cond, message) {
  if (!cond) {
    process.stderr.write(`test-rati-anchor-stamp: ${message}\n`);
    process.exit(1);
  }
}

const dir = mkdtempSync(join(tmpdir(), 'signal-rati-stamp-'));
const receiptPath = join(dir, 'receipt.json');
const batchPath = join(dir, 'rati-anchor-batch.json');
const dryManifestPath = join(dir, 'dry.manifest.json');
const proofPath = join(dir, 'rati-anchor-batch.ots');
const manifestPath = join(dir, 'rati-anchor-batch.manifest.json');
const badBatchPath = join(dir, 'bad-batch.json');
const fakeOtsPath = join(dir, 'fake-ots.mjs');

writeFileSync(receiptPath, JSON.stringify(receipt()));

let r = run(BUILD_SCRIPT, ['--created-at-unix=0', `--out=${batchPath}`, receiptPath]);
assert(r.status === 0, r.stderr || 'batch build failed');

r = run(STAMP_SCRIPT, ['--dry-run', '--created-at-unix=0', `--manifest-out=${dryManifestPath}`, batchPath]);
assert(r.status === 0, r.stderr || 'dry-run stamp failed');
const dryManifest = JSON.parse(readFileSync(dryManifestPath, 'utf8'));
assert(dryManifest.schema === 'signal.rati_anchor_stamp_manifest.v1', 'wrong manifest schema');
assert(dryManifest.opentimestamps.status === 'dry_run', 'dry run did not report dry_run');
assert(dryManifest.artifacts.ots_proof_file === null, 'dry run should not include proof artifact');
assert(dryManifest.batch.bitcoin_anchor_root.length === 64, 'missing anchor root');

writeFileSync(fakeOtsPath, `#!/usr/bin/env node
import { readFileSync, writeFileSync } from 'fs';
if (process.argv[2] !== 'stamp') process.exit(2);
const input = process.argv[3];
const bytes = readFileSync(input);
writeFileSync(\`\${input}.ots\`, Buffer.concat([Buffer.from('fake-ots-v1:'), bytes.subarray(0, 16)]));
`);
chmodSync(fakeOtsPath, 0o755);

r = run(STAMP_SCRIPT, [
  '--created-at-unix=0',
  `--ots-command=${fakeOtsPath}`,
  `--ots-out=${proofPath}`,
  `--manifest-out=${manifestPath}`,
  batchPath,
]);
assert(r.status === 0, r.stderr || 'fake OTS stamp failed');
const manifest = JSON.parse(readFileSync(manifestPath, 'utf8'));
assert(manifest.opentimestamps.status === 'stamped', 'stamp did not report stamped');
assert(manifest.artifacts.ots_proof_file.path === 'rati-anchor-batch.ots', 'wrong proof path');
assert(manifest.artifacts.ots_proof_file.bytes > 12, 'proof artifact looks empty');
assert(manifest.arweave_upload_plan.length === 3, 'wrong upload plan length');
assert(readFileSync(proofPath).toString('utf8').startsWith('fake-ots-v1:'), 'proof file missing fake signature');

const badBatch = JSON.parse(readFileSync(batchPath, 'utf8'));
badBatch.canonical.bitcoin_anchor_root = hex('0');
writeFileSync(badBatchPath, JSON.stringify(badBatch));
r = run(STAMP_SCRIPT, ['--dry-run', badBatchPath]);
assert(r.status !== 0, 'corrupt batch should fail validation');

process.stdout.write('test-rati-anchor-stamp: ok\n');
