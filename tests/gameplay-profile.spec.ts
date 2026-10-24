import { test, expect, type Page, type TestInfo } from '@playwright/test';
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { join } from 'node:path';
import { execFileSync } from 'node:child_process';
import { cpus, platform, arch, release } from 'node:os';

// The file is the stable selector for make jank-profile-browser.
// npm run smoke also runs the short form on every browser CI revision.
function secondsFromEnv(name: string, fallback: number): number {
  const seconds = Number(process.env[name] ?? fallback);
  if (!Number.isInteger(seconds) || seconds < 1 || seconds > 3600)
    throw new Error(`${name} must be a whole number from 1 to 3600`);
  return seconds;
}

const sampleSeconds = secondsFromEnv('SIGNAL_JANK_PROFILE_SECONDS', 3);
const ageSeconds = secondsFromEnv('SIGNAL_JANK_AGE_SECONDS', 60);

function optionalBudget(name: string): number | undefined {
  if (process.env[name] === undefined) return undefined;
  const value = Number(process.env[name]);
  if (!Number.isFinite(value) || value <= 0) throw new Error(`${name} must be positive`);
  return value;
}
const frameP95Budget = optionalBudget('SIGNAL_JANK_MAX_FRAME_P95_MS');
const tickLossBudget = optionalBudget('SIGNAL_JANK_MAX_TICK_LOSS_PERCENT');
if (frameP95Budget !== undefined && frameP95Budget >= 63.875)
  throw new Error('Frame p95 budget must be below the 63.875 ms histogram ceiling');
if (tickLossBudget !== undefined && tickLossBudget > 100)
  throw new Error('Tick-loss budget must be at most 100 percent');

type Summary = { seed: number; time: number; ship: number; credits: number; stations: number };
type Percentiles = { p50: number; p95: number; p99: number; max: number };
type Profile = {
  schema: string; enabled: boolean; window_sec: number; frames: number;
  frame_ms: Percentiles; simulation_ms: Percentiles;
  fixed_step: { completed: number; missed: number; accumulator_dropped: number };
  snapshots: { packets: number; bytes: number; max_gap_ms: number };
  phases: Record<string, { avg_ms: number; max_ms: number }>;
  entities: Record<string, { samples: number; max_correction_world: number; max_correction_jerk: number }>;
};

async function call<T>(page: Page, name: string, result = 'number', args: number[] = []): Promise<T> {
  return page.evaluate(({ name, result, args }) => {
    const mod = (window as any).Module;
    return mod.ccall(name, result, args.map(() => 'number'), args);
  }, { name, result, args });
}
async function summary(page: Page): Promise<Summary> {
  return JSON.parse(await call<string>(page, 'signal_debug_local_save_summary', 'string'));
}
async function ready(page: Page): Promise<void> {
  await expect(page.locator('canvas')).toBeVisible();
  await page.waitForFunction(() => typeof (window as any).Module?.ccall === 'function');
  await expect.poll(() => call<number>(page, 'signal_debug_auth_available'), { timeout: 20_000 }).toBe(1);
  await expect.poll(() => call<number>(page, 'signal_render_queued_vertices')).toBeGreaterThan(0);
  const checkpoint = await page.evaluate(checkpointInPage, false);
  expect(checkpoint.saved).toBeGreaterThan(0);
  expect(await call(page, 'signal_jank_profile_enabled')).toBe(1);
}
// Observe the IDBFS completion assignment in the page. A later autosave can
// return storage to busy before a cross-process generation poll sees ready.
async function checkpointInPage(requestSave = true): Promise<{ before: number; saved: number }> {
  const mod = (window as any).Module;
  const storage = mod.signalLocalPersistence;
  const original = Object.getOwnPropertyDescriptor(storage, 'state');
  if (!original || !('value' in original)) throw new Error('Expected local persistence state');
  let state = original.value;
  let pending: { resolve: (generation: number) => void; reject: (error: Error) => void } | undefined;
  const generation = () => mod.ccall('signal_debug_local_save_generation', 'number', [], []);
  Object.defineProperty(storage, 'state', {
    configurable: true, enumerable: original.enumerable,
    get: () => state,
    set: value => {
      state = value;
      if (pending && value !== 2) {
        const waiter = pending;
        pending = undefined;
        if (value === 1) waiter.resolve(generation());
        else waiter.reject(new Error(`Local checkpoint failed with storage state ${value}`));
      }
    },
  });
  const ready = (): Promise<number> => {
    if (state === 1) return Promise.resolve(generation());
    if (state !== 2) return Promise.reject(new Error(`Local checkpoint failed with storage state ${state}`));
    return new Promise((resolve, reject) => { pending = { resolve, reject }; });
  };
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      (async () => {
        const before = await ready();
        if (before <= 0) throw new Error('Expected a completed initial local checkpoint');
        if (!requestSave) return { before, saved: before };
        if (mod.ccall('signal_save_local_world', 'number', [], []) !== 1)
          throw new Error('Local checkpoint request failed');
        const saved = await ready();
        if (saved <= before) throw new Error('Local checkpoint generation should advance');
        return { before, saved };
      })(),
      new Promise<never>((_, reject) => {
        timer = setTimeout(() => reject(new Error('Local checkpoint completion timed out')), 30_000);
      }),
    ]);
  } finally {
    clearTimeout(timer);
    Object.defineProperty(storage, 'state', { ...original, value: state });
  }
}

async function attachJson(testInfo: TestInfo, name: string, value: unknown): Promise<void> {
  const path = testInfo.outputPath(`${name}.json`);
  await writeFile(path, JSON.stringify(value, null, 2));
  await testInfo.attach(name, { path, contentType: 'application/json' });
}

async function measure(page: Page, testInfo: TestInfo, name: string): Promise<void> {
  // Sampling starts after setup and warm-up. The frame loop owns every measured tick.
  const before = await summary(page);
  await call(page, 'signal_jank_profile_reset', 'void');
  const started = Date.now();
  await page.waitForTimeout(sampleSeconds * 1000);
  const profile: Profile = JSON.parse(await call<string>(page, 'signal_jank_profile_report_json', 'string'));
  const after = await summary(page);
  const elapsedSeconds = (Date.now() - started) / 1000;
  const accountedTicks = profile.fixed_step.completed + profile.fixed_step.accumulator_dropped;
  const tickLossPercent = 100 * profile.fixed_step.accumulator_dropped / Math.max(1, accountedTicks);
  // Save evidence before assertions so a failed check retains the actual measurements.
  await attachJson(testInfo, name, {
    scenario: name, workload: 'active dockside singleplayer', requestedSeconds: sampleSeconds,
    elapsedSeconds, before, after, simulationSeconds: after.time - before.time,
    simulationSecondsPerWallSecond: (after.time - before.time) / elapsedSeconds,
    tickLossPercent, profile,
    budgets: { frameP95Ms: frameP95Budget ?? null, tickLossPercent: tickLossBudget ?? null },
  });
  expect(profile.schema).toBe('signal.gameplay-jank.v1');
  expect(profile.enabled).toBe(true);
  expect(profile.frames).toBeGreaterThan(10);
  expect(profile.window_sec).toBeGreaterThanOrEqual(sampleSeconds * 0.9);
  expect(after.time - before.time, 'the authority should advance during sampling').toBeGreaterThan(0.1);
  expect(after.ship).toBe(before.ship);
  expect(profile.fixed_step.completed).toBeGreaterThan(10);
  expect(profile.snapshots.packets, 'active loopback should deliver world packets').toBeGreaterThan(0);
  expect(profile.snapshots.bytes).toBeGreaterThan(0);
  expect(profile.phases.authority_sim.max_ms).toBeGreaterThan(0);
  expect(profile.phases.loopback_codec.max_ms).toBeGreaterThan(0);
  expect(Object.values(profile.entities).reduce((sum, entity) => sum + entity.samples, 0),
    'nearby moving entities should have observed corrections').toBeGreaterThan(0);
  for (const values of [profile.frame_ms, profile.simulation_ms]) {
    for (const value of Object.values(values)) expect(Number.isFinite(value)).toBe(true);
    expect(values.p50).toBeGreaterThanOrEqual(0);
    expect(values.p95).toBeGreaterThanOrEqual(values.p50);
    expect(values.p99).toBeGreaterThanOrEqual(values.p95);
    expect(values.max).toBeGreaterThan(0);
  }
  expect(Number.isFinite(tickLossPercent)).toBe(true);
  if (frameP95Budget !== undefined) expect(profile.frame_ms.p95).toBeLessThanOrEqual(frameP95Budget);
  if (tickLossBudget !== undefined) expect(tickLossPercent).toBeLessThanOrEqual(tickLossBudget);
}

test('active fresh and aged saved worlds produce profiling evidence', async ({ page, browser }, testInfo) => {
  test.skip(!!process.env.SMOKE_URL, 'local profiling uses an isolated browser world');
  test.setTimeout(120_000 + sampleSeconds * 2000 + ageSeconds * 1000);
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  page.on('console', message => {
    if (message.type() === 'error' && /abort|RuntimeError|unreachable|Cannot enlarge memory/i.test(message.text()))
      errors.push(message.text());
  });
  await attachJson(testInfo, 'profile-environment', {
    sourceRevision: execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim(),
    wasmSha256: createHash('sha256').update(await readFile(join(__dirname, '..', 'build-web', 'signal.wasm'))).digest('hex'),
    platform: platform(), arch: arch(), osRelease: release(), cpu: cpus()[0]?.model,
    logicalCpus: cpus().length, browser: browser.version(), viewport: page.viewportSize(),
    sampleSeconds, ageSeconds,
  });
  await page.goto('/play.html?singleplayer=1&smoke=1&jankprofile=1');
  await ready(page);
  await measure(page, testInfo, 'fresh-world');

  // Exercise the test-only gate and batch bound before aging the real authority.
  expect(await call(page, 'signal_smoke_profile_advance', 'number', [0])).toBe(0);
  expect(await call(page, 'signal_smoke_profile_advance', 'number', [121])).toBe(0);
  await page.evaluate(() => history.replaceState(null, '', '/play.html?singleplayer=1&jankprofile=1'));
  expect(await call(page, 'signal_smoke_profile_advance', 'number', [1])).toBe(0);
  await page.evaluate(() => history.replaceState(null, '', '/play.html?singleplayer=1&smoke=1&jankprofile=1'));
  const ageStart = await summary(page);
  for (let second = 0; second < ageSeconds; second++)
    expect(await call(page, 'signal_smoke_profile_advance', 'number', [120])).toBe(120);
  const aged = await summary(page);
  expect(aged.time - ageStart.time).toBeGreaterThanOrEqual(ageSeconds - 0.01);

  const checkpoint = await page.evaluate(checkpointInPage);
  await page.reload();
  await ready(page);
  const restored = await summary(page);
  await attachJson(testInfo, 'saved-world-checkpoint', { ageStart, aged, restored, ageSeconds, checkpoint });
  expect(restored.seed).toBe(aged.seed);
  expect(restored.ship).toBe(aged.ship);
  expect(restored.time).toBeGreaterThanOrEqual(aged.time);
  expect(restored.stations).toBe(aged.stations);
  await measure(page, testInfo, 'aged-saved-world');
  expect(errors).toEqual([]);
});

// Reproduce the ready window closing before the next external browser poll.
for (const fail of [false, true]) {
  test(`checkpoint completion ${fail ? 'reports storage failure' : 'survives an immediate next autosave'}`, async ({ page }) => {
    await page.goto('about:blank');
    await page.evaluate(fail => {
      const storage = { state: 1 };
      let saved = 1;
      (window as any).Module = {
        signalLocalPersistence: storage,
        ccall(name: string) {
          if (name === 'signal_debug_local_save_generation') return storage.state === 1 ? saved : 0;
          if (name === 'signal_save_local_world') {
            storage.state = 2;
            setTimeout(() => {
              saved++;
              storage.state = fail ? -1 : 1;
              // An immediate next save hides the completed generation from polling.
              if (!fail) storage.state = 2;
            }, 10);
            return 1;
          }
          throw new Error('Unexpected checkpoint call');
        },
      };
    }, fail);
    if (fail) await expect(page.evaluate(checkpointInPage)).rejects.toThrow('Local checkpoint failed with storage state -1');
    else expect(await page.evaluate(checkpointInPage)).toEqual({ before: 1, saved: 2 });
    expect(await page.evaluate(() => {
      const descriptor = Object.getOwnPropertyDescriptor((window as any).Module.signalLocalPersistence, 'state');
      return descriptor && 'value' in descriptor;
    })).toBe(true);
  });
}

// Boot and reload can also enter while the first autosave is still busy.
for (const fail of [false, true]) {
  test(`checkpoint readiness ${fail ? 'reports storage failure' : 'captures a brief ready window'}`, async ({ page }) => {
    await page.goto('about:blank');
    await page.evaluate(() => {
      const storage = { state: 2 };
      (window as any).Module = {
        signalLocalPersistence: storage,
        ccall(name: string) {
          if (name === 'signal_debug_local_save_generation') return storage.state === 1 ? 3 : 0;
          throw new Error('Readiness should only read the checkpoint generation');
        },
      };
    });
    const observed = page.evaluate(checkpointInPage, false);
    // Attach rejection handling before triggering the storage error.
    const result = observed.then(value => ({ value, error: '' }), error => ({ value: undefined, error: error.message }));
    await page.waitForFunction(() => typeof Object.getOwnPropertyDescriptor(
      (window as any).Module.signalLocalPersistence, 'state')?.get === 'function');
    await page.evaluate(fail => {
      const storage = (window as any).Module.signalLocalPersistence;
      storage.state = fail ? -1 : 1;
      if (!fail) storage.state = 2;
    }, fail);
    const outcome = await result;
    if (fail) expect(outcome.error).toContain('Local checkpoint failed with storage state -1');
    else expect(outcome.value).toEqual({ before: 3, saved: 3 });
    expect(await page.evaluate(() => {
      const descriptor = Object.getOwnPropertyDescriptor((window as any).Module.signalLocalPersistence, 'state');
      return descriptor && 'value' in descriptor;
    })).toBe(true);
  });
}

test('active hauling and turns reuse verified history signatures', async ({ page }, testInfo) => {
  test.skip(!!process.env.SMOKE_URL, 'hauling profiling uses an isolated browser world');
  const seconds = secondsFromEnv('SIGNAL_HAUL_PROFILE_SECONDS', 3);
  if (seconds > 12) throw new Error('Hauling phases must be at most 12 seconds');
  test.setTimeout(90_000 + seconds * 3000);
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto('/play.html?singleplayer=1&smoke=1&jankprofile=1');
  await ready(page);
  await page.locator('canvas').click();
  expect(await call<number>(page, 'signal_smoke_prepare_tow_lifecycle')).toBeGreaterThan(0);
  await page.keyboard.down('Space');
  const attached = 0b111110001111100;
  await expect.poll(async () => (await call<number>(page, 'signal_smoke_tow_lifecycle_state')) & attached,
    { timeout: 20_000 }).toBe(attached);
  await page.waitForTimeout(600);
  await page.keyboard.up('Space');

  const position = async () => ({
    x: await call<number>(page, 'get_player_pos_x'), y: await call<number>(page, 'get_player_pos_y'),
  });
  const before = await position();
  for (const [name, keys] of [['tow-straight', ['w']], ['tow-turn', ['w', 'a']], ['tow-coast', []]] as const) {
    await call(page, 'signal_jank_profile_reset', 'void');
    const start = await position();
    const balanceBefore = (await summary(page)).credits;
    for (const key of keys) await page.keyboard.down(key);
    await page.waitForTimeout(seconds * 1000);
    for (const key of keys) await page.keyboard.up(key);
    const profile: Profile = JSON.parse(await call<string>(page, 'signal_jank_profile_report_json', 'string'));
    const end = await position();
    const tow = await call<number>(page, 'signal_smoke_tow_lifecycle_state');
    const balanceAfter = (await summary(page)).credits;
    await attachJson(testInfo, name, {
      workload: 'active local hauling', seconds, start, end, tow, profile, balanceBefore, balanceAfter,
    });
    expect(profile.fixed_step.completed).toBeGreaterThan(10);
    expect(profile.phases.authority_sim.max_ms).toBeGreaterThan(0);
    expect(profile.snapshots.packets).toBeGreaterThan(0);
    expect([end.x, end.y].every(Number.isFinite)).toBe(true);
    // A refinery can smelt this loaded rock during the final dock approach.
    // Prove a paid delivery when coast consumes it; flight and turns keep it held.
    if (name === 'tow-coast' && (tow & 1) === 0) {
      expect(tow & ((1 << 0) | (1 << 4))).toBe(0);
      expect(balanceAfter, 'consumed tow must pay into the station balance').toBeGreaterThan(balanceBefore);
    } else {
      expect(tow & ((1 << 0) | (1 << 4)),
        'authority retains the loaded rock through flight and turns').toBe((1 << 0) | (1 << 4));
    }
    await testInfo.attach(name, { body: await page.screenshot(), contentType: 'image/png' });
  }
  const after = await position();
  expect(Math.hypot(after.x - before.x, after.y - before.y)).toBeGreaterThan(30);
  // Station production appends records during ordinary play. Its next origin
  // scan reads the full history and reuses successful checks for older headers.
  await expect.poll(async () => JSON.parse(await call<string>(page,
    'signal_smoke_origin_signature_stats', 'string')).hits, { timeout: 20_000 }).toBeGreaterThan(0);
  const signatures = JSON.parse(await call<string>(page, 'signal_smoke_origin_signature_stats', 'string'));
  await attachJson(testInfo, 'hauling-signature-work', signatures);
  expect(signatures.checks).toBeGreaterThan(0);
  expect(signatures.historyScans).toBeGreaterThan(1);
  expect(errors).toEqual([]);
});
