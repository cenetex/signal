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
  await expect.poll(() => call<number>(page, 'signal_debug_local_save_generation'), { timeout: 20_000 }).toBeGreaterThan(0);
  expect(await call(page, 'signal_jank_profile_enabled')).toBe(1);
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

  const oldGeneration = await call<number>(page, 'signal_debug_local_save_generation');
  await expect.poll(() => call<number>(page, 'signal_save_local_world'), { timeout: 20_000 }).toBe(1);
  await expect.poll(() => call<number>(page, 'signal_debug_local_save_generation'), { timeout: 20_000 }).toBeGreaterThan(oldGeneration);
  await page.reload();
  await ready(page);
  const restored = await summary(page);
  await attachJson(testInfo, 'saved-world-checkpoint', { ageStart, aged, restored, ageSeconds });
  expect(restored.seed).toBe(aged.seed);
  expect(restored.ship).toBe(aged.ship);
  expect(restored.time).toBeGreaterThanOrEqual(aged.time);
  expect(restored.stations).toBe(aged.stations);
  await measure(page, testInfo, 'aged-saved-world');
  expect(errors).toEqual([]);
});
