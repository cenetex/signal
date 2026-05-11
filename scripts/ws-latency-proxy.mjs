#!/usr/bin/env node
import crypto from 'node:crypto';
import net from 'node:net';
import tls from 'node:tls';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function usage() {
  console.error(`usage: node scripts/ws-latency-proxy.mjs [options]

Options:
  --listen=HOST:PORT          listen address (default: 127.0.0.1:19091)
  --upstream=ws://HOST:PORT/P upstream websocket URL (default: ws://127.0.0.1:9091/ws)
  --client-ms=N               one-way delay for browser -> server frames (default: 0)
  --server-ms=N               one-way delay for server -> browser frames (default: 0)
  --jitter-ms=N               additive per-frame jitter, preserving order (default: 0)
  --log-frames                log forwarded frame sizes
`);
}

function parseArgs(argv) {
  const out = {
    listen: '127.0.0.1:19091',
    upstream: 'ws://127.0.0.1:9091/ws',
    clientMs: 0,
    serverMs: 0,
    jitterMs: 0,
    logFrames: false,
  };

  for (const arg of argv) {
    if (arg === '--help' || arg === '-h') {
      usage();
      process.exit(0);
    } else if (arg.startsWith('--listen=')) {
      out.listen = arg.slice('--listen='.length);
    } else if (arg.startsWith('--upstream=')) {
      out.upstream = arg.slice('--upstream='.length);
    } else if (arg.startsWith('--client-ms=')) {
      out.clientMs = Number(arg.slice('--client-ms='.length));
    } else if (arg.startsWith('--server-ms=')) {
      out.serverMs = Number(arg.slice('--server-ms='.length));
    } else if (arg.startsWith('--jitter-ms=')) {
      out.jitterMs = Number(arg.slice('--jitter-ms='.length));
    } else if (arg === '--log-frames') {
      out.logFrames = true;
    } else {
      console.error(`unknown option: ${arg}`);
      usage();
      process.exit(2);
    }
  }

  for (const [name, value] of [
    ['client-ms', out.clientMs],
    ['server-ms', out.serverMs],
    ['jitter-ms', out.jitterMs],
  ]) {
    if (!Number.isFinite(value) || value < 0) {
      console.error(`invalid --${name}: ${value}`);
      process.exit(2);
    }
  }

  return out;
}

function parseHostPort(value) {
  const at = value.lastIndexOf(':');
  if (at <= 0) throw new Error(`expected HOST:PORT, got ${value}`);
  const host = value.slice(0, at);
  const port = Number(value.slice(at + 1));
  if (!Number.isInteger(port) || port < 0 || port > 65535)
    throw new Error(`invalid port in ${value}`);
  return { host, port };
}

function findHeaderEnd(buffer) {
  return buffer.indexOf('\r\n\r\n');
}

function parseHeaders(text) {
  const lines = text.split('\r\n');
  const first = lines.shift() || '';
  const headers = new Map();
  for (const line of lines) {
    const at = line.indexOf(':');
    if (at < 0) continue;
    headers.set(line.slice(0, at).trim().toLowerCase(), line.slice(at + 1).trim());
  }
  return { first, headers };
}

function wsAccept(key) {
  return crypto.createHash('sha1').update(key + GUID).digest('base64');
}

function makeForwarder({ from, to, label, delayMs, jitterMs, logFrames }) {
  let buffer = Buffer.alloc(0);
  let nextWriteAt = 0;

  function schedule(frame) {
    const jitter = jitterMs > 0 ? Math.floor(Math.random() * (jitterMs + 1)) : 0;
    let due = Date.now() + delayMs + jitter;
    /* Model fixed path latency, not a bandwidth bottleneck. If frames arrive
     * faster than the delay, they should still emerge at roughly that same
     * cadence after the latency offset; only clamp enough to prevent jitter
     * from reordering the TCP byte stream. */
    if (due <= nextWriteAt) due = nextWriteAt + 1;
    nextWriteAt = due;
    if (logFrames) {
      console.error(`[ws-latency] ${label} ${frame.length}B delay=${due - Date.now()}ms`);
    }
    setTimeout(() => {
      if (!to.destroyed) to.write(frame);
    }, Math.max(0, due - Date.now()));
  }

  from.on('data', (chunk) => {
    buffer = buffer.length ? Buffer.concat([buffer, chunk]) : chunk;
    for (;;) {
      if (buffer.length < 2) return;
      const b1 = buffer[1];
      let len = b1 & 0x7f;
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
          from.destroy(new Error('websocket frame too large for latency proxy'));
          return;
        }
        len = low;
        off += 8;
      }
      if ((b1 & 0x80) !== 0) off += 4;
      const total = off + len;
      if (buffer.length < total) return;
      schedule(buffer.subarray(0, total));
      buffer = buffer.subarray(total);
    }
  });
}

function connectUpstream(upstreamUrl) {
  const url = new URL(upstreamUrl);
  if (url.protocol !== 'ws:' && url.protocol !== 'wss:')
    throw new Error(`unsupported upstream protocol: ${url.protocol}`);

  const port = Number(url.port || (url.protocol === 'wss:' ? 443 : 80));
  const host = url.hostname;
  const path = `${url.pathname || '/'}${url.search || ''}`;
  const key = crypto.randomBytes(16).toString('base64');
  const socket = url.protocol === 'wss:'
    ? tls.connect({ host, port, servername: host })
    : net.connect({ host, port });

  return new Promise((resolve, reject) => {
    let header = Buffer.alloc(0);
    let settled = false;

    function fail(err) {
      if (settled) return;
      settled = true;
      socket.destroy();
      reject(err);
    }

    socket.once('error', fail);
    socket.once('connect', () => {
      socket.write(
        `GET ${path} HTTP/1.1\r\n` +
        `Host: ${host}:${port}\r\n` +
        'Upgrade: websocket\r\n' +
        'Connection: Upgrade\r\n' +
        `Sec-WebSocket-Key: ${key}\r\n` +
        'Sec-WebSocket-Version: 13\r\n' +
        '\r\n'
      );
    });
    socket.on('data', function onHandshake(chunk) {
      header = Buffer.concat([header, chunk]);
      const end = findHeaderEnd(header);
      if (end < 0) return;
      socket.off('data', onHandshake);
      const text = header.subarray(0, end).toString('latin1');
      const { first } = parseHeaders(text);
      if (!/^HTTP\/1\.[01] 101\b/.test(first)) {
        fail(new Error(`upstream websocket rejected upgrade: ${first}`));
        return;
      }
      settled = true;
      socket.off('error', fail);
      resolve({ socket, leftover: header.subarray(end + 4) });
    });
  });
}

async function handleClient(client, opts) {
  let request = Buffer.alloc(0);

  client.on('error', () => {});
  client.on('data', async function onRequest(chunk) {
    request = Buffer.concat([request, chunk]);
    const end = findHeaderEnd(request);
    if (end < 0) return;
    client.off('data', onRequest);

    try {
      const text = request.subarray(0, end).toString('latin1');
      const { headers } = parseHeaders(text);
      const key = headers.get('sec-websocket-key');
      if (!key) throw new Error('missing Sec-WebSocket-Key');

      const { socket: upstream, leftover: upstreamLeftover } =
        await connectUpstream(opts.upstream);

      upstream.on('error', () => client.destroy());
      client.on('error', () => upstream.destroy());
      client.on('close', () => upstream.destroy());
      upstream.on('close', () => client.destroy());

      client.write(
        'HTTP/1.1 101 Switching Protocols\r\n' +
        'Upgrade: websocket\r\n' +
        'Connection: Upgrade\r\n' +
        `Sec-WebSocket-Accept: ${wsAccept(key)}\r\n` +
        '\r\n'
      );

      makeForwarder({
        from: client,
        to: upstream,
        label: 'client->server',
        delayMs: opts.clientMs,
        jitterMs: opts.jitterMs,
        logFrames: opts.logFrames,
      });
      makeForwarder({
        from: upstream,
        to: client,
        label: 'server->client',
        delayMs: opts.serverMs,
        jitterMs: opts.jitterMs,
        logFrames: opts.logFrames,
      });

      const clientLeftover = request.subarray(end + 4);
      if (clientLeftover.length) client.emit('data', clientLeftover);
      if (upstreamLeftover.length) upstream.emit('data', upstreamLeftover);
    } catch (err) {
      console.error(`[ws-latency] ${err instanceof Error ? err.message : String(err)}`);
      client.destroy();
    }
  });
}

const opts = parseArgs(process.argv.slice(2));
const listen = parseHostPort(opts.listen);
const server = net.createServer((client) => {
  void handleClient(client, opts);
});

server.listen(listen.port, listen.host, () => {
  const addr = server.address();
  const host = typeof addr === 'object' && addr ? addr.address : listen.host;
  const port = typeof addr === 'object' && addr ? addr.port : listen.port;
  console.error(
    `[ws-latency] listening ws://${host}:${port}/ -> ${opts.upstream} ` +
    `(client=${opts.clientMs}ms server=${opts.serverMs}ms jitter=${opts.jitterMs}ms)`
  );
});

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => server.close(() => process.exit(0)));
}
