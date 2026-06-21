#!/usr/bin/env node
import { mkdtempSync, readFileSync, writeFileSync } from 'fs';
import { tmpdir } from 'os';
import { join } from 'path';
import { spawnSync } from 'child_process';

const ROOT = new URL('..', import.meta.url).pathname.replace(/\/$/, '');
const SCRIPT = join(ROOT, 'scripts/build-rati-anchor-batch.mjs');

function hex(ch) {
  return ch.repeat(64);
}

function receipt(overrides = {}) {
  const i = overrides.i || '1';
  const gradeVerified = overrides.gradeVerified !== false;
  return {
    version: 'rati_mining_receipt_v1',
    receipt_hash: hex(i),
    station_pubkey: hex('a'),
    station_pubkey_b58: 'Station111111111111111111111111111111111',
    world: { world_id: 1, world_seq: 2, build_id: 'test' },
    event: {
      kind: 'CHAIN_EVT_SMELT',
      event_id: Number(i),
      segment_id: 0,
      epoch: 100 + Number(i),
      event_hash: hex(i === '1' ? 'b' : 'c'),
      payload_hash: hex('d'),
      prev_hash: hex('0'),
      signature: 'e'.repeat(128),
    },
    mining: {
      fragment_pub: hex(i === '1' ? '2' : '3'),
      cargo_pub: hex(i === '1' ? '4' : '5'),
      parent_merkle: hex(i === '1' ? '2' : '3'),
      grade: gradeVerified ? 'RATi' : null,
      grade_verified: gradeVerified,
      grade_note: gradeVerified
        ? 'verified from CHAIN_EVT_CLAIM_FRAGMENT'
        : 'no CHAIN_EVT_CLAIM_FRAGMENT matched this smelt',
      prefix_class: 'RATi',
      prefix_class_id: 7,
      mined_tick: 200 + Number(i),
    },
    claim: gradeVerified
      ? {
          kind: 'CHAIN_EVT_CLAIM_FRAGMENT',
          event_id: 10 + Number(i),
          segment_id: 0,
          epoch: 90 + Number(i),
          event_hash: hex(i === '1' ? '6' : '7'),
          fracture_seed: hex('8'),
          claimant_pubkey: hex('9'),
          fracture_id: 1,
          burst_nonce: 2,
          burst_cap: 50,
          asteroid_slot: 3,
          callsign: 'RJRAJZ7',
          claimed_grade: 'RATi',
          computed_grade: 'RATi',
          computed_fragment_pub: hex(i === '1' ? '2' : '3'),
          fragment_verified: true,
          grade_verified: true,
        }
      : null,
    arweave: { segment_tx: null, checkpoint_tx: null, manifest_tx: null },
    bitcoin: { batch_root: null, anchor_txid: null, block_height: null, block_hash: null },
  };
}

function run(args, opts = {}) {
  return spawnSync(process.execPath, [SCRIPT, ...args], {
    cwd: ROOT,
    encoding: 'utf8',
    ...opts,
  });
}

function assert(cond, message) {
  if (!cond) {
    process.stderr.write(`test-rati-anchor-batch: ${message}\n`);
    process.exit(1);
  }
}

const dir = mkdtempSync(join(tmpdir(), 'signal-rati-anchor-'));
const a = join(dir, 'a.json');
const b = join(dir, 'b.json');
const aCopy = join(dir, 'a-copy.json');
const bCopy = join(dir, 'b-copy.json');
const bad = join(dir, 'bad.json');
const out1 = join(dir, 'batch1.json');
const out2 = join(dir, 'batch2.json');
const out3 = join(dir, 'batch3.json');

const receiptA = JSON.stringify({ schema: 'signal.rati_mining_receipts.v1', receipts: [receipt({ i: '1' })] });
const receiptB = JSON.stringify(receipt({ i: '2' }));
writeFileSync(a, receiptA);
writeFileSync(b, receiptB);
writeFileSync(aCopy, receiptA);
writeFileSync(bCopy, receiptB);
writeFileSync(bad, JSON.stringify({ schema: 'signal.rati_mining_receipts.v1', receipts: [receipt({ i: '3', gradeVerified: false })] }));

let r = run(['--created-at-unix=0', `--out=${out1}`, a, b]);
assert(r.status === 0, r.stderr || 'batch build failed');
const batch = JSON.parse(readFileSync(out1, 'utf8'));
assert(batch.schema === 'signal.rati_anchor_batch_file.v1', 'wrong schema');
assert(batch.batch.version === 'rati_anchor_batch_v1', 'wrong batch version');
assert(batch.batch.receipt_count === 2, 'wrong receipt count');
assert(/^[0-9a-f]{64}$/.test(batch.batch.receipt_merkle_root), 'bad merkle root');
assert(/^[0-9a-f]{64}$/.test(batch.canonical.bitcoin_anchor_root), 'bad anchor root');
assert(batch.canonical.op_return_payload_hex === `5349475201${batch.canonical.bitcoin_anchor_root}`, 'bad op_return payload');
assert(batch.batch.receipts[0].receipt_hash < batch.batch.receipts[1].receipt_hash, 'receipts not sorted');

r = run(['--created-at-unix=0', `--out=${out2}`, b, a]);
assert(r.status === 0, r.stderr || 'second batch build failed');
assert(readFileSync(out1, 'utf8') === readFileSync(out2, 'utf8'), 'batch output is not deterministic');

r = run(['--created-at-unix=0', `--out=${out3}`, aCopy, bCopy]);
assert(r.status === 0, r.stderr || 'path-copy batch build failed');
assert(readFileSync(out1, 'utf8') === readFileSync(out3, 'utf8'), 'batch output depends on input paths');

r = run(['--created-at-unix=0', bad]);
assert(r.status !== 0, 'unverified receipt should fail by default');

r = run(['--created-at-unix=0', '--allow-unverified', bad]);
assert(r.status === 0, r.stderr || 'allow-unverified should permit weak receipt');

process.stdout.write('test-rati-anchor-batch: ok\n');
