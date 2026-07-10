import { test, expect, type Page, type Locator } from '@playwright/test';
import { inflateSync } from 'node:zlib';

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

test.describe('Browser smoke tests', () => {
  const rootBundleSmokeTest = process.env.SIGNAL_PRE_PROMOTION_SMOKE === '1' ? test.skip : test;

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
    expect(motion.maxCorrection).toBeLessThan(5);

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
    expect(await hudHintText(page)).toContain('SUPPLY NEED :: Outpost scaffold needs 30 frames at Outpost 4.');

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

  rootBundleSmokeTest('smooths remote towable scaffold, cargo pod, and fragment snapshots', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 1280, height: 720 });
    await loadGame(page, false, { singleplayer: true });

    expect(await remoteTowableInterpCheck(page)).toBe(1);

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

  rootBundleSmokeTest('touch controls keep stable slots while enabling contextual mobile actions', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 390, height: 760 });
    await page.goto(addQueryParam(smokeUrl({ singleplayer: true }), 'touch', '1'));
    await waitForRenderedGame(page, page.locator('canvas'), false);
    const touchScriptSrc = await page.locator('script[src*="signal-touch-controls.js"]').getAttribute('src');
    expect(touchScriptSrc).toMatch(/[?&]v=/);
    const primaryControls = ['use', 'fire', 'thrust', 'tractor', 'brake', 'scan', 'auto', 'plan', 'cycle', 'boost', 'left', 'right'];
    const stationControls = ['tab', 'page', 'sell', 'repair', 'laser', 'cargo', 'tractorUpgrade', 'one', 'two', 'three', 'four', 'five'];

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
    await expect.poll(async () => stationPanelLegend(page)).toBe('[1-5] trade  [F] page  [S] sell all  [TAB] panel');
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
    expect(motion.playerBatches).toBeGreaterThan(1);
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
    expect(motion.snapSamples).toBeLessThan(5);
    expect(motion.maxRenderOffset).toBeLessThanOrEqual(260);
    expect(motion.currentRenderOffset).toBeLessThanOrEqual(260);
    expect(motion.maxAppliedCorrection).toBeLessThan(360);
    expect(motion.maxCorrection).toBeLessThan(900);
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
    expect(motion.playerBatches).toBeGreaterThan(1);
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
    expect(motion.snapSamples).toBeLessThan(8);
    expect(motion.maxRenderOffset).toBeLessThanOrEqual(260);
    expect(motion.currentRenderOffset).toBeLessThanOrEqual(260);
    expect(motion.maxAppliedCorrection).toBeLessThan(420);
    expect(motion.maxCorrection).toBeLessThan(1100);
    expectNoFatalErrors(logs, { allowExpectedLiveClose: true });
  });
});
