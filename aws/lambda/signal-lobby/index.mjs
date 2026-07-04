import {
  ApiGatewayManagementApiClient,
  DeleteConnectionCommand,
  PostToConnectionCommand,
} from '@aws-sdk/client-apigatewaymanagementapi';
import { DynamoDBClient } from '@aws-sdk/client-dynamodb';
import {
  DeleteCommand,
  DynamoDBDocumentClient,
  GetCommand,
  PutCommand,
  QueryCommand,
  UpdateCommand,
} from '@aws-sdk/lib-dynamodb';

const TABLE = process.env.SIGNAL_LOBBY_TABLE;
const DEFAULT_ROOM = process.env.SIGNAL_DEFAULT_ROOM || 'signal-main';
const DEFAULT_WORLD_ID = process.env.SIGNAL_DEFAULT_WORLD_ID || 'shared-main';
const TTL_SECONDS = Number(process.env.SIGNAL_LOBBY_TTL_SECONDS || '90');
const MIN_PLAYERS = Number(process.env.SIGNAL_MIN_PLAYERS_TO_WAKE || '2');

const ddb = DynamoDBDocumentClient.from(new DynamoDBClient({}));

function nowSeconds() {
  return Math.floor(Date.now() / 1000);
}

function json(statusCode, body) {
  return {
    statusCode,
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(body),
  };
}

function parseBody(event) {
  if (!event.body) return {};
  try {
    return JSON.parse(event.body);
  } catch {
    return {};
  }
}

function roomKey(room) {
  const safe = String(room || DEFAULT_ROOM).slice(0, 128);
  return `ROOM#${safe}`;
}

function connKey(connectionId) {
  return `CONN#${connectionId}`;
}

function relayConfig() {
  if (process.env.SIGNAL_RELAYS_JSON) {
    const parsed = JSON.parse(process.env.SIGNAL_RELAYS_JSON);
    if (Array.isArray(parsed) && parsed.length > 0) return parsed;
  }
  return [{
    id: process.env.SIGNAL_RELAY_ID || 'primary',
    rtcUrl: process.env.SIGNAL_RTC_URL,
    wakeUrl: process.env.SIGNAL_WAKE_URL,
    wakeToken: process.env.SIGNAL_WAKE_TOKEN,
    region: process.env.SIGNAL_RELAY_REGION || '',
  }];
}

function relayScore(relay, members) {
  let score = 0;
  for (const member of members) {
    const rtt = member.rtt && typeof member.rtt === 'object'
      ? Number(member.rtt[relay.id])
      : NaN;
    score += Number.isFinite(rtt) && rtt > 0 ? rtt : 100000;
  }
  return score;
}

function chooseRelay(members) {
  const relays = relayConfig().filter((relay) => relay && relay.rtcUrl);
  if (relays.length === 0) throw new Error('no relay configured');
  let best = relays[0];
  let bestScore = relayScore(best, members);
  for (const relay of relays.slice(1)) {
    const score = relayScore(relay, members);
    if (score < bestScore) {
      best = relay;
      bestScore = score;
    }
  }
  return best;
}

async function postToClient(event, connectionId, payload) {
  const endpoint = `https://${event.requestContext.domainName}/${event.requestContext.stage}`;
  const api = new ApiGatewayManagementApiClient({ endpoint });
  try {
    await api.send(new PostToConnectionCommand({
      ConnectionId: connectionId,
      Data: Buffer.from(JSON.stringify(payload)),
    }));
  } catch (err) {
    if (err?.$metadata?.httpStatusCode === 410) {
      await deleteConnection(connectionId);
      return;
    }
    throw err;
  }
}

async function closeClient(event, connectionId) {
  const endpoint = `https://${event.requestContext.domainName}/${event.requestContext.stage}`;
  const api = new ApiGatewayManagementApiClient({ endpoint });
  try {
    await api.send(new DeleteConnectionCommand({ ConnectionId: connectionId }));
  } catch {
    // The connection may already be gone.
  }
}

async function wakeRelay(relay) {
  if (!relay.wakeUrl) return;
  const headers = {};
  if (relay.wakeToken) {
    headers.authorization = `Bearer ${relay.wakeToken}`;
    headers['x-signal-wake-token'] = relay.wakeToken;
  }
  const res = await fetch(relay.wakeUrl, { method: 'POST', headers });
  if (!res.ok) {
    throw new Error(`wake failed ${relay.id}: HTTP ${res.status}`);
  }
}

async function listRoomMembers(room) {
  const now = nowSeconds();
  const res = await ddb.send(new QueryCommand({
    TableName: TABLE,
    KeyConditionExpression: 'pk = :pk AND begins_with(sk, :prefix)',
    FilterExpression: '#ttl > :now',
    ExpressionAttributeNames: { '#ttl': 'ttl' },
    ExpressionAttributeValues: {
      ':pk': roomKey(room),
      ':prefix': 'CONN#',
      ':now': now,
    },
  }));
  return res.Items || [];
}

async function rememberConnection(connectionId, room, msg) {
  const ttl = nowSeconds() + TTL_SECONDS;
  const member = {
    pk: roomKey(room),
    sk: connKey(connectionId),
    connectionId,
    room,
    peer: String(msg.peer || connectionId).slice(0, 128),
    build: String(msg.build || '').slice(0, 64),
    rtt: msg.rtt && typeof msg.rtt === 'object' ? msg.rtt : {},
    joinedAt: Date.now(),
    ttl,
  };
  await ddb.send(new PutCommand({ TableName: TABLE, Item: member }));
  await ddb.send(new PutCommand({
    TableName: TABLE,
    Item: {
      pk: connKey(connectionId),
      sk: 'META',
      connectionId,
      room,
      ttl,
    },
  }));
}

async function touchConnection(connectionId) {
  const meta = await ddb.send(new GetCommand({
    TableName: TABLE,
    Key: { pk: connKey(connectionId), sk: 'META' },
  }));
  if (!meta.Item?.room) return null;
  const ttl = nowSeconds() + TTL_SECONDS;
  await ddb.send(new UpdateCommand({
    TableName: TABLE,
    Key: { pk: connKey(connectionId), sk: 'META' },
    UpdateExpression: 'SET #ttl = :ttl',
    ExpressionAttributeNames: { '#ttl': 'ttl' },
    ExpressionAttributeValues: { ':ttl': ttl },
  }));
  await ddb.send(new UpdateCommand({
    TableName: TABLE,
    Key: { pk: roomKey(meta.Item.room), sk: connKey(connectionId) },
    UpdateExpression: 'SET #ttl = :ttl',
    ExpressionAttributeNames: { '#ttl': 'ttl' },
    ExpressionAttributeValues: { ':ttl': ttl },
  }));
  return meta.Item.room;
}

async function deleteConnection(connectionId) {
  const meta = await ddb.send(new GetCommand({
    TableName: TABLE,
    Key: { pk: connKey(connectionId), sk: 'META' },
  }));
  await ddb.send(new DeleteCommand({
    TableName: TABLE,
    Key: { pk: connKey(connectionId), sk: 'META' },
  }));
  if (meta.Item?.room) {
    await ddb.send(new DeleteCommand({
      TableName: TABLE,
      Key: { pk: roomKey(meta.Item.room), sk: connKey(connectionId) },
    }));
  }
}

async function broadcast(event, members, payload) {
  await Promise.all(members.map((member) =>
    postToClient(event, member.connectionId, payload)));
}

async function handleJoin(event, msg) {
  const connectionId = event.requestContext.connectionId;
  const room = String(msg.room || DEFAULT_ROOM).slice(0, 128);
  await rememberConnection(connectionId, room, msg);
  const members = await listRoomMembers(room);

  if (members.length < MIN_PLAYERS) {
    await postToClient(event, connectionId, {
      type: 'waiting',
      room,
      players: members.length,
      needed: MIN_PLAYERS,
    });
    return json(200, { ok: true, room, players: members.length });
  }

  const relay = chooseRelay(members);
  await wakeRelay(relay);
  await broadcast(event, members, {
    type: 'serverReady',
    room,
    worldId: String(msg.worldId || DEFAULT_WORLD_ID).slice(0, 128),
    relayId: relay.id,
    region: relay.region || '',
    server: relay.rtcUrl,
  });
  return json(200, { ok: true, room, players: members.length, relayId: relay.id });
}

async function handleSignal(event, msg) {
  if (!msg.to) return json(400, { ok: false, error: 'missing-to' });
  await postToClient(event, String(msg.to), {
    type: 'signal',
    from: event.requestContext.connectionId,
    data: msg.data || {},
  });
  return json(200, { ok: true });
}

export async function handler(event) {
  if (!TABLE) return json(500, { ok: false, error: 'missing-table' });

  const routeKey = event.requestContext.routeKey;
  const connectionId = event.requestContext.connectionId;

  if (routeKey === '$connect') {
    return json(200, { ok: true });
  }
  if (routeKey === '$disconnect') {
    await deleteConnection(connectionId);
    return json(200, { ok: true });
  }

  const msg = parseBody(event);
  const action = msg.action || msg.type || routeKey;
  if (action === 'join') return handleJoin(event, msg);
  if (action === 'heartbeat' || action === 'ping') {
    const room = await touchConnection(connectionId);
    await postToClient(event, connectionId, { type: 'pong', room });
    return json(200, { ok: true });
  }
  if (action === 'signal') return handleSignal(event, msg);
  if (action === 'close') {
    await closeClient(event, connectionId);
    return json(200, { ok: true });
  }

  return json(400, { ok: false, error: 'unknown-action' });
}
