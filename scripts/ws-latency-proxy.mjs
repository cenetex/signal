#!/usr/bin/env node
import crypto from 'node:crypto';
import net from 'node:net';
import tls from 'node:tls';
import {
  createDeterministicUnitRandom,
  createFrameImpairment,
} from './ws-frame-impairment.mjs';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function usage() {
  console.error(`usage: node scripts/ws-latency-proxy.mjs [options]

Options:
  --listen=HOST:PORT          listen address (default: 127.0.0.1:19091)
  --upstream=ws://HOST:PORT/P upstream websocket URL (default: ws://127.0.0.1:9091/ws)
  --client-ms=N               one-way delay for browser -> server frames (default: 0)
  --server-ms=N               one-way delay for server -> browser frames (default: 0)
  --server-world-players-ms=N extra delay for server WORLD_PLAYERS frames (default: 0).
                              This is a logical stream delay; immediate control
                              frames such as LATENCY_PONG can pass it.
  --server-input-applied-ms=N extra delay for server INPUT_APPLIED frames (default: 0).
                              Also delays authoritative STATE tails. Use this
                              with WORLD_PLAYERS delay to model high
                              authoritative ack age without delaying pings.
  --jitter-ms=N               additive per-frame jitter, preserving order (default: 0)
  --seed=N                    deterministic jitter/impairment seed (default: 2037)
  --server-drop-every=N       drop every Nth selected server world frame (default: 0)
  --server-duplicate-every=N  duplicate every Nth selected server world frame (default: 0)
  --server-reorder-every=N    swap every Nth selected server world frame with
                              the next selected frame (default: 0)
  --log-frames                log forwarded frame sizes
`);
}

const NET_MSG_STATE = 0x03;
const NET_STATE_AUTH_SIZE = 55;
const NET_MSG_WORLD_PLAYERS = 0x18;
const NET_MSG_INPUT_APPLIED = 0x48;
const SERVER_ADVERSE_WORLD_TYPES = new Set([
  NET_MSG_STATE,
  0x10, // WORLD_ASTEROIDS
  0x11, // WORLD_NPCS
  NET_MSG_WORLD_PLAYERS,
  0x24, // WORLD_SCAFFOLDS
  0x46, // WORLD_CARGO_PODS
  0x4B, // WORLD_ASTEROID_MOTION
  0x4C, // WORLD_ASTEROID_MOTION_Q
  0x4D, // WORLD_PLAYER_MOTION
  0x4E, // WORLD_NPC_MOTION
  0x4F, // WORLD_CARGO_POD_MOTION
  0x52, // WORLD_NPC_MOTION_Q
  0x54, // WORLD_CARGO_POD_MOTION_Q
  0x55, // WORLD_ASTEROID_POS_Q
  0x56, // WORLD_ASTEROID_REMOVE
  0x57, // WORLD_CARGO_POD_REMOVE
  0x58, // WORLD_SCAFFOLD_REMOVE
  0x59, // WORLD_SCAFFOLD_MOTION_Q
  0x5A, // WORLD_NPC_POS_Q
  0x5B, // WORLD_NPC_POSE_Q
  0x5C, // WORLD_ASTEROID_POS8_Q
  0x5D, // WORLD_NPC_LINEAR_Q
  0x5F, // WORLD_CARGO_POD_LINEAR_Q
  0x62, // WORLD_CARGO_PODS_Q
  0x66, // WORLD_ASTEROID_POSD_Q
  0x67, // WORLD_ASTEROID_POSD8_Q
  0x68, // WORLD_ASTEROIDS_Q
  0x69, // WORLD_ASTEROIDS8_Q
  0x6A, // WORLD_PLAYER_MOTION_Q
  0x6B, // WORLD_PLAYER_DOCK_Q
  0x6C, // WORLD_PLAYER_MOTIOND_Q
  0x6D, // WORLD_PLAYER_POSED_Q
  0x6E, // WORLD_PLAYER_MOTIONM_Q
  0x70, // WORLD_TOW_LINKS
]);

function parseArgs(argv) {
  const out = {
    listen: '127.0.0.1:19091',
    upstream: 'ws://127.0.0.1:9091/ws',
    clientMs: 0,
    serverMs: 0,
    serverWorldPlayersMs: 0,
    serverInputAppliedMs: 0,
    jitterMs: 0,
    seed: 2037,
    serverDropEvery: 0,
    serverDuplicateEvery: 0,
    serverReorderEvery: 0,
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
    } else if (arg.startsWith('--server-world-players-ms=')) {
      out.serverWorldPlayersMs = Number(arg.slice('--server-world-players-ms='.length));
    } else if (arg.startsWith('--server-input-applied-ms=')) {
      out.serverInputAppliedMs = Number(arg.slice('--server-input-applied-ms='.length));
    } else if (arg.startsWith('--jitter-ms=')) {
      out.jitterMs = Number(arg.slice('--jitter-ms='.length));
    } else if (arg.startsWith('--seed=')) {
      out.seed = Number(arg.slice('--seed='.length));
    } else if (arg.startsWith('--server-drop-every=')) {
      out.serverDropEvery = Number(arg.slice('--server-drop-every='.length));
    } else if (arg.startsWith('--server-duplicate-every=')) {
      out.serverDuplicateEvery = Number(arg.slice('--server-duplicate-every='.length));
    } else if (arg.startsWith('--server-reorder-every=')) {
      out.serverReorderEvery = Number(arg.slice('--server-reorder-every='.length));
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
    ['server-world-players-ms', out.serverWorldPlayersMs],
    ['server-input-applied-ms', out.serverInputAppliedMs],
    ['jitter-ms', out.jitterMs],
  ]) {
    if (!Number.isFinite(value) || value < 0) {
      console.error(`invalid --${name}: ${value}`);
      process.exit(2);
    }
  }
  for (const [name, value] of [
    ['seed', out.seed],
    ['server-drop-every', out.serverDropEvery],
    ['server-duplicate-every', out.serverDuplicateEvery],
    ['server-reorder-every', out.serverReorderEvery],
  ]) {
    if (!Number.isInteger(value) || value < 0) {
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

function websocketPayloadInfo(frame) {
  if (frame.length < 2) return { type: -1, length: 0 };
  const masked = (frame[1] & 0x80) !== 0;
  let len = frame[1] & 0x7f;
  let off = 2;
  if (len === 126) {
    if (frame.length < off + 2) return { type: -1, length: 0 };
    len = frame.readUInt16BE(off);
    off += 2;
  } else if (len === 127) {
    if (frame.length < off + 8) return { type: -1, length: 0 };
    const high = frame.readUInt32BE(off);
    const low = frame.readUInt32BE(off + 4);
    if (high !== 0) return { type: -1, length: 0 };
    len = low;
    off += 8;
  }
  if (len <= 0) return { type: -1, length: 0 };
  if (masked) {
    if (frame.length < off + 5) return { type: -1, length: 0 };
    return { type: frame[off + 4] ^ frame[off], length: len };
  }
  if (frame.length < off + 1) return { type: -1, length: 0 };
  return { type: frame[off], length: len };
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

function makeForwarder({
  from,
  to,
  label,
  delayMs,
  extraDelayForFrame,
  jitterMs,
  seed,
  impairment,
  logFrames,
}) {
  let buffer = Buffer.alloc(0);
  let nextWriteAt = 0;
  let nextExtraWriteAt = 0;
  const random = createDeterministicUnitRandom(seed);

  function scheduleOne(frame) {
    const extraDelayMs = extraDelayForFrame ? extraDelayForFrame(frame) : 0;
    const jitter = jitterMs > 0 ? Math.floor(random() * (jitterMs + 1)) : 0;
    let due = Date.now() + delayMs + extraDelayMs + jitter;
    /* Model fixed path latency, not a bandwidth bottleneck. If frames arrive
     * faster than the delay, they should still emerge at roughly that same
     * cadence after the latency offset; only clamp enough to prevent jitter
     * from reordering each logical lane. Class-specific extra delays simulate
     * an authoritative snapshot stream lagging behind control probes, so they
     * must not head-of-line block LATENCY_PONG or other base-latency frames. */
    if (extraDelayMs > 0) {
      if (due <= nextExtraWriteAt) due = nextExtraWriteAt + 1;
      nextExtraWriteAt = due;
    } else {
      if (due <= nextWriteAt) due = nextWriteAt + 1;
      nextWriteAt = due;
    }
    if (logFrames) {
      console.error(`[ws-latency] ${label} ${frame.length}B delay=${due - Date.now()}ms`);
    }
    setTimeout(() => {
      if (!to.destroyed) to.write(frame);
    }, Math.max(0, due - Date.now()));
  }

  function schedule(frame) {
    const deliveries = impairment ? impairment.push(frame) : [frame];
    for (const delivery of deliveries) scheduleOne(delivery);
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
        seed: opts.seed ^ 0x51a7,
        logFrames: opts.logFrames,
      });
      const serverImpairment = createFrameImpairment({
        dropEvery: opts.serverDropEvery,
        duplicateEvery: opts.serverDuplicateEvery,
        reorderEvery: opts.serverReorderEvery,
        isSelected: (frame) =>
          SERVER_ADVERSE_WORLD_TYPES.has(websocketPayloadInfo(frame).type),
        onEvent: (action, ordinal) => {
          if (opts.logFrames) {
            console.error(
              `[ws-latency] server->client ${action} selected=${ordinal}`
            );
          }
        },
      });
      makeForwarder({
        from: upstream,
        to: client,
        label: 'server->client',
        delayMs: opts.serverMs,
        extraDelayForFrame: (frame) => {
          const { type, length } = websocketPayloadInfo(frame);
          if (type === NET_MSG_WORLD_PLAYERS) return opts.serverWorldPlayersMs;
          if (type === NET_MSG_INPUT_APPLIED) return opts.serverInputAppliedMs;
          if (type === NET_MSG_STATE && length >= NET_STATE_AUTH_SIZE) {
            return opts.serverInputAppliedMs;
          }
          return 0;
        },
        jitterMs: opts.jitterMs,
        seed: opts.seed ^ 0xa735,
        impairment: serverImpairment,
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
    `(client=${opts.clientMs}ms server=${opts.serverMs}ms ` +
    `world_players=${opts.serverWorldPlayersMs}ms ` +
    `input_applied=${opts.serverInputAppliedMs}ms jitter=${opts.jitterMs}ms ` +
    `seed=${opts.seed} drop_every=${opts.serverDropEvery} ` +
    `duplicate_every=${opts.serverDuplicateEvery} ` +
    `reorder_every=${opts.serverReorderEvery})`
  );
});

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => server.close(() => process.exit(0)));
}
