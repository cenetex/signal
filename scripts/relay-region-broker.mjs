const DEFAULT_REGION = 'us-east-1';

const TIMEZONE_REGION_RULES = [
  [/^America\/(Los_Angeles|Vancouver|Tijuana|Phoenix|Denver|Boise)/, 'us-west-2'],
  [/^America\/(Anchorage|Juneau|Metlakatla|Nome|Sitka|Yakutat)/, 'us-west-2'],
  [/^America\//, 'us-east-1'],
  [/^Atlantic\//, 'us-east-1'],
  [/^Europe\//, 'eu-west-1'],
  [/^Africa\//, 'eu-west-1'],
  [/^Asia\/(Tokyo|Seoul)/, 'ap-northeast-1'],
  [/^Asia\/(Shanghai|Hong_Kong|Taipei|Manila|Singapore|Kuala_Lumpur|Bangkok|Jakarta|Ho_Chi_Minh)/, 'ap-southeast-1'],
  [/^Asia\/(Kolkata|Colombo|Dhaka|Kathmandu|Karachi)/, 'ap-south-1'],
  [/^Asia\//, 'ap-southeast-1'],
  [/^(Australia|Pacific)\//, 'ap-southeast-2'],
];

const COUNTRY_REGION_HINTS = {
  AU: 'ap-southeast-2',
  BD: 'ap-south-1',
  BR: 'sa-east-1',
  CA: 'ca-central-1',
  CN: 'ap-southeast-1',
  DE: 'eu-central-1',
  ES: 'eu-west-1',
  FR: 'eu-west-3',
  GB: 'eu-west-2',
  HK: 'ap-southeast-1',
  ID: 'ap-southeast-1',
  IE: 'eu-west-1',
  IN: 'ap-south-1',
  JP: 'ap-northeast-1',
  KR: 'ap-northeast-1',
  MY: 'ap-southeast-1',
  NZ: 'ap-southeast-2',
  PH: 'ap-southeast-1',
  SG: 'ap-southeast-1',
  TH: 'ap-southeast-1',
  TW: 'ap-southeast-1',
  US: 'us-east-1',
  VN: 'ap-southeast-1',
  ZA: 'eu-west-1',
};

const REGION_FALLBACKS = {
  'us-east-1': ['us-east-1', 'us-east-2', 'ca-central-1', 'us-west-2', 'eu-west-1'],
  'us-east-2': ['us-east-2', 'us-east-1', 'ca-central-1', 'us-west-2', 'eu-west-1'],
  'us-west-2': ['us-west-2', 'us-east-1', 'ca-central-1', 'ap-northeast-1', 'ap-southeast-2'],
  'ca-central-1': ['ca-central-1', 'us-east-1', 'us-east-2', 'us-west-2'],
  'sa-east-1': ['sa-east-1', 'us-east-1', 'us-east-2'],
  'eu-west-1': ['eu-west-1', 'eu-west-2', 'eu-west-3', 'eu-central-1', 'us-east-1'],
  'eu-west-2': ['eu-west-2', 'eu-west-1', 'eu-west-3', 'eu-central-1', 'us-east-1'],
  'eu-west-3': ['eu-west-3', 'eu-west-1', 'eu-west-2', 'eu-central-1', 'us-east-1'],
  'eu-central-1': ['eu-central-1', 'eu-west-1', 'eu-west-2', 'eu-west-3', 'us-east-1'],
  'ap-south-1': ['ap-south-1', 'ap-southeast-1', 'ap-southeast-2', 'eu-west-1'],
  'ap-southeast-1': ['ap-southeast-1', 'ap-southeast-2', 'ap-northeast-1', 'ap-south-1', 'us-west-2'],
  'ap-southeast-2': ['ap-southeast-2', 'ap-southeast-1', 'ap-northeast-1', 'us-west-2'],
  'ap-northeast-1': ['ap-northeast-1', 'ap-southeast-1', 'ap-southeast-2', 'us-west-2'],
};

function header(headers, name) {
  if (!headers) return '';
  const lower = name.toLowerCase();
  for (const [key, value] of Object.entries(headers)) {
    if (key.toLowerCase() === lower) return String(value || '');
  }
  return '';
}

function queryFromEvent(event) {
  const out = { ...(event?.queryStringParameters || {}) };
  if (event?.rawQueryString) {
    for (const [key, value] of new URLSearchParams(event.rawQueryString)) {
      if (!(key in out)) out[key] = value;
    }
  }
  return out;
}

function corsHeaders(env, requestHeaders) {
  const allowed = env.SIGNAL_RELAY_ALLOWED_ORIGIN || env.SIGNAL_ALLOWED_ORIGIN || '*';
  const origin = header(requestHeaders, 'origin');
  return {
    'access-control-allow-origin': allowed === '$request_origin' ? (origin || '*') : allowed,
    'access-control-allow-methods': 'GET,OPTIONS',
    'access-control-allow-headers': 'content-type',
    'cache-control': 'no-store',
    'content-type': 'application/json',
  };
}

function jsonResponse(statusCode, body, env, requestHeaders) {
  return {
    statusCode,
    headers: corsHeaders(env, requestHeaders),
    body: JSON.stringify(body),
  };
}

function toWsUrl(endpoint) {
  if (!endpoint || typeof endpoint !== 'string') {
    throw new Error('relay endpoint is required');
  }
  if (endpoint.startsWith('ws://') || endpoint.startsWith('wss://')) return endpoint;
  if (endpoint.startsWith('https://')) {
    const url = new URL(endpoint);
    if (url.pathname === '/' || url.pathname === '') url.pathname = '/ws';
    url.protocol = 'wss:';
    return url.toString();
  }
  if (endpoint.startsWith('http://')) {
    const url = new URL(endpoint);
    if (url.pathname === '/' || url.pathname === '') url.pathname = '/ws';
    url.protocol = 'ws:';
    return url.toString();
  }
  throw new Error(`relay endpoint must start with ws://, wss://, http://, or https://: ${endpoint}`);
}

export function parseRelayRegions(raw) {
  const parsed = JSON.parse(raw);
  const items = Array.isArray(parsed) ? parsed : parsed.regions;
  if (!Array.isArray(items) || items.length === 0) {
    throw new Error('SIGNAL_RELAY_REGIONS must be a non-empty JSON array');
  }

  return items.map((item, index) => {
    const region = String(item.region || '').trim();
    const cluster = String(item.cluster || 'default').trim();
    const service = String(item.service || '').trim();
    const endpoint = item.wsUrl || item.endpoint;
    if (!region) throw new Error(`relay region entry ${index} is missing region`);
    if (!service) throw new Error(`relay region entry ${index} is missing service`);
    return {
      region,
      cluster,
      service,
      server: toWsUrl(String(endpoint || '').trim()),
    };
  });
}

export function relayRegionsFromEnv(env = process.env) {
  const raw = env.SIGNAL_RELAY_REGIONS || env.SIGNAL_RELAY_REGIONS_JSON;
  if (raw) return parseRelayRegions(raw);

  if (env.SIGNAL_RELAY_ENDPOINT) {
    return [{
      region: env.SIGNAL_RELAY_REGION || env.AWS_REGION || DEFAULT_REGION,
      cluster: env.SIGNAL_ECS_CLUSTER || 'default',
      service: env.SIGNAL_ECS_SERVICE || 'signal-relay',
      server: toWsUrl(env.SIGNAL_RELAY_ENDPOINT),
    }];
  }

  throw new Error('configure SIGNAL_RELAY_REGIONS or SIGNAL_RELAY_ENDPOINT');
}

export function regionFromTimezone(timezone) {
  if (!timezone) return '';
  for (const [pattern, region] of TIMEZONE_REGION_RULES) {
    if (pattern.test(timezone)) return region;
  }
  return '';
}

export function regionFromCountry(country) {
  if (!country) return '';
  return COUNTRY_REGION_HINTS[String(country).trim().toUpperCase()] || '';
}

export function preferredRegionFromRequest(query, headers) {
  const explicit = query.region || query.preferredRegion || query.aws_region;
  if (explicit) return String(explicit).trim();

  const timezone =
    query.tz ||
    query.timezone ||
    header(headers, 'cloudfront-viewer-time-zone') ||
    header(headers, 'x-timezone');
  const fromTz = regionFromTimezone(String(timezone || ''));
  if (fromTz) return fromTz;

  const country =
    query.country ||
    header(headers, 'cloudfront-viewer-country') ||
    header(headers, 'x-vercel-ip-country') ||
    header(headers, 'cf-ipcountry');
  return regionFromCountry(country);
}

export function selectRelayRegion(regions, preferredRegion, defaultRegion = DEFAULT_REGION) {
  const supported = new Map(regions.map((region) => [region.region, region]));
  if (preferredRegion && supported.has(preferredRegion)) return supported.get(preferredRegion);

  const chain = REGION_FALLBACKS[preferredRegion] || [];
  for (const candidate of chain) {
    if (supported.has(candidate)) return supported.get(candidate);
  }

  if (defaultRegion && supported.has(defaultRegion)) return supported.get(defaultRegion);
  return regions[0];
}

async function defaultEcsClientFactory(region) {
  const ecs = await import('@aws-sdk/client-ecs');
  const client = new ecs.ECSClient({ region });
  return {
    describeServices(input) {
      return client.send(new ecs.DescribeServicesCommand(input));
    },
    updateService(input) {
      return client.send(new ecs.UpdateServiceCommand(input));
    },
  };
}

export async function ensureRelayRunning(regionConfig, clientFactory = defaultEcsClientFactory) {
  const ecs = await clientFactory(regionConfig.region);
  const described = await ecs.describeServices({
    cluster: regionConfig.cluster,
    services: [regionConfig.service],
  });
  const service = described.services && described.services[0];
  if (!service || service.status === 'INACTIVE') {
    throw new Error(`ECS service not found or inactive: ${regionConfig.region}/${regionConfig.cluster}/${regionConfig.service}`);
  }

  const runningCount = Number(service.runningCount || 0);
  const pendingCount = Number(service.pendingCount || 0);
  let launched = false;

  if (runningCount === 0 && pendingCount === 0) {
    await ecs.updateService({
      cluster: regionConfig.cluster,
      service: regionConfig.service,
      desiredCount: 1,
    });
    launched = true;
  }

  return {
    region: regionConfig.region,
    cluster: regionConfig.cluster,
    service: regionConfig.service,
    server: regionConfig.server,
    launched,
    runningCount,
    pendingCount,
  };
}

export async function handleRequest(event, deps = {}) {
  const env = deps.env || process.env;
  const method = event?.requestContext?.http?.method || event?.httpMethod || 'GET';
  const headers = event?.headers || {};
  if (method === 'OPTIONS') {
    return { statusCode: 204, headers: corsHeaders(env, headers), body: '' };
  }
  if (method !== 'GET') {
    return jsonResponse(405, { ok: false, error: 'method not allowed' }, env, headers);
  }

  try {
    const query = queryFromEvent(event);
    const regions = relayRegionsFromEnv(env);
    const preferredRegion = preferredRegionFromRequest(query, headers);
    const selected = selectRelayRegion(
      regions,
      preferredRegion,
      env.SIGNAL_RELAY_DEFAULT_REGION || DEFAULT_REGION,
    );
    const relay = await ensureRelayRunning(selected, deps.clientFactory);
    return jsonResponse(200, {
      ok: true,
      preferredRegion: preferredRegion || null,
      region: relay.region,
      server: relay.server,
      launched: relay.launched,
      runningCount: relay.runningCount,
      pendingCount: relay.pendingCount,
    }, env, headers);
  } catch (err) {
    console.error('[relay-region-broker]', err);
    return jsonResponse(500, {
      ok: false,
      error: err && err.message ? err.message : 'relay broker failed',
    }, env, headers);
  }
}

export async function handler(event) {
  return handleRequest(event);
}
