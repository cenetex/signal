#!/usr/bin/env node
import crypto from 'node:crypto';
import http from 'node:http';
import https from 'node:https';
import nodeDataChannel from 'node-datachannel';
import WebSocket from 'ws';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const SERVER_PEER_ID = 'signal-authority';
const MAX_PENDING_PACKETS = 512;

function usage() {
  console.error(`usage: node scripts/webrtc-gateway.mjs [options]

Authoritative WebRTC gateway for Signal.

The browser connects to this service with rtc:// or rtcs://. Signaling stays on
WebSocket, but gameplay packets ride a WebRTC DataChannel and are proxied to the
existing native Signal server over its current WebSocket endpoint.

Options:
  --listen=HOST:PORT       signaling listen address (default: 127.0.0.1:19093)
  --upstream=URL           Signal server websocket (default: ws://127.0.0.1:9091/ws)
  --proxy=URL              HTTP/WebSocket proxy target for non-RTC paths
  --rtc-prefix=PATH        RTC signaling path prefix when proxying (default: /rtc)
  --stun=URL               STUN server URL (default: stun:stun.l.google.com:19302)
  --turn=URL               TURN server URL
  --turn-user=USER         TURN username
  --turn-pass=PASS         TURN credential
  --ice-bind=HOST          ICE UDP bind address
  --ice-port=PORT          fixed ICE UDP port; enables ICE UDP mux
  --ice-port-range=A:B     ICE UDP port range
  --ice-udp-mux            enable ICE UDP mux
  --ice-tcp                enable ICE TCP candidates
`);
}

function parseHostPort(value) {
  const at = value.lastIndexOf(':');
  if (at <= 0) throw new Error(`expected HOST:PORT, got ${value}`);
  const host = value.slice(0, at);
  const port = Number(value.slice(at + 1));
  if (!Number.isInteger(port) || port <= 0 || port > 65535)
    throw new Error(`invalid port in ${value}`);
  return { host, port };
}

function parseArgs(argv) {
  const opts = {
    listen: '127.0.0.1:19093',
    upstream: 'ws://127.0.0.1:9091/ws',
    proxy: '',
    rtcPrefix: '/rtc',
    stun: 'stun:stun.l.google.com:19302',
    turn: '',
    turnUser: '',
    turnPass: '',
    iceBind: '',
    icePortMin: 0,
    icePortMax: 0,
    iceUdpMux: false,
    iceTcp: false,
  };
  for (const arg of argv) {
    if (arg === '--help' || arg === '-h') {
      usage();
      process.exit(0);
    }
    if (arg.startsWith('--listen=')) {
      opts.listen = arg.slice('--listen='.length);
    } else if (arg.startsWith('--upstream=')) {
      opts.upstream = arg.slice('--upstream='.length);
    } else if (arg.startsWith('--proxy=')) {
      opts.proxy = arg.slice('--proxy='.length);
    } else if (arg.startsWith('--rtc-prefix=')) {
      opts.rtcPrefix = arg.slice('--rtc-prefix='.length);
    } else if (arg.startsWith('--stun=')) {
      opts.stun = arg.slice('--stun='.length);
    } else if (arg.startsWith('--turn=')) {
      opts.turn = arg.slice('--turn='.length);
    } else if (arg.startsWith('--turn-user=')) {
      opts.turnUser = arg.slice('--turn-user='.length);
    } else if (arg.startsWith('--turn-pass=')) {
      opts.turnPass = arg.slice('--turn-pass='.length);
    } else if (arg.startsWith('--ice-bind=')) {
      opts.iceBind = arg.slice('--ice-bind='.length);
    } else if (arg.startsWith('--ice-port=')) {
      const port = parsePort(arg.slice('--ice-port='.length));
      opts.icePortMin = port;
      opts.icePortMax = port;
      opts.iceUdpMux = true;
    } else if (arg.startsWith('--ice-port-range=')) {
      const range = arg.slice('--ice-port-range='.length);
      const at = range.lastIndexOf(':');
      if (at <= 0) throw new Error(`expected START:END, got ${range}`);
      opts.icePortMin = parsePort(range.slice(0, at));
      opts.icePortMax = parsePort(range.slice(at + 1));
    } else if (arg === '--ice-udp-mux') {
      opts.iceUdpMux = true;
    } else if (arg === '--ice-tcp') {
      opts.iceTcp = true;
    } else {
      console.error(`unknown option: ${arg}`);
      usage();
      process.exit(2);
    }
  }
  return {
    ...opts,
    rtcPrefix: normalizePathPrefix(opts.rtcPrefix),
    ...parseHostPort(opts.listen),
  };
}

function normalizePathPrefix(prefix) {
  if (!prefix) return '/rtc';
  let out = prefix.startsWith('/') ? prefix : `/${prefix}`;
  while (out.length > 1 && out.endsWith('/')) out = out.slice(0, -1);
  return out;
}

function parsePort(value) {
  const port = Number(value);
  if (!Number.isInteger(port) || port <= 0 || port > 65535)
    throw new Error(`invalid port: ${value}`);
  return port;
}

function wsAccept(key) {
  return crypto.createHash('sha1').update(key + GUID).digest('base64');
}

function sendFrame(socket, opcode, payload) {
  const data = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  let header;
  if (data.length < 126) {
    header = Buffer.from([0x80 | opcode, data.length]);
  } else if (data.length <= 0xffff) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | opcode;
    header[1] = 126;
    header.writeUInt16BE(data.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | opcode;
    header[1] = 127;
    header.writeUInt32BE(0, 2);
    header.writeUInt32BE(data.length, 6);
  }
  socket.write(Buffer.concat([header, data]));
}

function sendJson(socket, value) {
  if (!socket.destroyed) sendFrame(socket, 0x1, JSON.stringify(value));
}

function closeSocket(socket, code = 1000) {
  if (socket.destroyed) return;
  const payload = Buffer.from([(code >> 8) & 0xff, code & 0xff]);
  sendFrame(socket, 0x8, payload);
  socket.destroy();
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
        sendFrame(socket, 0xA, payload);
        continue;
      }
      if (opcode !== 0x1) continue;
      onText(payload.toString('utf8'));
    }
  });
}

function iceServers(opts) {
  const servers = [];
  if (opts.stun) servers.push(opts.stun);
  if (opts.turn) {
    servers.push(withTurnCredentials(opts.turn, opts.turnUser, opts.turnPass));
  }
  return servers;
}

function peerConnectionConfig(opts) {
  const config = { iceServers: iceServers(opts) };
  if (opts.iceBind) config.bindAddress = opts.iceBind;
  if (opts.icePortMin) config.portRangeBegin = opts.icePortMin;
  if (opts.icePortMax) config.portRangeEnd = opts.icePortMax;
  if (opts.iceUdpMux) config.enableIceUdpMux = true;
  if (opts.iceTcp) config.enableIceTcp = true;
  return config;
}

function withTurnCredentials(turnUrl, username, password) {
  if (!username && !password) return turnUrl;
  const split = turnUrl.indexOf(':');
  if (split <= 0) return turnUrl;
  const scheme = turnUrl.slice(0, split);
  let rest = turnUrl.slice(split + 1);
  while (rest.startsWith('//')) rest = rest.slice(2);
  if (rest.includes('@')) rest = rest.slice(rest.lastIndexOf('@') + 1);
  return `${scheme}:${username}:${password}@${rest}`;
}

function channelLabel(channel) {
  if (!channel) return '';
  return typeof channel.getLabel === 'function' ? channel.getLabel() : channel.label || '';
}

function channelIsOpen(channel) {
  if (!channel) return false;
  return typeof channel.isOpen === 'function'
    ? channel.isOpen()
    : channel.readyState === 'open';
}

function closePeerConnection(pc) {
  if (!pc) return;
  try {
    if (typeof pc.destroy === 'function') pc.destroy();
    else pc.close();
  } catch {
    // Best-effort shutdown; connection teardown should not crash the gateway.
  }
}

function signalDescription(data) {
  const desc = data?.description || data?.sdp || data;
  if (typeof desc === 'string') return { type: String(data?.type || ''), sdp: desc };
  if (desc && typeof desc === 'object' && typeof desc.sdp === 'string') {
    return { type: String(desc.type || data?.type || ''), sdp: desc.sdp };
  }
  return null;
}

function signalCandidate(candidate) {
  if (!candidate || typeof candidate.candidate !== 'string' || candidate.candidate.length === 0)
    return null;
  return {
    candidate: candidate.candidate,
    mid: candidate.sdpMid || String(candidate.sdpMLineIndex || 0),
  };
}

function rtcPath(pathname, prefix) {
  return prefix === '/' || pathname === prefix || pathname.startsWith(`${prefix}/`);
}

function proxyTarget(base, path) {
  const target = new URL(path || '/', base);
  return target;
}

function removeHopByHopHeaders(headers) {
  const out = { ...headers };
  for (const name of [
    'connection',
    'keep-alive',
    'proxy-authenticate',
    'proxy-authorization',
    'te',
    'trailer',
    'transfer-encoding',
    'upgrade',
  ]) {
    delete out[name];
  }
  return out;
}

function proxyTransport(url) {
  return url.protocol === 'https:' ? https : http;
}

function proxyHttp(req, res, base) {
  const target = proxyTarget(base, req.url);
  const headers = removeHopByHopHeaders(req.headers);
  headers.host = target.host;
  headers['x-forwarded-host'] = req.headers.host || '';
  headers['x-forwarded-proto'] = req.socket.encrypted ? 'https' : 'http';

  const proxyReq = proxyTransport(target).request({
    protocol: target.protocol,
    hostname: target.hostname,
    port: target.port,
    method: req.method,
    path: `${target.pathname}${target.search}`,
    headers,
  }, (proxyRes) => {
    res.writeHead(proxyRes.statusCode || 502, proxyRes.statusMessage, proxyRes.headers);
    proxyRes.pipe(res);
  });
  proxyReq.on('error', (err) => {
    console.error(`[rtc-gateway] proxy error: ${err.message || err}`);
    if (!res.headersSent) {
      res.writeHead(502, { 'content-type': 'text/plain' });
    }
    res.end('bad gateway\n');
  });
  req.pipe(proxyReq);
}

function proxyUpgrade(req, socket, head, base) {
  const target = proxyTarget(base, req.url);
  const headers = { ...req.headers, host: target.host };
  const proxyReq = proxyTransport(target).request({
    protocol: target.protocol,
    hostname: target.hostname,
    port: target.port,
    method: req.method,
    path: `${target.pathname}${target.search}`,
    headers,
  });
  proxyReq.on('upgrade', (proxyRes, proxySocket, proxyHead) => {
    socket.write(
      `HTTP/1.1 ${proxyRes.statusCode || 101} ${proxyRes.statusMessage || 'Switching Protocols'}\r\n`
    );
    for (const [name, value] of Object.entries(proxyRes.headers)) {
      if (Array.isArray(value)) {
        for (const item of value) socket.write(`${name}: ${item}\r\n`);
      } else if (value !== undefined) {
        socket.write(`${name}: ${value}\r\n`);
      }
    }
    socket.write('\r\n');
    if (proxyHead?.length) socket.write(proxyHead);
    if (head?.length) proxySocket.write(head);
    const closeBoth = () => {
      if (!socket.destroyed) socket.destroy();
      if (!proxySocket.destroyed) proxySocket.destroy();
    };
    socket.on('error', closeBoth);
    proxySocket.on('error', closeBoth);
    socket.on('close', () => {
      if (!proxySocket.destroyed) proxySocket.destroy();
    });
    proxySocket.on('close', () => {
      if (!socket.destroyed) socket.destroy();
    });
    proxySocket.pipe(socket).pipe(proxySocket);
  });
  proxyReq.on('response', (proxyRes) => {
    socket.write(
      `HTTP/1.1 ${proxyRes.statusCode || 502} ${proxyRes.statusMessage || 'Bad Gateway'}\r\n\r\n`
    );
    proxyRes.resume();
    socket.destroy();
  });
  proxyReq.on('error', (err) => {
    console.error(`[rtc-gateway] proxy upgrade error: ${err.message || err}`);
    if (!socket.destroyed) {
      socket.write('HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n');
      socket.destroy();
    }
  });
  proxyReq.end();
}

class PeerSession {
  constructor(socket, peer, room, opts) {
    this.socket = socket;
    this.peer = peer;
    this.room = room;
    this.opts = opts;
    this.pc = null;
    this.dc = null;
    this.upstream = null;
    this.upstreamOpen = false;
    this.pendingToUpstream = [];
    this.pendingToChannel = [];
    this.pendingRemoteCandidates = [];
    this.remoteDescriptionSet = false;
    this.closed = false;
  }

  async ensurePeerConnection() {
    if (this.pc) return this.pc;
    const pc = new nodeDataChannel.PeerConnection(
      `signal-${this.peer}`,
      peerConnectionConfig(this.opts)
    );
    this.pc = pc;

    pc.onLocalCandidate((candidate, sdpMid) => {
      if (!candidate) return;
      this.sendSignal({
        type: 'candidate',
        candidate: { candidate, sdpMid: sdpMid === 'unspec' ? '0' : sdpMid },
      });
    });
    pc.onStateChange((state) => {
      console.error(`[rtc-gateway] peer=${this.peer} connection=${state}`);
      if (state === 'closed' || state === 'failed' || state === 'disconnected') this.close();
    });
    pc.onDataChannel((channel) => this.attachDataChannel(channel));
    return pc;
  }

  sendSignal(data) {
    sendJson(this.socket, {
      type: 'signal',
      room: this.room,
      from: SERVER_PEER_ID,
      to: this.peer,
      data,
    });
  }

  attachDataChannel(channel) {
    this.dc = channel;
    console.error(`[rtc-gateway] peer=${this.peer} datachannel=${channelLabel(channel)}`);
    channel.onMessage((data) => {
      const bytes = Buffer.isBuffer(data) ? data : Buffer.from(data);
      this.sendUpstream(bytes);
    });
    channel.onOpen(() => {
      console.error(`[rtc-gateway] peer=${this.peer} dc=open`);
      this.ensureUpstream();
      this.flushChannel();
    });
    channel.onClosed(() => {
      console.error(`[rtc-gateway] peer=${this.peer} dc=closed`);
      this.close();
    });
    channel.onError((err) => {
      console.error(`[rtc-gateway] peer=${this.peer} dc error: ${err}`);
      this.close();
    });
    if (channelIsOpen(channel)) {
      this.ensureUpstream();
      this.flushChannel();
    }
  }

  async handleSignal(data) {
    const pc = await this.ensurePeerConnection();
    if (data.type === 'offer' && data.sdp) {
      const remote = signalDescription(data);
      if (!remote || !remote.type || !remote.sdp)
        throw new Error('invalid remote offer');
      let timeout = null;
      const answer = new Promise((resolve, reject) => {
        timeout = setTimeout(() => reject(new Error('timed out creating answer')), 5000);
        pc.onLocalDescription((sdp, type) => {
          if (String(type).toLowerCase() !== 'answer') return;
          clearTimeout(timeout);
          resolve({ type: 'answer', sdp: { type: 'answer', sdp } });
        });
      });
      try {
        pc.setRemoteDescription(remote.sdp, remote.type);
        this.remoteDescriptionSet = true;
        this.flushRemoteCandidates();
        this.sendSignal(await answer);
      } catch (err) {
        if (timeout) clearTimeout(timeout);
        answer.catch(() => {});
        throw err;
      }
      return;
    }
    if (data.type === 'candidate' && data.candidate) {
      const candidate = signalCandidate(data.candidate);
      if (!candidate) return;
      if (!this.remoteDescriptionSet) {
        this.pendingRemoteCandidates.push(candidate);
        if (this.pendingRemoteCandidates.length > MAX_PENDING_PACKETS)
          this.pendingRemoteCandidates.shift();
        return;
      }
      pc.addRemoteCandidate(candidate.candidate, candidate.mid);
      return;
    }
    sendJson(this.socket, { type: 'error', error: 'unknown-signal', signalType: data.type });
  }

  flushRemoteCandidates() {
    while (this.pendingRemoteCandidates.length && this.pc && this.remoteDescriptionSet) {
      const candidate = this.pendingRemoteCandidates.shift();
      this.pc.addRemoteCandidate(candidate.candidate, candidate.mid);
    }
  }

  ensureUpstream() {
    if (this.upstream || this.closed) return;
    const ws = new WebSocket(this.opts.upstream);
    ws.binaryType = 'arraybuffer';
    this.upstream = ws;
    ws.addEventListener('open', () => {
      this.upstreamOpen = true;
      this.flushUpstream();
      console.error(`[rtc-gateway] peer=${this.peer} upstream=open`);
    });
    ws.addEventListener('message', (ev) => {
      this.sendChannel(Buffer.from(ev.data));
    });
    ws.addEventListener('close', () => {
      this.upstreamOpen = false;
      console.error(`[rtc-gateway] peer=${this.peer} upstream=closed`);
      this.close();
    });
    ws.addEventListener('error', (ev) => {
      console.error(`[rtc-gateway] peer=${this.peer} upstream error`, ev.message || '');
      this.close();
    });
  }

  sendUpstream(bytes) {
    if (this.closed) return;
    if (!this.upstream) this.ensureUpstream();
    if (this.upstreamOpen && this.upstream.readyState === WebSocket.OPEN) {
      this.upstream.send(bytes);
      return;
    }
    this.pendingToUpstream.push(bytes);
    if (this.pendingToUpstream.length > MAX_PENDING_PACKETS)
      this.pendingToUpstream.shift();
  }

  flushUpstream() {
    while (this.pendingToUpstream.length &&
           this.upstreamOpen &&
           this.upstream?.readyState === WebSocket.OPEN) {
      this.upstream.send(this.pendingToUpstream.shift());
    }
  }

  sendChannel(bytes) {
    if (this.closed) return;
    if (channelIsOpen(this.dc)) {
      this.dc.sendMessageBinary(bytes);
      return;
    }
    this.pendingToChannel.push(bytes);
    if (this.pendingToChannel.length > MAX_PENDING_PACKETS)
      this.pendingToChannel.shift();
  }

  flushChannel() {
    while (this.pendingToChannel.length && channelIsOpen(this.dc)) {
      this.dc.sendMessageBinary(this.pendingToChannel.shift());
    }
  }

  close() {
    if (this.closed) return;
    this.closed = true;
    if (this.upstream && this.upstream.readyState < WebSocket.CLOSING)
      this.upstream.close(1000, 'gateway peer closed');
    if (this.dc && channelIsOpen(this.dc)) this.dc.close();
    closePeerConnection(this.pc);
  }
}

const opts = parseArgs(process.argv.slice(2));
const sessions = new Map();
let shuttingDown = false;

function leave(socket) {
  const session = sessions.get(socket);
  if (!session) return;
  sessions.delete(socket);
  session.close();
  console.error(`[rtc-gateway] leave room=${session.room} peer=${session.peer}`);
}

async function handleMessage(socket, text) {
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
    const session = new PeerSession(socket, peer, room, opts);
    sessions.set(socket, session);
    sendJson(socket, { type: 'peers', room, peer, peers: [SERVER_PEER_ID] });
    console.error(`[rtc-gateway] join room=${room} peer=${peer}`);
    return;
  }

  if (msg.type === 'signal') {
    const session = sessions.get(socket);
    if (!session) {
      sendJson(socket, { type: 'error', error: 'join-first' });
      return;
    }
    if (msg.to !== SERVER_PEER_ID) {
      sendJson(socket, { type: 'error', error: 'peer-not-found', to: msg.to });
      return;
    }
    try {
      await session.handleSignal(msg.data || {});
    } catch (err) {
      console.error(`[rtc-gateway] signal failed peer=${session.peer}: ${err.stack || err}`);
      sendJson(socket, { type: 'error', error: 'signal-failed' });
    }
    return;
  }

  sendJson(socket, { type: 'error', error: 'unknown-type' });
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url || '/', `http://${req.headers.host || '127.0.0.1'}`);
  if (url.pathname === '/rtc-health' || (!opts.proxy && url.pathname === '/health')) {
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(JSON.stringify({
      status: 'ok',
      peers: sessions.size,
      upstream: opts.upstream,
      proxy: opts.proxy || null,
      rtcPrefix: opts.rtcPrefix,
    }));
    return;
  }
  if (opts.proxy) {
    proxyHttp(req, res, opts.proxy);
    return;
  }
  res.writeHead(426, { 'content-type': 'text/plain' });
  res.end('WebSocket upgrade required\n');
});

server.on('upgrade', (req, socket, head) => {
  const url = new URL(req.url || '/', `http://${req.headers.host || '127.0.0.1'}`);
  if (opts.proxy && !rtcPath(url.pathname, opts.rtcPrefix)) {
    proxyUpgrade(req, socket, head, opts.proxy);
    return;
  }

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
  decodeFrames(socket, (text) => {
    handleMessage(socket, text).catch((err) => {
      console.error(`[rtc-gateway] message failed: ${err.stack || err}`);
      sendJson(socket, { type: 'error', error: 'message-failed' });
    });
  });
});

server.listen(opts.port, opts.host, () => {
  console.error(
    `[rtc-gateway] listening ws://${opts.host}:${opts.port} -> ${opts.upstream}`
  );
});

function shutdown(signal) {
  if (shuttingDown) return;
  shuttingDown = true;
  console.error(`[rtc-gateway] ${signal} shutdown`);
  for (const socket of [...sessions.keys()]) {
    leave(socket);
    closeSocket(socket);
  }
  server.close(() => {
    nodeDataChannel.cleanup();
    process.exit(0);
  });
  setTimeout(() => {
    nodeDataChannel.cleanup();
    process.exit(0);
  }, 1000).unref();
}

process.on('SIGINT', () => shutdown('SIGINT'));
process.on('SIGTERM', () => shutdown('SIGTERM'));
