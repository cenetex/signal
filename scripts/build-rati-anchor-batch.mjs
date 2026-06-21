#!/usr/bin/env node
import { createHash } from 'crypto';
import { dirname, resolve } from 'path';
import { mkdirSync, readFileSync, writeFileSync } from 'fs';

const ZERO32 = '0'.repeat(64);
const LEAF_DOMAIN = Buffer.from('SIGNAL:RATI:ANCHOR-LEAF:v1', 'utf8');
const NODE_DOMAIN = Buffer.from('SIGNAL:RATI:ANCHOR-NODE:v1', 'utf8');
const ANCHOR_DOMAIN = Buffer.from('SIGNAL:RATI:BTC-ANCHOR:v1', 'utf8');
const OP_RETURN_PREFIX_HEX = '5349475201'; // "SIGR" + v1

function usage(out = process.stdout) {
  out.write(`usage: node scripts/build-rati-anchor-batch.mjs [options] <receipt-json>...\n\n`);
  out.write(`Options:\n`);
  out.write(`  --out=<path>                       Write batch file instead of stdout\n`);
  out.write(`  --previous-batch-root=<hex32>      Previous batch root (default: zero)\n`);
  out.write(`  --settlement-checkpoint-root=<hex32|null>\n`);
  out.write(`  --arweave-manifest-tx=<txid|null>\n`);
  out.write(`  --created-at-unix=<n|now>          Default: 0 for reproducible roots\n`);
  out.write(`  --allow-unverified                 Include receipts without grade_verified\n`);
  out.write(`  -h, --help                         This message\n`);
}

function fail(message, code = 2) {
  process.stderr.write(`build-rati-anchor-batch: ${message}\n`);
  process.exit(code);
}

function parseArgs(argv) {
  const opts = {
    out: null,
    previousBatchRoot: ZERO32,
    settlementCheckpointRoot: null,
    arweaveManifestTx: null,
    createdAtUnix: 0,
    allowUnverified: false,
    inputs: [],
  };

  for (const arg of argv) {
    if (arg === '-h' || arg === '--help') {
      usage();
      process.exit(0);
    } else if (arg.startsWith('--out=')) {
      opts.out = arg.slice('--out='.length);
    } else if (arg.startsWith('--previous-batch-root=')) {
      opts.previousBatchRoot = arg.slice('--previous-batch-root='.length);
    } else if (arg.startsWith('--settlement-checkpoint-root=')) {
      const v = arg.slice('--settlement-checkpoint-root='.length);
      opts.settlementCheckpointRoot = v === 'null' || v === '' ? null : v;
    } else if (arg.startsWith('--arweave-manifest-tx=')) {
      const v = arg.slice('--arweave-manifest-tx='.length);
      opts.arweaveManifestTx = v === 'null' || v === '' ? null : v;
    } else if (arg.startsWith('--created-at-unix=')) {
      const v = arg.slice('--created-at-unix='.length);
      opts.createdAtUnix = v === 'now' ? Math.floor(Date.now() / 1000) : parseInteger(v, 'created-at-unix');
    } else if (arg === '--allow-unverified') {
      opts.allowUnverified = true;
    } else if (arg.startsWith('-')) {
      fail(`unknown option ${arg}`);
    } else {
      opts.inputs.push(arg);
    }
  }

  if (opts.inputs.length === 0) fail('at least one receipt JSON file is required');
  assertHex32(opts.previousBatchRoot, 'previous-batch-root');
  if (opts.settlementCheckpointRoot !== null)
    assertHex32(opts.settlementCheckpointRoot, 'settlement-checkpoint-root');
  return opts;
}

function parseInteger(text, name) {
  if (!/^(0|[1-9][0-9]*)$/.test(text)) fail(`invalid --${name}`);
  const n = Number(text);
  if (!Number.isSafeInteger(n)) fail(`--${name} exceeds safe integer range`);
  return n;
}

function assertHex32(text, name) {
  if (!/^[0-9a-fA-F]{64}$/.test(text)) fail(`invalid ${name}: expected 32-byte hex`);
}

function sha256Hex(data) {
  return createHash('sha256').update(data).digest('hex');
}

function sha256Buf(parts) {
  const h = createHash('sha256');
  for (const part of parts) h.update(part);
  return h.digest();
}

function hexToBuf(hex, name) {
  assertHex32(hex, name);
  return Buffer.from(hex, 'hex');
}

function canonical(value) {
  if (value === null) return 'null';
  if (typeof value === 'boolean') return value ? 'true' : 'false';
  if (typeof value === 'string') return JSON.stringify(value);
  if (typeof value === 'number') {
    if (!Number.isFinite(value) || !Number.isInteger(value))
      throw new Error('canonical JSON only supports finite integers');
    return String(value);
  }
  if (Array.isArray(value)) return `[${value.map(canonical).join(',')}]`;
  if (typeof value === 'object') {
    const keys = Object.keys(value).sort();
    return `{${keys.map((k) => `${JSON.stringify(k)}:${canonical(value[k])}`).join(',')}}`;
  }
  throw new Error(`unsupported canonical JSON value: ${typeof value}`);
}

function readJson(path) {
  try {
    return JSON.parse(readFileSync(path, 'utf8'));
  } catch (err) {
    fail(`cannot read JSON ${path}: ${err.message}`);
  }
}

function receiptsFromFile(path) {
  const doc = readJson(path);
  if (doc && doc.version === 'rati_mining_receipt_v1') {
    return [{ receipt: doc, sourcePath: path, sourceIndex: 0 }];
  }
  if (doc && doc.schema === 'signal.rati_mining_receipts.v1' && Array.isArray(doc.receipts)) {
    return doc.receipts.map((receipt, sourceIndex) => ({ receipt, sourcePath: path, sourceIndex }));
  }
  fail(`${path} is not a rati_mining_receipt_v1 or signal.rati_mining_receipts.v1 file`);
}

function requireString(obj, path) {
  const parts = path.split('.');
  let cur = obj;
  for (const part of parts) cur = cur && cur[part];
  if (typeof cur !== 'string') fail(`receipt missing string ${path}`);
  return cur;
}

function requireInteger(obj, path) {
  const parts = path.split('.');
  let cur = obj;
  for (const part of parts) cur = cur && cur[part];
  if (!Number.isSafeInteger(cur)) fail(`receipt missing integer ${path}`);
  return cur;
}

function normalizeReceipt(entry, opts) {
  const r = entry.receipt;
  if (!r || r.version !== 'rati_mining_receipt_v1') {
    fail(`${entry.sourcePath}#${entry.sourceIndex} is not a rati_mining_receipt_v1`);
  }

  const receiptHash = requireString(r, 'receipt_hash').toLowerCase();
  const stationPubkey = requireString(r, 'station_pubkey').toLowerCase();
  const eventHash = requireString(r, 'event.event_hash').toLowerCase();
  const cargoPub = requireString(r, 'mining.cargo_pub').toLowerCase();
  const fragmentPub = requireString(r, 'mining.fragment_pub').toLowerCase();
  const prefixClass = requireString(r, 'mining.prefix_class');
  const grade = r.mining && typeof r.mining.grade === 'string' ? r.mining.grade : null;
  const gradeVerified = !!(r.mining && r.mining.grade_verified);
  const claimEventHash = r.claim && typeof r.claim.event_hash === 'string'
    ? r.claim.event_hash.toLowerCase()
    : null;
  const stationB58 = typeof r.station_pubkey_b58 === 'string' ? r.station_pubkey_b58 : null;
  const eventId = requireInteger(r, 'event.event_id');
  const segmentId = requireInteger(r, 'event.segment_id');
  const epoch = requireInteger(r, 'event.epoch');
  const minedTick = requireInteger(r, 'mining.mined_tick');

  assertHex32(receiptHash, 'receipt_hash');
  assertHex32(stationPubkey, 'station_pubkey');
  assertHex32(eventHash, 'event.event_hash');
  assertHex32(cargoPub, 'mining.cargo_pub');
  assertHex32(fragmentPub, 'mining.fragment_pub');
  if (claimEventHash !== null) assertHex32(claimEventHash, 'claim.event_hash');
  if (!opts.allowUnverified && !gradeVerified) {
    fail(`${entry.sourcePath}#${entry.sourceIndex} receipt ${receiptHash} is not grade_verified`);
  }

  const leaf = {
    version: 'rati_anchor_leaf_v1',
    receipt_hash: receiptHash,
    station_pubkey: stationPubkey,
    station_pubkey_b58: stationB58,
    event_hash: eventHash,
    claim_event_hash: claimEventHash,
    cargo_pub: cargoPub,
    fragment_pub: fragmentPub,
    grade,
    grade_verified: gradeVerified,
    prefix_class: prefixClass,
    event_id: eventId,
    segment_id: segmentId,
    epoch,
    mined_tick: minedTick,
  };

  return {
    ...leaf,
    leaf_hash: sha256Hex(Buffer.concat([LEAF_DOMAIN, Buffer.from(canonical(leaf), 'utf8')])),
  };
}

function merkleRootHex(leafHashes) {
  if (leafHashes.length === 0) fail('cannot build a batch with zero receipts');
  let level = leafHashes.map((h) => hexToBuf(h, 'leaf_hash'));
  while (level.length > 1) {
    const next = [];
    for (let i = 0; i < level.length; i += 2) {
      const left = level[i];
      const right = level[i + 1] || left;
      next.push(sha256Buf([NODE_DOMAIN, left, right]));
    }
    level = next;
  }
  return level[0].toString('hex');
}

function uniqueOrNull(values) {
  const unique = [...new Set(values.filter((v) => v !== null && v !== undefined))];
  return unique.length === 1 ? unique[0] : null;
}

function buildBatch(opts) {
  const entries = opts.inputs.flatMap(receiptsFromFile);
  const leaves = entries.map((entry) => normalizeReceipt(entry, opts))
    .sort((a, b) => a.receipt_hash.localeCompare(b.receipt_hash));

  const seen = new Set();
  for (const leaf of leaves) {
    if (seen.has(leaf.receipt_hash)) fail(`duplicate receipt_hash ${leaf.receipt_hash}`);
    seen.add(leaf.receipt_hash);
  }

  const epochs = leaves.map((l) => l.epoch);
  const receiptMerkleRoot = merkleRootHex(leaves.map((l) => l.leaf_hash));
  const stationPubkey = uniqueOrNull(leaves.map((l) => l.station_pubkey));
  const stationB58 = uniqueOrNull(leaves.map((l) => l.station_pubkey_b58));

  const batch = {
    version: 'rati_anchor_batch_v1',
    station_pubkey: stationPubkey || 'multi',
    station_pubkey_b58: stationPubkey ? stationB58 : null,
    epoch_start_tick: Math.min(...epochs),
    epoch_end_tick: Math.max(...epochs),
    receipt_count: leaves.length,
    receipt_merkle_root: receiptMerkleRoot,
    settlement_checkpoint_root: opts.settlementCheckpointRoot,
    arweave_manifest_tx: opts.arweaveManifestTx,
    previous_batch_root: opts.previousBatchRoot.toLowerCase(),
    created_at_unix: opts.createdAtUnix,
    receipts: leaves,
  };

  const batchCanonical = canonical(batch);
  const batchCanonicalSha256 = sha256Hex(Buffer.from(batchCanonical, 'utf8'));
  const bitcoinAnchorRoot = sha256Hex(Buffer.concat([
    ANCHOR_DOMAIN,
    Buffer.from(batchCanonical, 'utf8'),
  ]));

  return {
    schema: 'signal.rati_anchor_batch_file.v1',
    batch,
    canonical: {
      json: 'sorted-key-json-no-whitespace',
      batch_sha256: batchCanonicalSha256,
      bitcoin_anchor_domain: ANCHOR_DOMAIN.toString('utf8'),
      bitcoin_anchor_root: bitcoinAnchorRoot,
      op_return_prefix_hex: OP_RETURN_PREFIX_HEX,
      op_return_payload_hex: `${OP_RETURN_PREFIX_HEX}${bitcoinAnchorRoot}`,
    },
    ots: {
      status: 'ready',
      target: 'this file or its bitcoin_anchor_root',
      command: 'ots stamp <rati-anchor-batch.json>',
    },
  };
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  const output = `${JSON.stringify(buildBatch(opts), null, 2)}\n`;
  if (opts.out) {
    const outPath = resolve(opts.out);
    mkdirSync(dirname(outPath), { recursive: true });
    writeFileSync(outPath, output);
  } else {
    process.stdout.write(output);
  }
}

main();
