#!/usr/bin/env node
import { createHash } from 'crypto';
import {
  basename,
  dirname,
  extname,
  resolve,
} from 'path';
import {
  existsSync,
  mkdirSync,
  readFileSync,
  renameSync,
  statSync,
  unlinkSync,
  writeFileSync,
} from 'fs';
import { spawnSync } from 'child_process';

const ANCHOR_DOMAIN = Buffer.from('SIGNAL:RATI:BTC-ANCHOR:v1', 'utf8');
const DEFAULT_OTS_COMMAND = process.env.SIGNAL_RATI_OTS_CMD || 'ots';

function usage(out = process.stdout) {
  out.write(`usage: node scripts/stamp-rati-anchor.mjs [options] <rati-anchor-batch.json>\n\n`);
  out.write(`Options:\n`);
  out.write(`  --ots-out=<path>             Write proof to this path (default: batch .json -> .ots)\n`);
  out.write(`  --manifest-out=<path>        Write stamp manifest here (default: batch .manifest.json)\n`);
  out.write(`  --ots-command=<path>         OpenTimestamps CLI executable (default: ots)\n`);
  out.write(`  --created-at-unix=<n|now>    Default: now\n`);
  out.write(`  --dry-run                    Validate batch and write manifest without stamping\n`);
  out.write(`  --overwrite                  Replace an existing proof/manifest file\n`);
  out.write(`  -h, --help                   This message\n`);
}

function fail(message, code = 2) {
  process.stderr.write(`stamp-rati-anchor: ${message}\n`);
  process.exit(code);
}

function parseArgs(argv) {
  const opts = {
    batchPath: null,
    proofPath: null,
    manifestPath: null,
    otsCommand: DEFAULT_OTS_COMMAND,
    createdAtUnix: Math.floor(Date.now() / 1000),
    dryRun: false,
    overwrite: false,
  };

  for (const arg of argv) {
    if (arg === '-h' || arg === '--help') {
      usage();
      process.exit(0);
    } else if (arg.startsWith('--ots-out=')) {
      opts.proofPath = arg.slice('--ots-out='.length);
    } else if (arg.startsWith('--manifest-out=')) {
      opts.manifestPath = arg.slice('--manifest-out='.length);
    } else if (arg.startsWith('--ots-command=')) {
      opts.otsCommand = arg.slice('--ots-command='.length);
    } else if (arg.startsWith('--created-at-unix=')) {
      const v = arg.slice('--created-at-unix='.length);
      opts.createdAtUnix = v === 'now' ? Math.floor(Date.now() / 1000) : parseInteger(v, 'created-at-unix');
    } else if (arg === '--dry-run') {
      opts.dryRun = true;
    } else if (arg === '--overwrite') {
      opts.overwrite = true;
    } else if (arg.startsWith('-')) {
      fail(`unknown option ${arg}`);
    } else if (opts.batchPath) {
      fail('only one batch file may be stamped at a time');
    } else {
      opts.batchPath = arg;
    }
  }

  if (!opts.batchPath) fail('a rati-anchor-batch JSON file is required');
  opts.batchPath = resolve(opts.batchPath);
  opts.proofPath = resolve(opts.proofPath || defaultProofPath(opts.batchPath));
  opts.manifestPath = resolve(opts.manifestPath || defaultManifestPath(opts.batchPath));
  return opts;
}

function parseInteger(text, name) {
  if (!/^(0|[1-9][0-9]*)$/.test(text)) fail(`invalid --${name}`);
  const n = Number(text);
  if (!Number.isSafeInteger(n)) fail(`--${name} exceeds safe integer range`);
  return n;
}

function defaultProofPath(batchPath) {
  return extname(batchPath) === '.json'
    ? `${batchPath.slice(0, -'.json'.length)}.ots`
    : `${batchPath}.ots`;
}

function defaultManifestPath(batchPath) {
  return extname(batchPath) === '.json'
    ? `${batchPath.slice(0, -'.json'.length)}.manifest.json`
    : `${batchPath}.manifest.json`;
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

function sha256Hex(data) {
  return createHash('sha256').update(data).digest('hex');
}

function assertHex32(text, name) {
  if (!/^[0-9a-f]{64}$/.test(text)) fail(`invalid ${name}: expected lowercase 32-byte hex`);
}

function readBatch(batchPath) {
  let bytes;
  let doc;
  try {
    bytes = readFileSync(batchPath);
    doc = JSON.parse(bytes.toString('utf8'));
  } catch (err) {
    fail(`cannot read batch JSON ${batchPath}: ${err.message}`);
  }

  if (!doc || doc.schema !== 'signal.rati_anchor_batch_file.v1') {
    fail(`${batchPath} is not a signal.rati_anchor_batch_file.v1 file`);
  }
  if (!doc.batch || doc.batch.version !== 'rati_anchor_batch_v1') {
    fail(`${batchPath} does not contain rati_anchor_batch_v1`);
  }
  if (!doc.canonical || typeof doc.canonical !== 'object') {
    fail(`${batchPath} is missing canonical anchor metadata`);
  }

  const batchCanonical = canonical(doc.batch);
  const batchSha256 = sha256Hex(Buffer.from(batchCanonical, 'utf8'));
  const anchorRoot = sha256Hex(Buffer.concat([
    ANCHOR_DOMAIN,
    Buffer.from(batchCanonical, 'utf8'),
  ]));

  if (doc.canonical.batch_sha256 !== batchSha256) {
    fail(`${batchPath} canonical.batch_sha256 does not match the batch body`);
  }
  if (doc.canonical.bitcoin_anchor_root !== anchorRoot) {
    fail(`${batchPath} canonical.bitcoin_anchor_root does not match the batch body`);
  }
  assertHex32(doc.batch.receipt_merkle_root, 'batch.receipt_merkle_root');
  assertHex32(doc.canonical.bitcoin_anchor_root, 'canonical.bitcoin_anchor_root');
  if (doc.canonical.op_return_payload_hex !== `5349475201${anchorRoot}`) {
    fail(`${batchPath} canonical.op_return_payload_hex does not match the anchor root`);
  }

  return {
    doc,
    fileBytes: bytes,
    fileSha256: sha256Hex(bytes),
    batchSha256,
    anchorRoot,
  };
}

function artifact(path, role) {
  const bytes = readFileSync(path);
  const st = statSync(path);
  return {
    role,
    path: basename(path),
    bytes: st.size,
    sha256: sha256Hex(bytes),
  };
}

function removeIfExists(path) {
  if (existsSync(path)) unlinkSync(path);
}

function prepareOutput(path, overwrite, label) {
  mkdirSync(dirname(path), { recursive: true });
  if (existsSync(path)) {
    if (!overwrite) fail(`${label} already exists: ${path}`);
    removeIfExists(path);
  }
}

function runOtsStamp(opts) {
  const cliProofPath = `${opts.batchPath}.ots`;
  prepareOutput(opts.proofPath, opts.overwrite, 'OTS proof');
  if (resolve(cliProofPath) !== opts.proofPath) {
    prepareOutput(cliProofPath, opts.overwrite, 'temporary OTS proof');
  }

  const result = spawnSync(opts.otsCommand, ['stamp', opts.batchPath], {
    encoding: 'utf8',
  });
  if (result.error) {
    fail(`cannot run OpenTimestamps command "${opts.otsCommand}": ${result.error.message}`);
  }
  if (result.status !== 0) {
    fail(`OpenTimestamps stamp failed with exit ${result.status}\n${result.stderr || result.stdout || ''}`);
  }
  if (!existsSync(cliProofPath)) {
    fail(`OpenTimestamps command did not create expected proof ${cliProofPath}`);
  }
  if (resolve(cliProofPath) !== opts.proofPath) {
    renameSync(cliProofPath, opts.proofPath);
  }

  return {
    command: [basename(opts.otsCommand), 'stamp', basename(opts.batchPath)],
  };
}

function buildManifest(opts, batchInfo, stampInfo) {
  const proofArtifact = opts.dryRun ? null : artifact(opts.proofPath, 'opentimestamps-proof');
  const batch = batchInfo.doc.batch;

  return {
    schema: 'signal.rati_anchor_stamp_manifest.v1',
    created_at_unix: opts.createdAtUnix,
    batch: {
      version: batch.version,
      receipt_count: batch.receipt_count,
      receipt_merkle_root: batch.receipt_merkle_root,
      previous_batch_root: batch.previous_batch_root,
      settlement_checkpoint_root: batch.settlement_checkpoint_root,
      arweave_manifest_tx: batch.arweave_manifest_tx,
      canonical_batch_sha256: batchInfo.batchSha256,
      bitcoin_anchor_domain: ANCHOR_DOMAIN.toString('utf8'),
      bitcoin_anchor_root: batchInfo.anchorRoot,
      op_return_payload_hex: batchInfo.doc.canonical.op_return_payload_hex,
    },
    artifacts: {
      batch_file: artifact(opts.batchPath, 'rati-anchor-batch'),
      ots_proof_file: proofArtifact,
      stamp_manifest_file: {
        role: 'rati-anchor-stamp-manifest',
        path: basename(opts.manifestPath),
        sha256: null,
        note: 'self hash intentionally omitted',
      },
    },
    opentimestamps: {
      status: opts.dryRun ? 'dry_run' : 'stamped',
      target: 'batch_file',
      proof_path: proofArtifact ? proofArtifact.path : null,
      command: stampInfo ? stampInfo.command : ['ots', 'stamp', basename(opts.batchPath)],
      note: opts.dryRun
        ? 'OpenTimestamps CLI was not invoked; rerun without --dry-run to create the .ots proof.'
        : 'Proof file was created by the OpenTimestamps CLI and may need ots upgrade after Bitcoin confirmation.',
    },
    arweave_upload_plan: [
      { role: 'rati-anchor-batch', path: basename(opts.batchPath) },
      ...(proofArtifact ? [{ role: 'opentimestamps-proof', path: proofArtifact.path }] : []),
      { role: 'rati-anchor-stamp-manifest', path: basename(opts.manifestPath) },
    ],
  };
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  const batchInfo = readBatch(opts.batchPath);
  const stampInfo = opts.dryRun ? null : runOtsStamp(opts);

  prepareOutput(opts.manifestPath, opts.overwrite, 'stamp manifest');
  const manifest = buildManifest(opts, batchInfo, stampInfo);
  writeFileSync(opts.manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);

  process.stdout.write(JSON.stringify({
    manifest: opts.manifestPath,
    ots: opts.dryRun ? null : opts.proofPath,
    bitcoin_anchor_root: batchInfo.anchorRoot,
    status: opts.dryRun ? 'dry_run' : 'stamped',
  }, null, 2));
  process.stdout.write('\n');
}

main();
