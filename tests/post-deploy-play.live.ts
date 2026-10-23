import { test, expect, type Page } from '@playwright/test';

const playUrl = process.env.SIGNAL_PLAY_URL;
const revision = process.env.SIGNAL_EXPECTED_REVISION;
if (!playUrl || !revision || !/^[0-9a-f]{7,40}$/.test(revision)) {
  throw new Error('Set SIGNAL_PLAY_URL and SIGNAL_EXPECTED_REVISION (7 to 40 hex characters).');
}
const expected = revision.slice(0, 7);
const url = new URL(playUrl);
for (const key of ['singleplayer', 'smokeState', 'lobby']) {
  if (url.searchParams.has(key)) throw new Error(`Live play URL must omit ${key}.`);
}
url.searchParams.set('online', '1');
url.searchParams.set('v', expected);
const healthUrl = process.env.SIGNAL_HEALTH_URL || new URL('/health', url).href;

async function number(page: Page, name: string): Promise<number> {
  return page.evaluate((exportName) => {
    const mod = (window as any).Module;
    const value = mod.ccall(exportName, 'number', [], []);
    if (!Number.isFinite(value)) throw new Error(`Invalid diagnostic ${exportName}`);
    return value;
  }, name);
}
async function text(page: Page, name: string): Promise<string> {
  return page.evaluate((exportName) => (window as any).Module.ccall(exportName, 'string', [], []), name);
}
async function position(page: Page) {
  return { x: await number(page, 'get_player_pos_x'), y: await number(page, 'get_player_pos_y') };
}

test('deployed game accepts player controls and reconnects', async ({ page, context, request }, info) => {
  const touch = info.project.name === 'touch';
  const evidence: Record<string, unknown> = { expectedRevision: expected, device: info.project.name };
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  page.on('console', message => {
    if (/abort\(|RuntimeError|memory access out of bounds|unreachable|WebGL.*context lost/i.test(message.text())) {
      errors.push(message.text());
    }
  });
  // The context starts empty. A fresh identity is created by the normal client.
  // Keep only presence checks in evidence; keys and tokens stay in the context.
  await context.addInitScript(() => {
    (window as any).__playCheckIdentityWasEmpty = localStorage.getItem('signal:identity') === null;
  });
  const checkHealth = async (label: string) => {
    let health: any;
    await expect.poll(async () => {
      const response = await request.get(healthUrl, { timeout: 10_000 });
      if (!response.ok()) return `HTTP ${response.status()}`;
      health = await response.json();
      return health.version;
    }, { timeout: 30_000, intervals: [1000, 2000, 3000], message: `${label}: deployed server revision` }).toBe(expected);
    expect(health.status).toBe('ok');
    evidence[label] = { version: health.version, status: health.status, worldTick: health.world_tick };
  };
  const joined = async () => {
    await expect(page.locator('canvas')).toBeVisible();
    await page.waitForFunction(() => (window as any).SignalGameWasmReady === true);
    await expect.poll(() => number(page, 'signal_debug_net_connected'), { timeout: 20_000 }).toBe(1);
    await expect.poll(() => number(page, 'signal_debug_auth_available'), { timeout: 20_000 }).toBe(1);
    expect(await text(page, 'signal_build_revision')).toBe(expected);
    expect(await text(page, 'signal_server_revision')).toBe(expected);
  };
  try {
    await checkHealth('before');
    const target = new URL(url);
    if (touch) target.searchParams.set('touch', '1');
    await page.goto(target.href);
    await joined();
    expect(await page.evaluate(() => (window as any).__playCheckIdentityWasEmpty)).toBe(true);
    expect(await number(page, 'get_player_docked')).toBe(1);
    evidence.freshIdentity = true;
    if (touch) await page.locator('[data-control="use"]').tap();
    else { await page.locator('canvas').focus(); await page.keyboard.press('e'); }
    await expect.poll(() => number(page, 'get_player_docked'), { timeout: 10_000 }).toBe(0);

    const before = await position(page);
    const acksBefore = await number(page, 'get_net_motion_total_input_acks');
    let release: () => Promise<unknown>;
    if (touch) {
      const control = page.locator('[data-control="thrust"]');
      await expect(control).toBeEnabled();
      const box = await control.boundingBox();
      expect(box).toBeTruthy();
      const session = await context.newCDPSession(page);
      await session.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: box!.x + box!.width / 2, y: box!.y + box!.height / 2 }] });
      release = async () => { await session.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] }); await session.detach(); };
    } else {
      await page.keyboard.down('w');
      release = () => page.keyboard.up('w');
    }
    try {
      await expect.poll(() => number(page, 'signal_debug_held_control_mask'), { timeout: 2000 }).toBe(1);
      // Keep the live action short: launch, a brief thrust, then release.
      await page.waitForTimeout(600);
    } finally { await release(); }
    await expect.poll(() => number(page, 'signal_debug_held_control_mask')).toBe(0);
    await expect.poll(() => number(page, 'get_net_motion_total_input_acks'), { timeout: 10_000 }).toBeGreaterThan(acksBefore);
    const after = await position(page);
    const distance = Math.hypot(after.x - before.x, after.y - before.y);
    expect(distance).toBeGreaterThan(2);
    evidence.flight = { distance, acksBefore, acksAfter: await number(page, 'get_net_motion_total_input_acks') };
    await info.attach('flight', { body: await page.screenshot(), contentType: 'image/png' });

    await page.reload();
    await joined();
    expect(await page.evaluate(() => (window as any).__playCheckIdentityWasEmpty)).toBe(false);
    await expect.poll(() => number(page, 'get_player_docked'), { timeout: 10_000 }).toBe(0);
    const reconnectAcks = await number(page, 'get_net_motion_total_input_acks');
    await expect.poll(() => number(page, 'get_net_motion_total_input_acks'), { timeout: 10_000 }).toBeGreaterThan(reconnectAcks);
    evidence.reconnected = true;
    await checkHealth('after');
    expect(errors).toEqual([]);
  } finally {
    evidence.runtimeErrors = errors;
    await info.attach('play-check', { body: JSON.stringify(evidence, null, 2), contentType: 'application/json' });
  }
});
