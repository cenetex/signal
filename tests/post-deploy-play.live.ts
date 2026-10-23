import { test, expect, type Page, type BrowserContext } from '@playwright/test';
import { createPrivateKey, createPublicKey, timingSafeEqual } from 'node:crypto';
import { writeFile } from 'node:fs/promises';

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
    const value = (window as any).Module.ccall(exportName, 'number', [], []);
    if (!Number.isFinite(value)) throw new Error(`Invalid diagnostic ${exportName}`);
    return value;
  }, name);
}
async function text(page: Page, name: string): Promise<string> {
  return page.evaluate((exportName) => (window as any).Module.ccall(exportName, 'string', [], []), name);
}
async function turn(page: Page, context: BrowserContext, touch: boolean, right: boolean) {
  let release: () => Promise<unknown>;
  if (touch) {
    const control = page.locator(`[data-control="${right ? 'right' : 'left'}"]`);
    await expect(control).toBeEnabled();
    const box = await control.boundingBox();
    expect(box).toBeTruthy();
    const session = await context.newCDPSession(page);
    await session.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: box!.x + box!.width / 2, y: box!.y + box!.height / 2 }] });
    release = async () => { await session.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] }); await session.detach(); };
  } else {
    await page.locator('canvas').focus();
    await page.keyboard.down(right ? 'd' : 'a');
    release = () => page.keyboard.up(right ? 'd' : 'a');
  }
  try {
    await expect.poll(() => number(page, 'signal_debug_held_control_mask'), { timeout: 2000 }).toBe(right ? 8 : 4);
    await page.waitForTimeout(300);
  } finally { await release(); }
  await expect.poll(() => number(page, 'signal_debug_held_control_mask')).toBe(0);
}

test('deployed game accepts player controls and reconnects', async ({ browser, request }, info) => {
  const touch = info.project.name === 'touch';
  const identity = process.env[`SIGNAL_PLAY_IDENTITY_${touch ? 'TOUCH' : 'DESKTOP'}`];
  if (!identity || Buffer.from(identity, 'base64').length !== 64 || Buffer.from(identity, 'base64').toString('base64') !== identity) {
    throw new Error('Prepare the dedicated play-check identity before this run.');
  }
  const identityBytes = Buffer.from(identity, 'base64');
  const key = createPrivateKey({ key: Buffer.concat([Buffer.from('302e020100300506032b657004220420', 'hex'), identityBytes.subarray(0, 32)]), format: 'der', type: 'pkcs8' });
  const publicKey = createPublicKey(key).export({ format: 'der', type: 'spki' }).subarray(-32);
  if (!timingSafeEqual(publicKey, identityBytes.subarray(32))) throw new Error('Play-check identity must contain a matching key pair.');
  const evidence: Record<string, unknown> = { expectedRevision: expected, device: info.project.name };
  const errors: string[] = [];
  // Manual tracing begins after identity setup. Private setup arguments and
  // browser storage are excluded from the trace and result attachments.
  const context = await browser.newContext({
    viewport: touch ? { width: 390, height: 760 } : { width: 1280, height: 720 },
    hasTouch: touch, isMobile: touch,
  });
  const page = await context.newPage();
  let tracing = false;
  page.on('pageerror', error => errors.push(error.message));
  page.on('console', message => {
    if (/abort\(|RuntimeError|memory access out of bounds|unreachable|WebGL.*context lost/i.test(message.text())) errors.push(message.text());
  });
  const checkHealth = async (label: string) => {
    let health: any;
    await expect.poll(async () => {
      try {
        const response = await request.get(healthUrl, { timeout: 5000 });
        if (!response.ok()) return `HTTP ${response.status()}`;
        health = await response.json();
        return health.version;
      } catch { return 'health request pending'; }
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
    const screenshot = await page.locator('canvas').screenshot();
    const renderedColors = await page.evaluate(async png => {
      const image = new Image();
      image.src = `data:image/png;base64,${png}`;
      await image.decode();
      const canvas = document.createElement('canvas');
      canvas.width = image.width;
      canvas.height = image.height;
      const draw = canvas.getContext('2d')!;
      draw.drawImage(image, 0, 0);
      const pixels = draw.getImageData(0, 0, canvas.width, canvas.height).data;
      const colors = new Set<number>();
      for (let i = 0; i < pixels.length; i += 4) {
        colors.add((pixels[i] >> 4) << 8 | (pixels[i + 1] >> 4) << 4 | (pixels[i + 2] >> 4));
      }
      return colors.size;
    }, screenshot.toString('base64'));
    expect(renderedColors, 'game canvas contains rendered detail').toBeGreaterThan(8);
  };
  const exercise = async (right: boolean) => {
    const before = await number(page, 'get_player_angle');
    const acksBefore = await number(page, 'get_net_motion_total_input_acks');
    await turn(page, context, touch, right);
    await expect.poll(() => number(page, 'get_net_motion_total_input_acks'), { timeout: 10_000 }).toBeGreaterThan(acksBefore);
    const after = await number(page, 'get_player_angle');
    const angleChange = Math.abs(Math.atan2(Math.sin(after - before), Math.cos(after - before)));
    expect(angleChange).toBeGreaterThan(0.04);
    return { angleChange, acksBefore, acksAfter: await number(page, 'get_net_motion_total_input_acks') };
  };
  try {
    // Fulfil this setup document locally; the game has yet to open a session.
    const setupUrl = new URL('/__signal_play_identity_setup__', url).href;
    await page.route(setupUrl, route => route.fulfill({ contentType: 'text/html', body: '<!doctype html><title>Play check setup</title>' }));
    await page.goto(setupUrl);
    await page.evaluate(value => localStorage.setItem('signal:identity', value), identity);
    await page.unroute(setupUrl);
    await context.tracing.start({ screenshots: true, snapshots: true });
    tracing = true;
    await checkHealth('before');
    const target = new URL(url);
    if (touch) target.searchParams.set('touch', '1');
    await page.goto(target.href);
    await joined();
    evidence.dedicatedAccount = true;
    if (await number(page, 'get_player_docked') === 1) {
      if (touch) await page.locator('[data-control="use"]').tap();
      else { await page.locator('canvas').focus(); await page.keyboard.press('e'); }
    }
    await expect.poll(() => number(page, 'get_player_docked'), { timeout: 10_000 }).toBe(0);
    evidence.flight = await exercise(false);
    const flightPath = info.outputPath('flight.png');
    await page.screenshot({ path: flightPath });
    await info.attach('flight', { path: flightPath, contentType: 'image/png' });
    await page.reload();
    await joined();
    await expect.poll(() => number(page, 'get_player_docked'), { timeout: 10_000 }).toBe(0);
    evidence.reconnectFlight = await exercise(true);
    evidence.reconnected = true;
    await checkHealth('after');
    expect(errors).toEqual([]);
  } finally {
    evidence.runtimeErrors = errors;
    const reportPath = info.outputPath('play-check.json');
    await writeFile(reportPath, JSON.stringify(evidence, null, 2));
    await info.attach('play-check', { path: reportPath, contentType: 'application/json' });
    try {
      if (tracing) {
        const screenPath = info.outputPath('final-screen.png');
        await page.screenshot({ path: screenPath });
        await info.attach('final-screen', { path: screenPath, contentType: 'image/png' });
        const path = info.outputPath('trace.zip');
        await context.tracing.stop({ path });
        await info.attach('trace', { path, contentType: 'application/zip' });
      }
    } finally { await context.close(); }
  }
});
