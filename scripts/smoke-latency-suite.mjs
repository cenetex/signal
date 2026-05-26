#!/usr/bin/env node
import { spawn } from 'node:child_process';
import fs from 'node:fs/promises';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const isWindows = process.platform === 'win32';
const signalServerBin = path.join(repoRoot, 'build', isWindows ? 'signal_server.exe' : 'signal_server');
const npxBin = isWindows ? 'npx.cmd' : 'npx';

const children = [];
const tempDirs = [];
let cleaningUp = false;

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function appendTail(current, chunk, max = 24000) {
  const next = current + chunk.toString('utf8');
  return next.length > max ? next.slice(next.length - max) : next;
}

function log(message) {
  console.log(`[smoke-latency-suite] ${message}`);
}

async function freePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const address = server.address();
      if (!address || typeof address !== 'object') {
        server.close();
        reject(new Error('could not allocate a local port'));
        return;
      }
      const { port } = address;
      server.close(() => resolve(port));
    });
  });
}

function startBackground(name, command, args, options = {}) {
  const child = spawn(command, args, {
    cwd: repoRoot,
    env: { ...process.env, ...(options.env || {}) },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  const proc = {
    name,
    child,
    stdout: '',
    stderr: '',
    exited: false,
    code: null,
    signal: null,
    done: null,
  };
  proc.done = new Promise((resolve) => {
    child.once('exit', (code, signal) => {
      proc.exited = true;
      proc.code = code;
      proc.signal = signal;
      resolve({ code, signal });
    });
  });
  child.stdout.on('data', (chunk) => {
    proc.stdout = appendTail(proc.stdout, chunk);
  });
  child.stderr.on('data', (chunk) => {
    proc.stderr = appendTail(proc.stderr, chunk);
  });
  child.once('error', (err) => {
    proc.exited = true;
    proc.stderr = appendTail(proc.stderr, `${err.message}\n`);
  });
  children.push(proc);
  return proc;
}

function processTail(proc) {
  return [
    proc.stdout.trim() ? `--- ${proc.name} stdout ---\n${proc.stdout.trim()}` : '',
    proc.stderr.trim() ? `--- ${proc.name} stderr ---\n${proc.stderr.trim()}` : '',
  ].filter(Boolean).join('\n');
}

function assertAlive(proc) {
  if (!proc.exited) return;
  const code = proc.signal ? proc.signal : proc.code;
  throw new Error(`${proc.name} exited early (${code})\n${processTail(proc)}`);
}

async function stopProcess(proc) {
  if (!proc || proc.exited) return;
  proc.child.kill('SIGTERM');
  const timeout = sleep(3000).then(() => 'timeout');
  const result = await Promise.race([proc.done, timeout]);
  if (result === 'timeout' && !proc.exited) {
    proc.child.kill('SIGKILL');
    await proc.done;
  }
}

async function cleanup() {
  if (cleaningUp) return;
  cleaningUp = true;
  for (const proc of [...children].reverse()) {
    await stopProcess(proc);
  }
  for (const dir of tempDirs.reverse()) {
    await fs.rm(dir, { recursive: true, force: true });
  }
}

async function withCleanup(fn) {
  const abort = async () => {
    await cleanup();
    process.exit(130);
  };
  process.once('SIGINT', abort);
  process.once('SIGTERM', abort);
  try {
    return await fn();
  } finally {
    process.removeListener('SIGINT', abort);
    process.removeListener('SIGTERM', abort);
    await cleanup();
  }
}

function httpStatus(url) {
  return new Promise((resolve, reject) => {
    const req = http.get(url, (res) => {
      res.resume();
      res.on('end', () => resolve(res.statusCode || 0));
    });
    req.setTimeout(1000, () => {
      req.destroy(new Error(`timeout waiting for ${url}`));
    });
    req.once('error', reject);
  });
}

async function waitForHttp(url, label, backgroundProcs, timeoutMs = 30000) {
  const deadline = Date.now() + timeoutMs;
  let lastError = '';
  while (Date.now() < deadline) {
    for (const proc of backgroundProcs) assertAlive(proc);
    try {
      const status = await httpStatus(url);
      if (status >= 200 && status < 500) return;
      lastError = `HTTP ${status}`;
    } catch (err) {
      lastError = err instanceof Error ? err.message : String(err);
    }
    await sleep(250);
  }
  throw new Error(`${label} did not become ready at ${url}: ${lastError}`);
}

async function waitForProxy(proc, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    assertAlive(proc);
    const match = proc.stderr.match(/listening ws:\/\/[^:]+:(\d+)\//);
    if (match) return Number(match[1]);
    await sleep(50);
  }
  throw new Error(`latency proxy did not start\n${processTail(proc)}`);
}

async function runForeground(name, command, args, env) {
  log(name);
  const child = spawn(command, args, {
    cwd: repoRoot,
    env: { ...process.env, ...env },
    stdio: 'inherit',
  });
  const { code, signal, error } = await new Promise((resolve) => {
    child.once('error', (err) => {
      resolve({ code: null, signal: null, error: err });
    });
    child.once('exit', (exitCode, exitSignal) => {
      resolve({ code: exitCode, signal: exitSignal, error: null });
    });
  });
  if (error) {
    throw new Error(`${name} failed to start: ${error.message}`);
  }
  if (code !== 0) {
    throw new Error(`${name} failed (${signal || code})`);
  }
}

async function runLatencyCase({
  name,
  grep,
  envFlag,
  httpPort,
  serverPort,
  proxyArgs,
}) {
  const proxy = startBackground(name, process.execPath, [
    'scripts/ws-latency-proxy.mjs',
    '--listen=127.0.0.1:0',
    `--upstream=ws://127.0.0.1:${serverPort}/ws`,
    ...proxyArgs,
  ]);
  const proxyPort = await waitForProxy(proxy);
  const smokeUrl =
    `http://127.0.0.1:${httpPort}/play.html?server=ws://127.0.0.1:${proxyPort}/ws`;

  try {
    await runForeground(`running ${name}`, npxBin, [
      'playwright',
      'test',
      'tests/browser-smoke.spec.ts',
      '--project=chromium',
      '--grep',
      grep,
    ], {
      SMOKE_URL: smokeUrl,
      [envFlag]: '1',
    });
  } finally {
    await stopProcess(proxy);
  }
}

await withCleanup(async () => {
  const serverPort = await freePort();
  const httpPort = await freePort();
  const dataDir = await fs.mkdtemp(path.join(os.tmpdir(), 'signal-latency-smoke-'));
  tempDirs.push(dataDir);

  log(`starting signal_server on :${serverPort}`);
  const server = startBackground('signal_server', signalServerBin, [], {
    env: {
      PORT: String(serverPort),
      SIGNAL_PERSISTENCE_MODE: 'ephemeral',
      SIGNAL_DATA_DIR: dataDir,
      SIGNAL_WORLD_SEED: '2037',
      SIGNAL_WORLD_SEQ: '1',
    },
  });
  await waitForHttp(`http://127.0.0.1:${serverPort}/health`, 'signal_server', [server], 30000);

  log(`starting static build-web server on :${httpPort}`);
  const staticServer = startBackground('static-http', 'python3', [
    '-m',
    'http.server',
    String(httpPort),
    '--bind',
    '127.0.0.1',
    '--directory',
    path.join(repoRoot, 'build-web'),
  ]);
  await waitForHttp(`http://127.0.0.1:${httpPort}/play.html`, 'static server', [server, staticServer], 10000);

  await runLatencyCase({
    name: 'high-latency proxy smoke',
    grep: 'high-latency',
    envFlag: 'SMOKE_LATENCY_ASSERT',
    httpPort,
    serverPort,
    proxyArgs: [
      '--client-ms=450',
      '--server-ms=450',
      '--jitter-ms=150',
    ],
  });

  await runLatencyCase({
    name: 'low-ping high-ack proxy smoke',
    grep: 'low-ping high-ack',
    envFlag: 'SMOKE_ACK_LAG_ASSERT',
    httpPort,
    serverPort,
    proxyArgs: [
      '--client-ms=20',
      '--server-ms=20',
      '--server-world-players-ms=550',
      '--jitter-ms=10',
    ],
  });

  log('latency proxy browser smokes passed');
});
