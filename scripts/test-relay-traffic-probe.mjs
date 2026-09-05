import assert from 'node:assert/strict';
import test from 'node:test';

import {
  decodeRecordInfo,
} from './relay-traffic-probe.mjs';

function cargoPodQPacket(count, trailingBytes = 0) {
    const packet = Buffer.alloc(2 + count * 67 + trailingBytes);
    packet[0] = 0x62;
    packet[1] = count;
    return packet;
}

function cargoPodPacket(count, trailingBytes = 0) {
  const packet = Buffer.alloc(2 + count * 77 + trailingBytes);
  packet[0] = 0x46;
  packet[1] = count;
  return packet;
}

test('relay probe accepts only exact v8 compact cargo-pod records', () => {
  const client = {};
  assert.deepEqual(
    decodeRecordInfo(cargoPodQPacket(2), client),
    { records: 2 });
  assert.equal(
    decodeRecordInfo(cargoPodQPacket(2).subarray(0, -1), client),
    null);
  assert.equal(
    decodeRecordInfo(cargoPodQPacket(2, 1), client),
    null);
});

test('relay probe accepts only exact v8 full cargo-pod records', () => {
  const client = {};
  assert.deepEqual(
    decodeRecordInfo(cargoPodPacket(2), client),
    { records: 2 });
  assert.equal(
    decodeRecordInfo(cargoPodPacket(2).subarray(0, -1), client),
    null);
  assert.equal(
    decodeRecordInfo(cargoPodPacket(2, 1), client),
    null);
});
