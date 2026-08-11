#!/usr/bin/env node
import crypto from 'node:crypto';
import net from 'node:net';
import path from 'node:path';
import tls from 'node:tls';
import { performance } from 'node:perf_hooks';
import { setTimeout as sleep } from 'node:timers/promises';
import { pathToFileURL } from 'node:url';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const NET_MSG_SESSION = 0x20;
const NET_MSG_INPUT = 0x04;
const NET_MSG_EVENTS = 0x26;
const NET_MSG_LATENCY_PING = 0x3c;
const NET_INPUT_MSG_SIZE = 22;
const NET_LATENCY_PING_SIZE = 9;
const NET_EVENT_RECORD_SIZE = 18;
const NET_INPUT_THRUST = 1 << 0;
const NET_ACTION_LAUNCH = 2;

const MESSAGE_NAMES = new Map([
  [0x01, 'JOIN'],
  [0x02, 'LEAVE'],
  [0x03, 'STATE'],
  [0x04, 'INPUT'],
  [0x10, 'WORLD_ASTEROIDS'],
  [0x11, 'WORLD_NPCS'],
  [0x12, 'WORLD_STATIONS'],
  [0x65, 'WORLD_STATIONS_Q'],
  [0x14, 'HOST_ASSIGN'],
  [0x15, 'PLAYER_SHIP'],
  [0x16, 'SERVER_INFO'],
  [0x17, 'STATION_IDENTITY'],
  [0x18, 'WORLD_PLAYERS'],
  [0x19, 'CONTRACTS'],
  [0x20, 'SESSION'],
  [0x22, 'WORLD_TIME'],
  [0x24, 'WORLD_SCAFFOLDS'],
  [0x25, 'HAIL_RESPONSE'],
  [0x26, 'EVENTS'],
  [0x27, 'SIGNAL_CHANNEL'],
  [0x2c, 'FRACTURE_CHALLENGE'],
  [0x2d, 'FRACTURE_CLAIM'],
  [0x2e, 'FRACTURE_RESOLVED'],
  [0x2f, 'STATION_MANIFEST'],
  [0x30, 'HIGHSCORES'],
  [0x31, 'PLAYER_MANIFEST'],
  [0x38, 'INSPECT_SNAPSHOT'],
  [0x39, 'PLAYER_KNOWN_CONTRACTS'],
  [0x3a, 'ACTION_ACK'],
  [0x3b, 'ACTION_RESULT'],
  [0x3c, 'LATENCY_PING'],
  [0x3d, 'LATENCY_PONG'],
  [0x40, 'STATION_DIAG'],
  [0x41, 'PROTOCOL_INFO'],
  [0x46, 'WORLD_CARGO_PODS'],
  [0x47, 'DELIVERY_LEDGER'],
  [0x48, 'INPUT_APPLIED'],
  [0x49, 'WORLD_INTERACTIONS'],
  [0x4a, 'PLAYER_KNOWN_LEDGER'],
  [0x4b, 'WORLD_ASTEROID_MOTION'],
  [0x4c, 'WORLD_ASTEROID_MOTION_Q'],
  [0x4d, 'WORLD_PLAYER_MOTION'],
  [0x4e, 'WORLD_NPC_MOTION'],
  [0x4f, 'WORLD_CARGO_POD_MOTION'],
  [0x50, 'WORLD_INTERACTION_DRIFT'],
  [0x51, 'WORLD_ASTEROID_STATE_Q'],
  [0x52, 'WORLD_NPC_MOTION_Q'],
  [0x53, 'WORLD_NPC_STATUS'],
  [0x54, 'WORLD_CARGO_POD_MOTION_Q'],
  [0x55, 'WORLD_ASTEROID_POS_Q'],
  [0x56, 'WORLD_ASTEROID_REMOVE'],
  [0x57, 'WORLD_CARGO_POD_REMOVE'],
  [0x58, 'WORLD_SCAFFOLD_REMOVE'],
  [0x59, 'WORLD_SCAFFOLD_MOTION_Q'],
  [0x5a, 'WORLD_NPC_POS_Q'],
  [0x5b, 'WORLD_NPC_POSE_Q'],
  [0x5c, 'WORLD_ASTEROID_POS8_Q'],
  [0x5d, 'WORLD_NPC_STATUS8_Q'],
  [0x5e, 'WORLD_NPC_LINEAR_Q'],
  [0x5f, 'WORLD_CARGO_POD_LINEAR_Q'],
  [0x60, 'WORLD_NPC_MOTION8_Q'],
  [0x61, 'WORLD_INTERACTIONS_Q'],
  [0x62, 'WORLD_CARGO_PODS_Q'],
  [0x63, 'STATION_IDENTITY_Q'],
  [0x64, 'CONTRACTS_Q'],
  [0x66, 'WORLD_ASTEROID_POSD_Q'],
  [0x67, 'WORLD_ASTEROID_POSD8_Q'],
  [0x68, 'WORLD_ASTEROIDS_Q'],
  [0x69, 'WORLD_ASTEROIDS8_Q'],
  [0x6a, 'WORLD_PLAYER_MOTION_Q'],
  [0x6b, 'WORLD_PLAYER_DOCK_Q'],
  [0x6c, 'WORLD_PLAYER_MOTIOND_Q'],
  [0x6d, 'WORLD_PLAYER_POSED_Q'],
  [0x6e, 'WORLD_PLAYER_MOTIONM_Q'],
  [0x70, 'PUBKEY_CHALLENGE'],
]);

const EVENT_NAMES = [
  'FRACTURE',
  'PICKUP',
  'MINING_TICK',
  'DOCK',
  'LAUNCH',
  'SELL',
  'BUY',
  'REPAIR',
  'UPGRADE',
  'DAMAGE',
  'OUTPOST_PLACED',
  'OUTPOST_ACTIVATED',
  'NPC_SPAWNED',
  'SIGNAL_LOST',
  'HAIL_RESPONSE',
  'MODULE_ACTIVATED',
  'STATION_CONNECTED',
  'CONTRACT_COMPLETE',
  'DEATH',
  'SCAFFOLD_READY',
  'ORDER_REJECTED',
  'NPC_KILL',
  'OPERATOR_POST',
];

function usage() {
  return `Usage: node scripts/relay-traffic-probe.mjs [options]

Options:
  --url=ws://127.0.0.1:9091/ws   Relay WebSocket URL
  --clients=2                    Synthetic clients to connect
  --warmup-ms=1500               Ignore join/snapshot traffic for this long
  --duration-ms=4000             Measurement window after warmup
  --ping-hz=0.5                  Send LATENCY_PING probes at this rate per client
  --input-hz=0                   Send movement input at this rate per client
  --input-ack-hz=2               Advance held-input ack sequence at this rate
  --input-flags=1                Movement flags for --input-hz, decimal or 0x hex
  --spoof-forwarded-for          Send a unique X-Forwarded-For per synthetic client
  --no-session                   Do not send SESSION after websocket upgrade
  --launch-after-ms=N            Send one launch action per client after N ms
  --json                         Print JSON only
  --help                         Show this help
`;
}

function parsePositiveInt(value, name) {
  const n = Number.parseInt(value, 10);
  if (!Number.isFinite(n) || n <= 0) {
    throw new Error(`${name} must be a positive integer`);
  }
  return n;
}

function parseNonNegativeInt(value, name) {
  const n = Number.parseInt(value, 10);
  if (!Number.isFinite(n) || n < 0) {
    throw new Error(`${name} must be a non-negative integer`);
  }
  return n;
}

function parseNonNegativeNumber(value, name) {
  const n = Number.parseFloat(value);
  if (!Number.isFinite(n) || n < 0) {
    throw new Error(`${name} must be a non-negative number`);
  }
  return n;
}

function parseByte(value, name) {
  const n = Number.parseInt(value, 0);
  if (!Number.isFinite(n) || n < 0 || n > 255) {
    throw new Error(`${name} must be a byte`);
  }
  return n;
}

function parseArgs(argv) {
  const opts = {
    url: 'ws://127.0.0.1:9091/ws',
    clients: 2,
    warmupMs: 1500,
    durationMs: 4000,
    pingHz: 0.5,
    inputHz: 0,
    inputAckHz: 1,
    inputFlags: NET_INPUT_THRUST,
    spoofForwardedFor: false,
    sendSession: true,
    launchAfterMs: -1,
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
    } else if (arg.startsWith('--clients=')) {
      opts.clients = parsePositiveInt(arg.slice('--clients='.length), '--clients');
    } else if (arg.startsWith('--warmup-ms=')) {
      opts.warmupMs = parseNonNegativeInt(arg.slice('--warmup-ms='.length), '--warmup-ms');
    } else if (arg.startsWith('--duration-ms=')) {
      opts.durationMs = parsePositiveInt(arg.slice('--duration-ms='.length), '--duration-ms');
    } else if (arg.startsWith('--ping-hz=')) {
      opts.pingHz = parseNonNegativeNumber(arg.slice('--ping-hz='.length), '--ping-hz');
    } else if (arg.startsWith('--input-hz=')) {
      opts.inputHz = parseNonNegativeNumber(arg.slice('--input-hz='.length), '--input-hz');
    } else if (arg.startsWith('--input-ack-hz=')) {
      opts.inputAckHz = parseNonNegativeNumber(arg.slice('--input-ack-hz='.length), '--input-ack-hz');
    } else if (arg.startsWith('--input-flags=')) {
      opts.inputFlags = parseByte(arg.slice('--input-flags='.length), '--input-flags');
    } else if (arg === '--spoof-forwarded-for') {
      opts.spoofForwardedFor = true;
    } else if (arg === '--no-session') {
      opts.sendSession = false;
    } else if (arg.startsWith('--launch-after-ms=')) {
      opts.launchAfterMs = parseNonNegativeInt(
        arg.slice('--launch-after-ms='.length), '--launch-after-ms');
    } else {
      throw new Error(`unknown option: ${arg}`);
    }
  }

  return opts;
}

function wsAccept(key) {
  return crypto.createHash('sha1').update(key + GUID).digest('base64');
}

function hexType(type) {
  return `0x${type.toString(16).padStart(2, '0')}`;
}

function msgName(type) {
  return MESSAGE_NAMES.get(type) ?? `UNKNOWN_${hexType(type)}`;
}

function makeStats() {
  return {
    totalPackets: 0,
    totalPayloadBytes: 0,
    byType: new Map(),
    byClient: new Map(),
    latencyPong: {
      count: 0,
      rawSumMs: 0,
      transportSumMs: 0,
      serverTurnaroundSumMs: 0,
      rawMinMs: null,
      rawMaxMs: 0,
      transportMinMs: null,
      transportMaxMs: 0,
      serverTurnaroundMaxMs: 0,
    },
  };
}

function getClientEntry(stats, clientId) {
  const entry = stats.byClient.get(clientId) ?? {
    clientId,
    packets: 0,
    payloadBytes: 0,
    hasPosition: false,
    x: 0,
    y: 0,
  };
  stats.byClient.set(clientId, entry);
  return entry;
}

function emptyDistanceBuckets() {
  return { near: 0, mid: 0, veryFar: 0, unknown: 0 };
}

function emptyMotionSpeedBuckets() {
  return { crawl: 0, slow: 0, medium: 0, fast: 0 };
}

function emptyAsteroidFullRecords() {
  return { active: 0, removal: 0 };
}

function eventName(type) {
  return EVENT_NAMES[type] ?? `UNKNOWN_${type}`;
}

function addEventTypeCount(map, type, count = 1) {
  const name = eventName(type);
  map[name] = (map[name] ?? 0) + count;
}

function addDistanceBucket(buckets, clientEntry, x, y) {
  if (!clientEntry.hasPosition) {
    buckets.unknown += 1;
    return;
  }
  const dx = x - clientEntry.x;
  const dy = y - clientEntry.y;
  const distSq = dx * dx + dy * dy;
  if (distSq <= 1200 * 1200) buckets.near += 1;
  else if (distSq < 2000 * 2000) buckets.mid += 1;
  else buckets.veryFar += 1;
}

function addMotionSpeedBucket(buckets, vx, vy) {
  const speedSq = vx * vx + vy * vy;
  if (speedSq >= 30 * 30) buckets.fast += 1;
  else if (speedSq >= 10 * 10) buckets.medium += 1;
  else if (speedSq >= 1 * 1) buckets.slow += 1;
  else buckets.crawl += 1;
}

function decodeRecordInfo(payload, clientEntry) {
  const type = payload[0];
  if (type === 0x03 && payload.length >= 10) {
    const id = payload[1];
    if (id === clientEntry.clientId) {
      clientEntry.x = payload.readFloatLE(2);
      clientEntry.y = payload.readFloatLE(6);
      clientEntry.hasPosition = true;
    }
    return null;
  }

  if (type === 0x18 && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 77;
    const expected = 2 + count * recordSize;
    if (payload.length >= expected) {
      for (let i = 0; i < count; i++) {
        const off = 2 + i * recordSize;
        if (payload[off] !== clientEntry.clientId) continue;
        clientEntry.x = payload.readFloatLE(off + 1);
        clientEntry.y = payload.readFloatLE(off + 5);
        clientEntry.hasPosition = true;
        break;
      }
    }
    return { records: count };
  }

  if (type === 0x4d && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 21;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x6a && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 10;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x6c && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 6;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x6d && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 4;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x6e && payload.length >= 2) {
    const count = payload[1];
    let off = 2;
    for (let i = 0; i < count; i++) {
      if (off + 4 > payload.length) return null;
      const idFlags = payload[off];
      off += (idFlags & 0x80) !== 0 ? 6 : 4;
      if (off > payload.length) return null;
    }
    return { records: count };
  }

  if (type === 0x6b && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 2;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x4e && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 22;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x52 && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 12;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x60 && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 9;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x53 && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 6;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x5d && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 4;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x5a && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 5;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x5b && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 7;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x5e && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 9;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x5f && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 9;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x54 && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 11;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x50 && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 12;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x4b && payload.length >= 3) {
    const count = payload.readUInt16LE(1);
    const recordSize = 18;
    const expected = 3 + count * recordSize;
    const buckets = emptyDistanceBuckets();
    const speedBuckets = emptyMotionSpeedBuckets();
    if (payload.length >= expected) {
      for (let i = 0; i < count; i++) {
        const off = 3 + i * recordSize;
        addDistanceBucket(
          buckets,
          clientEntry,
          payload.readFloatLE(off + 2),
          payload.readFloatLE(off + 6)
        );
        addMotionSpeedBucket(
          speedBuckets,
          payload.readFloatLE(off + 10),
          payload.readFloatLE(off + 14)
        );
      }
    }
    return { records: count, distanceBuckets: buckets, speedBuckets };
  }

  if (type === 0x4c && payload.length >= 3) {
    const count = payload.readUInt16LE(1);
    const recordSize = 10;
    const expected = 3 + count * recordSize;
    const buckets = emptyDistanceBuckets();
    const speedBuckets = emptyMotionSpeedBuckets();
    if (payload.length >= expected) {
      for (let i = 0; i < count; i++) {
        const off = 3 + i * recordSize;
        addDistanceBucket(
          buckets,
          clientEntry,
          payload.readInt16LE(off + 2) * 4.0,
          payload.readInt16LE(off + 4) * 4.0
        );
        addMotionSpeedBucket(
          speedBuckets,
          payload.readInt16LE(off + 6) * 0.25,
          payload.readInt16LE(off + 8) * 0.25
        );
      }
    }
    return { records: count, distanceBuckets: buckets, speedBuckets };
  }

  if (type === 0x55 && payload.length >= 3) {
    const count = payload.readUInt16LE(1);
    const recordSize = 6;
    const expected = 3 + count * recordSize;
    const buckets = emptyDistanceBuckets();
    if (payload.length >= expected) {
      for (let i = 0; i < count; i++) {
        const off = 3 + i * recordSize;
        addDistanceBucket(
          buckets,
          clientEntry,
          payload.readInt16LE(off + 2) * 4.0,
          payload.readInt16LE(off + 4) * 4.0
        );
      }
    }
    return { records: count, distanceBuckets: buckets };
  }

  if (type === 0x5c && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 5;
    const expected = 2 + count * recordSize;
    const buckets = emptyDistanceBuckets();
    if (payload.length >= expected) {
      for (let i = 0; i < count; i++) {
        const off = 2 + i * recordSize;
        addDistanceBucket(
          buckets,
          clientEntry,
          payload.readInt16LE(off + 1) * 4.0,
          payload.readInt16LE(off + 3) * 4.0
        );
      }
    }
    return { records: count, distanceBuckets: buckets };
  }

  if (type === 0x66 && payload.length >= 3) {
    const count = payload.readUInt16LE(1);
    const recordSize = 4;
    const expected = 3 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x67 && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 3;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x10 && payload.length >= 3) {
    const count = payload.readUInt16LE(1);
    const recordSize = 35;
    const expected = 3 + count * recordSize;
    const buckets = emptyDistanceBuckets();
    const asteroidFullRecords = emptyAsteroidFullRecords();
    if (payload.length >= expected) {
      for (let i = 0; i < count; i++) {
        const off = 3 + i * recordSize;
        const active = (payload[off + 2] & 1) !== 0;
        if (active) {
          asteroidFullRecords.active += 1;
          addDistanceBucket(
            buckets,
            clientEntry,
            payload.readFloatLE(off + 3),
            payload.readFloatLE(off + 7)
          );
        } else {
          asteroidFullRecords.removal += 1;
        }
      }
    }
    return { records: count, distanceBuckets: buckets, asteroidFullRecords };
  }

  if (type === 0x68 && payload.length >= 3) {
    const count = payload.readUInt16LE(1);
    const recordSize = 19;
    const expected = 3 + count * recordSize;
    const buckets = emptyDistanceBuckets();
    const asteroidFullRecords = emptyAsteroidFullRecords();
    if (payload.length >= expected) {
      asteroidFullRecords.active = count;
      for (let i = 0; i < count; i++) {
        const off = 3 + i * recordSize;
        addDistanceBucket(
          buckets,
          clientEntry,
          payload.readInt16LE(off + 3) * 4.0,
          payload.readInt16LE(off + 5) * 4.0
        );
      }
    }
    return payload.length >= expected
      ? { records: count, distanceBuckets: buckets, asteroidFullRecords }
      : null;
  }

  if (type === 0x69 && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 18;
    const expected = 2 + count * recordSize;
    const buckets = emptyDistanceBuckets();
    const asteroidFullRecords = emptyAsteroidFullRecords();
    if (payload.length >= expected) {
      asteroidFullRecords.active = count;
      for (let i = 0; i < count; i++) {
        const off = 2 + i * recordSize;
        addDistanceBucket(
          buckets,
          clientEntry,
          payload.readInt16LE(off + 2) * 4.0,
          payload.readInt16LE(off + 4) * 4.0
        );
      }
    }
    return payload.length >= expected
      ? { records: count, distanceBuckets: buckets, asteroidFullRecords }
      : null;
  }

  if (type === 0x51 && payload.length >= 3) {
    const count = payload.readUInt16LE(1);
    const recordSize = 18;
    const expected = 3 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x56 && payload.length >= 3) {
    const count = payload.readUInt16LE(1);
    const recordSize = 2;
    const expected = 3 + count * recordSize;
    const asteroidFullRecords = emptyAsteroidFullRecords();
    if (payload.length >= expected) {
      asteroidFullRecords.removal = count;
    }
    return payload.length >= expected
      ? { records: count, asteroidFullRecords }
      : null;
  }

  if (type === 0x57 && payload.length >= 2) {
    const count = payload[1];
    const expected = 2 + count;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x58 && payload.length >= 2) {
    const count = payload[1];
    const expected = 2 + count;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x59 && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 9;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x61 && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 25;
    const expected = 2 + count * recordSize;
    return payload.length >= expected ? { records: count } : null;
  }

  if (type === 0x62 && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 62;
    const expected = 2 + count * recordSize;
    return payload.length === expected ? { records: count } : null;
  }

  if (type === 0x46 && payload.length >= 2) {
    const count = payload[1];
    const recordSize = 72;
    const expected = 2 + count * recordSize;
    return payload.length === expected ? { records: count } : null;
  }

  if (type === 0x64 && payload.length >= 2) {
    return { records: payload[1] };
  }

  if (type === 0x65 && payload.length >= 2) {
    return { records: payload[1] };
  }

  if (type === NET_MSG_EVENTS && payload.length >= 2) {
    const count = payload[1];
    const expected = 2 + count * NET_EVENT_RECORD_SIZE;
    if (payload.length < expected) return null;
    const eventTypes = {};
    for (let i = 0; i < count; i++) {
      const off = 2 + i * NET_EVENT_RECORD_SIZE;
      addEventTypeCount(eventTypes, payload[off]);
    }
    return { records: count, eventTypes };
  }

  if ((type === 0x11 || type === 0x12 || type === 0x19 ||
       type === 0x24 || type === 0x49) &&
      payload.length >= 2) {
    return { records: payload[1] };
  }

  return null;
}

function recordStats(stats, clientId, payload) {
  if (!payload || payload.length === 0) return;
  const type = payload[0];
  const byClient = getClientEntry(stats, clientId);
  const recordInfo = decodeRecordInfo(payload, byClient);

  stats.totalPackets += 1;
  stats.totalPayloadBytes += payload.length;

  const byType = stats.byType.get(type) ?? {
    type,
    hex: hexType(type),
    name: msgName(type),
    packets: 0,
    payloadBytes: 0,
    records: 0,
    recordPackets: 0,
    minRecords: null,
    maxRecords: 0,
    distanceBuckets: emptyDistanceBuckets(),
    speedBuckets: emptyMotionSpeedBuckets(),
    asteroidFullRecords: emptyAsteroidFullRecords(),
    eventTypes: {},
  };
  byType.packets += 1;
  byType.payloadBytes += payload.length;
  if (recordInfo && Number.isFinite(recordInfo.records)) {
    byType.records += recordInfo.records;
    byType.recordPackets += 1;
    byType.minRecords = byType.minRecords === null
      ? recordInfo.records
      : Math.min(byType.minRecords, recordInfo.records);
    byType.maxRecords = Math.max(byType.maxRecords, recordInfo.records);
    if (recordInfo.distanceBuckets) {
      byType.distanceBuckets.near += recordInfo.distanceBuckets.near;
      byType.distanceBuckets.mid += recordInfo.distanceBuckets.mid;
      byType.distanceBuckets.veryFar += recordInfo.distanceBuckets.veryFar;
      byType.distanceBuckets.unknown += recordInfo.distanceBuckets.unknown;
    }
    if (recordInfo.speedBuckets) {
      byType.speedBuckets.crawl += recordInfo.speedBuckets.crawl;
      byType.speedBuckets.slow += recordInfo.speedBuckets.slow;
      byType.speedBuckets.medium += recordInfo.speedBuckets.medium;
      byType.speedBuckets.fast += recordInfo.speedBuckets.fast;
    }
    if (recordInfo.asteroidFullRecords) {
      byType.asteroidFullRecords.active += recordInfo.asteroidFullRecords.active;
      byType.asteroidFullRecords.removal += recordInfo.asteroidFullRecords.removal;
    }
    if (recordInfo.eventTypes) {
      for (const [eventType, count] of Object.entries(recordInfo.eventTypes)) {
        byType.eventTypes[eventType] = (byType.eventTypes[eventType] ?? 0) + count;
      }
    }
  }
  stats.byType.set(type, byType);

  if (type === 0x3d && payload.length >= 17) {
    const nowMs = Date.now() >>> 0;
    const clientSentMs = payload.readUInt32LE(5);
    const serverRecvMs = payload.readUInt32LE(9);
    const serverSendMs = payload.readUInt32LE(13);
    const rawMs = (nowMs - clientSentMs) >>> 0;
    const serverTurnaroundMs = (serverSendMs - serverRecvMs) >>> 0;
    const transportMs = serverTurnaroundMs > rawMs
      ? rawMs
      : rawMs - serverTurnaroundMs;
    if (rawMs > 0 && rawMs < 30000 && transportMs > 0) {
      const latency = stats.latencyPong;
      latency.count += 1;
      latency.rawSumMs += rawMs;
      latency.transportSumMs += transportMs;
      latency.serverTurnaroundSumMs += serverTurnaroundMs;
      latency.rawMinMs = latency.rawMinMs === null
        ? rawMs
        : Math.min(latency.rawMinMs, rawMs);
      latency.rawMaxMs = Math.max(latency.rawMaxMs, rawMs);
      latency.transportMinMs = latency.transportMinMs === null
        ? transportMs
        : Math.min(latency.transportMinMs, transportMs);
      latency.transportMaxMs = Math.max(latency.transportMaxMs, transportMs);
      latency.serverTurnaroundMaxMs =
        Math.max(latency.serverTurnaroundMaxMs, serverTurnaroundMs);
    }
  }

  byClient.packets += 1;
  byClient.payloadBytes += payload.length;
}

function carryClientPositions(fromStats, toStats) {
  for (const entry of fromStats.byClient.values()) {
    if (!entry.hasPosition) continue;
    toStats.byClient.set(entry.clientId, {
      clientId: entry.clientId,
      packets: 0,
      payloadBytes: 0,
      hasPosition: true,
      x: entry.x,
      y: entry.y,
    });
  }
}

function sortedEntries(map) {
  return [...map.values()].sort((a, b) => {
    if (b.payloadBytes !== a.payloadBytes) return b.payloadBytes - a.payloadBytes;
    return b.packets - a.packets;
  });
}

function summarizeStats(stats, elapsedMs) {
  const seconds = Math.max(elapsedMs / 1000, 0.001);
  const latency = stats.latencyPong;
  return {
    elapsedMs: Math.round(elapsedMs),
    totalPackets: stats.totalPackets,
    totalPayloadBytes: stats.totalPayloadBytes,
    packetsPerSec: stats.totalPackets / seconds,
    payloadBytesPerSec: stats.totalPayloadBytes / seconds,
    latencyPong: latency.count > 0 ? {
      count: latency.count,
      rawAvgMs: latency.rawSumMs / latency.count,
      rawMinMs: latency.rawMinMs,
      rawMaxMs: latency.rawMaxMs,
      transportAvgMs: latency.transportSumMs / latency.count,
      transportMinMs: latency.transportMinMs,
      transportMaxMs: latency.transportMaxMs,
      serverTurnaroundAvgMs: latency.serverTurnaroundSumMs / latency.count,
      serverTurnaroundMaxMs: latency.serverTurnaroundMaxMs,
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
    },
    byType: sortedEntries(stats.byType).map((entry) => ({
      ...entry,
      packetsPerSec: entry.packets / seconds,
      payloadBytesPerSec: entry.payloadBytes / seconds,
      recordsPerSec: entry.records / seconds,
      avgRecordsPerPacket: entry.recordPackets > 0
        ? entry.records / entry.recordPackets
        : 0,
    })),
    byClient: sortedEntries(stats.byClient).map((entry) => ({
      ...entry,
      packetsPerSec: entry.packets / seconds,
      payloadBytesPerSec: entry.payloadBytes / seconds,
    })),
  };
}

function wsClientFrame(payload, opcode = 0x2) {
  let header;
  if (payload.length < 126) {
    header = Buffer.from([0x80 | opcode, 0x80 | payload.length]);
  } else if (payload.length <= 0xffff) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | opcode;
    header[1] = 0x80 | 126;
    header.writeUInt16BE(payload.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | opcode;
    header[1] = 0x80 | 127;
    header.writeBigUInt64BE(BigInt(payload.length), 2);
  }

  const mask = crypto.randomBytes(4);
  const masked = Buffer.alloc(payload.length);
  for (let i = 0; i < payload.length; i++) {
    masked[i] = payload[i] ^ mask[i & 3];
  }
  return Buffer.concat([header, mask, masked]);
}

function safeSocketWrite(socket, data) {
  if (!socket || socket.destroyed || socket.writableEnded) return false;
  try {
    socket.write(data);
    return true;
  } catch (err) {
    if (err?.code === 'EPIPE' ||
        err?.code === 'ECONNRESET' ||
        err?.code === 'ERR_STREAM_WRITE_AFTER_END') {
      return false;
    }
    throw err;
  }
}

function makeInputPayload(client, flags, advanceSeq, action = 0) {
  if (advanceSeq || client.inputSeq === 0) {
    client.inputSeq = (client.inputSeq + 1) & 0xffff;
    if (client.inputSeq === 0) client.inputSeq = 1;
  }
  client.inputTick += 1;

  const payload = Buffer.alloc(NET_INPUT_MSG_SIZE);
  payload[0] = NET_MSG_INPUT;
  payload[1] = flags;
  payload[2] = action;
  payload[3] = 0xff;
  payload[4] = 0;
  payload[5] = 0xff;
  payload[6] = 0xff;
  payload[7] = 0xff;
  payload.writeUInt16LE(client.inputSeq, 8);
  payload.writeUInt16LE(0xffff, 10);
  payload.writeUInt16LE(0, 12);
  payload.writeUInt32LE(0, 14);
  payload.writeUInt32LE(Date.now() >>> 0, 18);
  return payload;
}

function makeLatencyPingPayload(client) {
  client.latencyPingSeq = (client.latencyPingSeq + 1) >>> 0;
  if (client.latencyPingSeq === 0)
    client.latencyPingSeq = (client.latencyPingSeq + 1) >>> 0;

  const payload = Buffer.alloc(NET_LATENCY_PING_SIZE);
  payload[0] = NET_MSG_LATENCY_PING;
  payload.writeUInt32LE(client.latencyPingSeq, 1);
  payload.writeUInt32LE(Date.now() >>> 0, 5);
  return payload;
}

function syntheticForwardedIp(clientId) {
  const n = clientId + 1;
  const third = 100 + Math.floor((n - 1) / 254);
  const fourth = ((n - 1) % 254) + 1;
  return `198.51.${third}.${fourth}`;
}

function startPingPump(clients, hz) {
  if (!hz) return () => {};
  const intervalMs = Math.max(1, Math.round(1000 / hz));
  const sendAll = () => {
    for (const client of clients) client.sendLatencyPing();
  };
  sendAll();
  const timer = setInterval(sendAll, intervalMs);
  timer.unref?.();
  return () => clearInterval(timer);
}

function startInputPump(clients, hz, ackHz, flags) {
  if (!hz) return () => {};
  const intervalMs = Math.max(1, Math.round(1000 / hz));
  const ackIntervalMs = ackHz > 0
    ? Math.max(1, Math.round(1000 / ackHz))
    : Number.POSITIVE_INFINITY;
  const timer = setInterval(() => {
    const now = performance.now();
    for (const client of clients) {
      const advanceSeq = client.inputSeq === 0 ||
        now >= client.nextInputAckAt;
      client.sendInput(flags, advanceSeq);
      if (advanceSeq)
        client.nextInputAckAt = now + ackIntervalMs;
    }
  }, intervalMs);
  timer.unref?.();
  return () => clearInterval(timer);
}

function startLaunchTimer(clients, delayMs) {
  if (delayMs < 0) return () => {};
  const timer = setTimeout(() => {
    for (const client of clients)
      client.sendInput(0, true, NET_ACTION_LAUNCH);
  }, delayMs);
  timer.unref?.();
  return () => clearTimeout(timer);
}

function installFrameReader(socket, onPayload) {
  let buffer = Buffer.alloc(0);
  let closed = false;

  function push(chunk) {
    if (closed) return;
    buffer = buffer.length ? Buffer.concat([buffer, chunk]) : chunk;

    for (;;) {
      if (buffer.length < 2) return;
      const first = buffer[0];
      const second = buffer[1];
      const opcode = first & 0x0f;
      const masked = (second & 0x80) !== 0;
      let len = second & 0x7f;
      let off = 2;

      if (len === 126) {
        if (buffer.length < off + 2) return;
        len = buffer.readUInt16BE(off);
        off += 2;
      } else if (len === 127) {
        if (buffer.length < off + 8) return;
        const bigLen = buffer.readBigUInt64BE(off);
        if (bigLen > BigInt(Number.MAX_SAFE_INTEGER)) {
          throw new Error('incoming websocket frame is too large');
        }
        len = Number(bigLen);
        off += 8;
      }

      let mask = null;
      if (masked) {
        if (buffer.length < off + 4) return;
        mask = buffer.subarray(off, off + 4);
        off += 4;
      }

      if (buffer.length < off + len) return;
      let payload = buffer.subarray(off, off + len);
      if (mask) {
        const unmasked = Buffer.alloc(payload.length);
        for (let i = 0; i < payload.length; i++) {
          unmasked[i] = payload[i] ^ mask[i & 3];
        }
        payload = unmasked;
      }
      buffer = buffer.subarray(off + len);

      if (opcode === 0x8) {
        closed = true;
        socket.end();
        return;
      }
      if (opcode === 0x2) onPayload(payload);
      if (opcode === 0x9) safeSocketWrite(socket, wsClientFrame(payload, 0xA));
    }
  }

  socket.on('data', push);
  return { push };
}

function connectSocket(target) {
  const isTls = target.protocol === 'wss:';
  const port = Number(target.port || (isTls ? 443 : 80));
  const host = target.hostname;
  const connectOptions = { host, port };
  const socket = isTls
    ? tls.connect({ ...connectOptions, servername: host })
    : net.connect(connectOptions);

  return new Promise((resolve, reject) => {
    const event = isTls ? 'secureConnect' : 'connect';
    const timer = setTimeout(() => {
      socket.destroy();
      reject(new Error(`timed out connecting to ${host}:${port}`));
    }, 5000);
    function fail(err) {
      clearTimeout(timer);
      reject(err);
    }
    socket.once('error', fail);
    socket.once(event, () => {
      clearTimeout(timer);
      socket.off('error', fail);
      socket.setNoDelay(true);
      resolve(socket);
    });
  });
}

async function connectClient(clientId, urlText, opts, onPayload, onSentPayload, onUnexpectedClose) {
  const target = new URL(urlText);
  if (target.protocol !== 'ws:' && target.protocol !== 'wss:') {
    throw new Error(`unsupported websocket protocol: ${target.protocol}`);
  }

  const socket = await connectSocket(target);
  const key = crypto.randomBytes(16).toString('base64');
  const path = `${target.pathname || '/'}${target.search}`;
  const expectedAccept = wsAccept(key);
  let reader = null;
  let closing = false;
  let closeReported = false;

  function reportClose() {
    if (closing || closeReported) return;
    closeReported = true;
    onUnexpectedClose(clientId);
  }

  await new Promise((resolve, reject) => {
    let responseBytes = Buffer.alloc(0);
    const timer = setTimeout(() => {
      socket.destroy();
      reject(new Error(`timed out during websocket handshake for client ${clientId}`));
    }, 5000);

    function fail(err) {
      clearTimeout(timer);
      socket.off('data', onData);
      socket.destroy();
      reject(err);
    }

    function onData(chunk) {
      responseBytes = responseBytes.length
        ? Buffer.concat([responseBytes, chunk])
        : chunk;
      const end = responseBytes.indexOf('\r\n\r\n');
      if (end < 0) return;

      const response = responseBytes.subarray(0, end).toString('latin1');
      if (!/^HTTP\/1\.[01] 101\b/.test(response)) {
        fail(new Error(`websocket upgrade failed for client ${clientId}: ${response.split('\r\n')[0]}`));
        return;
      }
      const accept = response.match(/\r\nSec-WebSocket-Accept: ([^\r\n]+)/i)?.[1];
      if (accept !== expectedAccept) {
        fail(new Error(`bad Sec-WebSocket-Accept for client ${clientId}`));
        return;
      }

      clearTimeout(timer);
      socket.off('data', onData);
      socket.off('error', fail);
      reader = installFrameReader(socket, (payload) => onPayload(clientId, payload));
      const leftover = responseBytes.subarray(end + 4);
      if (leftover.length) reader.push(leftover);
      resolve();
    }

    socket.on('data', onData);
    socket.once('error', fail);
    const forwardedFor = opts.spoofForwardedFor
      ? `X-Forwarded-For: ${syntheticForwardedIp(clientId)}\r\n`
      : '';
    if (!safeSocketWrite(socket,
      `GET ${path || '/'} HTTP/1.1\r\n` +
      `Host: ${target.host}\r\n` +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      `Sec-WebSocket-Key: ${key}\r\n` +
      forwardedFor +
      'Sec-WebSocket-Version: 13\r\n' +
      '\r\n'
    )) {
      fail(new Error(`socket closed before websocket handshake for client ${clientId}`));
    }
  });

  socket.on('error', reportClose);
  socket.on('close', () => {
    reportClose();
  });

  if (opts.sendSession) {
    const session = Buffer.alloc(18);
    session[0] = NET_MSG_SESSION;
    crypto.randomBytes(8).copy(session, 1);
    Buffer.from(`P${String(clientId).padStart(6, '0')}`).copy(session, 9, 0, 7);
    session.writeUInt16LE(4, 16);
    if (safeSocketWrite(socket, wsClientFrame(session))) {
      onSentPayload(clientId, session);
    } else {
      reportClose();
    }
  }

  return {
    clientId,
    inputSeq: 0,
    inputTick: 0,
    nextInputAckAt: 0,
    latencyPingSeq: 0,
    sendInput(flags, advanceSeq, action = 0) {
      const payload = makeInputPayload(this, flags, advanceSeq, action);
      if (safeSocketWrite(socket, wsClientFrame(payload)))
        onSentPayload(clientId, payload);
    },
    sendLatencyPing() {
      const payload = makeLatencyPingPayload(this);
      if (safeSocketWrite(socket, wsClientFrame(payload)))
        onSentPayload(clientId, payload);
    },
    close() {
      closing = true;
      if (!socket.destroyed) {
        safeSocketWrite(socket, wsClientFrame(Buffer.alloc(0), 0x8));
        socket.end();
      }
    },
  };
}

function printHuman(summary) {
  console.log(`Signal relay traffic probe`);
  console.log(`url=${summary.url} clients=${summary.connectedClients}/${summary.clients} warmup=${summary.warmupMs}ms sample=${summary.durationMs}ms pingHz=${summary.pingHz} inputHz=${summary.inputHz} inputAckHz=${summary.inputAckHz} spoofForwardedFor=${summary.spoofForwardedFor} session=${summary.sendSession} launchAfterMs=${summary.launchAfterMs}`);
  console.log(`rx payload=${summary.measurement.totalPayloadBytes} B packets=${summary.measurement.totalPackets} rate=${summary.measurement.payloadBytesPerSec.toFixed(1)} B/s`);
  console.log(`tx payload=${summary.sentMeasurement.totalPayloadBytes} B packets=${summary.sentMeasurement.totalPackets} rate=${summary.sentMeasurement.payloadBytesPerSec.toFixed(1)} B/s`);
  const lp = summary.measurement.latencyPong;
  if (lp.count > 0) {
    console.log(
      `latency pong samples=${lp.count} ` +
      `transport avg/min/max=${lp.transportAvgMs.toFixed(1)}/` +
      `${lp.transportMinMs}/${lp.transportMaxMs} ms ` +
      `raw avg/max=${lp.rawAvgMs.toFixed(1)}/${lp.rawMaxMs} ms ` +
      `server-turnaround avg/max=${lp.serverTurnaroundAvgMs.toFixed(1)}/` +
      `${lp.serverTurnaroundMaxMs} ms`
    );
  }
  if (summary.connectionFailures.length) {
    console.log(`connection failures: ${summary.connectionFailures.map((f) => `${f.clientId}:${f.error}`).join(', ')}`);
  }
  if (summary.unexpectedCloses.length) {
    console.log(`unexpected closes: ${summary.unexpectedCloses.join(', ')}`);
  }
  console.log('');
  console.log('rx type                      packets       pps        bytes        B/s     records     avg');
  for (const entry of summary.measurement.byType) {
    const label = `${entry.name} (${entry.hex})`.padEnd(28);
    console.log(
      `${label}` +
      `${String(entry.packets).padStart(8)}` +
      `${entry.packetsPerSec.toFixed(1).padStart(10)}` +
      `${String(entry.payloadBytes).padStart(12)}` +
      `${entry.payloadBytesPerSec.toFixed(1).padStart(11)}` +
      `${entry.recordPackets ? String(entry.records).padStart(12) : ''.padStart(12)}` +
      `${entry.recordPackets ? entry.avgRecordsPerPacket.toFixed(1).padStart(8) : ''.padStart(8)}`
    );
    const b = entry.distanceBuckets;
    if (b && (b.near || b.mid || b.veryFar || b.unknown)) {
      console.log(
        `${''.padEnd(28)}` +
        ` records: near=${b.near} mid=${b.mid} veryFar=${b.veryFar}` +
        (b.unknown ? ` unknown=${b.unknown}` : '')
      );
    }
    const speed = entry.speedBuckets;
    if (speed && (speed.crawl || speed.slow || speed.medium || speed.fast)) {
      console.log(
        `${''.padEnd(28)}` +
        ` speed: crawl=${speed.crawl} slow=${speed.slow}` +
        ` medium=${speed.medium} fast=${speed.fast}`
      );
    }
    const full = entry.asteroidFullRecords;
    if ((entry.name === 'WORLD_ASTEROIDS' ||
         entry.name === 'WORLD_ASTEROID_REMOVE') && full &&
        (full.active || full.removal)) {
      console.log(
        `${''.padEnd(28)}` +
        ` full-records: active=${full.active} removal=${full.removal}`
      );
    }
  }
  if (summary.sentMeasurement.byType.length) {
    console.log('');
    console.log('tx type                      packets       pps        bytes        B/s');
    for (const entry of summary.sentMeasurement.byType) {
      const label = `${entry.name} (${entry.hex})`.padEnd(28);
      console.log(
        `${label}` +
        `${String(entry.packets).padStart(8)}` +
        `${entry.packetsPerSec.toFixed(1).padStart(10)}` +
        `${String(entry.payloadBytes).padStart(12)}` +
        `${entry.payloadBytesPerSec.toFixed(1).padStart(11)}`
      );
    }
  }
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (opts.help) {
    process.stdout.write(usage());
    return;
  }

  const warmupStats = makeStats();
  const measurementStats = makeStats();
  const sentWarmupStats = makeStats();
  const sentMeasurementStats = makeStats();
  let activeStats = warmupStats;
  let activeSentStats = sentWarmupStats;
  const connectionFailures = [];
  const unexpectedCloses = [];

  const clients = [];
  try {
    for (let i = 0; i < opts.clients; i++) {
      try {
        clients.push(await connectClient(
          i,
          opts.url,
          opts,
          (clientId, payload) => recordStats(activeStats, clientId, payload),
          (clientId, payload) => recordStats(activeSentStats, clientId, payload),
          (clientId) => unexpectedCloses.push(clientId)
        ));
      } catch (err) {
        connectionFailures.push({
          clientId: i,
          error: err?.message ?? String(err),
        });
      }
    }
    if (clients.length === 0) {
      throw new Error(`no clients connected to ${opts.url}`);
    }

    const stopPingPump = startPingPump(clients, opts.pingHz);
    const stopInputPump = startInputPump(
      clients, opts.inputHz, opts.inputAckHz, opts.inputFlags);
    const stopLaunchTimer = startLaunchTimer(
      clients, opts.sendSession ? opts.launchAfterMs : -1);
    await sleep(opts.warmupMs);
    carryClientPositions(warmupStats, measurementStats);
    activeStats = measurementStats;
    activeSentStats = sentMeasurementStats;
    const start = performance.now();
    await sleep(opts.durationMs);
    const elapsedMs = performance.now() - start;
    stopLaunchTimer();
    stopInputPump();
    stopPingPump();
    activeStats = makeStats();
    activeSentStats = makeStats();

    const summary = {
      url: opts.url,
      clients: opts.clients,
      connectedClients: clients.length,
      warmupMs: opts.warmupMs,
      durationMs: opts.durationMs,
      pingHz: opts.pingHz,
      inputHz: opts.inputHz,
      inputAckHz: opts.inputAckHz,
      inputFlags: opts.inputFlags,
      spoofForwardedFor: opts.spoofForwardedFor,
      sendSession: opts.sendSession,
      launchAfterMs: opts.launchAfterMs,
      connectionFailures,
      unexpectedCloses,
      warmup: summarizeStats(warmupStats, opts.warmupMs),
      measurement: summarizeStats(measurementStats, elapsedMs),
      sentWarmup: summarizeStats(sentWarmupStats, opts.warmupMs),
      sentMeasurement: summarizeStats(sentMeasurementStats, elapsedMs),
    };

    if (opts.json) {
      console.log(JSON.stringify(summary, null, 2));
    } else {
      printHuman(summary);
    }

    if (connectionFailures.length || unexpectedCloses.length) {
      process.exitCode = 1;
    }
  } finally {
    for (const client of clients) client.close();
  }
}

if (process.argv[1] &&
    import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  main().catch((err) => {
    console.error(`[relay-traffic-probe] ${err.message}`);
    process.exitCode = 1;
  });
}

export {
  decodeRecordInfo,
};
