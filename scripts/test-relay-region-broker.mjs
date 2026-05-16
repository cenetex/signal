import assert from 'node:assert/strict';
import {
  handleRequest,
  parseRelayRegions,
  preferredRegionFromRequest,
  regionFromTimezone,
  selectRelayRegion,
} from './relay-region-broker.mjs';

const regions = parseRelayRegions(JSON.stringify([
  {
    region: 'us-east-1',
    cluster: 'default',
    service: 'signal-relay-us',
    endpoint: 'https://signal-ws-use1.ratimics.com',
  },
  {
    region: 'ap-southeast-1',
    cluster: 'default',
    service: 'signal-relay-sg',
    endpoint: 'wss://signal-ws-apse1.ratimics.com/ws',
  },
]));

assert.equal(regions[0].server, 'wss://signal-ws-use1.ratimics.com/ws');
assert.equal(regionFromTimezone('Asia/Bangkok'), 'ap-southeast-1');
assert.equal(regionFromTimezone('America/Los_Angeles'), 'us-west-2');
assert.equal(preferredRegionFromRequest({ tz: 'Asia/Tokyo' }, {}), 'ap-northeast-1');
assert.equal(
  preferredRegionFromRequest({}, { 'CloudFront-Viewer-Country': 'TH' }),
  'ap-southeast-1',
);
assert.equal(selectRelayRegion(regions, 'ap-northeast-1').region, 'ap-southeast-1');
assert.equal(selectRelayRegion(regions, 'us-west-2').region, 'us-east-1');

const calls = [];
const response = await handleRequest(
  {
    requestContext: { http: { method: 'GET' } },
    queryStringParameters: { tz: 'Asia/Bangkok' },
    headers: { origin: 'https://signal.ratimics.com' },
  },
  {
    env: {
      SIGNAL_RELAY_REGIONS: JSON.stringify(regions.map((r) => ({
        region: r.region,
        cluster: r.cluster,
        service: r.service,
        endpoint: r.server,
      }))),
      SIGNAL_RELAY_ALLOWED_ORIGIN: '$request_origin',
    },
    clientFactory: async (region) => ({
      async describeServices(input) {
        calls.push(['describe', region, input.cluster, input.services[0]]);
        return { services: [{ status: 'ACTIVE', runningCount: 0, pendingCount: 0 }] };
      },
      async updateService(input) {
        calls.push(['update', region, input.cluster, input.service, input.desiredCount]);
        return {};
      },
    }),
  },
);

assert.equal(response.statusCode, 200);
assert.equal(response.headers['access-control-allow-origin'], 'https://signal.ratimics.com');
const body = JSON.parse(response.body);
assert.equal(body.ok, true);
assert.equal(body.region, 'ap-southeast-1');
assert.equal(body.server, 'wss://signal-ws-apse1.ratimics.com/ws');
assert.equal(body.launched, true);
assert.deepEqual(calls, [
  ['describe', 'ap-southeast-1', 'default', 'signal-relay-sg'],
  ['update', 'ap-southeast-1', 'default', 'signal-relay-sg', 1],
]);

calls.length = 0;
const alreadyRunning = await handleRequest(
  {
    requestContext: { http: { method: 'GET' } },
    queryStringParameters: { region: 'us-east-1' },
    headers: {},
  },
  {
    env: {
      SIGNAL_RELAY_REGIONS: JSON.stringify(regions.map((r) => ({
        region: r.region,
        cluster: r.cluster,
        service: r.service,
        endpoint: r.server,
      }))),
    },
    clientFactory: async (region) => ({
      async describeServices(input) {
        calls.push(['describe', region, input.cluster, input.services[0]]);
        return { services: [{ status: 'ACTIVE', runningCount: 1, pendingCount: 0 }] };
      },
      async updateService(input) {
        calls.push(['update', region, input.cluster, input.service, input.desiredCount]);
        return {};
      },
    }),
  },
);

assert.equal(alreadyRunning.statusCode, 200);
assert.equal(JSON.parse(alreadyRunning.body).launched, false);
assert.deepEqual(calls, [
  ['describe', 'us-east-1', 'default', 'signal-relay-us'],
]);

console.log('relay-region-broker tests passed');
