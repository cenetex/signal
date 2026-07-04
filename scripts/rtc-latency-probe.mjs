#!/usr/bin/env node
import crypto from 'node:crypto';
import { performance } from 'node:perf_hooks';
import { setTimeout as sleep } from 'node:timers/promises';
import nodeDataChannel from 'node-datachannel';
import WebSocket from 'ws';

const SERVER_PEER_ID = 'signal-authority';
const NET_MSG_LATENCY_PING = 0x3c;
const NET_MSG_LATENCY_PONG = 0x3d;
const NET_LATENCY_PING_SIZE = 9;

function usage() {
  return `usage: node scripts/rtc-latency-probe.mjs [options]

Measure Signal's browser-default WebRTC DataChannel latency path.

Options:
  --url=URL             RTC signaling URL (default: rtcs://signal.ratimics.com/rtc/signal-main)
  --room=ROOM           signaling room (default: path without leading slash)
  --warmup-ms=N         warmup before measuring (default: 1000)
  --duration-ms=N       measurement window (default: 10000)
  --ping-hz=N           LATENCY_PING rate (default: 1)
  --stun=URL            STUN server URL (default: stun:stun.l.google.com:19302)
  --json                print JSON summary
`;
}

function parsePositiveNumber(value, name) {
  const n = Number(value);
  if (!Number.isFinite(n) || n <= 0) throw new Error(`invalid ${name}: ${value}`);
  return n;
}

function parseArgs(argv) {
  const opts = {
    url: 'rtcs://signal.ratimics.com/rtc/signal-main',
    room: '',
    warmupMs: 1000,
    durationMs: 10000,
    pingHz: 1,
    stun: 'stun:stun.l.google.com:19302',
    json: false,
    help: false,
  };
  for (const arg of argv) {
    if (arg === '--help' || arg === '-h') {
      opts.help = true;
    } else if (arg === '--json') {
      opts.json = true;
    } else if (arg.startsWith('--url=')) {
      opts.url = arg.slice('--url='.length);
    } else if (arg.startsWith('--room=')) {
      opts.room = arg.slice('--room='.length);
    } else if (arg.startsWith('--warmup-ms=')) {
      opts.warmupMs = Math.round(parsePositiveNumber(arg.slice('--warmup-ms='.length), 'warmup-ms'));
    } else if (arg.startsWith('--duration-ms=')) {
      opts.durationMs = Math.round(parsePositiveNumber(arg.slice('--duration-ms='.length), 'duration-ms'));
    } else if (arg.startsWith('--ping-hz=')) {
      opts.pingHz = parsePositiveNumber(arg.slice('--ping-hz='.length), 'ping-hz');
    } else if (arg.startsWith('--stun=')) {
      opts.stun = arg.slice('--stun='.length);
    } else {
      throw new Error(`unknown option: ${arg}`);
    }
  }
  return opts;
}

function signalingUrl(raw) {
  if (raw.startsWith('rtc://')) return `ws://${raw.slice('rtc://'.length)}`;
  if (raw.startsWith('rtcs://')) return `wss://${raw.slice('rtcs://'.length)}`;
  if (raw.startsWith('webrtc+ws://')) return `ws://${raw.slice('webrtc+ws://'.length)}`;
  if (raw.startsWith('webrtc+wss://')) return `wss://${raw.slice('webrtc+wss://'.length)}`;
  return raw;
}

function roomFromUrl(urlText) {
  const parsed = new URL(urlText);
  const queryRoom = parsed.searchParams.get('room');
  if (queryRoom) return queryRoom;
  const pathRoom = parsed.pathname.replace(/^\/+/, '');
  return pathRoom || 'signal-main';
}

function makeLatencyPing(seq) {
  const payload = Buffer.alloc(NET_LATENCY_PING_SIZE);
  payload[0] = NET_MSG_LATENCY_PING;
  payload.writeUInt32LE(seq >>> 0, 1);
  payload.writeUInt32LE(Date.now() >>> 0, 5);
  return payload;
}

function makeStats() {
  return {
    count: 0,
    rawSumMs: 0,
    transportSumMs: 0,
    serverTurnaroundSumMs: 0,
    rawMinMs: null,
    rawMaxMs: 0,
    transportMinMs: null,
    transportMaxMs: 0,
    serverTurnaroundMaxMs: 0,
  };
}

function recordPong(stats, payload) {
  if (!Buffer.isBuffer(payload)) payload = Buffer.from(payload);
  if (payload.length < 17 || payload[0] !== NET_MSG_LATENCY_PONG) return;
  const nowMs = Date.now() >>> 0;
  const clientSentMs = payload.readUInt32LE(5);
  const serverRecvMs = payload.readUInt32LE(9);
  const serverSendMs = payload.readUInt32LE(13);
  const rawMs = (nowMs - clientSentMs) >>> 0;
  const serverTurnaroundMs = (serverSendMs - serverRecvMs) >>> 0;
  const transportMs = serverTurnaroundMs > rawMs ? rawMs : rawMs - serverTurnaroundMs;
  if (rawMs <= 0 || rawMs >= 30000 || transportMs <= 0) return;
  stats.count += 1;
  stats.rawSumMs += rawMs;
  stats.transportSumMs += transportMs;
  stats.serverTurnaroundSumMs += serverTurnaroundMs;
  stats.rawMinMs = stats.rawMinMs === null ? rawMs : Math.min(stats.rawMinMs, rawMs);
  stats.rawMaxMs = Math.max(stats.rawMaxMs, rawMs);
  stats.transportMinMs = stats.transportMinMs === null
    ? transportMs
    : Math.min(stats.transportMinMs, transportMs);
  stats.transportMaxMs = Math.max(stats.transportMaxMs, transportMs);
  stats.serverTurnaroundMaxMs = Math.max(stats.serverTurnaroundMaxMs, serverTurnaroundMs);
}

function summarizeStats(stats) {
  return stats.count > 0 ? {
    count: stats.count,
    rawAvgMs: stats.rawSumMs / stats.count,
    rawMinMs: stats.rawMinMs,
    rawMaxMs: stats.rawMaxMs,
    transportAvgMs: stats.transportSumMs / stats.count,
    transportMinMs: stats.transportMinMs,
    transportMaxMs: stats.transportMaxMs,
    serverTurnaroundAvgMs: stats.serverTurnaroundSumMs / stats.count,
    serverTurnaroundMaxMs: stats.serverTurnaroundMaxMs,
  } : {
    count: 0,
    rawAvgMs: 0,
    rawMinMs: null,
    rawMaxMs: 0,
    transportAvgMs: 0,
    transportMinMs: null,
    transportMaxMs: 0,
    serverTurnaroundAvgMs: 0,
    serverTurnaroundMaxMs: 0,
  };
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
  if (!candidate || typeof candidate.candidate !== 'string') return null;
  return {
    candidate: candidate.candidate,
    mid: candidate.sdpMid || candidate.mid || String(candidate.sdpMLineIndex || 0),
  };
}

function describeCandidatePair(pc) {
  try {
    return pc.getSelectedCandidatePair();
  } catch {
    return null;
  }
}

function describePeerRtt(pc) {
  try {
    const rtt = pc.rtt();
    return Number.isFinite(rtt) ? rtt : null;
  } catch {
    return null;
  }
}

async function connectRtc(opts) {
  const sigUrl = signalingUrl(opts.url);
  const room = opts.room || roomFromUrl(sigUrl);
  const peer = `rtc-probe-${crypto.randomUUID()}`;
  const ws = new WebSocket(sigUrl);
  const pc = new nodeDataChannel.PeerConnection(peer, {
    iceServers: opts.stun ? [opts.stun] : [],
  });
  let dc = null;
  let remotePeer = SERVER_PEER_ID;
  let remoteDescriptionSet = false;
  const pendingRemoteCandidates = [];

  function sendSignal(to, data) {
    if (ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify({ type: 'signal', room, from: peer, to, data }));
  }

  pc.onLocalDescription((sdp, type) => {
    sendSignal(remotePeer, {
      type: String(type).toLowerCase(),
      sdp: { type: String(type).toLowerCase(), sdp },
    });
  });
  pc.onLocalCandidate((candidate, sdpMid) => {
    if (!candidate) return;
    sendSignal(remotePeer, {
      type: 'candidate',
      candidate: { candidate, sdpMid: sdpMid === 'unspec' ? '0' : sdpMid },
    });
  });

  let rejectOpen = null;
  const opened = new Promise((resolve, reject) => {
    rejectOpen = reject;
    const timer = setTimeout(() => reject(new Error('timed out opening datachannel')), 15000);
    const attachChannel = () => {
      if (dc) return;
      dc = pc.createDataChannel('signal');
      dc.onOpen(() => {
        clearTimeout(timer);
        resolve();
      });
      dc.onError((err) => {
        clearTimeout(timer);
        reject(new Error(`datachannel error: ${err}`));
      });
    };
    ws.once('open', () => {
      ws.send(JSON.stringify({ type: 'join', room, peer }));
    });
    ws.on('message', (message) => {
      let msg;
      try {
        msg = JSON.parse(String(message));
      } catch {
        return;
      }
      if (msg.type === 'peers' && Array.isArray(msg.peers) && msg.peers.length > 0) {
        remotePeer = msg.peers[0] || SERVER_PEER_ID;
        attachChannel();
        return;
      }
      if (msg.type !== 'signal') return;
      const data = msg.data || {};
      if (data.type === 'answer') {
        const desc = signalDescription(data);
        if (desc) {
          pc.setRemoteDescription(desc.sdp, desc.type);
          remoteDescriptionSet = true;
          while (pendingRemoteCandidates.length) {
            const candidate = pendingRemoteCandidates.shift();
            pc.addRemoteCandidate(candidate.candidate, candidate.mid);
          }
        }
        return;
      }
      if (data.type === 'candidate' && data.candidate) {
        const candidate = signalCandidate(data.candidate);
        if (!candidate) return;
        if (!remoteDescriptionSet) {
          pendingRemoteCandidates.push(candidate);
          return;
        }
        pc.addRemoteCandidate(candidate.candidate, candidate.mid);
      }
    });
  });

  ws.on('error', (err) => {
    if (rejectOpen) rejectOpen(err);
  });

  await opened;
  return { sigUrl, room, peer, ws, pc, dc };
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (opts.help) {
    process.stdout.write(usage());
    return;
  }

  const rtc = await connectRtc(opts);
  const warmupStats = makeStats();
  const measurementStats = makeStats();
  let activeStats = warmupStats;
  let seq = 0;
  rtc.dc.onMessage((message) => recordPong(activeStats, message));
  const intervalMs = Math.max(1, Math.round(1000 / opts.pingHz));
  const timer = setInterval(() => {
    seq = (seq + 1) >>> 0;
    if (seq === 0) seq = 1;
    rtc.dc.sendMessageBinary(makeLatencyPing(seq));
  }, intervalMs);

  const connectedAt = performance.now();
  await sleep(opts.warmupMs);
  activeStats = measurementStats;
  const measuredAt = performance.now();
  await sleep(opts.durationMs);
  clearInterval(timer);

  const summary = {
    url: opts.url,
    signalingUrl: rtc.sigUrl,
    room: rtc.room,
    warmupMs: Math.round(measuredAt - connectedAt),
    durationMs: Math.round(performance.now() - measuredAt),
    pingHz: opts.pingHz,
    candidatePair: describeCandidatePair(rtc.pc),
    peerRtt: describePeerRtt(rtc.pc),
    warmup: summarizeStats(warmupStats),
    measurement: summarizeStats(measurementStats),
  };

  rtc.dc.close();
  rtc.ws.close();
  if (typeof rtc.pc.destroy === 'function') rtc.pc.destroy();
  else rtc.pc.close();
  nodeDataChannel.cleanup();

  if (opts.json) {
    console.log(JSON.stringify(summary, null, 2));
    return;
  }
  const lp = summary.measurement;
  console.log('Signal RTC latency probe');
  console.log(`url=${summary.url} signaling=${summary.signalingUrl} room=${summary.room}`);
  console.log(`warmup=${summary.warmupMs}ms sample=${summary.durationMs}ms pingHz=${summary.pingHz}`);
  if (summary.candidatePair) {
    const local = summary.candidatePair.local;
    const remote = summary.candidatePair.remote;
    console.log(
      `candidate local=${local?.type || '?'}:${local?.transportType || '?'} ` +
      `${local?.address || '?'}:${local?.port || '?'} ` +
      `remote=${remote?.type || '?'}:${remote?.transportType || '?'} ` +
      `${remote?.address || '?'}:${remote?.port || '?'}`
    );
  }
  if (summary.peerRtt !== null) console.log(`peer rtt=${summary.peerRtt}`);
  console.log(
    `latency pong samples=${lp.count} ` +
    `transport avg/min/max=${lp.transportAvgMs.toFixed(1)}/${lp.transportMinMs ?? '-'}/${lp.transportMaxMs} ms ` +
    `raw avg/max=${lp.rawAvgMs.toFixed(1)}/${lp.rawMaxMs} ms ` +
    `server-turnaround avg/max=${lp.serverTurnaroundAvgMs.toFixed(1)}/${lp.serverTurnaroundMaxMs} ms`
  );
}

main().catch((err) => {
  console.error(`[rtc-latency-probe] ${err.message || err}`);
  nodeDataChannel.cleanup();
  process.exitCode = 1;
});
