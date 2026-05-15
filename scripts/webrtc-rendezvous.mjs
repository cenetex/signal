#!/usr/bin/env node
import crypto from 'node:crypto';
import http from 'node:http';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function usage() {
  console.error(`usage: node scripts/webrtc-rendezvous.mjs [--listen=HOST:PORT]

Minimal WebRTC signaling rendezvous for Signal browser peers.

It does not proxy game packets and does not make authority decisions. Clients
join a room, receive the current peer list, and send directed SDP/ICE envelopes
through this service until their WebRTC DataChannel is open.
`);
}

function parseArgs(argv) {
  const out = { listen: '127.0.0.1:19092' };
  for (const arg of argv) {
    if (arg === '--help' || arg === '-h') {
      usage();
      process.exit(0);
    }
    if (arg.startsWith('--listen=')) {
      out.listen = arg.slice('--listen='.length);
      continue;
    }
    console.error(`unknown option: ${arg}`);
    usage();
    process.exit(2);
  }
  const at = out.listen.lastIndexOf(':');
  if (at <= 0) throw new Error(`expected HOST:PORT, got ${out.listen}`);
  out.host = out.listen.slice(0, at);
  out.port = Number(out.listen.slice(at + 1));
  if (!Number.isInteger(out.port) || out.port <= 0 || out.port > 65535)
    throw new Error(`invalid port in ${out.listen}`);
  return out;
}

function wsAccept(key) {
  return crypto.createHash('sha1').update(key + GUID).digest('base64');
}

function sendFrame(socket, text) {
  const payload = Buffer.from(text);
  let header;
  if (payload.length < 126) {
    header = Buffer.from([0x81, payload.length]);
  } else if (payload.length <= 0xffff) {
    header = Buffer.alloc(4);
    header[0] = 0x81;
    header[1] = 126;
    header.writeUInt16BE(payload.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x81;
    header[1] = 127;
    header.writeUInt32BE(0, 2);
    header.writeUInt32BE(payload.length, 6);
  }
  socket.write(Buffer.concat([header, payload]));
}

function sendJson(socket, value) {
  if (!socket.destroyed) sendFrame(socket, JSON.stringify(value));
}

function closeSocket(socket, code = 1000) {
  if (socket.destroyed) return;
  const frame = Buffer.from([0x88, 0x02, (code >> 8) & 0xff, code & 0xff]);
  socket.write(frame, () => socket.destroy());
}

function decodeFrames(socket, onText) {
  let buffer = Buffer.alloc(0);
  socket.on('data', (chunk) => {
    buffer = buffer.length ? Buffer.concat([buffer, chunk]) : chunk;
    for (;;) {
      if (buffer.length < 2) return;
      const opcode = buffer[0] & 0x0f;
      const masked = (buffer[1] & 0x80) !== 0;
      let len = buffer[1] & 0x7f;
      let off = 2;
      if (len === 126) {
        if (buffer.length < off + 2) return;
        len = buffer.readUInt16BE(off);
        off += 2;
      } else if (len === 127) {
        if (buffer.length < off + 8) return;
        const high = buffer.readUInt32BE(off);
        const low = buffer.readUInt32BE(off + 4);
        if (high !== 0) {
          closeSocket(socket, 1009);
          return;
        }
        len = low;
        off += 8;
      }
      let mask = null;
      if (masked) {
        if (buffer.length < off + 4) return;
        mask = buffer.subarray(off, off + 4);
        off += 4;
      }
      if (buffer.length < off + len) return;
      const payload = Buffer.from(buffer.subarray(off, off + len));
      buffer = buffer.subarray(off + len);
      if (mask) {
        for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
      }
      if (opcode === 0x8) {
        closeSocket(socket);
        return;
      }
      if (opcode === 0x9) {
        socket.write(Buffer.concat([Buffer.from([0x8a, payload.length]), payload]));
        continue;
      }
      if (opcode !== 0x1) continue;
      onText(payload.toString('utf8'));
    }
  });
}

const opts = parseArgs(process.argv.slice(2));
const rooms = new Map();
const peers = new Map();

function roomPeers(room) {
  let set = rooms.get(room);
  if (!set) {
    set = new Map();
    rooms.set(room, set);
  }
  return set;
}

function leave(socket) {
  const meta = peers.get(socket);
  if (!meta) return;
  peers.delete(socket);
  const set = rooms.get(meta.room);
  if (!set) return;
  set.delete(meta.peer);
  for (const [peer, other] of set) {
    sendJson(other, { type: 'peer-left', room: meta.room, peer: meta.peer });
  }
  if (set.size === 0) rooms.delete(meta.room);
  console.error(`[rendezvous] leave room=${meta.room} peer=${meta.peer}`);
}

function handleMessage(socket, text) {
  let msg;
  try {
    msg = JSON.parse(text);
  } catch {
    sendJson(socket, { type: 'error', error: 'bad-json' });
    return;
  }

  if (msg.type === 'join') {
    const room = String(msg.room || 'signal-main').slice(0, 128);
    const peer = String(msg.peer || crypto.randomUUID()).slice(0, 128);
    leave(socket);
    const set = roomPeers(room);
    const existing = [...set.keys()].filter((id) => id !== peer);
    set.set(peer, socket);
    peers.set(socket, { room, peer });
    sendJson(socket, { type: 'peers', room, peer, peers: existing });
    for (const [id, other] of set) {
      if (id !== peer) sendJson(other, { type: 'peer-joined', room, peer });
    }
    console.error(`[rendezvous] join room=${room} peer=${peer} peers=${existing.length}`);
    return;
  }

  if (msg.type === 'signal') {
    const meta = peers.get(socket);
    if (!meta) {
      sendJson(socket, { type: 'error', error: 'join-first' });
      return;
    }
    const to = String(msg.to || '');
    const set = rooms.get(meta.room);
    const target = set && set.get(to);
    if (!target) {
      sendJson(socket, { type: 'error', error: 'peer-not-found', to });
      return;
    }
    sendJson(target, {
      type: 'signal',
      room: meta.room,
      from: meta.peer,
      to,
      data: msg.data || {}
    });
    return;
  }

  sendJson(socket, { type: 'error', error: 'unknown-type' });
}

const server = http.createServer((req, res) => {
  if (req.url === '/health') {
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ status: 'ok', rooms: rooms.size }));
    return;
  }
  res.writeHead(426, { 'content-type': 'text/plain' });
  res.end('WebSocket upgrade required\n');
});

server.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'];
  if (!key) {
    socket.destroy();
    return;
  }
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\n' +
    'Connection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${wsAccept(key)}\r\n` +
    '\r\n'
  );
  socket.on('close', () => leave(socket));
  socket.on('error', () => leave(socket));
  decodeFrames(socket, (text) => handleMessage(socket, text));
});

server.listen(opts.port, opts.host, () => {
  console.error(`[rendezvous] listening on ws://${opts.host}:${opts.port}`);
});
