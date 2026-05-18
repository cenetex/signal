#!/usr/bin/env node
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';
import net from 'node:net';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function wsAccept(key) {
  return crypto.createHash('sha1').update(key + GUID).digest('base64');
}

function wsFrame(payload) {
  assert(payload.length < 126);
  return Buffer.concat([Buffer.from([0x82, payload.length]), payload]);
}

function waitForListening(child) {
  return new Promise((resolve, reject) => {
    let stderr = '';
    const timer = setTimeout(() => reject(new Error(`proxy did not start: ${stderr}`)), 5000);
    child.stderr.on('data', (chunk) => {
      stderr += chunk.toString('utf8');
      const match = stderr.match(/listening ws:\/\/127\.0\.0\.1:(\d+)\//);
      if (match) {
        clearTimeout(timer);
        resolve(Number(match[1]));
      }
    });
    child.once('exit', (code) => reject(new Error(`proxy exited early with code ${code}: ${stderr}`)));
  });
}

function startUpstream() {
  const server = net.createServer((socket) => {
    let request = Buffer.alloc(0);
    socket.on('data', function onData(chunk) {
      request = Buffer.concat([request, chunk]);
      const end = request.indexOf('\r\n\r\n');
      if (end < 0) return;
      socket.off('data', onData);
      const headers = request.subarray(0, end).toString('latin1');
      const key = headers.match(/\r\nSec-WebSocket-Key: ([^\r\n]+)/i)?.[1];
      assert(key);
      socket.write(
        'HTTP/1.1 101 Switching Protocols\r\n' +
        'Upgrade: websocket\r\n' +
        'Connection: Upgrade\r\n' +
        `Sec-WebSocket-Accept: ${wsAccept(key)}\r\n` +
        '\r\n'
      );
      socket.write(wsFrame(Buffer.from([0x18, 0x00])));
      socket.write(wsFrame(Buffer.alloc(17, 0x3d)));
    });
  });

  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => {
      const address = server.address();
      assert(address && typeof address === 'object');
      resolve({ server, port: address.port });
    });
  });
}

function installFrameReader(socket) {
  const frames = [];
  let buffer = Buffer.alloc(0);

  function push(chunk) {
    buffer = buffer.length ? Buffer.concat([buffer, chunk]) : chunk;
    for (;;) {
      if (buffer.length < 2) return;
      let len = buffer[1] & 0x7f;
      let off = 2;
      if (len === 126) {
        if (buffer.length < 4) return;
        len = buffer.readUInt16BE(2);
        off = 4;
      }
      if (buffer.length < off + len) return;
      frames.push({ type: buffer[off], at: Date.now() });
      buffer = buffer.subarray(off + len);
    }
  }

  socket.on('data', push);

  return { frames, push };
}

async function main() {
  const { server: upstream, port: upstreamPort } = await startUpstream();
  const proxy = spawn(process.execPath, [
    'scripts/ws-latency-proxy.mjs',
    '--listen=127.0.0.1:0',
    `--upstream=ws://127.0.0.1:${upstreamPort}/ws`,
    '--client-ms=0',
    '--server-ms=1',
    '--server-world-players-ms=200',
    '--jitter-ms=0',
  ], { cwd: new URL('..', import.meta.url), stdio: ['ignore', 'ignore', 'pipe'] });
  let socket = null;

  try {
    const proxyPort = await waitForListening(proxy);
    socket = net.connect(proxyPort, '127.0.0.1');
    const key = crypto.randomBytes(16).toString('base64');
    let reader = null;

    await new Promise((resolve, reject) => {
      let handshake = Buffer.alloc(0);
      const timer = setTimeout(() => reject(new Error('client timed out')), 5000);
      socket.once('error', reject);
      socket.on('connect', () => {
        socket.write(
          'GET /ws HTTP/1.1\r\n' +
          `Host: 127.0.0.1:${proxyPort}\r\n` +
          'Upgrade: websocket\r\n' +
          'Connection: Upgrade\r\n' +
          `Sec-WebSocket-Key: ${key}\r\n` +
          'Sec-WebSocket-Version: 13\r\n' +
          '\r\n'
        );
      });
      socket.on('data', function onHandshake(chunk) {
        handshake = Buffer.concat([handshake, chunk]);
        const end = handshake.indexOf('\r\n\r\n');
        if (end < 0) return;
        socket.off('data', onHandshake);
        const response = handshake.subarray(0, end).toString('latin1');
        assert.match(response, /^HTTP\/1\.[01] 101\b/);
        const leftover = handshake.subarray(end + 4);
        reader = installFrameReader(socket);
        if (leftover.length) reader.push(leftover);
      });
      const poll = setInterval(() => {
        if (reader?.frames.length >= 2) {
          clearInterval(poll);
          clearTimeout(timer);
          resolve();
        }
      }, 10);
    });

    const frames = reader.frames;
    assert.equal(frames[0].type, 0x3d, 'LATENCY_PONG should bypass delayed WORLD_PLAYERS');
    assert.equal(frames[1].type, 0x18, 'WORLD_PLAYERS should still arrive on its delayed lane');
    assert(frames[1].at - frames[0].at >= 100, 'WORLD_PLAYERS delay should remain observable');
  } finally {
    socket?.destroy();
    proxy.kill('SIGTERM');
    upstream.close();
  }
}

await main();
