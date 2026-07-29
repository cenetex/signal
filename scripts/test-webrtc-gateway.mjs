#!/usr/bin/env node
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const FIXTURE = Buffer.from([
  0x00, 0x00, 0x01, 0xba, 0x44, 0x00, 0x04, 0x00,
  0x04, 0x01, 0x89, 0xc3, 0xf8, 0x00,
]);

async function reservePort() {
  const server = net.createServer();
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const address = server.address();
  assert(address && typeof address === 'object');
  const { port } = address;
  await new Promise((resolve, reject) => {
    server.close((err) => err ? reject(err) : resolve());
  });
  return port;
}

function waitForGateway(child, port) {
  return new Promise((resolve, reject) => {
    let stderr = '';
    let settled = false;
    const expected = `[rtc-gateway] listening ws://127.0.0.1:${port}`;
    const timer = setTimeout(() => {
      finish(new Error(`gateway did not start: ${stderr}`));
    }, 10000);

    function cleanup() {
      clearTimeout(timer);
      child.stderr.off('data', onStderr);
      child.off('error', onError);
      child.off('exit', onExit);
    }

    function finish(err) {
      if (settled) return;
      settled = true;
      cleanup();
      if (err) reject(err);
      else resolve();
    }

    function onStderr(chunk) {
      stderr += chunk.toString('utf8');
      if (stderr.includes(expected)) finish();
    }

    function onError(err) {
      finish(err);
    }

    function onExit(code, signal) {
      finish(new Error(
        `gateway exited before listening (code=${code}, signal=${signal}): ${stderr}`,
      ));
    }

    child.stderr.on('data', onStderr);
    child.once('error', onError);
    child.once('exit', onExit);
  });
}

function request(port, method) {
  return new Promise((resolve, reject) => {
    const req = http.request({
      host: '127.0.0.1',
      port,
      path: '/fixture.mpg',
      method,
    }, (res) => {
      const chunks = [];
      res.on('data', (chunk) => chunks.push(chunk));
      res.on('end', () => {
        resolve({
          statusCode: res.statusCode,
          headers: res.headers,
          body: Buffer.concat(chunks),
        });
      });
    });
    req.setTimeout(5000, () => {
      req.destroy(new Error(`${method} request timed out`));
    });
    req.once('error', reject);
    req.end();
  });
}

async function stopChild(child) {
  if (child.exitCode !== null || child.signalCode !== null) return;
  const exited = once(child, 'exit');
  child.kill('SIGTERM');
  const stopped = await Promise.race([
    exited.then(() => true),
    new Promise((resolve) => setTimeout(() => resolve(false), 5000)),
  ]);
  if (stopped) return;
  child.kill('SIGKILL');
  await once(child, 'exit');
}

async function main() {
  const staticDir = fs.mkdtempSync(path.join(os.tmpdir(), 'signal-rtc-static-'));
  fs.writeFileSync(path.join(staticDir, 'fixture.mpg'), FIXTURE);
  const port = await reservePort();
  const gateway = spawn(process.execPath, [
    'scripts/webrtc-gateway.mjs',
    `--listen=127.0.0.1:${port}`,
    '--upstream=ws://127.0.0.1:1/ws',
    `--static=${staticDir}`,
    '--server-idle-ms=0',
  ], {
    cwd: ROOT,
    stdio: ['ignore', 'ignore', 'pipe'],
  });

  try {
    await waitForGateway(gateway, port);

    const get = await request(port, 'GET');
    assert.equal(get.statusCode, 200);
    assert.equal(get.headers['content-type'], 'video/mpeg');
    assert.equal(get.headers['x-content-type-options'], 'nosniff');
    assert.equal(Number(get.headers['content-length']), FIXTURE.length);
    assert.deepEqual(get.body, FIXTURE);

    const head = await request(port, 'HEAD');
    assert.equal(head.statusCode, 200);
    assert.equal(head.headers['content-type'], 'video/mpeg');
    assert.equal(Number(head.headers['content-length']), FIXTURE.length);
    assert.equal(head.body.length, 0);
  } finally {
    await stopChild(gateway);
    fs.rmSync(staticDir, { recursive: true, force: true });
  }
}

await main();
console.log('webrtc-gateway static serving tests passed');
