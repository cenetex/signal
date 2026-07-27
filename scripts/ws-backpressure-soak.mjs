#!/usr/bin/env node

import crypto from 'node:crypto';
import { execFile } from 'node:child_process';
import fs from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { performance } from 'node:perf_hooks';
import { pathToFileURL } from 'node:url';
import { promisify } from 'node:util';
import WebSocket from 'ws';

const execFileAsync = promisify(execFile);

const NET_MSG_SESSION = 0x20;
const NET_MSG_INPUT = 0x04;
const NET_MSG_STATE = 0x03;
const NET_MSG_REGISTER_PUBKEY = 0x32;
const NET_MSG_SIGNED_ACTION = 0x33;
const NET_MSG_PROVE_PUBKEY = 0x3f;
const NET_MSG_STATION_MANIFEST = 0x2f;
const NET_MSG_LATENCY_PING = 0x3c;
const NET_MSG_LATENCY_PONG = 0x3d;
const NET_MSG_INPUT_APPLIED = 0x48;
const NET_MSG_PUBKEY_CHALLENGE = 0x70;
const SIGNED_ACTION_BUY_INGOT = 2;
const SIGNED_ACTION_DELIVER = 4;
const NET_INPUT_MSG_SIZE = 22;
const NET_STATE_AUTH_SIZE = 67;
const PUBKEY_CHALLENGE_SIZE = 32;
const PUBKEY_PROOF_SIZE = 105;
const SIGNED_ACTION_HEADER_SIZE = 12;
const SIGNED_ACTION_SIGNATURE_SIZE = 64;
const FIXTURE_STATION_COUNT = 4;
const FIXTURE_NAMED_INGOTS = 16;
const FIXTURE_DETAIL_COUNT = 240;
const FIXTURE_PAYLOAD_BYTES = 19214;
const FIXTURE_OTHER_PAYLOAD_BYTES = 19210;
const MANIFEST_DETAIL_MAX = 256;
const PRESSURE_DISCONNECT_MS = 30000;

function usage() {
  return `Usage: node scripts/ws-backpressure-soak.mjs [options]

Runs a real signal_server with 32 WebSocket clients, records an all-reader
baseline, then pauses one socket's reader while the other clients continue.

Options:
  --server=PATH             Server binary (default: ./build/signal_server)
  --url=URL                 Target an existing server instead of launching one
  --server-pid=PID          RSS PID when --url targets an existing server
  --clients=N               Client count (default: 32)
  --baseline-ms=N           All-reader comparison window (default: 300000)
  --duration-ms=N           Slow-reader window (default: 300000)
  --sample-ms=N             Health/RSS sample interval (default: 1000)
  --input-hz=N              Authoritative ack probes/client/sec (default: 1)
  --mutation-hz=N           Provenance actions/sec (default: 0.1; --short: 0.4)
  --api-token=TOKEN         Station API token for an existing server
  --max-p95-ms=N            Healthy ack p95 ceiling (default: 100)
  --max-p99-ms=N            Healthy ack p99 ceiling (default: 250)
  --max-rss-mb=N            Absolute server RSS ceiling (default: 512)
  --max-rss-growth-mb=N     RSS growth after connect ceiling (default: 128)
  --json-out=PATH           Also write the JSON result to PATH
  --short                   30s baseline + 30s slow-reader CI regression
  --help                    Show this help
`;
}

function positiveInt(raw, name) {
  const value = Number.parseInt(raw, 10);
  if (!Number.isFinite(value) || value <= 0)
    throw new Error(`${name} must be a positive integer`);
  return value;
}

function nonNegativeNumber(raw, name) {
  const value = Number.parseFloat(raw);
  if (!Number.isFinite(value) || value < 0)
    throw new Error(`${name} must be non-negative`);
  return value;
}

function parseArgs(argv) {
  const opts = {
    server: './build/signal_server',
    url: null,
    serverPid: null,
    clients: 32,
    baselineMs: 300000,
    durationMs: 300000,
    sampleMs: 1000,
    inputHz: 1,
    mutationHz: null,
    apiToken: null,
    maxP95Ms: 100,
    maxP99Ms: 250,
    maxRssMb: 512,
    maxRssGrowthMb: 128,
    jsonOut: null,
    short: false,
    help: false,
  };
  for (const arg of argv) {
    if (arg === '--help' || arg === '-h') opts.help = true;
    else if (arg === '--short') opts.short = true;
    else if (arg.startsWith('--server='))
      opts.server = arg.slice('--server='.length);
    else if (arg.startsWith('--url='))
      opts.url = arg.slice('--url='.length);
    else if (arg.startsWith('--server-pid='))
      opts.serverPid = positiveInt(arg.slice('--server-pid='.length), '--server-pid');
    else if (arg.startsWith('--clients='))
      opts.clients = positiveInt(arg.slice('--clients='.length), '--clients');
    else if (arg.startsWith('--baseline-ms='))
      opts.baselineMs = positiveInt(arg.slice('--baseline-ms='.length), '--baseline-ms');
    else if (arg.startsWith('--duration-ms='))
      opts.durationMs = positiveInt(arg.slice('--duration-ms='.length), '--duration-ms');
    else if (arg.startsWith('--sample-ms='))
      opts.sampleMs = positiveInt(arg.slice('--sample-ms='.length), '--sample-ms');
    else if (arg.startsWith('--input-hz='))
      opts.inputHz = nonNegativeNumber(arg.slice('--input-hz='.length), '--input-hz');
    else if (arg.startsWith('--mutation-hz='))
      opts.mutationHz = nonNegativeNumber(arg.slice('--mutation-hz='.length), '--mutation-hz');
    else if (arg.startsWith('--api-token='))
      opts.apiToken = arg.slice('--api-token='.length);
    else if (arg.startsWith('--max-p95-ms='))
      opts.maxP95Ms = nonNegativeNumber(arg.slice('--max-p95-ms='.length), '--max-p95-ms');
    else if (arg.startsWith('--max-p99-ms='))
      opts.maxP99Ms = nonNegativeNumber(arg.slice('--max-p99-ms='.length), '--max-p99-ms');
    else if (arg.startsWith('--max-rss-mb='))
      opts.maxRssMb = nonNegativeNumber(arg.slice('--max-rss-mb='.length), '--max-rss-mb');
    else if (arg.startsWith('--max-rss-growth-mb='))
      opts.maxRssGrowthMb = nonNegativeNumber(
        arg.slice('--max-rss-growth-mb='.length), '--max-rss-growth-mb');
    else if (arg.startsWith('--json-out='))
      opts.jsonOut = arg.slice('--json-out='.length);
    else throw new Error(`unknown option: ${arg}`);
  }
  if (opts.short) {
    opts.baselineMs = 30000;
    opts.durationMs = 30000;
  }
  if (opts.mutationHz === null)
    opts.mutationHz = opts.short ? 0.4 : 0.1;
  if (opts.clients < 2)
    throw new Error('--clients must be at least 2');
  return opts;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function reservePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const address = server.address();
  const port = address.port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}

async function fetchHealth(httpUrl) {
  const response = await fetch(`${httpUrl}/health`, {
    signal: AbortSignal.timeout(3000),
  });
  if (!response.ok)
    throw new Error(`/health returned ${response.status}`);
  return response.json();
}

async function waitForHealth(httpUrl, timeoutMs = 30000) {
  const deadline = performance.now() + timeoutMs;
  let lastError = null;
  while (performance.now() < deadline) {
    try {
      return await fetchHealth(httpUrl);
    } catch (error) {
      lastError = error;
      await sleep(100);
    }
  }
  throw new Error(`server health timeout: ${lastError?.message ?? 'unknown'}`);
}

async function fetchFundedFixtureToken(httpUrl, apiToken) {
  if (!apiToken) return null;
  const response = await fetch(`${httpUrl}/api/station/0/state`, {
    headers: { Authorization: `Bearer ${apiToken}` },
    signal: AbortSignal.timeout(3000),
  });
  if (!response.ok)
    throw new Error(`station fixture state returned ${response.status}`);
  const state = await response.json();
  for (const relationship of state.relationships ?? []) {
    const pubkey = relationship.pubkey;
    if (typeof pubkey !== 'string' ||
        !/^[0-9a-f]{64}$/i.test(pubkey) ||
        !pubkey.slice(16).match(/^0+$/) ||
        (relationship.lifetime_credits_in ?? 0) < 1200) {
      continue;
    }
    return Buffer.from(pubkey.slice(0, 16), 'hex');
  }
  return null;
}

async function rssBytes(pid) {
  if (!pid) return null;
  try {
    const { stdout } = await execFileAsync(
      '/bin/ps', ['-o', 'rss=', '-p', String(pid)]);
    const kib = Number.parseInt(stdout.trim(), 10);
    return Number.isFinite(kib) ? kib * 1024 : null;
  } catch {
    return null;
  }
}

function percentile(values, p) {
  if (!values.length) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const index = Math.max(
    0, Math.min(sorted.length - 1, Math.ceil(sorted.length * p / 100) - 1));
  return sorted[index];
}

function u32Delta(later, earlier) {
  return (later - earlier) >>> 0;
}

function makeSession(clientId, sessionToken = null) {
  const payload = Buffer.alloc(16);
  payload[0] = NET_MSG_SESSION;
  (sessionToken ?? crypto.randomBytes(8)).copy(payload, 1);
  Buffer.from(`S${String(clientId).padStart(6, '0')}`)
    .copy(payload, 9, 0, 7);
  return payload;
}

function createClientIdentity() {
  const { publicKey, privateKey } =
    crypto.generateKeyPairSync('ed25519');
  const exported = publicKey.export({ format: 'jwk' });
  if (typeof exported.x !== 'string')
    throw new Error('Ed25519 public key export did not include x');
  const publicKeyBytes = Buffer.from(exported.x, 'base64url');
  if (publicKeyBytes.length !== 32)
    throw new Error(`Ed25519 public key was ${publicKeyBytes.length} bytes`);
  return {
    publicKey,
    privateKey,
    publicKeyBytes,
    lastNonce: 0n,
  };
}

function makeRegisterPubkey(identity) {
  if (!identity || identity.publicKeyBytes?.length !== 32)
    throw new Error('registered identity must contain a 32-byte public key');
  const payload = Buffer.alloc(33);
  payload[0] = NET_MSG_REGISTER_PUBKEY;
  identity.publicKeyBytes.copy(payload, 1);
  return payload;
}

function makePubkeyProof(identity, sessionToken, challenge) {
  if (!identity || identity.publicKeyBytes?.length !== 32 ||
      !identity.privateKey) {
    throw new Error('proof identity is incomplete');
  }
  if (!Buffer.isBuffer(sessionToken) || sessionToken.length !== 8)
    throw new Error('proof session token must be 8 bytes');
  if (!Buffer.isBuffer(challenge) ||
      challenge.length !== PUBKEY_CHALLENGE_SIZE) {
    throw new Error(
      `proof challenge must be ${PUBKEY_CHALLENGE_SIZE} bytes`);
  }
  const signed = Buffer.concat([
    Buffer.from('prove-pubkey-v2', 'ascii'),
    identity.publicKeyBytes,
    sessionToken,
    challenge,
  ]);
  const signature = crypto.sign(null, signed, identity.privateKey);
  if (signature.length !== SIGNED_ACTION_SIGNATURE_SIZE)
    throw new Error(`Ed25519 proof signature was ${signature.length} bytes`);
  const payload = Buffer.alloc(PUBKEY_PROOF_SIZE);
  payload[0] = NET_MSG_PROVE_PUBKEY;
  identity.publicKeyBytes.copy(payload, 1);
  sessionToken.copy(payload, 33);
  signature.copy(payload, 41);
  return payload;
}

function makeSignedAction(identity, actionType, actionPayload) {
  if (!identity?.privateKey)
    throw new Error('signed action identity is incomplete');
  if (!Buffer.isBuffer(actionPayload) || actionPayload.length > 256)
    throw new Error('signed action payload must be a Buffer of at most 256 bytes');
  const wallClockNonce = BigInt(Date.now()) * 1000n;
  identity.lastNonce = wallClockNonce > identity.lastNonce
    ? wallClockNonce : identity.lastNonce + 1n;
  const payload = Buffer.alloc(
    SIGNED_ACTION_HEADER_SIZE + actionPayload.length +
    SIGNED_ACTION_SIGNATURE_SIZE);
  payload[0] = NET_MSG_SIGNED_ACTION;
  payload.writeBigUInt64LE(identity.lastNonce, 1);
  payload[9] = actionType;
  payload.writeUInt16LE(actionPayload.length, 10);
  actionPayload.copy(payload, SIGNED_ACTION_HEADER_SIZE);
  const signedEnd = SIGNED_ACTION_HEADER_SIZE + actionPayload.length;
  const signature = crypto.sign(
    null, payload.subarray(1, signedEnd), identity.privateKey);
  if (signature.length !== SIGNED_ACTION_SIGNATURE_SIZE)
    throw new Error(`Ed25519 action signature was ${signature.length} bytes`);
  signature.copy(payload, signedEnd);
  return payload;
}

function makeInput(client) {
  client.inputSeq = (client.inputSeq + 1) & 0xffff;
  if (client.inputSeq === 0) client.inputSeq = 1;
  client.inputTick = (client.inputTick + 1) >>> 0;
  const payload = Buffer.alloc(NET_INPUT_MSG_SIZE);
  payload[0] = NET_MSG_INPUT;
  payload[3] = 0xff;
  payload[5] = 0xff;
  payload[6] = 0xff;
  payload[7] = 0xff;
  payload.writeUInt16LE(client.inputSeq, 8);
  payload.writeUInt16LE(0xffff, 10);
  payload.writeUInt16LE(0, 12);
  payload.writeUInt32LE(client.inputTick, 14);
  payload.writeUInt32LE(Date.now() >>> 0, 18);
  return payload;
}

function makePing(client) {
  client.pingSeq = (client.pingSeq + 1) >>> 0;
  const payload = Buffer.alloc(9);
  payload[0] = NET_MSG_LATENCY_PING;
  payload.writeUInt32LE(client.pingSeq, 1);
  payload.writeUInt32LE(Date.now() >>> 0, 5);
  return payload;
}

function parseManifest(payload) {
  if (payload.length < 6 || payload[0] !== NET_MSG_STATION_MANIFEST)
    return null;
  const summaryCount = payload.readUInt16LE(2);
  const detailCount = payload.readUInt16LE(4);
  const detailOffset = 6 + summaryCount * 4;
  if (detailOffset + detailCount * 80 > payload.length)
    return null;
  const pubs = [];
  const namedIngotPubs = [];
  for (let i = 0; i < detailCount; i++) {
    const recordOffset = detailOffset + i * 80;
    const pub = Buffer.from(
      payload.subarray(recordOffset + 16, recordOffset + 48));
    if (!pub.some((byte) => byte !== 0)) continue;
    pubs.push(pub);
    if (payload[recordOffset] === 0 && payload[recordOffset + 3] !== 0)
      namedIngotPubs.push(pub);
  }
  return {
    station: payload[1],
    hash: crypto.createHash('sha256').update(payload).digest('hex'),
    summaryCount,
    detailCount,
    payloadBytes: payload.length,
    pubs,
    namedIngotPubs,
  };
}

function phaseStats() {
  return {
    ackRawMs: [],
    ackServerExcludedMs: [],
    pingRawMs: [],
    pingServerExcludedMs: [],
    ackTicks: [],
    payloadBytesByClient: new Map(),
    manifestPackets: 0,
    manifestBytes: 0,
  };
}

async function connectClient(
  id, url, state, sessionToken = null, identity = null,
) {
  const boundSessionToken =
    Buffer.from(sessionToken ?? crypto.randomBytes(8));
  const ws = new WebSocket(url, {
    headers: {
      'X-Forwarded-For': `198.51.${100 + Math.floor(id / 254)}.${1 + (id % 254)}`,
    },
    perMessageDeflate: false,
  });
  ws.binaryType = 'nodebuffer';
  const client = {
    id,
    ws,
    inputSeq: 0,
    inputTick: 0,
    pingSeq: 0,
    lastAckSeq: 0,
    paused: false,
    expectedClose: false,
    identity,
    sessionToken: boundSessionToken,
    authProofSent: false,
  };
  ws.on('message', (raw) => {
    const payload = Buffer.isBuffer(raw) ? raw : Buffer.from(raw);
    if (!payload.length) return;
    const stats = state.activeStats;
    stats.payloadBytesByClient.set(
      id, (stats.payloadBytesByClient.get(id) ?? 0) + payload.length);
    if (payload[0] === NET_MSG_PUBKEY_CHALLENGE) {
      if (!client.identity ||
          payload.length !== 1 + PUBKEY_CHALLENGE_SIZE ||
          client.authProofSent) {
        state.clientErrors.push({
          id,
          error: `unexpected pubkey challenge length=${payload.length}`,
        });
        return;
      }
      const challenge = Buffer.from(payload.subarray(1));
      if (!challenge.some((byte) => byte !== 0)) {
        state.clientErrors.push({
          id,
          error: 'server sent an all-zero pubkey challenge',
        });
        return;
      }
      client.ws.send(makePubkeyProof(
        client.identity, client.sessionToken, challenge));
      client.authProofSent = true;
      state.authProofsSent++;
    } else if (
      (payload[0] === NET_MSG_INPUT_APPLIED && payload.length >= 23) ||
        (payload[0] === NET_MSG_STATE &&
         payload.length >= NET_STATE_AUTH_SIZE)) {
      const stateAck = payload[0] === NET_MSG_STATE;
      const inputSeq = payload.readUInt16LE(stateAck ? 45 : 1);
      if (inputSeq === 0 || inputSeq === client.lastAckSeq) return;
      client.lastAckSeq = inputSeq;
      const now = Date.now() >>> 0;
      const sent = payload.readUInt32LE(stateAck ? 55 : 11);
      const serverRecv = payload.readUInt32LE(stateAck ? 59 : 15);
      const serverSend = payload.readUInt32LE(stateAck ? 63 : 19);
      const rawMs = u32Delta(now, sent);
      const turnaround = u32Delta(serverSend, serverRecv);
      if (rawMs < 30000 && turnaround <= rawMs) {
        stats.ackRawMs.push(rawMs);
        stats.ackServerExcludedMs.push(rawMs - turnaround);
      }
      stats.ackTicks.push(payload.readUInt32LE(stateAck ? 47 : 3));
    } else if (payload[0] === NET_MSG_LATENCY_PONG &&
               payload.length >= 21) {
      const now = Date.now() >>> 0;
      const sent = payload.readUInt32LE(5);
      const serverRecv = payload.readUInt32LE(9);
      const serverSend = payload.readUInt32LE(13);
      const rawMs = u32Delta(now, sent);
      const turnaround = u32Delta(serverSend, serverRecv);
      if (rawMs < 30000 && turnaround <= rawMs) {
        stats.pingRawMs.push(rawMs);
        stats.pingServerExcludedMs.push(rawMs - turnaround);
      }
    } else if (payload[0] === NET_MSG_STATION_MANIFEST) {
      stats.manifestPackets++;
      stats.manifestBytes += payload.length;
      if (id !== 0) return;
      const manifest = parseManifest(payload);
      if (manifest) {
        const previous = state.manifests.get(manifest.station);
        const currentPubs = new Set(
          manifest.pubs.map((pub) => pub.toString('hex')));
        if (previous) {
          const previousPubs = new Set(
            previous.pubs.map((pub) => pub.toString('hex')));
          for (const pub of previousPubs) {
            if (currentPubs.has(pub)) continue;
            state.concreteUnitsRemoved++;
            if (state.pendingBuyPubs.delete(pub)) {
              state.verifiedBuyRemovals++;
              if (state.transferPub === pub &&
                  state.transferPhase === 'buy_pending') {
                state.transferPhase = 'removal_verified';
              }
            }
          }
          for (const pub of currentPubs) {
            if (previousPubs.has(pub)) continue;
            state.concreteUnitsAdded++;
            if (state.pendingDeliveryPubs.delete(pub)) {
              state.verifiedDeliveryAdds++;
              if (state.transferPub === pub &&
                  state.transferPhase === 'delivery_pending') {
                state.transferPhase = 'idle';
                state.transferPub = null;
              }
            }
          }
        }
        state.manifests.set(manifest.station, manifest);
        let revisions = state.manifestRevisions.get(manifest.station);
        if (!revisions) {
          revisions = new Set();
          state.manifestRevisions.set(manifest.station, revisions);
        }
        revisions.add(manifest.hash);
      }
    }
  });
  ws.on('close', (code, reason) => {
    if (!client.expectedClose && !client.paused)
      state.healthyUnexpectedCloses.push(
        `${id}:${code}:${Buffer.from(reason).toString('utf8')}`);
  });
  ws.on('error', (error) => {
    if (!client.expectedClose && !client.paused)
      state.clientErrors.push({ id, error: error.message });
  });
  await new Promise((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error(`client ${id} open timeout`)), 10000);
    ws.once('open', () => {
      clearTimeout(timer);
      if (client.identity)
        ws.send(makeRegisterPubkey(client.identity));
      ws.send(makeSession(id, client.sessionToken));
      resolve();
    });
    ws.once('error', reject);
  });
  return client;
}

function summarizePhase(stats, elapsedMs) {
  const bytes = [...stats.payloadBytesByClient.values()];
  const totalBytes = bytes.reduce((sum, value) => sum + value, 0);
  const elapsedSeconds = elapsedMs / 1000;
  return {
    elapsed_ms: elapsedMs,
    ack_samples: stats.ackRawMs.length,
    ack_p50_ms: percentile(stats.ackRawMs, 50),
    ack_p95_ms: percentile(stats.ackRawMs, 95),
    ack_p99_ms: percentile(stats.ackRawMs, 99),
    ack_server_excluded_p95_ms:
      percentile(stats.ackServerExcludedMs, 95),
    ack_server_excluded_p99_ms:
      percentile(stats.ackServerExcludedMs, 99),
    ping_samples: stats.pingRawMs.length,
    ping_p95_ms: percentile(stats.pingRawMs, 95),
    ping_p99_ms: percentile(stats.pingRawMs, 99),
    ping_server_excluded_p95_ms:
      percentile(stats.pingServerExcludedMs, 95),
    ping_server_excluded_p99_ms:
      percentile(stats.pingServerExcludedMs, 99),
    payload_clients_observed: bytes.length,
    total_payload_bytes: totalBytes,
    payload_bytes_per_second:
      elapsedSeconds > 0 ? totalBytes / elapsedSeconds : 0,
    manifest_packets: stats.manifestPackets,
    manifest_payload_bytes: stats.manifestBytes,
    manifest_payload_bytes_per_second:
      elapsedSeconds > 0 ? stats.manifestBytes / elapsedSeconds : 0,
    min_payload_bytes_per_client: bytes.length ? Math.min(...bytes) : 0,
    max_payload_bytes_per_client: bytes.length ? Math.max(...bytes) : 0,
  };
}

function transferSnapshot(state, buyAttempts, deliveryAttempts) {
  return {
    buyAttempts,
    deliveryAttempts,
    concreteUnitsRemoved: state.concreteUnitsRemoved,
    concreteUnitsAdded: state.concreteUnitsAdded,
    verifiedBuyRemovals: state.verifiedBuyRemovals,
    verifiedDeliveryAdds: state.verifiedDeliveryAdds,
  };
}

function summarizeTransfers(start, end, elapsedMs) {
  const elapsedSeconds = elapsedMs / 1000;
  const verifiedBuyRemovals =
    end.verifiedBuyRemovals - start.verifiedBuyRemovals;
  const verifiedDeliveryAdds =
    end.verifiedDeliveryAdds - start.verifiedDeliveryAdds;
  const verifiedActions = verifiedBuyRemovals + verifiedDeliveryAdds;
  return {
    buy_attempts: end.buyAttempts - start.buyAttempts,
    delivery_attempts:
      end.deliveryAttempts - start.deliveryAttempts,
    concrete_units_removed:
      end.concreteUnitsRemoved - start.concreteUnitsRemoved,
    concrete_units_added:
      end.concreteUnitsAdded - start.concreteUnitsAdded,
    verified_buy_removals: verifiedBuyRemovals,
    verified_delivery_additions: verifiedDeliveryAdds,
    verified_actions: verifiedActions,
    verified_actions_per_second:
      elapsedSeconds > 0 ? verifiedActions / elapsedSeconds : 0,
  };
}

function healthTickSummary(samples, expectedSampleMs) {
  if (samples.length < 2)
    return { samples: samples.length, hz: 0, min_interval_hz: 0 };
  const first = samples[0];
  const last = samples[samples.length - 1];
  const elapsed = (last.atMs - first.atMs) / 1000;
  const hz = elapsed > 0
    ? u32Delta(last.worldTick, first.worldTick) / elapsed : 0;
  let minIntervalHz = Number.POSITIVE_INFINITY;
  for (let i = 1; i < samples.length; i++) {
    const dt = (samples[i].atMs - samples[i - 1].atMs) / 1000;
    if (dt <= 0) continue;
    /* Explicit phase-boundary samples can land immediately after the
     * interval sampler and observe the same sim tick. They are not a cadence
     * window; keep every real interval, including a zero-tick stalled one. */
    if (dt * 1000 < expectedSampleMs / 2) continue;
    minIntervalHz = Math.min(
      minIntervalHz,
      u32Delta(samples[i].worldTick, samples[i - 1].worldTick) / dt);
  }
  return {
    samples: samples.length,
    hz,
    min_interval_hz:
      Number.isFinite(minIntervalHz) ? minIntervalHz : 0,
  };
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (opts.help) {
    process.stdout.write(usage());
    return;
  }

  let child = null;
  let tempDir = null;
  let serverLog = null;
  let serverLogPath = null;
  let resultEmitted = false;
  let serverPid = opts.serverPid;
  let wsUrl = opts.url;
  let httpUrl = null;
  let stationApiToken = opts.apiToken;
  const clients = [];
  const timers = [];
  const state = {
    activeStats: phaseStats(),
    manifests: new Map(),
    manifestRevisions: new Map(),
    pendingBuyPubs: new Set(),
    pendingDeliveryPubs: new Set(),
    concreteUnitsRemoved: 0,
    concreteUnitsAdded: 0,
    verifiedBuyRemovals: 0,
    verifiedDeliveryAdds: 0,
    transferPhase: 'idle',
    transferPub: null,
    authProofsSent: 0,
    healthyUnexpectedCloses: [],
    clientErrors: [],
  };

  try {
    if (!wsUrl) {
      const port = await reservePort();
      tempDir = await fs.mkdtemp(
        path.join(os.tmpdir(), 'signal-ws-backpressure-'));
      const binary = path.resolve(opts.server);
      serverLogPath = path.join(tempDir, 'signal-server.log');
      stationApiToken =
        stationApiToken ?? crypto.randomBytes(24).toString('hex');
      serverLog = await fs.open(serverLogPath, 'w');
      child = spawn(binary, [], {
        cwd: tempDir,
        env: {
          ...process.env,
          PORT: String(port),
          SIGNAL_BIND_HOST: '127.0.0.1',
          SIGNAL_DATA_DIR: tempDir,
          SIGNAL_TRUST_PROXY_HEADERS: '1',
          SIGNAL_ALLOW_DEV_STATION_AUTH_SECRET: '1',
          SIGNAL_API_TOKEN: stationApiToken,
          SIGNAL_WS_BACKPRESSURE_FIXTURE: '1',
          SIGNAL_WORLD_SEED: '663',
          SIGNAL_WORLD_SEQ: '1',
        },
        stdio: ['ignore', serverLog.fd, serverLog.fd],
      });
      child.once('exit', (code, signal) => {
        if (code !== null && code !== 0)
          state.clientErrors.push({
            id: 'server',
            error: `server exited code=${code} signal=${signal}`,
          });
      });
      serverPid = child.pid;
      wsUrl = `ws://127.0.0.1:${port}/ws`;
      httpUrl = `http://127.0.0.1:${port}`;
    } else {
      const parsed = new URL(wsUrl);
      httpUrl = `${parsed.protocol === 'wss:' ? 'https:' : 'http:'}//${parsed.host}`;
    }

    const healthBefore = await waitForHealth(httpUrl);
    const fundedFixtureToken = await fetchFundedFixtureToken(
      httpUrl, stationApiToken);
    if (child && !fundedFixtureToken) {
      throw new Error(
        'backpressure fixture did not expose a funded docked identity');
    }
    const fundedFixtureIdentity = fundedFixtureToken
      ? createClientIdentity() : null;
    const rssBeforeConnect = await rssBytes(serverPid);
    for (let i = 0; i < opts.clients; i++)
      clients.push(await connectClient(
        i, wsUrl, state,
        i === 0 ? fundedFixtureToken : null,
        i === 0 ? fundedFixtureIdentity : null));

    const initialDeadline = performance.now() + 90000;
    let healthAfterInitial = healthBefore;
    while (performance.now() < initialDeadline) {
      try {
        healthAfterInitial = await fetchHealth(httpUrl);
      } catch {
        await sleep(250);
        continue;
      }
      if (healthAfterInitial.websocket_backpressure?.initial_active === 0 &&
          healthAfterInitial.live_connections === opts.clients) break;
      if (state.healthyUnexpectedCloses.length > 0) break;
      await sleep(250);
    }
    if (state.healthyUnexpectedCloses.length > 0) {
      throw new Error(
        `clients closed during initial sync: ` +
        `${state.healthyUnexpectedCloses.join(',')}`);
    }
    if (healthAfterInitial.websocket_backpressure?.initial_active !== 0 ||
        healthAfterInitial.live_connections !== opts.clients) {
      throw new Error(
        `healthy initial sync did not settle within 90s: ` +
        `active=${healthAfterInitial.websocket_backpressure?.initial_active} ` +
        `live=${healthAfterInitial.live_connections}/${opts.clients}`);
    }
    const initialCompletedDelta =
      (healthAfterInitial.websocket_backpressure?.initial_completed ?? 0) -
      (healthBefore.websocket_backpressure?.initial_completed ?? 0);
    if (initialCompletedDelta !== opts.clients) {
      throw new Error(
        `initial sync restarted before the measurement boundary: ` +
        `completed_delta=${initialCompletedDelta}, expected=${opts.clients}`);
    }
    if (child) {
      const manifestDeadline = performance.now() + 5000;
      while (!state.manifests.has(0) &&
             performance.now() < manifestDeadline) {
        await sleep(25);
      }
    }
    const fixtureManifest = state.manifests.get(0);
    if (child && (!fixtureManifest ||
        fixtureManifest.namedIngotPubs.length !== FIXTURE_NAMED_INGOTS)) {
      throw new Error(
        `backpressure fixture named-ingot count was ` +
        `${fixtureManifest?.namedIngotPubs.length ?? 0}, ` +
        `expected ${FIXTURE_NAMED_INGOTS}`);
    }
    if (fixtureManifest &&
        fixtureManifest.detailCount > MANIFEST_DETAIL_MAX) {
      throw new Error(
        `fixture manifest details ${fixtureManifest.detailCount} exceed ` +
        `${MANIFEST_DETAIL_MAX}`);
    }
    if (child && fixtureManifest.detailCount !== FIXTURE_DETAIL_COUNT) {
      throw new Error(
        `backpressure fixture detail count was ` +
        `${fixtureManifest.detailCount}, expected ${FIXTURE_DETAIL_COUNT}`);
    }
    if (child && fixtureManifest.payloadBytes !== FIXTURE_PAYLOAD_BYTES) {
      throw new Error(
        `backpressure fixture payload was ` +
        `${fixtureManifest.payloadBytes} bytes, expected ` +
        `${FIXTURE_PAYLOAD_BYTES}`);
    }
    if (child) {
      for (let station = 1; station < FIXTURE_STATION_COUNT; station++) {
        const manifest = state.manifests.get(station);
        if (!manifest ||
            manifest.namedIngotPubs.length !== 0 ||
            manifest.detailCount !== FIXTURE_DETAIL_COUNT ||
            manifest.payloadBytes !== FIXTURE_OTHER_PAYLOAD_BYTES) {
          throw new Error(
            `backpressure fixture station ${station} was not the expected ` +
            `${FIXTURE_DETAIL_COUNT}-detail/${FIXTURE_OTHER_PAYLOAD_BYTES}` +
            '-byte frame manifest');
        }
      }
    }

    let mutationAttempts = 0;
    let deliveryAttempts = 0;
    let mutationCursor = 0;
    const fixtureMutationPubs =
      fixtureManifest?.namedIngotPubs.map((pub) => Buffer.from(pub)) ?? [];
    const mutate = () => {
      const client = clients[0];
      if (client.ws.readyState !== WebSocket.OPEN || client.paused) return;
      if (state.transferPhase === 'removal_verified') {
        state.pendingDeliveryPubs.add(state.transferPub);
        client.ws.send(makeSignedAction(
          client.identity, SIGNED_ACTION_DELIVER, Buffer.from([0])));
        deliveryAttempts++;
        state.transferPhase = 'delivery_pending';
        return;
      }
      if (state.transferPhase !== 'idle') return;
      const manifest = state.manifests.get(0);
      if (!manifest || manifest.namedIngotPubs.length === 0) return;
      let pub = null;
      if (fixtureMutationPubs.length > 0) {
        const available = new Set(
          manifest.namedIngotPubs.map((item) => item.toString('hex')));
        for (let i = 0; i < fixtureMutationPubs.length; i++) {
          const candidate =
            fixtureMutationPubs[mutationCursor % fixtureMutationPubs.length];
          mutationCursor++;
          if (available.has(candidate.toString('hex'))) {
            pub = candidate;
            break;
          }
        }
      } else {
        pub = manifest.namedIngotPubs[
          mutationAttempts % manifest.namedIngotPubs.length];
      }
      if (!pub) return;
      const pubHex = pub.toString('hex');
      state.pendingBuyPubs.add(pubHex);
      state.transferPub = pubHex;
      state.transferPhase = 'buy_pending';
      client.ws.send(makeSignedAction(
        client.identity, SIGNED_ACTION_BUY_INGOT, pub));
      mutationAttempts++;
    };

    /*
     * Preserve the configured per-client probe rate while staggering peers.
     * A single synchronized 32-client timer measures an artificial client
     * herd burst every second instead of steady multi-client latency.
     */
    let inputCursor = 0;
    const inputInterval = opts.inputHz > 0
      ? setInterval(() => {
          const client = clients[inputCursor % clients.length];
          inputCursor++;
          if (!client.paused &&
              client.ws.readyState === WebSocket.OPEN) {
            client.ws.send(makeInput(client));
            client.ws.send(makePing(client));
          }
        }, Math.max(
          1, Math.round(1000 / (opts.inputHz * clients.length))))
      : null;
    if (inputInterval) timers.push(inputInterval);
    const mutationInterval = opts.mutationHz > 0
      ? setInterval(mutate,
          Math.max(1, Math.round(1000 / opts.mutationHz)))
      : null;
    if (mutationInterval) timers.push(mutationInterval);

    const baselineStats = phaseStats();
    const boundaryStats = phaseStats();
    const baselineHealthSamples = [];
    const baselineRssSamples = [];
    const slowHealthSamples = [];
    const slowRssSamples = [];
    let activeHealthSamples = baselineHealthSamples;
    let activeRssSamples = baselineRssSamples;
    let samplingChain = Promise.resolve();
    const sample = () => {
      const targetHealthSamples = activeHealthSamples;
      const targetRssSamples = activeRssSamples;
      const run = samplingChain.catch(() => {}).then(async () => {
        const [health, rss] = await Promise.all([
          fetchHealth(httpUrl),
          rssBytes(serverPid),
        ]);
        targetHealthSamples.push({
          atMs: performance.now(),
          worldTick: health.world_tick >>> 0,
          websocket: health.websocket_backpressure,
        });
        if (rss !== null) targetRssSamples.push(rss);
      });
      samplingChain = run;
      return run;
    };
    await sample();
    const sampleInterval = setInterval(() => {
      void sample().catch((error) => {
        state.clientErrors.push({
          id: 'sampler',
          error: error.message,
        });
      });
    }, opts.sampleMs);
    timers.push(sampleInterval);
    state.activeStats = baselineStats;
    const baselineTransferStart = transferSnapshot(
      state, mutationAttempts, deliveryAttempts);
    const baselineStart = performance.now();
    await sleep(opts.baselineMs);
    const baselineElapsed = performance.now() - baselineStart;
    /*
     * Stop attributing replies at the nominal boundary. The explicit health
     * sample below can itself overlap a synchronous mutation, and messages
     * arriving while it is awaited belong to neither comparison window.
     */
    state.activeStats = boundaryStats;
    const baselineTransferEnd = transferSnapshot(
      state, mutationAttempts, deliveryAttempts);
    await sample();

    const healthBeforeSlow = await fetchHealth(httpUrl);
    const rssAtSlowStart = await rssBytes(serverPid);
    const slowClient = clients[clients.length - 1];
    slowClient.paused = true;
    slowClient.ws._socket?.pause();
    const slowStart = performance.now();

    const slowStats = phaseStats();
    state.activeStats = slowStats;
    activeHealthSamples = slowHealthSamples;
    activeRssSamples = slowRssSamples;
    const slowTransferStart = transferSnapshot(
      state, mutationAttempts, deliveryAttempts);
    await sample();
    await sleep(opts.durationMs);
    const slowElapsed = performance.now() - slowStart;
    state.activeStats = boundaryStats;
    const slowTransferEnd = transferSnapshot(
      state, mutationAttempts, deliveryAttempts);

    for (const timer of timers) clearInterval(timer);
    timers.length = 0;
    await sample();
    const healthAfterSlow = await fetchHealth(httpUrl);
    const baseline = summarizePhase(baselineStats, baselineElapsed);
    const slow = summarizePhase(slowStats, slowElapsed);
    const baselineTransfers = summarizeTransfers(
      baselineTransferStart, baselineTransferEnd, baselineElapsed);
    const slowTransfers = summarizeTransfers(
      slowTransferStart, slowTransferEnd, slowElapsed);
    const baselineTicks = healthTickSummary(
      baselineHealthSamples, opts.sampleMs);
    const slowTicks = healthTickSummary(
      slowHealthSamples, opts.sampleMs);
    const baselineMaxRss = baselineRssSamples.length
      ? Math.max(...baselineRssSamples) : null;
    const slowMaxRss = slowRssSamples.length
      ? Math.max(...slowRssSamples) : null;
    const allPostSyncRssSamples = [
      ...baselineRssSamples,
      ...slowRssSamples,
    ];
    const maxRss = allPostSyncRssSamples.length
      ? Math.max(...allPostSyncRssSamples) : null;
    const postSyncRss = baselineRssSamples.length
      ? baselineRssSamples[0] : rssAtSlowStart;
    const rssGrowth = maxRss !== null && postSyncRss !== null
      ? Math.max(0, maxRss - postSyncRss) : null;
    const bpBefore = healthBeforeSlow.websocket_backpressure;
    const bpAfter = healthAfterSlow.websocket_backpressure;
    const sampledMaxConnection = Math.max(
      bpBefore?.max_connection_bytes ?? 0,
      bpAfter?.max_connection_bytes ?? 0,
      ...slowHealthSamples.map(
        (samplePoint) =>
          samplePoint.websocket?.max_connection_bytes ?? 0));
    const disconnectDelta =
      bpAfter.disconnects - bpBefore.disconnects;
    const disconnectSample = slowHealthSamples.find(
      (samplePoint) =>
        (samplePoint.websocket?.disconnects ?? 0) >
          bpBefore.disconnects);
    const disconnectAfterPauseMs = disconnectSample
      ? Math.max(0, disconnectSample.atMs - slowStart)
      : null;
    const reasonDelta = {};
    for (const [reason, count] of Object.entries(
      bpAfter.disconnect_reasons ?? {})) {
      reasonDelta[reason] =
        count - (bpBefore.disconnect_reasons?.[reason] ?? 0);
    }
    const manifestRevisionCount = [...state.manifestRevisions.values()]
      .reduce((sum, revisions) => sum + revisions.size, 0);
    const mutatedStations = [...state.manifestRevisions.values()]
      .filter((revisions) => revisions.size >= 2).length;

    const failures = [];
    const healthyClients = opts.clients - 1;
    if (healthAfterInitial.live_connections !== opts.clients)
      failures.push(`connected ${healthAfterInitial.live_connections}/${opts.clients}`);
    if (state.healthyUnexpectedCloses.length)
      failures.push(`healthy closes: ${state.healthyUnexpectedCloses.join(',')}`);
    if (state.clientErrors.length)
      failures.push(`client/server errors: ${JSON.stringify(state.clientErrors)}`);
    if (baseline.ack_samples < healthyClients)
      failures.push(`too few baseline ack samples: ${baseline.ack_samples}`);
    if (slow.ack_samples < healthyClients)
      failures.push(`too few slow-phase ack samples: ${slow.ack_samples}`);
    if (slow.ack_p95_ms > opts.maxP95Ms)
      failures.push(`ack p95 ${slow.ack_p95_ms}ms > ${opts.maxP95Ms}ms`);
    if (slow.ack_p99_ms > opts.maxP99Ms)
      failures.push(`ack p99 ${slow.ack_p99_ms}ms > ${opts.maxP99Ms}ms`);
    if (slow.ack_p95_ms > baseline.ack_p95_ms + 25)
      failures.push(`ack p95 regressed ${baseline.ack_p95_ms}->${slow.ack_p95_ms}ms`);
    if (slow.ack_p99_ms > baseline.ack_p99_ms + 50)
      failures.push(`ack p99 regressed ${baseline.ack_p99_ms}->${slow.ack_p99_ms}ms`);
    if (baselineTicks.hz < 100 || baselineTicks.hz > 140)
      failures.push(`baseline world tick cadence ${baselineTicks.hz.toFixed(1)}Hz outside 100..140`);
    if (slowTicks.hz < 100 || slowTicks.hz > 140)
      failures.push(`slow-phase world tick cadence ${slowTicks.hz.toFixed(1)}Hz outside 100..140`);
    if (slowTicks.min_interval_hz < 80)
      failures.push(`minimum sampled tick cadence ${slowTicks.min_interval_hz.toFixed(1)}Hz < 80`);
    if (Math.abs(slowTicks.hz - baselineTicks.hz) > 5)
      failures.push(`world tick cadence regressed ${baselineTicks.hz.toFixed(1)}->${slowTicks.hz.toFixed(1)}Hz`);
    if (sampledMaxConnection > bpAfter.connection_hard_bytes) {
      failures.push(
        `sampled per-connection ceiling exceeded: ${sampledMaxConnection}`);
    }
    if (bpAfter.high_water_bytes > bpAfter.connection_hard_bytes) {
      failures.push(
        `cumulative per-connection ceiling exceeded: ${bpAfter.high_water_bytes}`);
    }
    if (bpAfter.application_hard_bytes +
          bpAfter.transport_control_reserve_bytes !==
        bpAfter.connection_hard_bytes) {
      failures.push('application and transport-control ceilings do not compose');
    }
    if (bpAfter.normal_limit_bytes + bpAfter.control_reserve_bytes !==
        bpAfter.application_hard_bytes) {
      failures.push('normal and application-control ceilings do not compose');
    }
    if (bpAfter.transport_application_bytes +
          bpAfter.transport_control_reserve_bytes !==
        bpAfter.transport_hard_bytes) {
      failures.push('transport application and control ceilings do not compose');
    }
    if (bpAfter.transport_hard_bytes > bpAfter.application_hard_bytes)
      failures.push('transport ceiling exceeds the application ceiling');
    if (disconnectDelta < 1)
      failures.push('slow reader did not trigger a backpressure disconnect');
    const disconnectDeadlineMs =
      PRESSURE_DISCONNECT_MS + opts.sampleMs + 250;
    if (disconnectAfterPauseMs === null ||
        disconnectAfterPauseMs > disconnectDeadlineMs) {
      failures.push(
        `slow-reader disconnect timing ` +
        `${disconnectAfterPauseMs ?? 'missing'}ms exceeds ` +
        `${disconnectDeadlineMs}ms`);
    }
    const explicitBackpressureReasons =
      (reasonDelta.queue_hard_limit ?? 0) +
      (reasonDelta.control_headroom_exhausted ?? 0) +
      (reasonDelta.no_write_progress ?? 0) +
      (reasonDelta.sustained_pressure ?? 0) +
      (reasonDelta.descriptor_exhausted ?? 0) +
      (reasonDelta.transport_rejected ?? 0);
    if (explicitBackpressureReasons < 1) {
      failures.push(`slow-reader reason missing: ${JSON.stringify(reasonDelta)}`);
    }
    const minimumManifestBytesPerSecond =
      FIXTURE_PAYLOAD_BYTES * healthyClients * 2;
    if (baseline.manifest_payload_bytes_per_second <
          minimumManifestBytesPerSecond ||
        slow.manifest_payload_bytes_per_second <
          minimumManifestBytesPerSecond) {
      failures.push(
        `manifest traffic missing: baseline=` +
        `${baseline.manifest_payload_bytes_per_second.toFixed(1)}B/s ` +
        `slow=${slow.manifest_payload_bytes_per_second.toFixed(1)}B/s ` +
        `minimum=${minimumManifestBytesPerSecond}B/s`);
    }
    const minimumVerifiedActionRate =
      opts.mutationHz > 0 ? Math.min(1, opts.mutationHz * 0.5) : 0;
    if (baselineTransfers.verified_buy_removals < 1 ||
        baselineTransfers.verified_delivery_additions < 1 ||
        slowTransfers.verified_buy_removals < 1 ||
        slowTransfers.verified_delivery_additions < 1 ||
        slowTransfers.verified_actions_per_second <
          minimumVerifiedActionRate ||
        mutatedStations < 1) {
      failures.push(
        'sustained concrete manifest transfer cycle not observed: ' +
        `baseline=${JSON.stringify(baselineTransfers)} ` +
        `slow=${JSON.stringify(slowTransfers)} ` +
        `minimum_slow_rate=${minimumVerifiedActionRate} ` +
        `mutated_stations=${mutatedStations}`);
    }
    if (maxRss !== null && maxRss > opts.maxRssMb * 1024 * 1024)
      failures.push(`RSS ${(maxRss / 1048576).toFixed(1)}MiB > ${opts.maxRssMb}MiB`);
    if (rssGrowth !== null &&
        rssGrowth > opts.maxRssGrowthMb * 1024 * 1024) {
      failures.push(`RSS growth ${(rssGrowth / 1048576).toFixed(1)}MiB > ${opts.maxRssGrowthMb}MiB`);
    }

    const result = {
      ok: failures.length === 0,
      mode: opts.short ? 'short' : 'acceptance',
      url: wsUrl,
      clients: opts.clients,
      healthy_clients: healthyClients,
      slow_client: slowClient.id,
      baseline,
      slow_reader: slow,
      manifest_transfers: {
        configured_actions_per_second: opts.mutationHz,
        minimum_manifest_bytes_per_second:
          minimumManifestBytesPerSecond,
        baseline: baselineTransfers,
        slow_reader: slowTransfers,
      },
      world_tick: {
        baseline: baselineTicks,
        slow_reader: slowTicks,
      },
      websocket_backpressure: {
        before: bpBefore,
        after: bpAfter,
        initial_completed_delta: initialCompletedDelta,
        disconnect_delta: disconnectDelta,
        disconnect_after_pause_ms: disconnectAfterPauseMs,
        disconnect_deadline_ms: disconnectDeadlineMs,
        disconnect_reason_delta: reasonDelta,
        sampled_max_connection_bytes: sampledMaxConnection,
      },
      rss: {
        pid: serverPid,
        before_connect_bytes: rssBeforeConnect,
        post_sync_baseline_bytes: postSyncRss,
        slow_start_bytes: rssAtSlowStart,
        baseline_max_bytes: baselineMaxRss,
        slow_max_bytes: slowMaxRss,
        max_bytes: maxRss,
        growth_bytes: rssGrowth,
        baseline_samples: baselineRssSamples.length,
        slow_samples: slowRssSamples.length,
      },
      manifests: {
        auth_proofs_sent: state.authProofsSent,
        buy_attempts: mutationAttempts,
        delivery_attempts: deliveryAttempts,
        stations_observed: state.manifestRevisions.size,
        unique_revisions: manifestRevisionCount,
        mutated_stations: mutatedStations,
        concrete_units_removed: state.concreteUnitsRemoved,
        concrete_units_added: state.concreteUnitsAdded,
        verified_buy_removals: state.verifiedBuyRemovals,
        verified_delivery_additions: state.verifiedDeliveryAdds,
        fixture_named_ingots:
          fixtureManifest?.namedIngotPubs.length ?? null,
        fixture_detail_count: fixtureManifest?.detailCount ?? null,
        fixture_payload_bytes: fixtureManifest?.payloadBytes ?? null,
      },
      failures,
    };

    const json = `${JSON.stringify(result, null, 2)}\n`;
    process.stdout.write(json);
    if (opts.jsonOut)
      await fs.writeFile(path.resolve(opts.jsonOut), json);
    if (failures.length) process.exitCode = 1;
    resultEmitted = true;
  } finally {
    for (const timer of timers) clearInterval(timer);
    for (const client of clients) {
      client.expectedClose = true;
      client.ws._socket?.resume();
      try {
        client.ws.close();
      } catch {
        client.ws.terminate();
      }
    }
    if (child && child.exitCode === null) {
      child.kill('SIGTERM');
      await Promise.race([
        new Promise((resolve) => child.once('exit', resolve)),
        sleep(5000),
      ]);
      if (child.exitCode === null) child.kill('SIGKILL');
    }
    if (serverLog) await serverLog.close();
    if (!resultEmitted && serverLogPath) {
      try {
        const log = await fs.readFile(serverLogPath, 'utf8');
        process.stderr.write(
          `[ws-backpressure-soak] server log tail:\n` +
          log.slice(-16384));
      } catch {
        // Best-effort diagnostics must not replace the original failure.
      }
    }
    if (tempDir) await fs.rm(tempDir, { recursive: true, force: true });
  }
}

if (process.argv[1] &&
    import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  main().catch((error) => {
    console.error(`[ws-backpressure-soak] ${error.stack ?? error.message}`);
    process.exitCode = 1;
  });
}

export {
  SIGNED_ACTION_BUY_INGOT,
  SIGNED_ACTION_DELIVER,
  createClientIdentity,
  makePubkeyProof,
  makeRegisterPubkey,
  makeSignedAction,
};
