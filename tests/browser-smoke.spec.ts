import { test, expect, type Page, type Locator, type TestInfo } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { inflateSync } from 'node:zlib';

const episodeSmokeMpeg = Buffer.from(
  readFileSync(join(__dirname, 'fixtures', 'episode-smoke.mpg.b64'), 'utf8').trim(),
  'base64',
);
const protocolHeader = readFileSync(
  join(__dirname, '..', 'shared', 'protocol.h'),
  'utf8',
);
const protocolVersionMatch = protocolHeader.match(
  /^#define SIGNAL_PROTOCOL_VERSION ([0-9]+)u$/m,
);
if (!protocolVersionMatch) {
  throw new Error('SIGNAL_PROTOCOL_VERSION is missing from shared/protocol.h');
}
const currentProtocolVersion = Number(protocolVersionMatch[1]);

const fatalPattern =
  /abort|unreachable|RuntimeError|LinkError|compile failed|Cannot enlarge memory|exception thrown|websocket error|WebSocket is already in CLOSING|WebSocket connection .* failed/i;
const expectedLiveClosePattern =
  /^(websocket error wss:\/\/signal-ws\.ratimics\.com\/ws: undefined|WebSocket connection to 'wss:\/\/signal-ws\.ratimics\.com\/ws' failed: Data frame received after close)$/i;

type FatalCollectors = {
  pageErrors: string[];
  consoleErrors: string[];
};

type FatalExpectationOptions = {
  allowExpectedLiveClose?: boolean;
};

type CanvasStats = {
  pixels: number;
  nonBlackRatio: number;
  uniqueBuckets: number;
  avgLuma: number;
};

type TouchControlRect = {
  x: number;
  y: number;
  width: number;
  height: number;
};

type NetMotionSnapshot = {
  samples: number;
  deferredSamples: number;
  replayedSamples: number;
  replayedFrames: number;
  maxCorrection: number;
  maxAppliedCorrection: number;
  maxVelocityError: number;
  lastAckRttMs: number;
  lastPingRttMs: number;
  smoothedPingRttMs: number;
  lastAckGapMs: number;
  maxPingRttMs: number;
  pingSamples: number;
  pingServerTurnaroundMs: number;
  playerIntervalMs: number;
  maxPlayerIntervalMs: number;
  maxPlayerJitterMs: number;
  rawPlayerIntervalMs: number;
  maxRawPlayerIntervalMs: number;
  maxRawPlayerJitterMs: number;
  maxAckRttMs: number;
  currentRenderOffset: number;
  maxRenderOffset: number;
  playerBatches: number;
  snapSamples: number;
  lerpSamples: number;
  inputAcks: number;
  tickSkew: number;
  maxTickSkewAbs: number;
  inputApplyErrorTicks: number;
  maxInputApplyErrorAbs: number;
  replayDepth: number;
  unackedInputs: number;
  actionQueueDepth: number;
};

type PlayerCameraSnapshot = {
  offsetX: number;
  offsetY: number;
  narrowFocus: number;
};

type PlayerStateSnapshot = {
  x: number;
  y: number;
  vx: number;
  vy: number;
  angle: number;
  docked: number;
};

type JankProfileReport = {
  schema: string;
  enabled: boolean;
  window_sec: number;
  frames: number;
  frame_ms: { p50: number; p95: number; p99: number; max: number };
  simulation_ms: { p50: number; p95: number; p99: number; max: number };
  slow_frames: { over_16_6: number; over_33_3: number; unexplained: number };
  fixed_step: { completed: number; missed: number; accumulator_dropped: number };
  snapshots: { packets: number; bytes: number; max_packet_bytes: number; max_gap_ms: number };
  phases: Record<string, { avg_ms: number; max_ms: number; slow_cause: number }>;
  entities: Record<string, {
    samples: number;
    max_correction_world: number;
    max_velocity_discontinuity: number;
    max_correction_jerk: number;
  }>;
};

async function jankProfileReport(page: Page): Promise<JankProfileReport> {
  const raw = await page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: {
        ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string;
      };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('signal_jank_profile_report_json', 'string', [], []) || '';
  });
  return JSON.parse(raw) as JankProfileReport;
}

function addQueryParam(rawUrl: string, key: string, value: string): string {
  const hashAt = rawUrl.indexOf('#');
  const beforeHash = hashAt >= 0 ? rawUrl.slice(0, hashAt) : rawUrl;
  const afterHash = hashAt >= 0 ? rawUrl.slice(hashAt) : '';
  if (new RegExp(`[?&]${key}=`).test(beforeHash)) return rawUrl;
  return `${beforeHash}${beforeHash.includes('?') ? '&' : '?'}${key}=${encodeURIComponent(value)}${afterHash}`;
}

function smokeUrl(options: { singleplayer?: boolean } = {}): string {
  let url = process.env.SMOKE_URL || '/play.html?singleplayer=1';
  url = addQueryParam(url, 'smoke', '1');
  if (options.singleplayer) url = addQueryParam(url, 'singleplayer', '1');
  if (process.env.SMOKE_LIVE_RELAY_ASSERT &&
      !/[?&](server|online|multiplayer|network)=/.test(url)) {
    url = addQueryParam(url, 'online', '1');
  }
  return url;
}

function usesLiveSmokeUrl(): boolean {
  return !!process.env.SMOKE_URL;
}

function installFatalCollectors(page: Page): FatalCollectors {
  const logs: FatalCollectors = { pageErrors: [], consoleErrors: [] };
  page.on('pageerror', (err) => logs.pageErrors.push(err.message));
  page.on('console', (msg) => {
    if (msg.type() === 'error') logs.consoleErrors.push(msg.text());
  });
  page.on('websocket', (ws) => {
    ws.on('socketerror', (err) => logs.consoleErrors.push(`websocket error ${ws.url()}: ${err.message}`));
  });
  return logs;
}

function expectNoFatalErrors(logs: FatalCollectors, options: FatalExpectationOptions = {}): void {
  expect(logs.pageErrors.filter((e) => fatalPattern.test(e))).toEqual([]);
  const consoleErrors = logs.consoleErrors.filter((e) => fatalPattern.test(e));
  expect(
    options.allowExpectedLiveClose ? consoleErrors.filter((e) => !expectedLiveClosePattern.test(e)) : consoleErrors,
  ).toEqual([]);
}

function paethPredictor(a: number, b: number, c: number): number {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

function pngStats(png: Buffer): CanvasStats {
  const signature = '89504e470d0a1a0a';
  expect(png.subarray(0, 8).toString('hex')).toBe(signature);

  let width = 0;
  let height = 0;
  let bitDepth = 0;
  let colorType = 0;
  let interlace = 0;
  const idat: Buffer[] = [];

  for (let off = 8; off < png.length;) {
    const len = png.readUInt32BE(off);
    const type = png.subarray(off + 4, off + 8).toString('ascii');
    const data = png.subarray(off + 8, off + 8 + len);
    off += 12 + len;

    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
      interlace = data[12];
    } else if (type === 'IDAT') {
      idat.push(Buffer.from(data));
    } else if (type === 'IEND') {
      break;
    }
  }

  expect(width).toBeGreaterThan(0);
  expect(height).toBeGreaterThan(0);
  expect(bitDepth).toBe(8);
  expect(interlace).toBe(0);

  const channels = colorType === 6 ? 4 : colorType === 2 ? 3 : 0;
  expect(channels).toBeGreaterThan(0);

  const stride = width * channels;
  const inflated = inflateSync(Buffer.concat(idat));
  const pixels = new Uint8Array(height * stride);
  let src = 0;

  for (let y = 0; y < height; y++) {
    const filter = inflated[src++];
    const row = pixels.subarray(y * stride, (y + 1) * stride);
    const prev = y > 0 ? pixels.subarray((y - 1) * stride, y * stride) : undefined;

    for (let x = 0; x < stride; x++) {
      const raw = inflated[src++];
      const left = x >= channels ? row[x - channels] : 0;
      const up = prev ? prev[x] : 0;
      const upLeft = prev && x >= channels ? prev[x - channels] : 0;
      let value: number;

      if (filter === 0) value = raw;
      else if (filter === 1) value = raw + left;
      else if (filter === 2) value = raw + up;
      else if (filter === 3) value = raw + Math.floor((left + up) / 2);
      else if (filter === 4) value = raw + paethPredictor(left, up, upLeft);
      else throw new Error(`unsupported PNG filter ${filter}`);

      row[x] = value & 0xff;
    }
  }

  let nonBlack = 0;
  let lumaSum = 0;
  const buckets = new Set<string>();

  for (let i = 0; i < pixels.length; i += channels) {
    const r = pixels[i];
    const g = pixels[i + 1];
    const b = pixels[i + 2];
    const a = channels === 4 ? pixels[i + 3] : 255;
    const luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    lumaSum += luma;
    if (a > 0 && luma > 6) nonBlack++;
    buckets.add(`${r >> 4}:${g >> 4}:${b >> 4}:${a >> 6}`);
  }

  const count = width * height;
  return {
    pixels: count,
    nonBlackRatio: count ? nonBlack / count : 0,
    uniqueBuckets: buckets.size,
    avgLuma: count ? lumaSum / count : 0,
  };
}

async function readCanvasStats(canvas: Locator): Promise<CanvasStats> {
  return pngStats(await canvas.screenshot());
}

async function waitForRuntime(page: Page): Promise<void> {
  await page.waitForFunction(
    () => {
      const mod = (window as unknown as {
        Module?: { ccall?: unknown; calledRun?: boolean; _get_signal_strength?: unknown };
      }).Module;
      return (
        !!mod &&
        mod.calledRun === true &&
        typeof mod.ccall === 'function' &&
        typeof mod._get_signal_strength === 'function'
      );
    },
    undefined,
    { timeout: 20_000 },
  );
}

async function wasmNumber(page: Page, name: string): Promise<number | null> {
  return page.evaluate((fnName) => {
    const mod = (window as unknown as {
      Module?: {
        ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number;
        [key: string]: unknown;
      };
    }).Module;
    if (!mod) return null;
    try {
      const direct = mod[`_${fnName}`];
      const value = typeof direct === 'function'
        ? (direct as () => number)()
        : typeof mod.ccall === 'function'
          ? mod.ccall(fnName, 'number', [], [])
          : null;
      return Number.isFinite(value) ? value : null;
    } catch {
      return null;
    }
  }, name);
}

async function wasmNumberArg(page: Page, name: string, value: number): Promise<number | null> {
  return page.evaluate(({ fnName, arg }) => {
    const mod = (window as unknown as {
      Module?: {
        ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number;
      };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return null;
    try {
      const result = mod.ccall(fnName, 'number', ['number'], [arg]);
      return Number.isFinite(result) ? result : null;
    } catch {
      return null;
    }
  }, { fnName: name, arg: value });
}

async function wasmMemoryPages(page: Page): Promise<number> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { HEAPU8?: Uint8Array };
    }).Module;
    return mod?.HEAPU8
      ? mod.HEAPU8.buffer.byteLength / 65536
      : 0;
  });
}

async function signalStrength(page: Page): Promise<number | null> {
  return wasmNumber(page, 'get_signal_strength');
}

async function signalVisualSaturation(page: Page): Promise<number | null> {
  return wasmNumber(page, 'get_signal_visual_saturation');
}

async function signalVisualBaseSaturation(page: Page): Promise<number | null> {
  return wasmNumber(page, 'get_signal_visual_base_saturation');
}

async function signalVisualCueSaturation(page: Page): Promise<number | null> {
  return wasmNumber(page, 'get_signal_visual_cue_saturation');
}

async function signalVisualPlayerSaturation(page: Page): Promise<number | null> {
  return wasmNumber(page, 'get_signal_visual_player_saturation');
}

async function playerCameraSnapshot(page: Page): Promise<PlayerCameraSnapshot | null> {
  const [offsetX, offsetY, narrowFocus] = await Promise.all([
    wasmNumber(page, 'get_player_camera_offset_x'),
    wasmNumber(page, 'get_player_camera_offset_y'),
    wasmNumber(page, 'get_camera_narrow_focus'),
  ]);
    if (![offsetX, offsetY, narrowFocus].every(Number.isFinite)) return null;
  return { offsetX: offsetX!, offsetY: offsetY!, narrowFocus: narrowFocus! };
}

async function playerStateSnapshot(page: Page): Promise<PlayerStateSnapshot | null> {
  const [x, y, vx, vy, angle, docked] = await Promise.all([
    wasmNumber(page, 'get_player_pos_x'),
    wasmNumber(page, 'get_player_pos_y'),
    wasmNumber(page, 'get_player_vel_x'),
    wasmNumber(page, 'get_player_vel_y'),
    wasmNumber(page, 'get_player_angle'),
    wasmNumber(page, 'get_player_docked'),
  ]);
  if (![x, y, vx, vy, angle, docked].every(Number.isFinite)) return null;
  return { x: x!, y: y!, vx: vx!, vy: vy!, angle: angle!, docked: docked! };
}

function playerDistance(a: PlayerStateSnapshot, b: PlayerStateSnapshot): number {
  return Math.hypot(b.x - a.x, b.y - a.y);
}

function playerAngleDistance(a: number, b: number): number {
  let delta = b - a;
  while (delta > Math.PI) delta -= Math.PI * 2;
  while (delta < -Math.PI) delta += Math.PI * 2;
  return Math.abs(delta);
}

async function heldControlMask(page: Page): Promise<number> {
  return ((await wasmNumber(page, 'signal_debug_held_control_mask')) ?? -1) | 0;
}

async function netMotionSnapshot(page: Page): Promise<NetMotionSnapshot> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod) {
      return {
        samples: 0,
        deferredSamples: 0,
        replayedSamples: 0,
        replayedFrames: 0,
        maxCorrection: 0,
        maxAppliedCorrection: 0,
        maxVelocityError: 0,
        lastAckRttMs: 0,
        lastPingRttMs: 0,
        smoothedPingRttMs: 0,
        lastAckGapMs: 0,
        maxPingRttMs: 0,
        pingSamples: 0,
        pingServerTurnaroundMs: 0,
        playerIntervalMs: 0,
        maxPlayerIntervalMs: 0,
        maxPlayerJitterMs: 0,
        rawPlayerIntervalMs: 0,
        maxRawPlayerIntervalMs: 0,
        maxRawPlayerJitterMs: 0,
        maxAckRttMs: 0,
        currentRenderOffset: 0,
        maxRenderOffset: 0,
        playerBatches: 0,
        snapSamples: 0,
        lerpSamples: 0,
        inputAcks: 0,
        tickSkew: 0,
        maxTickSkewAbs: 0,
        inputApplyErrorTicks: 0,
        maxInputApplyErrorAbs: 0,
        replayDepth: 0,
        unackedInputs: 0,
        actionQueueDepth: 0,
      };
    }

    const read = (name: string) => {
      try {
        const direct = (mod as Record<string, unknown>)[`_${name}`];
        const value = typeof direct === 'function'
          ? (direct as () => number)()
          : typeof mod.ccall === 'function'
            ? mod.ccall(name, 'number', [], [])
            : 0;
        return Number.isFinite(value) ? value : 0;
      } catch {
        return 0;
      }
    };

    return {
      samples: read('get_net_motion_total_samples'),
      deferredSamples: read('get_net_motion_total_deferred_samples'),
      replayedSamples: read('get_net_motion_total_replayed_samples'),
      replayedFrames: read('get_net_motion_total_replayed_frames'),
      maxCorrection: read('get_net_motion_max_correction'),
      maxAppliedCorrection: read('get_net_motion_max_applied_correction'),
      maxVelocityError: read('get_net_motion_max_velocity_error'),
      lastAckRttMs: read('get_net_motion_last_ack_rtt_ms'),
      lastPingRttMs: read('get_net_motion_last_ping_rtt_ms'),
      smoothedPingRttMs: read('get_net_motion_smoothed_ping_rtt_ms'),
      lastAckGapMs: read('get_net_motion_last_ack_gap_ms'),
      maxPingRttMs: read('get_net_motion_max_ping_rtt_ms'),
      pingSamples: read('get_net_motion_total_ping_samples'),
      pingServerTurnaroundMs: read('get_net_motion_last_ping_server_turnaround_ms'),
      playerIntervalMs: read('get_net_motion_player_interval_ms'),
      maxPlayerIntervalMs: read('get_net_motion_max_player_interval_ms'),
      maxPlayerJitterMs: read('get_net_motion_max_player_jitter_ms'),
      rawPlayerIntervalMs: read('get_net_motion_raw_player_interval_ms'),
      maxRawPlayerIntervalMs: read('get_net_motion_max_raw_player_interval_ms'),
      maxRawPlayerJitterMs: read('get_net_motion_max_raw_player_jitter_ms'),
      maxAckRttMs: read('get_net_motion_max_ack_rtt_ms'),
      currentRenderOffset: read('get_net_motion_current_render_offset'),
      maxRenderOffset: read('get_net_motion_max_render_offset'),
      playerBatches: read('get_net_motion_total_player_batches'),
      snapSamples: read('get_net_motion_total_snap_samples'),
      lerpSamples: read('get_net_motion_total_lerp_samples'),
      inputAcks: read('get_net_motion_total_input_acks'),
      tickSkew: read('get_net_motion_tick_skew'),
      maxTickSkewAbs: read('get_net_motion_max_tick_skew_abs'),
      inputApplyErrorTicks: read('get_net_motion_input_apply_error_ticks'),
      maxInputApplyErrorAbs: read('get_net_motion_max_input_apply_error_abs'),
      replayDepth: read('get_net_motion_replay_depth'),
      unackedInputs: read('get_net_motion_unacked_inputs'),
      actionQueueDepth: read('get_net_motion_action_queue_depth'),
    };
  });
}

async function resetNetMotionTelemetry(page: Page): Promise<void> {
  await page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: {
        ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number;
        _reset_net_motion_telemetry?: () => number;
      };
    }).Module;
    if (mod && typeof mod._reset_net_motion_telemetry === 'function') {
      mod._reset_net_motion_telemetry();
    } else if (mod && typeof mod.ccall === 'function') {
      mod.ccall('reset_net_motion_telemetry', 'number', [], []);
    }
  });
}

async function zeroLatencyGateReport(page: Page): Promise<{
  status: number;
  failure_stage: string;
  scenario_mask: number;
  required_mask: number;
  ticks: number;
  mining: {
    hp_before: number;
    hp_after: number;
    signal: number;
    hover: number;
    beam: number;
    input: number;
  };
  normal: {
    samples: number;
    exact: number;
    input_frontier: number;
    semantic: number;
    numeric_drift: number;
    asteroid_motion: number;
    npc_motion: number;
    death_respawn: number;
  };
  current_numeric_drift: number;
  first_drift: {
    class?: string;
    server_tick?: number;
    prediction_tick?: number;
    predicted_input_seq?: number;
    authoritative_input_seq?: number;
    domain?: string;
    predicted_bits?: string;
    authoritative_bits?: string;
    input_cause_mask?: number;
    semantic_cause_mask?: number;
    transport_cause_mask?: number;
    root_schema?: string;
    authoritative_root?: string;
  };
}> {
  const json = await page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: {
        ccall?: (
          name: string,
          returnType: string,
          argTypes: unknown[],
          args: unknown[],
        ) => string;
      };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '{}';
    return mod.ccall(
      'signal_zero_latency_gate_report_json', 'string', [], [],
    ) || '{}';
  });
  return JSON.parse(json);
}

async function hudHintText(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('get_hud_hint_text', 'string', [], []) || '';
  });
}

async function hudActionText(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('get_hud_action_text', 'string', [], []) || '';
  });
}

async function hudAttentionSurface(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('signal_hud_attention_surface', 'string', [], []) || '';
  });
}

async function hudAttentionTelemetry(page: Page): Promise<{
  debugVisible: number;
  asteroidBudget: number;
  npcBudget: number;
}> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    const read = (name: string) => {
      if (!mod || typeof mod.ccall !== 'function') return 0;
      return mod.ccall(name, 'number', [], []) | 0;
    };
    return {
      debugVisible: read('signal_hud_debug_visible'),
      asteroidBudget: read('signal_hud_scan_asteroid_budget'),
      npcBudget: read('signal_hud_scan_npc_budget'),
    };
  });
}

async function laserRefitSummary(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('signal_laser_refit_summary', 'string', [], []) || '';
  });
}

async function stationProductionSummary(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('signal_station_production_summary', 'string', [], []) || '';
  });
}

async function remoteTowableInterpCheck(page: Page): Promise<number> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('signal_smoke_remote_towable_interp_check', 'number', [], []);
  });
}

async function localTowReplayStabilityCheck(page: Page): Promise<number> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('signal_smoke_local_tow_replay_stability_check', 'number', [], []);
  });
}

async function adverseTowableGate(page: Page): Promise<number> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('signal_smoke_adverse_towable_gate', 'number', [], []);
  });
}

async function adverseTowableReport(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('signal_smoke_adverse_towable_report', 'string', [], []) || '';
  });
}

async function prepareKnownLedgerSync(page: Page): Promise<number> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('signal_smoke_prepare_known_ledger_sync', 'number', [], []);
  });
}

async function knownLedgerSyncState(page: Page): Promise<number> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('signal_smoke_known_ledger_sync_state', 'number', [], []);
  });
}

async function prepareTowLifecycle(page: Page): Promise<number> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('signal_smoke_prepare_tow_lifecycle', 'number', [], []);
  });
}

async function towLifecycleState(page: Page): Promise<number> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('signal_smoke_tow_lifecycle_state', 'number', [], []);
  });
}

async function tapTowOnNextSample(page: Page): Promise<number> {
  return page.evaluate(async () => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string | null, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') throw new Error('WASM ccall unavailable');
    mod.ccall('signal_mobile_key', null, ['number', 'number'], [6, 1]);
    await new Promise<void>((resolve, reject) => {
      const deadline = performance.now() + 1_000;
      const releaseWhenSampled = () => {
        const state = mod.ccall!('signal_smoke_tow_lifecycle_state', 'number', [], []);
        if ((state & 0b10000000) !== 0) {
          mod.ccall!('signal_mobile_key', null, ['number', 'number'], [6, 0]);
          resolve();
          return;
        }
        if (performance.now() >= deadline) {
          mod.ccall!('signal_mobile_key', null, ['number', 'number'], [6, 0]);
          reject(new Error('tractor press was not sampled'));
          return;
        }
        requestAnimationFrame(releaseWhenSampled);
      };
      requestAnimationFrame(releaseWhenSampled);
    });
    return mod.ccall('signal_smoke_tow_lifecycle_state', 'number', [], []);
  });
}

async function remotePlayerScanned(page: Page, playerId: number): Promise<number> {
  return page.evaluate((id) => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('get_remote_player_scanned', 'number', ['number'], [id]);
  }, playerId);
}

async function mobileControlFlags(page: Page): Promise<number> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('signal_mobile_control_flags', 'number', [], []) | 0;
  });
}

async function legacyRecoveryFlags(page: Page): Promise<number> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('signal_legacy_recovery_ui_flags', 'number', [], []) | 0;
  });
}

async function legacyRecoverySemantic(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('signal_legacy_recovery_ui_semantic', 'string', [], []) || '';
  });
}

async function legacyRecoveryCopy(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('signal_legacy_recovery_ui_copy', 'string', [], []) || '';
  });
}

async function smokeLegacyRecoveryOffer(page: Page, seconds = 30): Promise<number> {
  return page.evaluate((ttl) => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall(
      'signal_smoke_legacy_recovery_offer', 'number', ['number'], [ttl],
    );
  }, seconds);
}

async function smokeLegacyRecoveryResult(page: Page, status: number): Promise<number> {
  return page.evaluate((wireStatus) => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall(
      'signal_smoke_legacy_recovery_result',
      'number', ['number'], [wireStatus],
    );
  }, status);
}

async function smokeLegacyRecoveryReset(page: Page): Promise<void> {
  await page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: null, argTypes: unknown[], args: unknown[]) => void };
    }).Module;
    mod?.ccall?.('signal_smoke_legacy_recovery_reset', null, [], []);
  });
}

async function smokeLegacyRecoverySetSendAdmitted(
  page: Page,
  admitted: boolean,
): Promise<void> {
  await page.evaluate((value) => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: null, argTypes: unknown[], args: unknown[]) => void };
    }).Module;
    mod?.ccall?.(
      'signal_smoke_legacy_recovery_set_send_admitted',
      null, ['number'], [value ? 1 : 0],
    );
  }, admitted);
}

async function smokeLegacyRecoveryCount(
  page: Page,
  kind: 'confirm' | 'cancel' | 'expire',
): Promise<number> {
  return page.evaluate((counter) => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall(
      `signal_smoke_legacy_recovery_${counter}_count`,
      'number', [], [],
    ) | 0;
  }, kind);
}

async function mobileDigitMask(page: Page): Promise<number> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('signal_mobile_digit_mask', 'number', [], []) | 0;
  });
}

async function stationPanelLabel(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('signal_station_panel_label', 'string', [], []) || '';
  });
}

async function stationPanelLegend(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('signal_station_panel_legend', 'string', [], []) || '';
  });
}

async function cargoLineageText(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('signal_trade_lineage_text', 'string', [], []) || '';
  });
}

async function stationCreditPerceptionSummary(page: Page): Promise<string> {
  return page.evaluate(() => new Promise<string>((resolve) => {
    requestAnimationFrame(() => {
      const mod = (window as unknown as {
        Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
      }).Module;
      if (!mod || typeof mod.ccall !== 'function') {
        resolve('');
        return;
      }
      resolve(mod.ccall('signal_station_credit_perception_summary', 'string', [], []) || '');
    });
  }));
}

async function signalLossPerceptionSummary(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('signal_signal_loss_perception_summary', 'string', [], []) || '';
  });
}

async function npcMotivePerceptionSummary(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('signal_npc_motive_perception_summary', 'string', [], []) || '';
  });
}

async function rememberedWorkPerceptionSummary(page: Page): Promise<string> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => string };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return '';
    return mod.ccall('signal_remembered_work_perception_summary', 'string', [], []) || '';
  });
}

async function stationPanelDigitSlots(page: Page): Promise<number> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('signal_station_panel_digit_slot_count', 'number', [], []) | 0;
  });
}

async function expectTouchControlsFit(page: Page): Promise<void> {
  const problems = await page.evaluate(() => {
    const buttons = Array.from(document.querySelectorAll<HTMLButtonElement>('.signal-touch-button'));
    const visible = buttons.filter((el) => {
      const rect = el.getBoundingClientRect();
      const style = window.getComputedStyle(el);
      return !el.hidden && style.display !== 'none' && rect.width > 0 && rect.height > 0;
    });
    const issues: string[] = [];
    const viewportWidth = window.innerWidth;
    const viewportHeight = window.innerHeight;

    for (const el of visible) {
      const rect = el.getBoundingClientRect();
      if (rect.left < -1 || rect.top < -1 || rect.right > viewportWidth + 1 || rect.bottom > viewportHeight + 1) {
        issues.push(`${el.dataset.control || 'button'} outside viewport`);
      }
      if (el.scrollWidth > el.clientWidth + 1 || el.scrollHeight > el.clientHeight + 1) {
        issues.push(`${el.dataset.control || 'button'} text overflow`);
      }
    }

    for (let i = 0; i < visible.length; i++) {
      const a = visible[i].getBoundingClientRect();
      for (let j = i + 1; j < visible.length; j++) {
        const b = visible[j].getBoundingClientRect();
        const overlapX = Math.max(0, Math.min(a.right, b.right) - Math.max(a.left, b.left));
        const overlapY = Math.max(0, Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top));
        if (overlapX > 1 && overlapY > 1) {
          issues.push(`${visible[i].dataset.control || 'button'} overlaps ${visible[j].dataset.control || 'button'}`);
        }
      }
    }

    return issues;
  });
  expect(problems).toEqual([]);
}

async function touchControlRects(page: Page, controlNames: string[]): Promise<Record<string, TouchControlRect>> {
  return page.evaluate((names) => {
    const rects: Record<string, TouchControlRect> = {};
    for (const name of names) {
      const el = document.querySelector<HTMLElement>(`[data-control="${name}"]`);
      if (!el) continue;
      const rect = el.getBoundingClientRect();
      const style = window.getComputedStyle(el);
      if (style.display === 'none' || rect.width <= 0 || rect.height <= 0) continue;
      rects[name] = {
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height,
      };
    }
    return rects;
  }, controlNames);
}

function expectTouchControlsKeepSlots(
  before: Record<string, TouchControlRect>,
  after: Record<string, TouchControlRect>,
  controlNames: string[],
): void {
  for (const name of controlNames) {
    expect(before[name], `${name} should have an initial slot`).toBeTruthy();
    expect(after[name], `${name} should keep a later slot`).toBeTruthy();
    for (const key of ['x', 'y', 'width', 'height'] as const) {
      expect(Math.abs(before[name][key] - after[name][key]), `${name} ${key} shifted`).toBeLessThanOrEqual(1.5);
    }
  }
}

async function setSmokeLoopState(page: Page, state: number): Promise<void> {
  const ok = await page.evaluate((nextState) => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('set_smoke_loop_state', 'number', ['number'], [nextState]);
  }, state);
  expect(ok).toBe(1);
}

async function constructionStateMask(page: Page): Promise<number> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return 0;
    return mod.ccall('signal_smoke_construction_state_mask', 'number', [], []);
  });
}

type TractorDrawTelemetry = {
  count: number;
  sourceType: number;
  targetType: number;
  sourceX: number;
  sourceY: number;
  targetX: number;
  targetY: number;
  span: number;
  amplitude: number;
  tautness: number;
  intensity: number;
};

async function tractorDrawTelemetry(
  page: Page,
  visual: number,
): Promise<TractorDrawTelemetry> {
  return page.evaluate((visualKind) => {
    const mod = (window as unknown as {
      Module?: {
        ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number;
      };
    }).Module;
    const read = (name: string, returnType = 'number') => {
      if (!mod || typeof mod.ccall !== 'function') return 0;
      return Number(mod.ccall(name, returnType, ['number'], [visualKind])) || 0;
    };
    return {
      count: read('signal_tractor_draw_count'),
      sourceType: read('signal_tractor_draw_source_type'),
      targetType: read('signal_tractor_draw_target_type'),
      sourceX: read('signal_tractor_draw_source_x'),
      sourceY: read('signal_tractor_draw_source_y'),
      targetX: read('signal_tractor_draw_target_x'),
      targetY: read('signal_tractor_draw_target_y'),
      span: read('signal_tractor_draw_span'),
      amplitude: read('signal_tractor_draw_amplitude'),
      tautness: read('signal_tractor_draw_tautness'),
      intensity: read('signal_tractor_draw_intensity'),
    };
  }, visual);
}

type RenderQueueTelemetry = {
  vertices: number;
  commands: number;
  errorMask: number;
  frameMs: number;
};

async function renderQueueTelemetry(page: Page): Promise<RenderQueueTelemetry> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: {
        ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number;
      };
    }).Module;
    const read = (name: string, returnType = 'number') => {
      if (!mod || typeof mod.ccall !== 'function') return 0;
      return Number(mod.ccall(name, returnType, [], [])) || 0;
    };
    return {
      vertices: read('signal_render_queued_vertices'),
      commands: read('signal_render_queued_commands'),
      errorMask: read('signal_render_sgl_error_mask'),
      frameMs: read('signal_render_frame_duration_ms'),
    };
  });
}

const smokeLoopState = {
  clear: 0,
  fragmentsNearby: 1,
  tractorReaching: 2,
  tractorLock: 3,
  towing: 4,
  hailReady: 5,
  hailNotice: 6,
  planGhost: 7,
  planSlot: 8,
  scaffoldSnap: 9,
  supplyNeed: 10,
  yardBlocked: 11,
  abandonedPlan: 12,
  fractureTableau: 13,
  remotePilotScan: 14,
  weakSignalVisual: 15,
  narrowCameraOffset: 16,
  cupriteGate: 17,
  scanLaserFab: 18,
  trackedCupriteContract: 19,
  onboardingDeliver: 20,
  onboardingReturn: 21,
  onboardingMarket: 22,
  onboardingComplete: 23,
  cargoTowing: 24,
  moduleCargoTractor: 25,
  rockSmeltPath: 26,
  rockRouteTarget: 27,
  rockRouteTow: 28,
  rockRouteDegraded: 29,
  cargoLineage: 30,
  localMoney: 31,
  npcMotiveCrisp: 32,
  npcMotiveDegraded: 33,
  rememberedWorkCrisp: 34,
  rememberedWorkDegraded: 35,
  constructionConsequence: 36,
  stationFragmentTractor: 37,
  refitSupplyActive: 38,
  refitSupplyInactive: 39,
  refitWorkAged: 40,
  cargoHopperGuide: 41,
} as const;

const mobileFlag = {
  docked: 1 << 0,
  stationTrade: 1 << 10,
  stationWork: 1 << 11,
  planActive: 1 << 3,
  canFlight: 1 << 16,
  canPage: 1 << 23,
  canSell: 1 << 24,
  canDigits: 1 << 25,
} as const;

const legacyRecoveryFlag = {
  visible: 1 << 0,
  canConfirm: 1 << 1,
  canCancel: 1 << 2,
  confirming: 1 << 3,
  result: 1 << 4,
  success: 1 << 5,
} as const;

async function waitForRenderedGame(
  page: Page,
  canvas: Locator,
  requireLiveRelay = usesLiveSmokeUrl(),
): Promise<void> {
  await expect(canvas).toBeVisible({ timeout: 20_000 });
  await waitForRuntime(page);

  const box = await canvas.boundingBox();
  expect(box).toBeTruthy();
  expect(box!.width).toBeGreaterThan(100);
  expect(box!.height).toBeGreaterThan(100);

  await expect
    .poll(async () => (await readCanvasStats(canvas)).nonBlackRatio, {
      timeout: 12_000,
      message: 'canvas should contain rendered pixels',
    })
    .toBeGreaterThan(0.05);

  await expect
    .poll(async () => (await readCanvasStats(canvas)).uniqueBuckets, {
      timeout: 12_000,
      message: 'canvas should contain varied game pixels',
    })
    .toBeGreaterThan(8);

  const signal = await signalStrength(page);
  expect(signal).not.toBeNull();
  expect(signal!).toBeGreaterThanOrEqual(0);

  if (requireLiveRelay) {
    await expect
      .poll(async () => (await signalStrength(page)) ?? -1, {
        timeout: 8_000,
        message: 'live smoke should connect to the relay',
      })
      .toBeGreaterThan(0);
    await page.waitForTimeout(2_000);
    await expect
      .poll(async () => (await signalStrength(page)) ?? -1, {
        timeout: 2_000,
        message: 'live smoke should stay connected to the relay',
      })
      .toBeGreaterThan(0);
  }
}

async function attachPerceptionReview(
  testInfo: TestInfo,
  canvas: Locator,
  scenario: string,
  viewport: 'desktop' | 'narrow',
): Promise<void> {
  await canvas.page().waitForTimeout(100);
  const box = await canvas.boundingBox();
  expect(box, `${scenario} — ${viewport} canvas should exist`).toBeTruthy();
  expect(box!.x, `${scenario} — ${viewport} canvas should stay on-screen`).toBeGreaterThanOrEqual(0);
  expect(box!.y, `${scenario} — ${viewport} canvas should stay on-screen`).toBeGreaterThanOrEqual(0);
  const viewportSize = canvas.page().viewportSize();
  expect(viewportSize).toBeTruthy();
  expect(
    box!.x + box!.width,
    `${scenario} — ${viewport} canvas should not overflow horizontally`,
  ).toBeLessThanOrEqual(viewportSize!.width + 1);
  expect(
    box!.y + box!.height,
    `${scenario} — ${viewport} canvas should not overflow vertically`,
  ).toBeLessThanOrEqual(viewportSize!.height + 1);
  const path = testInfo.outputPath(`perception-${scenario}-${viewport}.png`);
  await canvas.screenshot({ path });
  await testInfo.attach(`perception-${scenario}-${viewport}`, {
    path,
    contentType: 'image/png',
  });
}

async function loadGame(
  page: Page,
  requireLiveRelay = usesLiveSmokeUrl(),
  options: { singleplayer?: boolean } = {},
): Promise<Locator> {
  await page.goto(smokeUrl(options));
  const canvas = page.locator('canvas');
  await waitForRenderedGame(page, canvas, requireLiveRelay);
  return canvas;
}

async function tap(page: Page, key: string, pauseMs = 80): Promise<void> {
  await page.keyboard.press(key);
  await page.waitForTimeout(pauseMs);
}

async function hold(page: Page, key: string, ms: number): Promise<void> {
  await page.keyboard.down(key);
  await page.waitForTimeout(ms);
  await page.keyboard.up(key);
  await page.waitForTimeout(80);
}

async function holdChord(page: Page, keys: string[], ms: number): Promise<void> {
  for (const key of keys) await page.keyboard.down(key);
  try {
    await page.waitForTimeout(ms);
  } finally {
    for (let i = keys.length - 1; i >= 0; i--) await page.keyboard.up(keys[i]);
  }
  await page.waitForTimeout(80);
}

async function driveCoreControls(page: Page, canvas: Locator): Promise<void> {
  await canvas.click();

  await tap(page, 'Escape');
  await tap(page, 'E');        // launch if docked, interact if already undocked
  await hold(page, 'W', 450);
  await hold(page, 'A', 220);
  await hold(page, 'D', 220);
  await hold(page, 'Shift', 300);
  await tap(page, 'H');        // hail / contact scan
  await hold(page, 'M', 500);  // mining beam
  await hold(page, 'Space', 550);
  await tap(page, 'Space');    // release tow tap path
  await tap(page, 'B');        // plan mode
  await tap(page, 'R');        // cycle planned module / tow control
  await tap(page, 'E');        // place / interact
  await tap(page, 'Escape');   // leave plan mode
  await tap(page, 'Tab');      // docked tab cycling if docked
  await tap(page, 'F');        // docked buy primary product if docked
  await tap(page, 'S');        // docked sell-all if docked
  await tap(page, '1');        // first visible row action
  await tap(page, '2');        // second visible row action / repair
  await tap(page, 'O');        // autopilot toggle
}

const authFixtureIdentity =
  'nWGxne/9WmC6hEr0kuwsxERJxWl7MmkZcDusAxyuf2DXWpgBgrEKt9VL/tPJZAc6DuFy89qmIyWvAhpo9wdRGg==';
const authFixtureToken = '0102030405060708';

async function installAuthFixture(page: Page): Promise<void> {
  await page.addInitScript(({ identity, token }) => {
    window.localStorage.setItem('signal:identity', identity);
    window.localStorage.setItem('signal_session_token', token);
  }, { identity: authFixtureIdentity, token: authFixtureToken });
}

function protocolInfoPacket(version: number): Buffer {
  const packet = Buffer.alloc(8 + 2 * 12);
  packet[0] = 0x41;
  packet.writeUInt16LE(version, 1);
  packet.writeUInt32LE(1, 3); // SIGNAL_PROTOCOL_CAP_PROTOCOL_INFO
  packet[7] = 2;

  // WORLD_CARGO_PODS: live, server→client, relevance-filtered,
  // header=2, record=72, max=64, non-zero cadence.
  packet[8] = 0x46;
  packet[9] = 2;
  packet.writeUInt16LE(0x0009, 10);
  packet.writeUInt16LE(2, 12);
  packet.writeUInt16LE(72, 14);
  packet.writeUInt16LE(64, 16);
  packet.writeUInt16LE(100, 18);

  // WORLD_CARGO_PODS_Q: the exact compact v4 companion schema.
  packet[20] = 0x62;
  packet[21] = 2;
  packet.writeUInt16LE(0x0009, 22);
  packet.writeUInt16LE(2, 24);
  packet.writeUInt16LE(62, 26);
  packet.writeUInt16LE(64, 28);
  packet.writeUInt16LE(100, 30);
  return packet;
}

function authoritativeLocalStatePacket(playerId = 0): Buffer {
  const packet = Buffer.alloc(67);
  packet[0] = 0x03; // NET_MSG_STATE
  packet[1] = playerId;
  return packet;
}

function isAuthBootstrapPacket(packet: Buffer): boolean {
  return packet[0] === 0x32 || packet[0] === 0x20 || packet[0] === 0x3f;
}

async function expectProtocolRejectionWithoutAuth(
  page: Page,
  serverUrl: string,
  serverPacket: Buffer | string,
  sendInitialJoin = true,
): Promise<void> {
  const authPackets: Buffer[] = [];
  await page.routeWebSocket(serverUrl, ws => {
    ws.onMessage(message => {
      const packet = typeof message === 'string'
        ? Buffer.from(message)
        : Buffer.from(message);
      if (isAuthBootstrapPacket(packet)) authPackets.push(packet);
    });
    if (sendInitialJoin) ws.send(Buffer.from([0x01, 0x00]));
    ws.send(serverPacket);
  });

  await page.goto(
    `/play.html?smoke=1&pv=${currentProtocolVersion}&server=${encodeURIComponent(serverUrl)}`,
  );
  await expect.poll(
    () => wasmNumber(page, 'signal_debug_auth_transport_closes'),
    { timeout: 10_000 },
  ).toBe(1);
  expect(authPackets).toEqual([]);
}

async function verifyCapturedPubkeyProof(
  page: Page,
  packet: Buffer,
  domain: 'prove-pubkey-v1' | 'prove-pubkey-v2',
  challenge: number[] = [],
): Promise<boolean> {
  return page.evaluate(async ({ wireBytes, proofDomain, challengeBytes }) => {
    const wire = Uint8Array.from(wireBytes);
    const pubkey = wire.slice(1, 33);
    const token = wire.slice(33, 41);
    const signature = wire.slice(41, 105);
    const domainBytes = new TextEncoder().encode(proofDomain);
    const signed = new Uint8Array(
      domainBytes.length + pubkey.length + token.length + challengeBytes.length,
    );
    signed.set(domainBytes);
    signed.set(pubkey, domainBytes.length);
    signed.set(token, domainBytes.length + pubkey.length);
    signed.set(
      Uint8Array.from(challengeBytes),
      domainBytes.length + pubkey.length + token.length,
    );
    const key = await crypto.subtle.importKey(
      'raw', pubkey, { name: 'Ed25519' }, false, ['verify'],
    );
    return crypto.subtle.verify(
      { name: 'Ed25519' }, key, signature, signed,
    );
  }, {
    wireBytes: Array.from(packet),
    proofDomain: domain,
    challengeBytes: challenge,
  });
}

test.describe('Browser smoke tests', () => {
  const rootBundleSmokeTest = process.env.SIGNAL_PRE_PROMOTION_SMOKE === '1' ? test.skip : test;

  test('local authority memory is lazy remotely and restart-safe locally', async ({ page }) => {
    test.skip(
      usesLiveSmokeUrl() && process.env.SIGNAL_LOCAL_MEMORY_SMOKE !== '1',
      'mode-specific memory checks require the local browser bundle',
    );

    const remoteUrl = 'ws://signal-memory-budget.invalid/ws';
    await page.routeWebSocket(remoteUrl, ws => {
      ws.onMessage(() => {
        // Holding the synthetic transport open is enough to prove startup
        // mode selection; this test deliberately sends no world snapshot.
      });
    });

    const logs = installFatalCollectors(page);
    await page.goto(
      `/play.html?smoke=1&server=${encodeURIComponent(remoteUrl)}`,
    );
    await waitForRuntime(page);

    expect(await wasmNumber(page, 'signal_debug_local_authority_state'))
      .toBe(1 << 3);
    expect(await wasmNumber(page, 'signal_debug_local_authority_generation'))
      .toBe(0);
    expect(await wasmMemoryPages(page)).toBeLessThanOrEqual(896);

    await page.goto('/play.html?singleplayer=1&smoke=1');
    await waitForRenderedGame(page, page.locator('canvas'), false);
    expect(await wasmNumber(page, 'signal_debug_local_authority_state'))
      .toBe(0x0f);
    const firstGeneration =
      await wasmNumber(page, 'signal_debug_local_authority_generation');
    expect(firstGeneration).toBe(1);

    expect(await wasmNumber(page, 'signal_debug_restart_local_authority'))
      .toBe(1);
    await expect.poll(
      () => wasmNumber(page, 'signal_debug_local_authority_state'),
    ).toBe(0x0f);
    expect(await wasmNumber(page, 'signal_debug_local_authority_generation'))
      .toBe(2);
    expect(await wasmMemoryPages(page)).toBeLessThanOrEqual(1280);
    expectNoFatalErrors(logs);
  });

  test('a multiplayer disconnect never starts a fresh local universe', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'mock transport requires the local browser bundle');

    const remoteUrl = 'ws://signal-disconnect-parity.invalid/ws';
    let closeRemote: (() => Promise<void>) | undefined;
    await page.routeWebSocket(remoteUrl, ws => {
      closeRemote = () => ws.close({ code: 1012, reason: 'smoke disconnect' });
      ws.onMessage(() => {});
    });

    const logs = installFatalCollectors(page);
    await page.goto(
      `/play.html?smoke=1&server=${encodeURIComponent(remoteUrl)}`,
    );
    await waitForRuntime(page);
    await expect.poll(
      () => wasmNumber(page, 'signal_debug_net_connected'),
      { timeout: 5_000 },
    ).toBe(1);
    expect(await wasmNumber(page, 'signal_debug_local_authority_state'))
      .toBe(1 << 3);
    expect(await wasmNumber(page, 'signal_debug_local_authority_generation'))
      .toBe(0);

    expect(closeRemote).toBeDefined();
    await closeRemote!();
    await expect.poll(
      () => wasmNumber(page, 'signal_debug_net_connected'),
      { timeout: 5_000 },
    ).toBe(0);
    expect(await wasmNumber(page, 'signal_debug_local_authority_state'))
      .toBe(1 << 3);
    expect(await wasmNumber(page, 'signal_debug_local_authority_generation'))
      .toBe(0);
    expectNoFatalErrors(logs);
  });

  test('multiplayer is the default while singleplayer and RTC remain explicit options', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'transport selection is covered against the local bundle');

    await page.goto('/play.html', { waitUntil: 'domcontentloaded' });
    expect(await page.evaluate(() => (window as unknown as { SIGNAL_SERVER?: string }).SIGNAL_SERVER))
      .toBe(`ws://${new URL(page.url()).host}/ws`);

    await page.goto('/play.html?singleplayer=1', { waitUntil: 'domcontentloaded' });
    expect(await page.evaluate(() => (window as unknown as { SIGNAL_SERVER?: string }).SIGNAL_SERVER))
      .toBe('');

    await page.goto('/play.html?transport=rtc', { waitUntil: 'domcontentloaded' });
    expect(await page.evaluate(() => (window as unknown as { SIGNAL_SERVER?: string }).SIGNAL_SERVER))
      .toBe(`rtc://${new URL(page.url()).host}/rtc/signal-main`);
  });

  test(`v${currentProtocolVersion} auth waits for exact discovery and then sends one challenged proof`, async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'mock transport requires the local browser bundle');
    await installAuthFixture(page);

    const messageTypes: number[] = [];
    const messageTypesBeforeAdvertisement: number[] = [];
    let protocolAdvertised = false;
    let authBeforeAdvertisement = false;
    let challengeSent = false;
    let registerPacket: Buffer | undefined;
    let sessionPacket: Buffer | undefined;
    let proofPacket: Buffer | undefined;
    const challenge = Array.from({ length: 32 }, (_, i) => 0x40 + i);
    await page.routeWebSocket('ws://signal-auth-current.invalid/ws', ws => {
      ws.onMessage(message => {
        const packet = typeof message === 'string'
          ? Buffer.from(message)
          : Buffer.from(message);
        messageTypes.push(packet[0]);
        if (!protocolAdvertised) {
          messageTypesBeforeAdvertisement.push(packet[0]);
        }
        if (isAuthBootstrapPacket(packet) && !protocolAdvertised) {
          authBeforeAdvertisement = true;
        }
        if (packet[0] === 0x3f) {
          proofPacket = packet;
        } else if (packet[0] === 0x32) {
          registerPacket = packet;
        } else if (packet[0] === 0x20) {
          sessionPacket = packet;
          if (!challengeSent) {
            challengeSent = true;
            ws.send(Buffer.from([0x70, ...challenge]));
          }
        }
      });
      ws.send(Buffer.from([0x01, 0x00]));
      setTimeout(() => {
        if (!protocolAdvertised) {
          protocolAdvertised = true;
          ws.send(protocolInfoPacket(currentProtocolVersion));
        }
      }, 75);
    });

    await page.goto(
      `/play.html?smoke=1&server=${encodeURIComponent('ws://signal-auth-current.invalid/ws')}`,
    );
    await expect.poll(() => proofPacket?.length ?? 0, { timeout: 10_000 })
      .toBe(105);

    expect(authBeforeAdvertisement).toBe(false);
    expect(messageTypesBeforeAdvertisement).toEqual([]);
    expect(registerPacket?.length).toBe(33);
    expect(sessionPacket?.length).toBe(18);
    expect(sessionPacket?.readUInt16LE(16)).toBe(currentProtocolVersion);
    expect(challengeSent).toBe(true);
    expect(messageTypes.indexOf(0x32)).toBeLessThan(messageTypes.indexOf(0x20));
    expect(messageTypes.indexOf(0x20)).toBeLessThan(messageTypes.indexOf(0x3f));
    expect(await verifyCapturedPubkeyProof(
      page, proofPacket!, 'prove-pubkey-v2', challenge,
    )).toBe(true);
    expect(await verifyCapturedPubkeyProof(
      page, proofPacket!, 'prove-pubkey-v1',
    )).toBe(false);
  });

  test(`v${currentProtocolVersion} blocks gameplay until proof receives authoritative admission`, async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'mock transport requires the local browser bundle');
    await installAuthFixture(page);

    const authTypes: number[] = [];
    const preAdmissionGameplayTypes: number[] = [];
    const postAdmissionGameplayTypes: number[] = [];
    const challenge = Array.from({ length: 32 }, (_, i) => 0xa0 + i);
    let challengeScheduled = false;
    let authoritativeStateSent = false;

    await page.routeWebSocket('ws://signal-auth-current-admission.invalid/ws', ws => {
      ws.onMessage(message => {
        const packet = typeof message === 'string'
          ? Buffer.from(message)
          : Buffer.from(message);
        if (isAuthBootstrapPacket(packet)) {
          authTypes.push(packet[0]);
        } else if (authoritativeStateSent) {
          postAdmissionGameplayTypes.push(packet[0]);
        } else {
          preAdmissionGameplayTypes.push(packet[0]);
        }

        if (packet[0] === 0x20 && !challengeScheduled) {
          challengeScheduled = true;
          setTimeout(() => {
            ws.send(Buffer.from([0x70, ...challenge]));
          }, 250);
        } else if (packet[0] === 0x3f && !authoritativeStateSent) {
          setTimeout(() => {
            authoritativeStateSent = true;
            ws.send(authoritativeLocalStatePacket());
          }, 25);
        }
      });
      ws.send(Buffer.from([0x01, 0x00]));
      ws.send(protocolInfoPacket(currentProtocolVersion));
    });

    await page.goto(
      `/play.html?smoke=1&pv=${currentProtocolVersion}&server=${encodeURIComponent('ws://signal-auth-current-admission.invalid/ws')}`,
    );
    await expect.poll(
      () => authTypes.filter(type => type === 0x3f).length,
      { timeout: 10_000 },
    ).toBe(1);
    await expect.poll(
      () => postAdmissionGameplayTypes.includes(0x04),
      { timeout: 10_000 },
    ).toBe(true);

    expect(preAdmissionGameplayTypes).toEqual([]);
    expect(authTypes.filter(type => type === 0x32)).toHaveLength(1);
    expect(authTypes.filter(type => type === 0x20)).toHaveLength(1);
    expect(authTypes.filter(type => type === 0x3f)).toHaveLength(1);
    expect(await wasmNumber(page, 'signal_debug_auth_transport_closes'))
      .toBe(0);
  });

  test(`v${currentProtocolVersion} identical duplicate discovery does not resend authentication`, async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'mock transport requires the local browser bundle');
    await installAuthFixture(page);

    const authTypes: number[] = [];
    let duplicateSent = false;
    const challenge = Array.from({ length: 32 }, (_, i) => 0x20 + i);
    await page.routeWebSocket('ws://signal-auth-current-duplicate.invalid/ws', ws => {
      ws.onMessage(message => {
        const packet = typeof message === 'string'
          ? Buffer.from(message)
          : Buffer.from(message);
        if (isAuthBootstrapPacket(packet)) authTypes.push(packet[0]);
        if (packet[0] === 0x20 && !duplicateSent) {
          duplicateSent = true;
          ws.send(protocolInfoPacket(currentProtocolVersion));
          ws.send(Buffer.from([0x70, ...challenge]));
        }
      });
      ws.send(Buffer.from([0x01, 0x00]));
      ws.send(protocolInfoPacket(currentProtocolVersion));
    });

    await page.goto(
      `/play.html?smoke=1&pv=${currentProtocolVersion}&server=${encodeURIComponent('ws://signal-auth-current-duplicate.invalid/ws')}`,
    );
    await expect.poll(
      () => authTypes.filter(type => type === 0x3f).length,
      { timeout: 10_000 },
    ).toBe(1);
    expect(authTypes.filter(type => type === 0x32)).toHaveLength(1);
    expect(authTypes.filter(type => type === 0x20)).toHaveLength(1);
    expect(await wasmNumber(page, 'signal_debug_auth_transport_closes'))
      .toBe(0);
  });

  test(`v${currentProtocolVersion} changed duplicate discovery closes without reauthentication`, async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'mock transport requires the local browser bundle');
    await installAuthFixture(page);

    const authTypes: number[] = [];
    let changedDuplicateSent = false;
    await page.routeWebSocket('ws://signal-auth-current-renegotiate.invalid/ws', ws => {
      ws.onMessage(message => {
        const packet = typeof message === 'string'
          ? Buffer.from(message)
          : Buffer.from(message);
        if (isAuthBootstrapPacket(packet)) authTypes.push(packet[0]);
        if (packet[0] === 0x20 && !changedDuplicateSent) {
          changedDuplicateSent = true;
          const changed = protocolInfoPacket(currentProtocolVersion);
          changed.writeUInt16LE(101, 18);
          ws.send(changed);
        }
      });
      ws.send(Buffer.from([0x01, 0x00]));
      ws.send(protocolInfoPacket(currentProtocolVersion));
    });

    await page.goto(
      `/play.html?smoke=1&pv=${currentProtocolVersion}&server=${encodeURIComponent('ws://signal-auth-current-renegotiate.invalid/ws')}`,
    );
    await expect.poll(
      () => wasmNumber(page, 'signal_debug_auth_transport_closes'),
      { timeout: 10_000 },
    ).toBe(1);
    expect(authTypes.filter(type => type === 0x32)).toHaveLength(1);
    expect(authTypes.filter(type => type === 0x20)).toHaveLength(1);
    expect(authTypes.filter(type => type === 0x3f)).toHaveLength(0);
  });

  const incompatibleProtocolVersions = [
    ...Array.from(
      { length: Math.max(0, currentProtocolVersion - 2) },
      (_, index) => index + 2,
    ),
    currentProtocolVersion + 1,
  ];
  for (const version of incompatibleProtocolVersions) {
    test(`v${currentProtocolVersion} client rejects protocol v${version} without sending auth`, async ({ page }) => {
      test.skip(usesLiveSmokeUrl(), 'mock transport requires the local browser bundle');
      await installAuthFixture(page);
      await expectProtocolRejectionWithoutAuth(
        page,
        `ws://signal-auth-version-${version}.invalid/ws`,
        protocolInfoPacket(version),
      );
    });
  }

  const malformedProtocolCases: Array<{
    name: string;
    packet: () => Buffer | string;
  }> = [
    {
      name: 'empty binary traffic before discovery',
      packet: () => Buffer.alloc(0),
    },
    {
      name: 'text traffic before discovery',
      packet: () => 'protocol-noise',
    },
    {
      name: 'challenge before discovery',
      packet: () => Buffer.from([
        0x70,
        ...Array.from({ length: 32 }, (_, i) => 0x60 + i),
      ]),
    },
    {
      name: 'truncated protocol info',
      packet: () => Buffer.from([0x41, currentProtocolVersion & 0xff]),
    },
    {
      name: 'protocol info with a trailing partial record',
      packet: () => Buffer.concat([
        protocolInfoPacket(currentProtocolVersion),
        Buffer.from([0x00]),
      ]),
    },
    {
      name: 'protocol info with the wrong pod stride',
      packet: () => {
        const packet = protocolInfoPacket(currentProtocolVersion);
        packet.writeUInt16LE(71, 14);
        return packet;
      },
    },
  ];
  for (const protocolCase of malformedProtocolCases) {
    test(`v${currentProtocolVersion} client rejects ${protocolCase.name} without sending auth`, async ({ page }) => {
      test.skip(usesLiveSmokeUrl(), 'mock transport requires the local browser bundle');
      await installAuthFixture(page);
      const slug = protocolCase.name.replaceAll(' ', '-');
      await expectProtocolRejectionWithoutAuth(
        page,
        `ws://signal-auth-${slug}.invalid/ws`,
        protocolCase.packet(),
      );
    });
  }

  const invalidJoinCases: Array<{
    name: string;
    packet: () => Buffer;
  }> = [
    {
      name: 'protocol discovery before remote join',
      packet: () => protocolInfoPacket(currentProtocolVersion),
    },
    {
      name: 'reserved remote join id',
      packet: () => Buffer.from([0x01, 0xff]),
    },
    {
      name: 'out of range remote join id',
      packet: () => Buffer.from([0x01, 0x20]),
    },
  ];
  for (const joinCase of invalidJoinCases) {
    test(`v${currentProtocolVersion} client rejects ${joinCase.name} without sending auth`, async ({ page }) => {
      test.skip(usesLiveSmokeUrl(), 'mock transport requires the local browser bundle');
      await installAuthFixture(page);
      const slug = joinCase.name.replaceAll(' ', '-');
      await expectProtocolRejectionWithoutAuth(
        page,
        `ws://signal-auth-${slug}.invalid/ws`,
        joinCase.packet(),
        false,
      );
    });
  }

  test('rejected proof send closes authentication transport', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'fault injection requires the local browser bundle');
    await installAuthFixture(page);
    await page.addInitScript(() => {
      (globalThis as unknown as {
        SIGNAL_TEST_REJECT_AUTH_PROOF_SEND: boolean;
        SIGNAL_TEST_AUTH_PROOF_SEND_FAILURES: number;
      }).SIGNAL_TEST_REJECT_AUTH_PROOF_SEND = true;
      (globalThis as unknown as {
        SIGNAL_TEST_AUTH_PROOF_SEND_FAILURES: number;
      }).SIGNAL_TEST_AUTH_PROOF_SEND_FAILURES = 0;
    });

    let challengeSent = false;
    let proofPacketsReceived = 0;
    await page.routeWebSocket('ws://signal-auth-send-fail.invalid/ws', ws => {
      ws.onMessage(message => {
        const packet = typeof message === 'string'
          ? Buffer.from(message)
          : Buffer.from(message);
        if (packet[0] === 0x3f) proofPacketsReceived++;
        if (packet[0] === 0x20 && !challengeSent) {
          challengeSent = true;
          ws.send(Buffer.from([
            0x70,
            ...Array.from({ length: 32 }, (_, i) => 0x90 + i),
          ]));
        }
      });
      ws.send(Buffer.from([0x01, 0x00]));
      ws.send(protocolInfoPacket(currentProtocolVersion));
    });

    await page.goto(
      `/play.html?smoke=1&pv=${currentProtocolVersion}&server=${encodeURIComponent('ws://signal-auth-send-fail.invalid/ws')}`,
    );
    await expect.poll(
      () => page.evaluate(() => (
        globalThis as unknown as {
          SIGNAL_TEST_AUTH_PROOF_SEND_FAILURES: number;
        }
      ).SIGNAL_TEST_AUTH_PROOF_SEND_FAILURES),
      { timeout: 10_000 },
    ).toBe(1);
    await expect.poll(
      () => wasmNumber(page, 'signal_debug_auth_transport_closes'),
    ).toBe(1);
    expect(challengeSent).toBe(true);
    expect(proofPacketsReceived).toBe(0);
  });

  test('boots, renders, and persists browser identity across reload', async ({ page }) => {
    const logs = installFatalCollectors(page);

    const canvas = await loadGame(page);
    await expect
      .poll(async () => hudHintText(page), { timeout: 5_000 })
      .toContain('SIGNAL // GUIDE // LAUNCH FROM DOCK');

    const firstIdentity = await page.evaluate(() => window.localStorage.getItem('signal:identity'));
    expect(firstIdentity).toMatch(/^[A-Za-z0-9+/]{86}==$/);

    await canvas.click();
    await tap(page, 'E');
    if (usesLiveSmokeUrl()) {
      // Deployed smoke runs against the multiplayer URL, where launch timing is
      // not the deterministic singleplayer transition that local smoke proves.
      await expect
        .poll(async () => hudHintText(page), { timeout: 8_000 })
        .toMatch(/SIGNAL \/\/ GUIDE \/\/ LAUNCH FROM DOCK|SIGNAL \/\/ GUIDE \/\/ FLIGHT CHECK/);
    } else {
      await expect
        .poll(async () => hudHintText(page), { timeout: 8_000 })
        .toContain('SIGNAL // GUIDE // FLIGHT CHECK');
    }

    await page.reload();
    await waitForRenderedGame(page, page.locator('canvas'));
    const secondIdentity = await page.evaluate(() => window.localStorage.getItem('signal:identity'));
    expect(secondIdentity).toBe(firstIdentity);

    expectNoFatalErrors(logs);
  });

  test('WebCrypto failure leaves identity and authentication unpersisted', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'fault injection requires the local browser bundle');

    await page.addInitScript(() => {
      window.localStorage.removeItem('signal:identity');
      window.localStorage.removeItem('signal:identity.bad');
      window.localStorage.removeItem('signal_session_token');
      Object.defineProperty(globalThis.crypto, 'getRandomValues', {
        configurable: true,
        value: () => {
          throw new DOMException('injected entropy failure', 'OperationError');
        },
      });
    });

    const logs = installFatalCollectors(page);
    await page.goto('/play.html?singleplayer=1&smoke=1');
    await waitForRenderedGame(page, page.locator('canvas'), false);

    expect(await wasmNumber(page, 'signal_debug_identity_available')).toBe(0);
    expect(await wasmNumber(page, 'signal_debug_auth_available')).toBe(0);
    expect(
      await page.evaluate(() => window.localStorage.getItem('signal:identity')),
    ).toBeNull();
    expect(
      await page.evaluate(() => window.localStorage.getItem('signal:identity.bad')),
    ).toBeNull();
    expect(
      await page.evaluate(() => window.localStorage.getItem('signal_session_token')),
    ).toBeNull();
    expectNoFatalErrors(logs);
  });

  test('WebCrypto failure refuses token creation with an existing identity', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'fault injection requires the local browser bundle');

    const persistedIdentity =
      'nWGxne/9WmC6hEr0kuwsxERJxWl7MmkZcDusAxyuf2DXWpgBgrEKt9VL/tPJZAc6DuFy89qmIyWvAhpo9wdRGg==';
    await page.addInitScript((identity) => {
      window.localStorage.setItem('signal:identity', identity);
      window.localStorage.removeItem('signal_session_token');
      Object.defineProperty(globalThis.crypto, 'getRandomValues', {
        configurable: true,
        value: () => {
          throw new DOMException('injected token entropy failure', 'OperationError');
        },
      });
    }, persistedIdentity);

    const logs = installFatalCollectors(page);
    await page.goto('/play.html?singleplayer=1&smoke=1');
    await waitForRenderedGame(page, page.locator('canvas'), false);

    expect(await wasmNumber(page, 'signal_debug_identity_available')).toBe(1);
    expect(await wasmNumber(page, 'signal_debug_auth_available')).toBe(0);
    expect(
      await page.evaluate(() => window.localStorage.getItem('signal:identity')),
    ).toBe(persistedIdentity);
    expect(
      await page.evaluate(() => window.localStorage.getItem('signal_session_token')),
    ).toBeNull();
    expectNoFatalErrors(logs);
  });

  test('WebCrypto failure refuses a loopback challenge with persisted auth', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'fault injection requires the local browser bundle');

    const persistedIdentity =
      'nWGxne/9WmC6hEr0kuwsxERJxWl7MmkZcDusAxyuf2DXWpgBgrEKt9VL/tPJZAc6DuFy89qmIyWvAhpo9wdRGg==';
    const persistedToken = '0102030405060708';
    await page.addInitScript(({ identity, token }) => {
      window.localStorage.setItem('signal:identity', identity);
      window.localStorage.setItem('signal_session_token', token);
      Object.defineProperty(globalThis.crypto, 'getRandomValues', {
        configurable: true,
        value: () => {
          throw new DOMException('injected challenge entropy failure', 'OperationError');
        },
      });
    }, { identity: persistedIdentity, token: persistedToken });

    const logs = installFatalCollectors(page);
    await page.goto('/play.html?singleplayer=1&smoke=1');
    await waitForRenderedGame(page, page.locator('canvas'), false);

    expect(await wasmNumber(page, 'signal_debug_identity_available')).toBe(1);
    expect(await wasmNumber(page, 'signal_debug_auth_available')).toBe(0);
    expect(
      await page.evaluate(() => window.localStorage.getItem('signal:identity')),
    ).toBe(persistedIdentity);
    expect(
      await page.evaluate(() => window.localStorage.getItem('signal_session_token')),
    ).toBe(persistedToken);
    expectNoFatalErrors(logs);
  });

  test('episode fetch 404 stays unwatched and a successful retry commits once', async ({ page }) => {
    test.skip(
      usesLiveSmokeUrl() && process.env.SIGNAL_EPISODE_RETRY_SMOKE !== '1',
      'fault injection requires the local browser bundle',
    );

    const episodeIndex = 2;
    const watchedMask = 1 << 0;
    const pendingMask = 1 << 1;
    const failureShift = 8;
    let fetches = 0;
    let releaseFirstResponse: (() => void) | undefined;
    const firstResponseGate = new Promise<void>(resolve => {
      releaseFirstResponse = resolve;
    });
    await page.route('**/anime/ep2-furnace.mpg', async route => {
      fetches++;
      if (fetches === 1) {
        await firstResponseGate;
        await route.fulfill({
          status: 404,
          contentType: 'text/plain',
          body: 'injected episode miss',
        });
        return;
      }
      await route.fulfill({
        status: 200,
        contentType: 'video/mpeg',
        body: episodeSmokeMpeg,
      });
    });

    const logs = installFatalCollectors(page);
    await loadGame(page, false, { singleplayer: true });
    expect(await wasmNumberArg(
      page, 'signal_smoke_episode_prepare', episodeIndex,
    )).toBe(1);
    expect(
      (Number(await page.evaluate(() => window.localStorage.getItem('signal_episodes'))) || 0) &
        (1 << episodeIndex),
    ).toBe(0);

    expect(await wasmNumberArg(
      page, 'signal_smoke_episode_trigger', episodeIndex,
    )).toBe(1);
    await expect.poll(() => fetches).toBe(1);
    expect(
      ((await wasmNumberArg(
        page, 'signal_smoke_episode_state', episodeIndex,
      )) ?? 0) & pendingMask,
    ).toBe(pendingMask);
    expect(
      (Number(await page.evaluate(() => window.localStorage.getItem('signal_episodes'))) || 0) &
        (1 << episodeIndex),
    ).toBe(0);
    releaseFirstResponse!();

    await expect.poll(async () => {
      const state = (await wasmNumberArg(
        page, 'signal_smoke_episode_state', episodeIndex,
      )) ?? 0;
      return {
        watched: state & watchedMask,
        pending: state & pendingMask,
        failure: state >> failureShift,
      };
    }).toEqual({ watched: 0, pending: 0, failure: 1 });

    expect(await wasmNumberArg(
      page, 'signal_smoke_episode_trigger', episodeIndex,
    )).toBe(1);
    await expect.poll(() => fetches).toBe(2);
    await expect.poll(async () => {
      const state = (await wasmNumberArg(
        page, 'signal_smoke_episode_state', episodeIndex,
      )) ?? 0;
      return state & watchedMask;
    }, {
      timeout: 10_000,
      message: 'first decoded frame should commit watched state',
    }).toBe(watchedMask);
    expect(
      (Number(await page.evaluate(() => window.localStorage.getItem('signal_episodes'))) || 0) &
        (1 << episodeIndex),
    ).toBe(1 << episodeIndex);

    expect(await wasmNumberArg(
      page, 'signal_smoke_episode_trigger', episodeIndex,
    )).toBe(1);
    await page.waitForTimeout(250);
    expect(fetches).toBe(2);
    expect(await wasmMemoryPages(page)).toBeLessThanOrEqual(1280);
    expectNoFatalErrors(logs);
  });

  test('E docks again immediately after launch while still in range', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'deterministic launch/dock timing requires local authority');

    const logs = installFatalCollectors(page);
    const canvas = await loadGame(page, false, { singleplayer: true });
    await canvas.click();

    await expect.poll(async () => (await playerStateSnapshot(page))?.docked ?? -1)
      .toBe(1);
    await tap(page, 'E');
    await expect
      .poll(async () => (await playerStateSnapshot(page))?.docked ?? -1, {
        timeout: 5_000,
      })
      .toBe(0);

    /* Launch leaves the ship inside DOCK_APPROACH_RANGE. An authoritative
     * undocked snapshot used to clear the client's proximity hint here, so E
     * emitted no action even though the server would have accepted docking. */
    await tap(page, 'E');
    await expect
      .poll(async () => (await playerStateSnapshot(page))?.docked ?? -1, {
        timeout: 8_000,
      })
      .toBe(1);

    expectNoFatalErrors(logs);
  });

  test('desktop core controls stay alive through the golden path keys', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    const canvas = await loadGame(page);

    await expect
      .poll(async () => hudHintText(page), { timeout: 5_000 })
      .toContain('SIGNAL // GUIDE // LAUNCH FROM DOCK');

    await driveCoreControls(page, canvas);
    if (!usesLiveSmokeUrl()) {
      await expect
        .poll(async () => (await netMotionSnapshot(page)).inputAcks, {
          timeout: 5_000,
          message: 'singleplayer loopback should produce authoritative input ACKs',
        })
        .toBeGreaterThan(0);
    }
    await expect
      .poll(async () => (await readCanvasStats(canvas)).uniqueBuckets, { timeout: 5_000 })
      .toBeGreaterThan(8);

    expectNoFatalErrors(logs, { allowExpectedLiveClose: true });
  });

  test('loopback flight corrections apply without stale deferral', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'requires local singleplayer loopback');
    test.setTimeout(45_000);

    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    const canvas = await loadGame(page);

    await canvas.click();
    await tap(page, 'Escape');
    await tap(page, 'E');
    await expect
      .poll(async () => (await playerStateSnapshot(page))?.docked ?? 1, {
        timeout: 8_000,
        message: 'local loopback launch should leave dock before measuring flight corrections',
      })
      .toBe(0);

    await resetNetMotionTelemetry(page);
    const acksBefore = (await netMotionSnapshot(page)).inputAcks;
    await hold(page, 'W', 2_500);

    await expect
      .poll(async () => (await netMotionSnapshot(page)).inputAcks, {
        timeout: 10_000,
        message: 'held thrust should receive authoritative loopback ACKs',
      })
      .toBeGreaterThan(acksBefore);
    await expect
      .poll(async () => (await netMotionSnapshot(page)).samples, {
        timeout: 10_000,
        message: 'loopback flight should collect motion correction samples',
      })
      .toBeGreaterThan(0);

    const motion = await netMotionSnapshot(page);
    expect(motion.deferredSamples).toBe(0);
    /* Raw loopback distance varies with browser frame scheduling because the
     * in-process server advances before this frame records prediction. Guard
     * the player-visible outcome and reject snaps/stale samples instead. */
    expect(motion.maxAppliedCorrection).toBeLessThan(40);
    expect(motion.maxRenderOffset).toBeLessThan(40);
    expect(motion.snapSamples).toBe(0);
    expect(motion.maxTickSkewAbs).toBeLessThan(12);

    expectNoFatalErrors(logs);
  });

  test('fixed-tick loopback gate rejects one-bit numeric drift', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'requires local singleplayer loopback');
    test.setTimeout(30_000);

    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    await loadGame(page, false, { singleplayer: true });
    await expect.poll(
      () => wasmNumber(page, 'signal_zero_latency_gate_ready'),
      {
        timeout: 10_000,
        message: 'local loopback authority should finish authentication',
      },
    ).toBe(1);

    const normalResult = await wasmNumber(
      page, 'signal_zero_latency_gate_run',
    );
    const normal = await zeroLatencyGateReport(page);
    expect(normalResult, JSON.stringify(normal)).toBe(1);
    expect(normal.status).toBe(1);
    expect(normal.failure_stage).toBe('');
    expect(normal.required_mask).toBe(1023);
    expect(normal.scenario_mask).toBe(normal.required_mask);
    expect(normal.ticks).toBeGreaterThan(0);
    expect(normal.mining.beam).toBe(1);
    expect(normal.mining.input).toBe(1);
    expect(normal.mining.signal).toBeGreaterThan(0);
    expect(normal.mining.hover).toBeGreaterThanOrEqual(0);
    expect(normal.mining.hp_after).toBeLessThan(normal.mining.hp_before);
    expect(normal.normal.samples).toBeGreaterThan(0);
    expect(normal.normal.exact).toBeGreaterThan(0);
    expect(normal.normal.numeric_drift).toBe(0);
    expect(normal.normal.asteroid_motion).toBeGreaterThan(0);
    expect(normal.normal.npc_motion).toBeGreaterThan(0);
    expect(normal.normal.death_respawn).toBe(1);

    expect(await wasmNumber(page, 'signal_zero_latency_gate_perturb')).toBe(1);
    const perturbed = await zeroLatencyGateReport(page);
    expect(perturbed.status).toBe(2);
    expect(perturbed.current_numeric_drift).toBe(1);
    expect(perturbed.first_drift.class).toBe('numeric-drift');
    expect(perturbed.first_drift.server_tick)
      .toBe(perturbed.first_drift.prediction_tick);
    expect(perturbed.first_drift.predicted_input_seq)
      .toBe(perturbed.first_drift.authoritative_input_seq);
    expect(perturbed.first_drift.domain).toBe('player.ship.pos.x');
    expect(perturbed.first_drift.predicted_bits)
      .not.toBe(perturbed.first_drift.authoritative_bits);
    expect(perturbed.first_drift.input_cause_mask).toBe(0);
    expect(perturbed.first_drift.semantic_cause_mask).toBe(0);
    expect(perturbed.first_drift.transport_cause_mask).toBe(0);
    expect(perturbed.first_drift.root_schema)
      .toBe('signal.authoritative_state.v3');
    expect(perturbed.first_drift.authoritative_root)
      .toMatch(/^[0-9a-f]{64}$/);

    expectNoFatalErrors(logs);
  });

  test('singleplayer renders the same packet-driven asteroid stream as multiplayer', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'requires local singleplayer authority');
    test.setTimeout(45_000);

    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    await page.goto(smokeUrl({ singleplayer: true }));
    await waitForRuntime(page);
    const canvas = page.locator('canvas');
    await expect(canvas).toBeVisible({ timeout: 20_000 });
    await canvas.click();
    await tap(page, 'Escape');
    await tap(page, 'E');
    await expect
      .poll(async () => (await playerStateSnapshot(page))?.docked ?? 1, {
        timeout: 8_000,
        message: 'asteroid motion gate should launch into local flight',
      })
      .toBe(0);
    await expect.poll(
      () => wasmNumber(page, 'get_local_asteroid_motion_feed_active'),
      {
        timeout: 10_000,
        message: 'singleplayer must not bypass packets with local authority poses',
      },
    ).toBe(0);

    expect(await wasmNumber(page, 'get_local_asteroid_motion_presented_samples'))
      .toBe(0);

    expectNoFatalErrors(logs);
  });

  test('fresh and mature play produce attributed jank reports and smooth accelerated rocks', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'requires the local singleplayer authority');
    const seconds = Number(process.env.SIGNAL_JANK_PROFILE_SECONDS || '2');
    test.setTimeout(Math.max(45_000, seconds * 2_000 + 30_000));

    const logs = installFatalCollectors(page);
    await page.goto(addQueryParam(
      smokeUrl({ singleplayer: true }), 'jankprofile', '1',
    ));
    await waitForRenderedGame(page, page.locator('canvas'), false);
    expect(await wasmNumber(page, 'signal_jank_profile_enabled')).toBe(1);
    expect(
      await wasmNumber(page, 'signal_smoke_accelerated_asteroid_prediction_gate'),
    ).toBe(1);

    await page.waitForTimeout(seconds * 1_000);
    const fresh = await jankProfileReport(page);
    expect(fresh.schema).toBe('signal.gameplay-jank.v1');
    expect(fresh.enabled).toBe(true);
    expect(fresh.frames).toBeGreaterThan(30);
    expect(fresh.frame_ms.p50).toBeGreaterThan(0);
    expect(fresh.frame_ms.p95).toBeGreaterThanOrEqual(fresh.frame_ms.p50);
    expect(fresh.frame_ms.p99).toBeGreaterThanOrEqual(fresh.frame_ms.p95);
    expect(fresh.simulation_ms.p99).toBeGreaterThanOrEqual(0);
    expect(fresh.snapshots.packets).toBeGreaterThan(0);
    expect(fresh.snapshots.bytes).toBeGreaterThan(0);
    expect(fresh.fixed_step.completed).toBeGreaterThan(0);
    expect(fresh.slow_frames.unexplained)
      .toBeLessThanOrEqual(fresh.slow_frames.over_16_6);

    await page.evaluate(() => {
      const mod = (window as unknown as {
        Module?: { ccall?: (name: string, returnType: null, argTypes: unknown[], args: unknown[]) => void };
      }).Module;
      mod?.ccall?.('signal_jank_profile_reset', null, [], []);
    });
    await setSmokeLoopState(page, smokeLoopState.fractureTableau);
    await page.waitForTimeout(seconds * 1_000);
    const mature = await jankProfileReport(page);
    expect(mature.frames).toBeGreaterThan(30);
    expect(mature.frame_ms.p99).toBeGreaterThan(0);
    expect(mature.snapshots.packets).toBeGreaterThan(0);
    for (const entity of [
      'asteroid', 'cargo_pod', 'scaffold', 'npc', 'remote_player',
    ]) {
      expect(mature.entities[entity]).toBeTruthy();
      expect(mature.entities[entity].samples).toBeGreaterThanOrEqual(0);
    }
    expectNoFatalErrors(logs);
  });

  test('singleplayer rebuilds market memories from the private packet', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'requires local singleplayer authority');

    const logs = installFatalCollectors(page);
    await loadGame(page, false, { singleplayer: true });

    expect(
      await wasmNumber(page, 'signal_smoke_market_memory_packet_parity'),
    ).toBe(1);
    expectNoFatalErrors(logs);
  });

  test('live relay launch accepts flight input', async ({ page }) => {
    test.skip(!usesLiveSmokeUrl(), 'requires SMOKE_URL pointed at a live relay URL');
    test.skip(
      !process.env.SMOKE_LIVE_RELAY_ASSERT,
      'set SMOKE_LIVE_RELAY_ASSERT=1 to require live relay input acks',
    );
    test.setTimeout(60_000);

    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    const canvas = await loadGame(page, true);

    await expect
      .poll(async () => hudHintText(page), { timeout: 5_000 })
      .toContain('SIGNAL // GUIDE // LAUNCH FROM DOCK');

    await canvas.click();
    await tap(page, 'Escape');
    await tap(page, 'E');
    await expect
      .poll(async () => {
        const flags = await mobileControlFlags(page);
        return (flags & mobileFlag.docked) === 0 &&
          (flags & mobileFlag.canFlight) !== 0;
      }, {
        timeout: 10_000,
        message: 'live launch should leave the local client in flight mode',
      })
      .toBeTruthy();
    const positionBeforeThrust = await playerStateSnapshot(page);
    expect(positionBeforeThrust).toBeTruthy();
    expect(positionBeforeThrust!.docked).toBe(0);
    const cameraAfterLaunch = await playerCameraSnapshot(page);
    expect(cameraAfterLaunch).toBeTruthy();
    expect(Math.max(
      Math.abs(cameraAfterLaunch!.offsetX),
      Math.abs(cameraAfterLaunch!.offsetY),
    )).toBeLessThan(140);

    const launchMotion = await netMotionSnapshot(page);
    expect(launchMotion.maxAppliedCorrection).toBeLessThan(80);

    await resetNetMotionTelemetry(page);
    const acksBefore = (await netMotionSnapshot(page)).inputAcks;
    await page.keyboard.down('W');
    try {
      await expect
        .poll(async () => heldControlMask(page), {
          timeout: 3_000,
          message: 'W should register as a held flight control',
        })
        .toBe(1);
      await page.waitForTimeout(1_200);
    } finally {
      await page.keyboard.up('W');
    }
    const positionAfterThrust = await playerStateSnapshot(page);
    expect(positionAfterThrust).toBeTruthy();
    expect(playerDistance(positionBeforeThrust!, positionAfterThrust!)).toBeGreaterThan(18);

    await expect
      .poll(async () => (await netMotionSnapshot(page)).inputAcks, {
        timeout: 10_000,
        message: 'held flight input should receive authoritative acks',
      })
      .toBeGreaterThan(acksBefore);

    const motion = await netMotionSnapshot(page);
    expect(motion.samples).toBeGreaterThan(0);
    expect(motion.unackedInputs).toBeLessThan(16);
    expect(motion.actionQueueDepth).toBe(0);
    expect(motion.maxAppliedCorrection).toBeLessThan(40);

    await resetNetMotionTelemetry(page);
    const turnAcksBefore = (await netMotionSnapshot(page)).inputAcks;
    const angleBeforeTurn = (await playerStateSnapshot(page))?.angle;
    expect(angleBeforeTurn).toBeDefined();
    await page.keyboard.down('A');
    try {
      await expect
        .poll(async () => heldControlMask(page), {
          timeout: 3_000,
          message: 'A should register as a held flight control',
        })
        .toBe(1 << 2);
      await page.waitForTimeout(1_000);
    } finally {
      await page.keyboard.up('A');
    }
    const stateAfterTurn = await playerStateSnapshot(page);
    expect(stateAfterTurn).toBeTruthy();
    expect(playerAngleDistance(angleBeforeTurn!, stateAfterTurn!.angle)).toBeGreaterThan(0.08);
    await expect
      .poll(async () => (await netMotionSnapshot(page)).inputAcks, {
        timeout: 10_000,
        message: 'held left turn should receive authoritative acks',
      })
      .toBeGreaterThan(turnAcksBefore);
    expect((await netMotionSnapshot(page)).maxAppliedCorrection).toBeLessThan(40);

    await page.reload();
    const reloadedCanvas = page.locator('canvas');
    await waitForRenderedGame(page, reloadedCanvas, true);
    await expect
      .poll(async () => {
        const flags = await mobileControlFlags(page);
        return (flags & mobileFlag.docked) === 0 &&
          (flags & mobileFlag.canFlight) !== 0;
      }, {
        timeout: 10_000,
        message: 'live reload should preserve flight authority without a correction snap',
      })
      .toBeTruthy();
    const reloadMotion = await netMotionSnapshot(page);
    expect(reloadMotion.maxAppliedCorrection).toBeLessThan(80);

    const positionAfterReload = await playerStateSnapshot(page);
    expect(positionAfterReload).toBeTruthy();
    expect(positionAfterReload!.docked).toBe(0);
    await resetNetMotionTelemetry(page);
    const reloadAcksBefore = (await netMotionSnapshot(page)).inputAcks;
    await page.keyboard.down('W');
    try {
      await expect
        .poll(async () => heldControlMask(page), {
          timeout: 3_000,
          message: 'W should register after a live reload',
        })
        .toBe(1);
      await page.waitForTimeout(1_000);
    } finally {
      await page.keyboard.up('W');
    }
    const positionAfterReloadThrust = await playerStateSnapshot(page);
    expect(positionAfterReloadThrust).toBeTruthy();
    expect(playerDistance(positionAfterReload!, positionAfterReloadThrust!)).toBeGreaterThan(12);
    await expect
      .poll(async () => (await netMotionSnapshot(page)).inputAcks, {
        timeout: 10_000,
        message: 'held flight input should keep receiving acks after reload',
      })
      .toBeGreaterThan(reloadAcksBefore);
    expect((await netMotionSnapshot(page)).maxAppliedCorrection).toBeLessThan(40);

    await resetNetMotionTelemetry(page);
    const reloadTurnAcksBefore = (await netMotionSnapshot(page)).inputAcks;
    const angleBeforeReloadTurn = (await playerStateSnapshot(page))?.angle;
    expect(angleBeforeReloadTurn).toBeDefined();
    await page.keyboard.down('D');
    try {
      await expect
        .poll(async () => heldControlMask(page), {
          timeout: 3_000,
          message: 'D should register after a live reload',
        })
        .toBe(1 << 3);
      await page.waitForTimeout(1_000);
    } finally {
      await page.keyboard.up('D');
    }
    const stateAfterReloadTurn = await playerStateSnapshot(page);
    expect(stateAfterReloadTurn).toBeTruthy();
    expect(playerAngleDistance(angleBeforeReloadTurn!, stateAfterReloadTurn!.angle)).toBeGreaterThan(0.08);
    await expect
      .poll(async () => (await netMotionSnapshot(page)).inputAcks, {
        timeout: 10_000,
        message: 'held right turn should keep receiving acks after reload',
      })
      .toBeGreaterThan(reloadTurnAcksBefore);
    expect((await netMotionSnapshot(page)).maxAppliedCorrection).toBeLessThan(40);
    expectNoFatalErrors(logs, { allowExpectedLiveClose: true });
  });

  test('live relay touch controls drive flight input', async ({ page }) => {
    test.skip(!usesLiveSmokeUrl(), 'requires SMOKE_URL pointed at a live relay URL');
    test.skip(
      !process.env.SMOKE_LIVE_RELAY_ASSERT,
      'set SMOKE_LIVE_RELAY_ASSERT=1 to require live relay input acks',
    );
    test.setTimeout(45_000);

    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 390, height: 760 });
    await page.goto(addQueryParam(smokeUrl(), 'touch', '1'));
    await waitForRenderedGame(page, page.locator('canvas'), true);

    await page.locator('[data-control="use"]').click();
    await expect
      .poll(async () => {
        const flags = await mobileControlFlags(page);
        return (flags & mobileFlag.docked) === 0 &&
          (flags & mobileFlag.canFlight) !== 0;
      }, {
        timeout: 10_000,
        message: 'live touch launch should enter flight mode',
      })
      .toBeTruthy();

    const positionBeforeThrust = await playerStateSnapshot(page);
    expect(positionBeforeThrust).toBeTruthy();
    expect(positionBeforeThrust!.docked).toBe(0);
    const cameraAfterLaunch = await playerCameraSnapshot(page);
    expect(cameraAfterLaunch).toBeTruthy();
    expect(Math.max(
      Math.abs(cameraAfterLaunch!.offsetX),
      Math.abs(cameraAfterLaunch!.offsetY),
    )).toBeLessThan(140);

    const thrust = page.locator('[data-control="thrust"]');
    await expect(thrust).toBeVisible();
    await expect(thrust).toBeEnabled();
    const box = await thrust.boundingBox();
    expect(box).toBeTruthy();
    await page.mouse.move(box!.x + box!.width * 0.5, box!.y + box!.height * 0.5);
    await page.mouse.down();
    try {
      await expect
        .poll(async () => heldControlMask(page), {
          timeout: 3_000,
          message: 'touch Accel should register as held thrust',
        })
        .toBe(1);
      await page.waitForTimeout(1_200);
    } finally {
      await page.mouse.up();
    }

    const positionAfterThrust = await playerStateSnapshot(page);
    expect(positionAfterThrust).toBeTruthy();
    expect(playerDistance(positionBeforeThrust!, positionAfterThrust!)).toBeGreaterThan(18);
    expectNoFatalErrors(logs, { allowExpectedLiveClose: true });
  });

  test('touch controls stay synced when canvas focus moves to overlay', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'requires the local play.html build with debug input exports');

    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 390, height: 760 });
    await page.goto(addQueryParam(smokeUrl({ singleplayer: true }), 'touch', '1'));
    await waitForRenderedGame(page, page.locator('canvas'), false);

    await page.locator('[data-control="use"]').click();
    await expect
      .poll(async () => {
        const flags = await mobileControlFlags(page);
        return (flags & mobileFlag.docked) === 0 &&
          (flags & mobileFlag.canFlight) !== 0;
      }, {
        timeout: 8_000,
        message: 'touch launch should enter flight mode',
      })
      .toBeTruthy();

    const thrust = page.locator('[data-control="thrust"]');
    await expect(thrust).toBeVisible();
    await expect(thrust).toBeEnabled();
    const box = await thrust.boundingBox();
    expect(box).toBeTruthy();

    await page.mouse.move(box!.x + box!.width * 0.5, box!.y + box!.height * 0.5);
    await page.mouse.down();
    try {
      await expect
        .poll(async () => heldControlMask(page), {
          timeout: 2_000,
          message: 'touch Accel should register as held thrust',
        })
        .toBe(1);

      await page.evaluate(() => {
        const canvas = document.querySelector('canvas');
        const thrustButton = document.querySelector('[data-control="thrust"]');
        canvas?.dispatchEvent(new FocusEvent('blur', {
          bubbles: false,
          relatedTarget: thrustButton,
        }));
      });
      expect(await heldControlMask(page)).toBe(1);

      await page.evaluate(() => {
        const mod = (window as unknown as {
          SignalGameModule?: { ccall?: (name: string, returnType: string | null, argTypes: unknown[], args: unknown[]) => void };
        }).SignalGameModule;
        mod?.ccall?.('signal_mobile_clear', null, [], []);
      });
      await expect
        .poll(async () => heldControlMask(page), {
          timeout: 1_000,
          message: 'visually held touch controls should reassert into WASM',
        })
        .toBe(1);
    } finally {
      await page.mouse.up();
    }

    await expect
      .poll(async () => heldControlMask(page), {
        timeout: 1_000,
        message: 'releasing touch Accel should clear thrust',
      })
      .toBe(0);
    expectNoFatalErrors(logs);
  });

  test('play page clears held flight controls when browser focus leaves', async ({ page }) => {
    test.skip(usesLiveSmokeUrl(), 'requires the local play.html build with debug input exports');

    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    const canvas = await loadGame(page, false, { singleplayer: true });

    await canvas.click();
    await page.keyboard.down('w');
    try {
      await expect
        .poll(async () => heldControlMask(page), {
          timeout: 2_000,
          message: 'W should register as a held flight control before blur',
        })
        .toBe(1);

      await page.evaluate(() => window.dispatchEvent(new Event('blur')));
      await expect
        .poll(async () => heldControlMask(page), {
          timeout: 2_000,
          message: 'blur should release held controls so launch cannot inherit stale thrust',
        })
        .toBe(0);
    } finally {
      await page.keyboard.up('w');
    }

    expectNoFatalErrors(logs);
  });

  rootBundleSmokeTest('visual saturation follows signal strength and H resaturates weak signal', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    const canvas = await loadGame(page, false, { singleplayer: true });

    await setSmokeLoopState(page, smokeLoopState.weakSignalVisual);
    await expect
      .poll(async () => (await signalStrength(page)) ?? 1, {
        timeout: 3_000,
        message: 'weak-signal smoke state should move the ship outside coverage',
      })
      .toBeLessThan(0.01);
    await expect
      .poll(async () => (await signalVisualSaturation(page)) ?? 1, {
        timeout: 3_000,
        message: 'weak signal should drain world saturation toward grayscale',
      })
      .toBeLessThan(0.05);
    await expect
      .poll(async () => (await signalVisualCueSaturation(page)) ?? 0, {
        timeout: 3_000,
        message: 'critical cue saturation should retain readable color in weak signal',
      })
      .toBeGreaterThan(0.7);
    await expect
      .poll(async () => (await signalVisualPlayerSaturation(page)) ?? 0, {
        timeout: 3_000,
        message: 'player ship should stay readable as the world loses color',
      })
      .toBeGreaterThan(0.9);

    const beforeHail = (await signalVisualBaseSaturation(page)) ?? 0;
    await canvas.click();
    await tap(page, 'H', 20);
    await expect
      .poll(async () => (await signalVisualBaseSaturation(page)) ?? 0, {
        timeout: 2_000,
        message: 'H should create a temporary resaturation pulse',
      })
      .toBeGreaterThan(Math.max(0.10, beforeHail + 0.05));

    expectNoFatalErrors(logs);
  });

  rootBundleSmokeTest('exposes deterministic HUD copy for fragment, tractor, tow, and hail states', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    await loadGame(page, false, { singleplayer: true });

    const refitSummary = await laserRefitSummary(page);
    expect(refitSummary).toContain('Laser Modules: Crystal Ingots + Frames');
    expect(refitSummary).toContain('Crystal source requires L3 laser');
    expect(refitSummary).toContain('Kepler: 8 stock');
    expect(refitSummary).toContain('cr');
    expect(refitSummary).toContain(
      'tow 8 Prospect FE Ingots to Kepler hopper',
    );
    expect(refitSummary).toContain('[M]');
    expect(refitSummary).not.toContain('dock + [M]');
    expect(refitSummary).toMatch(/\b[1-9]\d* cr\b/);

    await setSmokeLoopState(page, smokeLoopState.refitWorkAged);
    const agedRefitSummary = await laserRefitSummary(page);
    expect(agedRefitSummary).toContain(
      'tow 8 Prospect FE Ingots to Kepler hopper',
    );
    expect(agedRefitSummary).not.toContain(
      'tow 7 Prospect FE Ingots',
    );

    await setSmokeLoopState(page, smokeLoopState.refitSupplyActive);
    const activeSupply = await laserRefitSummary(page);
    expect(activeSupply).toContain('Helios production pending');
    expect(activeSupply).toContain('check WORK / haul');

    await setSmokeLoopState(page, smokeLoopState.refitSupplyInactive);
    const inactiveSupply = await laserRefitSummary(page);
    expect(inactiveSupply).not.toContain('Helios');
    expect(inactiveSupply).toContain('need 8 Laser Modules');

    const productionSummary = await stationProductionSummary(page);
    expect(productionSummary).toContain('Ferrite Ore -> Ferrite Ingots');
    expect(productionSummary).toContain('missing input');

    await setSmokeLoopState(page, smokeLoopState.fractureTableau);
    expect(await hudActionText(page)).toContain('needs L2 laser for L rock');

    await setSmokeLoopState(page, smokeLoopState.cupriteGate);
    expect(await hudActionText(page)).toContain('needs L2 laser for Cuprite');

    await setSmokeLoopState(page, smokeLoopState.scanLaserFab);
    expect(await hudActionText(page)).toContain('Crystal Ingots + Frames -> Laser Modules');

    await setSmokeLoopState(page, smokeLoopState.trackedCupriteContract);
    expect(await hudHintText(page)).toContain('requires L2 laser');

    await setSmokeLoopState(page, smokeLoopState.fragmentsNearby);
    expect(await hudActionText(page)).toContain('Hold [Space] tractor // 3 nearby');

    await setSmokeLoopState(page, smokeLoopState.tractorReaching);
    expect(await hudActionText(page)).toContain('Tractor reaching // 4 nearby');

    await setSmokeLoopState(page, smokeLoopState.tractorLock);
    expect(await hudActionText(page)).toContain('Tractor lock // 2 frags');

    await setSmokeLoopState(page, smokeLoopState.towing);
    expect(await hudActionText(page)).toContain('Towing 1 // needed at Prospect');

    await setSmokeLoopState(page, smokeLoopState.rockSmeltPath);
    expect(await hudActionText(page)).toContain('smelts to FE Ingot at Prospect');

    await setSmokeLoopState(page, smokeLoopState.rockRouteTarget);
    const crispTargetRoute = await hudActionText(page);
    expect(crispTargetRoute).toContain('route remembers Prospect>Kepler');

    await setSmokeLoopState(page, smokeLoopState.rockRouteTow);
    const crispTowRoute = await hudActionText(page);
    expect(crispTowRoute).toContain('route remembers Prospect>Kepler');

    await setSmokeLoopState(page, smokeLoopState.rockRouteDegraded);
    const degradedTowRoute = await hudActionText(page);
    expect(degradedTowRoute).toContain('route remembers');
    expect(degradedTowRoute).toContain('?');

    /* Keep a renderable, near-taut cargo tow in the deployed bundle so the
     * canonical pod wave can be inspected without playing through the whole
     * production chain. setSmokeLoopState returning 1 also guards the fixture
     * against drifting out of the WebAssembly build. */
    await setSmokeLoopState(page, smokeLoopState.cargoTowing);
    await page.waitForTimeout(100);

    await setSmokeLoopState(page, smokeLoopState.cargoHopperGuide);
    await expect.poll(
      () => wasmNumber(page, 'signal_hopper_guide_draw_count'),
      {
        timeout: 3_000,
        message: 'towed Prospect cargo should mark one visible Kepler intake',
      },
    ).toBe(1);
    expect(await wasmNumber(page, 'signal_hopper_guide_station')).toBe(1);
    expect(await wasmNumber(page, 'signal_hopper_guide_commodity')).toBe(3);
    expect(
      await wasmNumber(page, 'signal_cargo_readability_draw_count'),
      'handoff view should contain a rendered physical cargo carrier',
    ).toBeGreaterThanOrEqual(1);
    expect(
      await wasmNumber(page, 'signal_cargo_readability_towed_screen_radius'),
      'the towed carrier should retain a readable radius at the flight camera',
    ).toBeGreaterThanOrEqual(21.5);
    expect(
      await wasmNumber(page, 'signal_cargo_readability_max_scale'),
      'towing must not inflate a carrier beyond the normal readability cap',
    ).toBeLessThanOrEqual(1.5);
    expect(
      await wasmNumber(page, 'signal_station_hopper_glyph_count'),
      'visible station storage cells should retain their hopper silhouettes',
    ).toBeGreaterThanOrEqual(1);
    expect(
      await wasmNumber(page, 'signal_station_producer_glyph_count'),
      'visible station factory cells should retain their machine silhouettes',
    ).toBeGreaterThanOrEqual(1);

    /* Guard the deployed fixture used to inspect module-surface origins. */
    await setSmokeLoopState(page, smokeLoopState.moduleCargoTractor);
    await expect
      .poll(async () => (await tractorDrawTelemetry(page, 1)).count, {
        timeout: 3_000,
        message: 'module cargo fixture should draw all three tractor waves',
      })
      .toBeGreaterThanOrEqual(3);
    const moduleTractor = await tractorDrawTelemetry(page, 1);
    expect(moduleTractor.sourceType).toBe(1); // station module
    expect(moduleTractor.targetType).toBe(2); // cargo pod
    expect(moduleTractor.span).toBeGreaterThan(180);
    expect(Math.hypot(
      moduleTractor.targetX - moduleTractor.sourceX,
      moduleTractor.targetY - moduleTractor.sourceY,
    )).toBeCloseTo(moduleTractor.span, 3);
    expect(moduleTractor.amplitude).toBeGreaterThan(4);
    expect(moduleTractor.tautness).toBeGreaterThanOrEqual(0);
    expect(moduleTractor.tautness).toBeLessThanOrEqual(1);
    expect(moduleTractor.intensity).toBeGreaterThan(0.5);
    await page.waitForTimeout(100);
    const rotatedModuleTractor = await tractorDrawTelemetry(page, 1);
    expect(Math.hypot(
      rotatedModuleTractor.sourceX - moduleTractor.sourceX,
      rotatedModuleTractor.sourceY - moduleTractor.sourceY,
    )).toBeGreaterThan(1);

    await setSmokeLoopState(page, smokeLoopState.stationFragmentTractor);
    await expect
      .poll(async () => (await tractorDrawTelemetry(page, 2)).count, {
        timeout: 3_000,
        message: 'station fragment fixture should draw both tractor waves',
      })
      .toBeGreaterThanOrEqual(2);
    const stationTractor = await tractorDrawTelemetry(page, 2);
    expect(stationTractor.sourceType).toBe(1); // station module
    expect(stationTractor.targetType).toBe(4); // asteroid
    expect(stationTractor.span).toBeGreaterThan(80);
    expect(stationTractor.intensity).toBeGreaterThan(0.5);
    const renderQueue = await renderQueueTelemetry(page);
    expect(renderQueue.errorMask, 'station scene must not overflow Sokol GL').toBe(0);
    expect(renderQueue.vertices).toBeLessThan(65_536);
    expect(renderQueue.commands).toBeLessThan(16_384);

    await setSmokeLoopState(page, smokeLoopState.hailReady);
    expect(await hudActionText(page)).toContain('123 prospect vouchers available // dock to spend');

    await setSmokeLoopState(page, smokeLoopState.hailNotice);
    expect(await hudHintText(page)).toContain('Prospect: channel open. Balance 123 cr.');

    await setSmokeLoopState(page, smokeLoopState.remotePilotScan);
    await expect
      .poll(async () => remotePlayerScanned(page, 1), {
        timeout: 5_000,
        message: 'remote pilot should reveal once inside tractor scan range',
      })
      .toBe(1);

    await setSmokeLoopState(page, smokeLoopState.onboardingDeliver);
    expect(await hudHintText(page)).toContain(
      'DELIVER ORE ::::: TOW IT TO THE GLOWING FURNACE AT Prospect Refinery',
    );

    await setSmokeLoopState(page, smokeLoopState.onboardingReturn);
    const paidInSpaceHint = await hudHintText(page);
    expect(paidInSpaceHint).toContain(
      'ECONOMY LOOP COMPLETE ::::: MONEY STAYS LOCAL // GOODS TRAVEL',
    );
    expect(paidInSpaceHint).not.toContain('RETURN TO');

    await setSmokeLoopState(page, smokeLoopState.onboardingMarket);
    const noMarketGateHint = await hudHintText(page);
    expect(noMarketGateHint).toContain(
      'ECONOMY LOOP COMPLETE ::::: MONEY STAYS LOCAL // GOODS TRAVEL',
    );
    expect(noMarketGateHint).not.toContain('OPEN LOCAL MARKET');

    await setSmokeLoopState(page, smokeLoopState.onboardingComplete);
    expect(await hudHintText(page)).toContain(
      'ECONOMY LOOP COMPLETE ::::: MONEY STAYS LOCAL // GOODS TRAVEL',
    );

    expectNoFatalErrors(logs);
  });

  rootBundleSmokeTest('exposes deterministic HUD copy for plan, snap, and construction supply states', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    await loadGame(page, false, { singleplayer: true });

    await setSmokeLoopState(page, smokeLoopState.planGhost);
    expect(await hudHintText(page)).toContain('PLAN GHOST :: Signal Relay preview ring 1 slot 2');

    await setSmokeLoopState(page, smokeLoopState.planSlot);
    expect(await hudHintText(page)).toContain('PLAN SLOT :: Hopper at Prospect ring 1 slot 0');

    await setSmokeLoopState(page, smokeLoopState.scaffoldSnap);
    expect(await hudHintText(page)).toContain('SCAFFOLD SNAP :: Furnace snapping to Outpost 4 ring 2 slot 3');

    await setSmokeLoopState(page, smokeLoopState.supplyNeed);
    expect(await hudHintText(page)).toContain('SUPPLY NEED :: Outpost scaffold needs 24 frames at Outpost 4.');

    await setSmokeLoopState(page, smokeLoopState.yardBlocked);
    expect(await hudHintText(page)).toContain(
      'YARD BLOCKED :: Outpost 4 yard blocked by loose scaffold. Tow it clear to start Furnace.',
    );

    await setSmokeLoopState(page, smokeLoopState.abandonedPlan);
    expect(await hudHintText(page)).toContain(
      'ABANDONED PLAN :: Outpost 4 has no reserved modules.',
    );

    expectNoFatalErrors(logs);
  });

  rootBundleSmokeTest('smooths remote towables and draws a local online tether across generation mismatch', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    await loadGame(page, false, { singleplayer: true });

    expect(await remoteTowableInterpCheck(page)).toBe(1);
    expect(
      await localTowReplayStabilityCheck(page),
      'player reconciliation must not advance an already-predicted tow body again',
    ).toBe(1);
    expect(
      await wasmNumber(
        page, 'signal_smoke_prepare_local_generation_mismatch_tether',
      ),
      'an authenticated server tow generation must project onto the live local player slot',
    ).toBe(1);
    await expect
      .poll(async () => (await tractorDrawTelemetry(page, 0)).count, {
        timeout: 3_000,
        message: 'the local player-to-cargo tractor line should render',
      })
      .toBeGreaterThanOrEqual(1);
    const localTether = await tractorDrawTelemetry(page, 0);
    expect(localTether.sourceType).toBe(3); // ship
    expect(localTether.targetType).toBe(2); // cargo pod
    expect(localTether.span).toBeGreaterThan(40);
    expect(localTether.intensity).toBeGreaterThan(0);

    expectNoFatalErrors(logs);
  });

  rootBundleSmokeTest('renders an NPC-owned scaffold tether from canonical tow state', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    await loadGame(page, false, { singleplayer: true });

    expect(
      await wasmNumber(page, 'signal_smoke_prepare_npc_scaffold_tether'),
    ).toBe(1);
    await expect
      .poll(async () => (await tractorDrawTelemetry(page, 0)).count, {
        timeout: 3_000,
        message: 'the NPC-scaffold canonical relation should render a tether',
      })
      .toBeGreaterThanOrEqual(1);
    const tether = await tractorDrawTelemetry(page, 0);
    expect(tether.sourceType).toBe(3); // ship
    expect(tether.targetType).toBe(5); // scaffold
    expect(tether.span).toBeGreaterThan(40);
    expect(tether.intensity).toBeGreaterThan(0);

    expectNoFatalErrors(logs);
  });

  rootBundleSmokeTest('keeps every supported tow owner and target atomic under deterministic adverse delivery', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    await loadGame(page, false, { singleplayer: true });

    expect(await adverseTowableGate(page)).toBe(1);
    const reportText = await adverseTowableReport(page);
    expect(reportText).not.toBe('');
    const report = JSON.parse(reportText) as {
      status: number;
      scope: string;
      profiles: Array<{
        latency_ms: number;
        pass: number;
        dropped: number;
        duplicated: number;
        reordered: number;
        lifecycle: number;
        stale: number;
        post_reentry_relation: number;
      }>;
      matrix: {
        scenarios: string[];
        scenario_count: number;
        profile_count: number;
        passed_profiles: number;
        stale: number;
        lifecycle_failures: number;
        failure: string;
      };
    };
    expect(report.status).toBe(1);
    expect(report.scope).toBe('valid_owner_target_lifecycle_matrix');
    expect(report.profiles.map((profile) => profile.latency_ms)).toEqual([50, 125, 250]);
    for (const profile of report.profiles) {
      expect(profile.pass).toBe(1);
      expect(profile.dropped).toBeGreaterThan(0);
      expect(profile.duplicated).toBeGreaterThan(0);
      expect(profile.reordered).toBeGreaterThan(0);
      expect(profile.lifecycle).toBe(0x1ff);
      expect(profile.stale).toBe(0);
      expect(profile.post_reentry_relation).toBe(0);
    }
    expect(report.matrix.scenarios).toEqual([
      'player_fragment',
      'npc_fragment',
      'player_cargo',
      'station_cargo',
      'player_scaffold',
      'npc_scaffold',
    ]);
    expect(report.matrix.scenario_count).toBe(6);
    expect(report.matrix.profile_count).toBe(18);
    expect(report.matrix.passed_profiles).toBe(18);
    expect(report.matrix.stale).toBe(0);
    expect(report.matrix.lifecycle_failures).toBe(0);
    expect(report.matrix.failure).toBe('none');

    expectNoFatalErrors(logs);
  });

  rootBundleSmokeTest('hooks, holds, and releases a towable through the real loopback lifecycle', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    const canvas = await loadGame(page, false, { singleplayer: true });
    await canvas.click();

    expect(await prepareTowLifecycle(page)).toBeGreaterThan(0);
    await expect
      .poll(async () => (await towLifecycleState(page)) & 0b1100000011, {
        timeout: 3_000,
        message: 'the seeded fragment and cargo pod should reach both authority and client',
      })
      .toBe(0b1100000011);

    await page.keyboard.down('Space');
    try {
      await expect
        .poll(async () => (await towLifecycleState(page)) & 0b111110001111100, {
          timeout: 3_000,
          message: 'Space hold should activate both tractors and project both authoritative tow links to the client',
        })
        .toBe(0b111110001111100);
      await expect
        .poll(async () => (await towLifecycleState(page)) & 0b11000000000000000, {
          timeout: 3_000,
          message: 'the atomic tow snapshot revision should converge with loopback authority',
        })
        .toBe(0b11000000000000000);
      await page.waitForTimeout(300);
    } finally {
      await page.keyboard.up('Space');
    }

    await expect
      .poll(async () => (await towLifecycleState(page)) & 0b1110001110000, {
        timeout: 3_000,
        message: 'a long hold release should keep both established tow links latched',
      })
      .toBe(0b1110001110000);
    await expect
      .poll(async () => (await towLifecycleState(page)) & 0b1100, {
        timeout: 3_000,
        message: 'the long-hold key-up should reach both tractor-active projections before the release tap',
      })
      .toBe(0);
    await expect
      .poll(async () => (await towLifecycleState(page)) & 0b10000000, {
        timeout: 1_000,
        message: 'the long-hold key-up must be sampled before starting a distinct release tap',
      })
      .toBe(0);

    const tapState = await tapTowOnNextSample(page);
    expect(tapState & 0b100000000000000000).toBe(0b100000000000000000);
    await expect
      .poll(async () => (await towLifecycleState(page)) & 0b1110001110000, {
        timeout: 3_000,
        message: 'a Space tap should clear both authoritative bindings and all tow-list projections',
      })
      .toBe(0);

    expectNoFatalErrors(logs);
  });

  rootBundleSmokeTest('projects recipient-scoped station balances through the multiplayer packet path', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    await loadGame(page, false, { singleplayer: true });

    expect(await prepareKnownLedgerSync(page)).toBe(1);
    await expect
      .poll(knownLedgerSyncState.bind(null, page), {
        timeout: 4_000,
        message: 'the private snapshot should carry only Prospect credit while the player is docked at zero-balance Helios',
      })
      .toBe(0x1ff);

    const summary = await stationCreditPerceptionSummary(page);
    expect(summary).toContain('Helios 0');
    expect(summary).toContain('Prospect 123');
    expect(summary).toContain('Prospect: buy > haul');
    expect(summary).not.toMatch(/Local balances:.*(?:Kepler|Blackglass)/);
    expectNoFatalErrors(logs);
  });

  rootBundleSmokeTest('opens a manifest-backed cargo story and paged proof on desktop and narrow layouts', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    const canvas = await loadGame(page, false, { singleplayer: true });

    await setSmokeLoopState(page, smokeLoopState.cargoLineage);
    await expect.poll(async () => stationPanelLabel(page)).toBe('TRADE');
    await canvas.click();
    await tap(page, 'L');

    await expect.poll(async () => cargoLineageText(page)).toContain('Fragment');
    const story = await cargoLineageText(page);
    expect(story).toContain('-> FE Ingot at Prospect');
    expect(story).toContain('FE Ingot -> Frame at Prospect');
    expect(story).toContain('Now: Frame crate for sale at Prospect dock');
    expect(story).toContain('Custody gap: portable receipt links are not local here');
    await expect.poll(async () => stationPanelLegend(page)).toContain('[I] proof');

    await tap(page, 'I');
    const proof = await cargoLineageText(page);
    expect(proof).toContain('SMELT event');
    expect(proof).toContain('CRAFT event');
    expect(proof).toContain('Selected cargo ID');
    expect(proof).toContain('Manifest parent root');
    expect(proof).toMatch(/[0-9a-f]{32}/);

    await tap(page, 'L');
    const legacyStory = await cargoLineageText(page);
    expect(legacyStory).toContain('Manifest: legacy cargo at Helios');
    expect(legacyStory).toContain('Gap: Frame event');

    await page.setViewportSize({ width: 390, height: 760 });
    await tap(page, 'I');
    await expect.poll(async () => cargoLineageText(page)).toContain('Selected cargo ID');
    const box = await canvas.boundingBox();
    expect(box).toBeTruthy();
    expect(box!.width).toBeGreaterThan(300);
    expect(box!.height).toBeGreaterThan(500);
    await expect
      .poll(async () => (await readCanvasStats(canvas)).nonBlackRatio, { timeout: 5_000 })
      .toBeGreaterThan(0.05);

    await tap(page, 'Escape');
    await expect.poll(async () => cargoLineageText(page)).toBe('');
    expectNoFatalErrors(logs);
  });

  rootBundleSmokeTest('perception acceptance answers all six player questions on desktop and narrow layouts', async ({ page }, testInfo) => {
    test.setTimeout(90_000);
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    const canvas = await loadGame(page, false, { singleplayer: true });

    await setSmokeLoopState(page, smokeLoopState.rockSmeltPath);
    expect(
      await hudActionText(page),
      'Rock value — why is this rock useful?',
    ).toContain('smelts to FE Ingot at Prospect');
    await attachPerceptionReview(testInfo, canvas, 'rock-value', 'desktop');

    await setSmokeLoopState(page, smokeLoopState.weakSignalVisual);
    expect(
      await signalLossPerceptionSummary(page),
      'Signal loss — why is civilization/control thinning here?',
    ).toBe('[ SIGNAL LOST ]');
    expect((await signalVisualSaturation(page)) ?? 1).toBeLessThan(0.05);
    await attachPerceptionReview(testInfo, canvas, 'signal-loss', 'desktop');

    await setSmokeLoopState(page, smokeLoopState.localMoney);
    await expect.poll(async () => stationPanelLabel(page)).toBe('TRADE');
    await expect
      .poll(async () => stationCreditPerceptionSummary(page), {
        message: 'local-money view should name both station ledgers and the cargo bridge',
      })
      .toContain('Local balances: Kepler 0 kepl');
    const desktop = await stationCreditPerceptionSummary(page);
    expect(desktop).toContain('Prospect 80 pros');
    expect(desktop).toContain(
      'Prospect: buy > haul',
    );
    await attachPerceptionReview(testInfo, canvas, 'local-money', 'desktop');

    await setSmokeLoopState(page, smokeLoopState.npcMotiveCrisp);
    const crispMotive = await npcMotivePerceptionSummary(page);
    expect(
      crispMotive,
      'NPC motive — why did that worker choose that route?',
    ).toContain('haul FE Ingot -> Kepler Yard');
    expect(crispMotive).toContain('because route memory');
    expect(crispMotive).toContain('heard route @Prospe h0 age2 anchor');
    expect(crispMotive).not.toContain('?');
    await attachPerceptionReview(testInfo, canvas, 'npc-motive', 'desktop');

    await setSmokeLoopState(page, smokeLoopState.npcMotiveDegraded);
    const degradedMotive = await npcMotivePerceptionSummary(page);
    expect(degradedMotive).toContain('haul FE Ingot -> Kepler Yard');
    expect(
      degradedMotive,
      'NPC motive — relayed evidence should read as degraded, not certain',
    ).toContain('?');

    await setSmokeLoopState(page, smokeLoopState.rememberedWorkCrisp);
    const crispMemory = await rememberedWorkPerceptionSummary(page);
    expect(
      crispMemory,
      'Remembered work — who remembers the verified delivery?',
    ).toContain('LOCAL SIGNED PROOF | route success Prospect>Kepler FR');
    expect(crispMemory).toContain('signed proof: delivery via FR, 6 receipts');
    expect(crispMemory).toContain('known event');
    await attachPerceptionReview(testInfo, canvas, 'remembered-work', 'desktop');

    await setSmokeLoopState(page, smokeLoopState.rememberedWorkDegraded);
    const degradedMemory = await rememberedWorkPerceptionSummary(page);
    expect(degradedMemory).toContain('signed proof: delivery via FR, 2 receipts');
    expect(
      degradedMemory,
      'Remembered work — weak route knowledge should read faint without raw confidence',
    ).toContain('faint event');
    expect(degradedMemory).not.toContain('conf ');

    await setSmokeLoopState(page, smokeLoopState.constructionConsequence);
    expect(
      await constructionStateMask(page),
      'construction tableau should include supply, active assembly, and complete material',
    ).toBe(0b111);
    expect(
      await hudHintText(page),
      'Construction consequence — what changed because I built this?',
    ).toContain('Signal relay online -- civilization reaches farther.');
    await attachPerceptionReview(testInfo, canvas, 'construction-consequence', 'desktop');

    await page.setViewportSize({ width: 390, height: 760 });

    await setSmokeLoopState(page, smokeLoopState.rockSmeltPath);
    expect(await hudActionText(page)).toContain('smelts to FE Ingot at Prospect');
    await attachPerceptionReview(testInfo, canvas, 'rock-value', 'narrow');

    await setSmokeLoopState(page, smokeLoopState.weakSignalVisual);
    expect(await signalLossPerceptionSummary(page)).toBe('[ SIGNAL LOST ]');
    await attachPerceptionReview(testInfo, canvas, 'signal-loss', 'narrow');

    await setSmokeLoopState(page, smokeLoopState.localMoney);
    await expect.poll(async () => stationCreditPerceptionSummary(page)).toBe(desktop);
    await attachPerceptionReview(testInfo, canvas, 'local-money', 'narrow');

    await setSmokeLoopState(page, smokeLoopState.npcMotiveCrisp);
    expect(await npcMotivePerceptionSummary(page)).toContain('because route memory');
    await attachPerceptionReview(testInfo, canvas, 'npc-motive', 'narrow');

    await setSmokeLoopState(page, smokeLoopState.rememberedWorkCrisp);
    expect(await rememberedWorkPerceptionSummary(page)).toContain('known event');
    await attachPerceptionReview(testInfo, canvas, 'remembered-work', 'narrow');

    await setSmokeLoopState(page, smokeLoopState.constructionConsequence);
    expect(await constructionStateMask(page)).toBe(0b111);
    expect(await hudHintText(page)).toContain(
      'Signal relay online -- civilization reaches farther.',
    );
    await attachPerceptionReview(testInfo, canvas, 'construction-consequence', 'narrow');

    await expect
      .poll(async () => (await readCanvasStats(canvas)).nonBlackRatio, { timeout: 5_000 })
      .toBeGreaterThan(0.05);

    expectNoFatalErrors(logs);
  });

  rootBundleSmokeTest('HUD attention keeps one primary surface and semantic scan budgets on desktop and narrow layouts', async ({ page }, testInfo) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    const canvas = await loadGame(page, false, { singleplayer: true });

    await expect.poll(async () => hudAttentionSurface(page)).toBe('station');
    expect(await hudAttentionTelemetry(page)).toEqual({
      debugVisible: 0,
      asteroidBudget: 8,
      npcBudget: 4,
    });
    await attachPerceptionReview(testInfo, canvas, 'hud-attention-station', 'desktop');

    await page.setViewportSize({ width: 390, height: 760 });
    await setSmokeLoopState(page, smokeLoopState.narrowCameraOffset);
    await expect.poll(async () => hudAttentionSurface(page)).toBe('message');
    expect(await hudAttentionTelemetry(page)).toEqual({
      debugVisible: 0,
      asteroidBudget: 4,
      npcBudget: 2,
    });
    await attachPerceptionReview(testInfo, canvas, 'hud-attention-message', 'narrow');

    await canvas.click();
    await tap(page, 'Tab');
    await expect.poll(async () => hudAttentionSurface(page)).toBe('scoreboard');
    await attachPerceptionReview(testInfo, canvas, 'hud-attention-scoreboard', 'narrow');

    await tap(page, 'F3');
    await expect.poll(async () => (await hudAttentionTelemetry(page)).debugVisible).toBe(1);
    await tap(page, 'F3');
    await expect.poll(async () => (await hudAttentionTelemetry(page)).debugVisible).toBe(0);

    await setSmokeLoopState(page, smokeLoopState.npcMotiveCrisp);
    await expect.poll(async () => hudAttentionSurface(page)).toBe('inspect');
    await attachPerceptionReview(testInfo, canvas, 'hud-attention-inspect', 'narrow');

    await expect
      .poll(async () => (await readCanvasStats(canvas)).nonBlackRatio, { timeout: 5_000 })
      .toBeGreaterThan(0.05);
    expectNoFatalErrors(logs);
  });

  test('narrow viewport renders and accepts dock/trade/build hotkeys', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 390, height: 760 });
    const canvas = await loadGame(page);

    await canvas.click();
    await setSmokeLoopState(page, smokeLoopState.narrowCameraOffset);
    await expect
      .poll(async () => (await playerCameraSnapshot(page))?.narrowFocus ?? 0, {
        timeout: 2_000,
        message: 'portrait viewport should activate narrow camera framing',
      })
      .toBeGreaterThan(0.95);
    await expect
      .poll(
        async () => {
          const snap = await playerCameraSnapshot(page);
          if (!snap) return Number.POSITIVE_INFINITY;
          return Math.max(Math.abs(snap.offsetX), Math.abs(snap.offsetY));
        },
        {
          timeout: 6_000,
          message: 'narrow camera should keep the ship near screen center',
        },
      )
      .toBeLessThan(95);
    await setSmokeLoopState(page, smokeLoopState.clear);

    await tap(page, 'Escape');
    await tap(page, 'Tab');
    await tap(page, 'Tab');
    await tap(page, 'F');
    await tap(page, 'S');
    await tap(page, '1');
    await tap(page, '2');
    await tap(page, 'E');
    await hold(page, 'W', 300);

    const box = await canvas.boundingBox();
    expect(box).toBeTruthy();
    expect(box!.width).toBeGreaterThan(300);
    expect(box!.height).toBeGreaterThan(500);
    await expect
      .poll(async () => (await readCanvasStats(canvas)).nonBlackRatio, { timeout: 5_000 })
      .toBeGreaterThan(0.05);

    expectNoFatalErrors(logs, { allowExpectedLiveClose: true });
  });

  rootBundleSmokeTest('secure legacy recovery console is bounded, opaque, and one-shot on touch and keyboard', async ({ page }) => {
    test.setTimeout(60_000);
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 390, height: 760 });
    await page.goto(addQueryParam(smokeUrl({ singleplayer: true }), 'touch', '1'));
    await waitForRenderedGame(page, page.locator('canvas'), false);

    expect(await smokeLegacyRecoveryOffer(page, 30)).toBe(1);
    await expect
      .poll(async () => await legacyRecoveryFlags(page), { timeout: 5_000 })
      .toEqual(
        legacyRecoveryFlag.visible |
        legacyRecoveryFlag.canConfirm |
        legacyRecoveryFlag.canCancel,
      );

    const recoveryCopy = await legacyRecoveryCopy(page);
    expect(recoveryCopy).toContain('IDENTITY RECOVERY // SECURE DOCK');
    expect(recoveryCopy).toContain('OPAQUE CANDIDATE AUTHORIZED');
    expect(recoveryCopy).toContain('Import is atomic and one-time.');
    expect(recoveryCopy).not.toMatch(
      /(?:\.sav\b|\/Users\/|[A-Za-z]:\\|player_[0-9a-f]{8,}|[0-9a-f]{64})/i,
    );

    const confirm = page.locator('[data-control="recoveryConfirm"]');
    const cancel = page.locator('[data-control="recoveryCancel"]');
    await expect(confirm).toBeVisible();
    await expect(confirm).toBeEnabled();
    await expect(confirm).toHaveText('Recover');
    await expect(cancel).toBeVisible();
    await expect(cancel).toBeEnabled();
    await expect(cancel).toHaveText('Leave Untouched');
    for (const name of ['left', 'right', 'thrust', 'use', 'tab', 'plan']) {
      await expect(page.locator(`[data-control="${name}"]`)).toBeHidden();
    }
    await expectTouchControlsFit(page);

    const beforeGameplayFlags = await mobileControlFlags(page);
    const beforeActionDepth =
      (await wasmNumber(page, 'get_net_motion_action_queue_depth')) ?? -1;
    await hold(page, 'W', 180);
    for (const key of ['B', 'Tab', 'E', 'F', 'S', 'Digit1']) {
      await tap(page, key);
    }
    expect(await mobileControlFlags(page)).toBe(beforeGameplayFlags);
    expect(await wasmNumber(page, 'get_net_motion_action_queue_depth'))
      .toBe(beforeActionDepth);
    expect(await legacyRecoverySemantic(page)).toBe('offer');

    await confirm.click();
    await expect
      .poll(async () => await smokeLegacyRecoveryCount(page, 'confirm'))
      .toBe(1);
    await expect
      .poll(async () => await legacyRecoveryFlags(page))
      .toEqual(
        legacyRecoveryFlag.visible |
        legacyRecoveryFlag.confirming,
      );
    await tap(page, 'Enter');
    expect(await smokeLegacyRecoveryCount(page, 'confirm')).toBe(1);

    expect(await smokeLegacyRecoveryResult(page, 7)).toBe(1);
    expect(await legacyRecoverySemantic(page)).toBe('success');
    expect(await legacyRecoveryFlags(page)).toEqual(
      legacyRecoveryFlag.visible |
      legacyRecoveryFlag.result |
      legacyRecoveryFlag.success,
    );
    expect(await legacyRecoveryCopy(page)).toContain(
      'Authoritative ship, economy, and ownership state refreshed.',
    );

    expect(await smokeLegacyRecoveryOffer(page, 30)).toBe(1);
    await expect
      .poll(async () => (await legacyRecoveryFlags(page)) &
        legacyRecoveryFlag.canConfirm)
      .toBe(legacyRecoveryFlag.canConfirm);
    await smokeLegacyRecoverySetSendAdmitted(page, false);
    await tap(page, 'Enter');
    await expect.poll(async () => legacyRecoverySemantic(page))
      .toBe('retryable-send');
    expect(await smokeLegacyRecoveryCount(page, 'confirm')).toBe(1);
    await expect
      .poll(async () => (await legacyRecoveryFlags(page)) &
        legacyRecoveryFlag.canConfirm)
      .toBe(legacyRecoveryFlag.canConfirm);
    await smokeLegacyRecoverySetSendAdmitted(page, true);
    await confirm.click();
    await expect
      .poll(async () => await smokeLegacyRecoveryCount(page, 'confirm'))
      .toBe(2);

    const rejectionCases = [
      [1, 'no-match', 'No matching legacy save remained.'],
      [2, 'stale-offer', 'Reconnect to retry.'],
      [3, 'replay', 'already used'],
      [4, 'invalid-source', 'corrupt or unsupported'],
      [5, 'destination-conflict', 'was not overwritten'],
      [6, 'migration-failure', 'atomic import could not be completed'],
    ] as const;
    for (const [status, semantic, copyFragment] of rejectionCases) {
      expect(await smokeLegacyRecoveryOffer(page, 30)).toBe(1);
      expect(await smokeLegacyRecoveryResult(page, status)).toBe(1);
      expect(await legacyRecoverySemantic(page)).toBe(semantic);
      expect(await legacyRecoveryCopy(page)).toContain(copyFragment);
    }

    expect(await smokeLegacyRecoveryOffer(page, 30)).toBe(1);
    await expect
      .poll(async () => (await legacyRecoveryFlags(page)) &
        legacyRecoveryFlag.canCancel)
      .toBe(legacyRecoveryFlag.canCancel);
    await cancel.click();
    await expect
      .poll(async () => await smokeLegacyRecoveryCount(page, 'cancel'))
      .toBe(1);
    expect(await legacyRecoverySemantic(page)).toBe('cancelled');
    expect(await legacyRecoveryCopy(page)).toContain(
      'legacy save was left untouched',
    );

    expect(await smokeLegacyRecoveryOffer(page, 1)).toBe(1);
    await expect
      .poll(async () => await smokeLegacyRecoveryCount(page, 'expire'), {
        timeout: 4_000,
      })
      .toBe(1);
    expect(await legacyRecoverySemantic(page)).toBe('stale-offer');

    await smokeLegacyRecoveryReset(page);
    expect(await legacyRecoveryFlags(page)).toBe(0);
    expect(await smokeLegacyRecoveryOffer(page, 30)).toBe(1);
    await expect
      .poll(async () => (await legacyRecoveryFlags(page)) &
        legacyRecoveryFlag.canConfirm)
      .toBe(legacyRecoveryFlag.canConfirm);
    expectNoFatalErrors(logs);
  });

  rootBundleSmokeTest('gamepad recovery requires release and a rising edge without exposing touch chrome', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.addInitScript(() => {
      const state = { confirm: false, cancel: false };
      const buttons = [
        { pressed: false, touched: false, value: 0 },
        { pressed: false, touched: false, value: 0 },
      ];
      const pad = {
        axes: [],
        buttons,
        connected: true,
        id: 'signal-recovery-smoke-pad',
        index: 0,
        mapping: 'standard',
        timestamp: 0,
        vibrationActuator: null,
      };
      Object.defineProperty(navigator, 'getGamepads', {
        configurable: true,
        value: () => {
          buttons[0].pressed = state.confirm;
          buttons[0].touched = state.confirm;
          buttons[0].value = state.confirm ? 1 : 0;
          buttons[1].pressed = state.cancel;
          buttons[1].touched = state.cancel;
          buttons[1].value = state.cancel ? 1 : 0;
          return [pad];
        },
      });
      (window as unknown as {
        __setRecoveryGamepad: (confirm: boolean, cancel: boolean) => void;
      }).__setRecoveryGamepad = (confirm, cancel) => {
        state.confirm = confirm;
        state.cancel = cancel;
      };
    });
    const setPad = async (confirm: boolean, cancel: boolean) => {
      await page.evaluate(({ a, b }) => {
        (window as unknown as {
          __setRecoveryGamepad: (confirm: boolean, cancel: boolean) => void;
        }).__setRecoveryGamepad(a, b);
      }, { a: confirm, b: cancel });
    };

    await page.setViewportSize({ width: 520, height: 720 });
    await page.goto(smokeUrl({ singleplayer: true }));
    await waitForRenderedGame(page, page.locator('canvas'), false);
    await expect(page.locator('.signal-touch-controls')).toHaveCount(0);

    await setPad(true, false);
    await page.waitForTimeout(150);
    expect(await smokeLegacyRecoveryOffer(page, 30)).toBe(1);
    await expect
      .poll(async () => (await legacyRecoveryFlags(page)) &
        legacyRecoveryFlag.canConfirm)
      .toBe(legacyRecoveryFlag.canConfirm);
    await page.waitForTimeout(250);
    expect(await smokeLegacyRecoveryCount(page, 'confirm')).toBe(0);

    await setPad(false, false);
    await page.waitForTimeout(100);
    await setPad(true, false);
    await expect
      .poll(async () => await smokeLegacyRecoveryCount(page, 'confirm'))
      .toBe(1);
    expect(await legacyRecoverySemantic(page)).toBe('confirming');
    await page.waitForTimeout(250);
    expect(await smokeLegacyRecoveryCount(page, 'confirm')).toBe(1);

    await setPad(false, false);
    await page.waitForTimeout(100);
    expect(await smokeLegacyRecoveryOffer(page, 30)).toBe(1);
    await expect
      .poll(async () => (await legacyRecoveryFlags(page)) &
        legacyRecoveryFlag.canCancel)
      .toBe(legacyRecoveryFlag.canCancel);
    await setPad(true, true);
    await expect
      .poll(async () => await smokeLegacyRecoveryCount(page, 'cancel'))
      .toBe(1);
    expect(await smokeLegacyRecoveryCount(page, 'confirm')).toBe(0);
    expect(await legacyRecoverySemantic(page)).toBe('cancelled');

    await smokeLegacyRecoveryReset(page);
    expect(await legacyRecoveryFlags(page)).toBe(0);
    await setPad(false, false);
    expect(await smokeLegacyRecoveryOffer(page, 30)).toBe(1);
    await expect
      .poll(async () => (await legacyRecoveryFlags(page)) &
        legacyRecoveryFlag.canConfirm)
      .toBe(legacyRecoveryFlag.canConfirm);
    expectNoFatalErrors(logs);
  });

  rootBundleSmokeTest('touch controls keep stable slots while enabling contextual mobile actions', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 390, height: 760 });
    await page.goto(addQueryParam(smokeUrl({ singleplayer: true }), 'touch', '1'));
    await waitForRenderedGame(page, page.locator('canvas'), false);
    const touchScriptSrc = await page.locator('script[src*="signal-touch-controls.js"]').getAttribute('src');
    expect(touchScriptSrc).toMatch(/[?&]v=/);
    const primaryControls = ['use', 'fire', 'thrust', 'tractor', 'brake', 'scan', 'auto', 'plan', 'cycle', 'boost', 'left', 'right'];
    const stationControls = ['tab', 'page', 'sell', 'repair', 'laser', 'cargo', 'tractorUpgrade', 'one', 'two', 'three', 'four', 'five', 'lineage', 'lineageProof', 'back'];

    await expect
      .poll(async () => (await mobileControlFlags(page)) & mobileFlag.docked, { timeout: 5_000 })
      .toBe(mobileFlag.docked);
    await expect(page.locator('[data-control="use"]')).toHaveText('Launch');
    await expect(page.locator('[data-control="left"]')).toBeVisible();
    await expect(page.locator('[data-control="left"]')).toBeDisabled();
    await expect(page.locator('[data-control="thrust"]')).toBeVisible();
    await expect(page.locator('[data-control="thrust"]')).toBeDisabled();
    await expect(page.locator('[data-control="tab"]')).toBeVisible();
    await expect(page.locator('[data-control="tab"]')).toHaveText('Panel');
    await expect(page.locator('[data-control="page"]')).toBeVisible();
    await expect(page.locator('[data-control="page"]')).toBeDisabled();
    await expect.poll(async () => stationPanelLabel(page)).toBe('SHIP');
    await expect.poll(async () => stationPanelLegend(page)).toBe('[R] repair  [M/C/T] refit  [TAB] panel');
    const dockedPrimarySlots = await touchControlRects(page, primaryControls);
    const shipStationSlots = await touchControlRects(page, stationControls);
    await expectTouchControlsFit(page);

    await tap(page, 'Tab');
    await expect.poll(async () => stationPanelLabel(page)).toBe('TRADE');
    await expect
      .poll(async () => stationPanelLegend(page))
      .toMatch(/^\[1-5\] trade  \[F\] page(?:  \[L\] lineage)?  \[S\] sell all  \[TAB\] panel$/);
    expectTouchControlsKeepSlots(shipStationSlots, await touchControlRects(page, stationControls), stationControls);
    await expect
      .poll(async () => (await mobileControlFlags(page)) & mobileFlag.stationTrade)
      .toBe(mobileFlag.stationTrade);
    await expect
      .poll(async () => (await mobileControlFlags(page)) & mobileFlag.canPage)
      .toBe(mobileFlag.canPage);
    await expect
      .poll(async () => (await mobileControlFlags(page)) & mobileFlag.canSell)
      .toBe(mobileFlag.canSell);
    await expect
      .poll(async () => (await mobileControlFlags(page)) & mobileFlag.canDigits)
      .toBe(mobileFlag.canDigits);
    await expect(page.locator('[data-control="page"]')).toBeEnabled();
    await expect(page.locator('[data-control="sell"]')).toBeEnabled();
    const tradeSlots = await stationPanelDigitSlots(page);
    expect(tradeSlots).toBeGreaterThanOrEqual(0);
    expect(tradeSlots).toBeLessThanOrEqual(5);
    expect((await mobileDigitMask(page)) & 0x1f & ~((1 << tradeSlots) - 1)).toBe(0);

    await tap(page, 'Tab');
    await expect.poll(async () => stationPanelLabel(page)).toBe('CONTRACTS');
    await expect
      .poll(async () => stationPanelLegend(page))
      .toMatch(/^\[1-3\] select  \[S\] .+  \[TAB\] panel$/);
    await expect
      .poll(async () => (await mobileControlFlags(page)) & mobileFlag.stationWork)
      .toBe(mobileFlag.stationWork);
    await expect
      .poll(async () => (await mobileControlFlags(page)) & mobileFlag.canDigits)
      .toBe(mobileFlag.canDigits);
    const contractSlots = await stationPanelDigitSlots(page);
    expect(contractSlots).toBeGreaterThanOrEqual(0);
    expect(contractSlots).toBeLessThanOrEqual(3);
    expect((await mobileDigitMask(page)) & 0x1f & ~((1 << contractSlots) - 1)).toBe(0);
    await expectTouchControlsFit(page);

    await page.locator('[data-control="use"]').click();
    await expect
      .poll(async () => (await mobileControlFlags(page)) & mobileFlag.canFlight, { timeout: 8_000 })
      .toBe(mobileFlag.canFlight);
    await expect(page.locator('[data-control="left"]')).toBeVisible();
    await expect(page.locator('[data-control="right"]')).toBeVisible();
    await expect(page.locator('[data-control="boost"]')).toBeVisible();
    await expect(page.locator('[data-control="thrust"]')).toBeVisible();
    await expect(page.locator('[data-control="brake"]')).toBeVisible();
    await expect(page.locator('[data-control="thrust"]')).toBeEnabled();
    await expect(page.locator('[data-control="tab"]')).toBeHidden();
    expectTouchControlsKeepSlots(dockedPrimarySlots, await touchControlRects(page, primaryControls), primaryControls);
    await expectTouchControlsFit(page);

    await page.locator('[data-control="plan"]').click();
    await expect
      .poll(async () => (await mobileControlFlags(page)) & mobileFlag.planActive, { timeout: 5_000 })
      .toBe(mobileFlag.planActive);
    await expect(page.locator('[data-control="plan"]')).toHaveText('Exit');
    await expect(page.locator('[data-control="cycle"]')).toBeVisible();
    await expect(page.locator('[data-control="cycle"]')).toBeEnabled();
    await expect(page.locator('[data-control="fire"]')).toBeVisible();
    await expect(page.locator('[data-control="fire"]')).toBeDisabled();
    await expectTouchControlsFit(page);

    await page.setViewportSize({ width: 844, height: 390 });
    await page.waitForTimeout(250);
    await expectTouchControlsFit(page);

    expectNoFatalErrors(logs);
  });

  test('high-latency multiplayer correction telemetry stays bounded', async ({ page }) => {
    test.skip(
      !process.env.SMOKE_LATENCY_ASSERT,
      'set SMOKE_LATENCY_ASSERT=1 with SMOKE_URL pointed at a latency proxy',
    );
    test.setTimeout(70_000);

    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    const canvas = await loadGame(page, true);
    await resetNetMotionTelemetry(page);

    await canvas.click();
    await tap(page, 'Escape');
    await tap(page, 'E');

    for (let i = 0; i < 6; i++) {
      await holdChord(page, ['W', i % 2 === 0 ? 'A' : 'D'], 850);
      await hold(page, 'Shift', 300);
    }
    await page.waitForTimeout(2_000);

    await expect
      .poll(async () => (await netMotionSnapshot(page)).samples, {
        timeout: 20_000,
        message: 'latency smoke should collect local correction samples',
      })
      .toBeGreaterThan(0);

    const motion = await netMotionSnapshot(page);
    expect(motion.samples).toBeGreaterThan(10);
    // A one-client authority can suppress unchanged recipient-excluded player
    // batches after the initial baseline. Local correction samples below are
    // the meaningful repeated-authority signal for this case.
    expect(motion.playerBatches).toBeGreaterThan(0);
    expect(motion.inputAcks).toBeGreaterThan(0);
    expect(motion.pingSamples).toBeGreaterThan(0);
    expect(motion.lastPingRttMs).toBeGreaterThan(250);
    expect(motion.maxPingRttMs).toBeGreaterThan(250);
    expect(motion.lastAckRttMs).toBeGreaterThan(250);
    expect(motion.maxAckRttMs).toBeGreaterThan(250);
    expect(motion.lastAckGapMs).toBeGreaterThanOrEqual(0);
    expect(motion.pingServerTurnaroundMs).toBeGreaterThanOrEqual(0);
    expect(motion.maxPlayerIntervalMs).toBeGreaterThan(0);
    expect(motion.maxPlayerIntervalMs).toBeLessThanOrEqual(150);
    expect(motion.maxTickSkewAbs).toBeLessThan(720);
    expect(motion.actionQueueDepth).toBeLessThanOrEqual(1);
    expect(motion.replayDepth).toBeLessThanOrEqual(512);
    expect(motion.unackedInputs).toBeLessThan(64);
    expect(motion.snapSamples).toBe(0);
    expect(motion.maxRenderOffset).toBeLessThanOrEqual(48);
    expect(motion.currentRenderOffset).toBeLessThanOrEqual(2);
    expect(motion.maxAppliedCorrection).toBeLessThan(80);
    expect(motion.maxCorrection).toBeLessThan(160);
    expectNoFatalErrors(logs, { allowExpectedLiveClose: true });
  });

  test('low-ping high-ack multiplayer telemetry exposes authoritative lag', async ({ page }) => {
    test.skip(
      !process.env.SMOKE_ACK_LAG_ASSERT,
      'set SMOKE_ACK_LAG_ASSERT=1 with SMOKE_URL pointed at an authoritative-ack-delay proxy',
    );
    test.setTimeout(80_000);

    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    const canvas = await loadGame(page, true);
    await resetNetMotionTelemetry(page);

    await canvas.click();
    await tap(page, 'Escape');
    await tap(page, 'E');

    for (let i = 0; i < 8; i++) {
      await holdChord(page, ['W', i % 2 === 0 ? 'A' : 'D'], 750);
      await hold(page, 'Shift', 250);
    }
    await page.waitForTimeout(3_000);

    await expect
      .poll(async () => (await netMotionSnapshot(page)).inputAcks, {
        timeout: 30_000,
        message: 'ack-lag smoke should collect authoritative input acks',
      })
      .toBeGreaterThan(0);

    const motion = await netMotionSnapshot(page);
    expect(motion.samples).toBeGreaterThan(10);
    expect(motion.playerBatches).toBeGreaterThan(0);
    expect(motion.pingSamples).toBeGreaterThan(0);
    expect(motion.inputAcks).toBeGreaterThan(0);
    expect(motion.lastPingRttMs).toBeGreaterThan(0);
    expect(motion.smoothedPingRttMs).toBeGreaterThan(0);
    expect(motion.smoothedPingRttMs).toBeLessThan(180);
    expect(motion.lastPingRttMs).toBeLessThan(300);
    expect(motion.maxPingRttMs).toBeLessThan(300);
    expect(motion.lastAckRttMs).toBeGreaterThan(450);
    expect(motion.maxAckRttMs).toBeGreaterThan(450);
    expect(motion.lastAckGapMs).toBeGreaterThan(300);
    expect(motion.pingServerTurnaroundMs).toBeGreaterThanOrEqual(0);
    expect(motion.maxPlayerIntervalMs).toBeLessThanOrEqual(170);
    expect(motion.maxTickSkewAbs).toBeLessThan(720);
    expect(motion.actionQueueDepth).toBeLessThanOrEqual(1);
    expect(motion.replayDepth).toBeLessThanOrEqual(512);
    expect(motion.unackedInputs).toBeLessThan(96);
    expect(motion.snapSamples).toBe(0);
    expect(motion.maxRenderOffset).toBeLessThanOrEqual(48);
    expect(motion.currentRenderOffset).toBeLessThanOrEqual(2);
    expect(motion.maxAppliedCorrection).toBeLessThan(80);
    expect(motion.maxCorrection).toBeLessThan(160);
    expectNoFatalErrors(logs, { allowExpectedLiveClose: true });
  });
});
