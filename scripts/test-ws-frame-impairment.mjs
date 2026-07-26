#!/usr/bin/env node
import assert from 'node:assert/strict';

import {
  createDeterministicUnitRandom,
  createFrameImpairment,
} from './ws-frame-impairment.mjs';

function byte(value) {
  return Buffer.from([value]);
}

function values(frames) {
  return frames.map((frame) => frame[0]);
}

{
  const first = createDeterministicUnitRandom(2037);
  const second = createDeterministicUnitRandom(2037);
  const third = createDeterministicUnitRandom(2038);
  const firstSequence = Array.from({ length: 8 }, () => first());
  assert.deepEqual(
    firstSequence,
    Array.from({ length: 8 }, () => second()),
    'equal seeds must reproduce the same jitter sequence'
  );
  assert.notDeepEqual(
    firstSequence,
    Array.from({ length: 8 }, () => third()),
    'different seeds must produce a different jitter sequence'
  );
}

{
  const impairment = createFrameImpairment({ dropEvery: 2 });
  assert.deepEqual(values(impairment.push(byte(1))), [1]);
  assert.deepEqual(values(impairment.push(byte(2))), []);
  assert.deepEqual(values(impairment.push(byte(3))), [3]);
  assert.equal(impairment.selectedCount(), 3);
}

{
  const impairment = createFrameImpairment({ duplicateEvery: 2 });
  assert.deepEqual(values(impairment.push(byte(1))), [1]);
  assert.deepEqual(values(impairment.push(byte(2))), [2, 2]);
}

{
  const impairment = createFrameImpairment({ reorderEvery: 2 });
  assert.deepEqual(values(impairment.push(byte(1))), [1]);
  assert.deepEqual(values(impairment.push(byte(2))), []);
  assert.deepEqual(values(impairment.push(byte(3))), [3, 2]);
  assert.deepEqual(values(impairment.push(byte(4))), []);
  assert.deepEqual(values(impairment.flush()), [4]);
}

{
  const impairment = createFrameImpairment({
    reorderEvery: 2,
    isSelected: (frame) => frame[0] < 10,
  });
  assert.deepEqual(values(impairment.push(byte(1))), [1]);
  assert.deepEqual(values(impairment.push(byte(2))), []);
  assert.deepEqual(
    values(impairment.push(byte(99))),
    [99],
    'unselected control frames must bypass world-stream impairment'
  );
  assert.deepEqual(values(impairment.push(byte(3))), [3, 2]);
  assert.equal(impairment.selectedCount(), 3);
}

console.log('deterministic websocket frame impairment checks passed');
