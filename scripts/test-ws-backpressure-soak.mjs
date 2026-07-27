import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import test from 'node:test';

import {
  SIGNED_ACTION_BUY_INGOT,
  SIGNED_ACTION_DELIVER,
  createClientIdentity,
  makePubkeyProof,
  makeRegisterPubkey,
  makeSignedAction,
} from './ws-backpressure-soak.mjs';

test('backpressure client emits a challenge-bound v2 pubkey proof', () => {
  const identity = createClientIdentity();
  const token = Buffer.from('5753425036363321', 'hex');
  const challenge = Buffer.from(
    Array.from({ length: 32 }, (_, index) => index + 1));

  const registration = makeRegisterPubkey(identity);
  assert.equal(registration.length, 33);
  assert.equal(registration[0], 0x32);
  assert.deepEqual(registration.subarray(1), identity.publicKeyBytes);

  const proof = makePubkeyProof(identity, token, challenge);
  assert.equal(proof.length, 105);
  assert.equal(proof[0], 0x3f);
  assert.deepEqual(proof.subarray(1, 33), identity.publicKeyBytes);
  assert.deepEqual(proof.subarray(33, 41), token);
  const signed = Buffer.concat([
    Buffer.from('prove-pubkey-v2', 'ascii'),
    identity.publicKeyBytes,
    token,
    challenge,
  ]);
  assert.equal(
    crypto.verify(null, signed, identity.publicKey, proof.subarray(41)),
    true);
});

test('backpressure mutations use signed actions with increasing nonces', () => {
  const identity = createClientIdentity();
  const cargoPub = crypto.randomBytes(32);
  const buy = makeSignedAction(
    identity, SIGNED_ACTION_BUY_INGOT, cargoPub);
  const deliver = makeSignedAction(
    identity, SIGNED_ACTION_DELIVER, Buffer.from([0]));

  assert.equal(buy[0], 0x33);
  assert.equal(buy[9], 2);
  assert.equal(buy.readUInt16LE(10), cargoPub.length);
  assert.deepEqual(buy.subarray(12, 44), cargoPub);
  assert.equal(
    crypto.verify(
      null, buy.subarray(1, 44), identity.publicKey, buy.subarray(44)),
    true);

  assert.equal(deliver[0], 0x33);
  assert.equal(deliver[9], 4);
  assert.equal(deliver.readUInt16LE(10), 1);
  assert.equal(deliver[12], 0);
  assert(
    deliver.readBigUInt64LE(1) > buy.readBigUInt64LE(1),
    'signed-action nonces must increase strictly');
  assert.equal(
    crypto.verify(
      null, deliver.subarray(1, 13),
      identity.publicKey, deliver.subarray(13)),
    true);
});
