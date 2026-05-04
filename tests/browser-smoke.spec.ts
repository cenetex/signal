import { test, expect, type Page, type Locator } from '@playwright/test';
import { inflateSync } from 'node:zlib';

const fatalPattern =
  /abort|unreachable|RuntimeError|LinkError|compile failed|Cannot enlarge memory|exception thrown/i;

type FatalCollectors = {
  pageErrors: string[];
  consoleErrors: string[];
};

type CanvasStats = {
  pixels: number;
  nonBlackRatio: number;
  uniqueBuckets: number;
  avgLuma: number;
};

function smokeUrl(): string {
  return process.env.SMOKE_URL || '/signal.html?singleplayer=1';
}

function installFatalCollectors(page: Page): FatalCollectors {
  const logs: FatalCollectors = { pageErrors: [], consoleErrors: [] };
  page.on('pageerror', (err) => logs.pageErrors.push(err.message));
  page.on('console', (msg) => {
    if (msg.type() === 'error') logs.consoleErrors.push(msg.text());
  });
  return logs;
}

function expectNoFatalErrors(logs: FatalCollectors): void {
  expect(logs.pageErrors.filter((e) => fatalPattern.test(e))).toEqual([]);
  expect(logs.consoleErrors.filter((e) => fatalPattern.test(e))).toEqual([]);
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
      const mod = (window as unknown as { Module?: { ccall?: unknown } }).Module;
      return !!mod && typeof mod.ccall === 'function';
    },
    undefined,
    { timeout: 20_000 },
  );
}

async function signalStrength(page: Page): Promise<number | null> {
  return page.evaluate(() => {
    const mod = (window as unknown as {
      Module?: { ccall?: (name: string, returnType: string, argTypes: unknown[], args: unknown[]) => number };
    }).Module;
    if (!mod || typeof mod.ccall !== 'function') return null;
    const value = mod.ccall('get_signal_strength', 'number', [], []);
    return Number.isFinite(value) ? value : null;
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

async function waitForRenderedGame(page: Page, canvas: Locator): Promise<void> {
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
}

async function loadGame(page: Page): Promise<Locator> {
  await page.goto(smokeUrl());
  const canvas = page.locator('canvas');
  await waitForRenderedGame(page, canvas);
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

async function driveCoreControls(page: Page, canvas: Locator): Promise<void> {
  await canvas.click();

  await tap(page, 'Escape');
  await tap(page, 'E');        // launch if docked, interact if already undocked
  await hold(page, 'W', 450);
  await hold(page, 'A', 220);
  await hold(page, 'D', 220);
  await hold(page, 'Shift', 300);
  await tap(page, 'H');        // hail / collect credits
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
  test('boots, renders, and persists browser identity across reload', async ({ page }) => {
    const logs = installFatalCollectors(page);

    await loadGame(page);
    await expect
      .poll(async () => hudHintText(page), { timeout: 5_000 })
      .toContain('Press E to launch.');

    const firstIdentity = await page.evaluate(() => window.localStorage.getItem('signal:identity'));
    expect(firstIdentity).toMatch(/^[A-Za-z0-9+/]{86}==$/);

    await page.locator('canvas').click();
    await tap(page, 'E');
    await expect
      .poll(async () => hudHintText(page), { timeout: 8_000 })
      .toContain('Fly with W A S D.');

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
      .toContain('Press E to launch.');

    await driveCoreControls(page, canvas);
    await expect
      .poll(async () => (await readCanvasStats(canvas)).uniqueBuckets, { timeout: 5_000 })
      .toBeGreaterThan(8);

    expectNoFatalErrors(logs);
  });

  test('narrow viewport renders and accepts dock/trade/build hotkeys', async ({ page }) => {
    const logs = installFatalCollectors(page);
    await page.setViewportSize({ width: 390, height: 760 });
    const canvas = await loadGame(page);

    await canvas.click();
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

    expectNoFatalErrors(logs);
  });
});
