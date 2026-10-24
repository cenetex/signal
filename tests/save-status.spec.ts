import { test, expect, Page } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';

async function call(page: Page, name: string, type = 'number') {
  return page.evaluate(({name, type}) => (window as any).Module.ccall(name, type, [], []), {name, type});
}
async function status(page: Page) {
  return JSON.parse(await call(page, 'signal_save_status_json', 'string'));
}
async function ready(page: Page, touch: boolean) {
  await page.goto(`/play.html?singleplayer=1&smoke=1${touch ? '&touch=1' : ''}`);
  await page.waitForFunction(() => (window as any).SignalGameWasmReady === true);
  await expect(page.locator('#loading')).toHaveClass('done');
  await expect.poll(async () => (await status(page)).completedAt, {timeout:30_000}).toBeGreaterThan(0);
}

async function expectTouchClearance(page: Page) {
  const saveBox = (await page.locator('#signal-save-status').boundingBox())!;
  for (const control of await page.locator('.signal-touch-button:visible').all()) {
    const box = (await control.boundingBox())!;
    expect(saveBox.x + saveBox.width <= box.x || box.x + box.width <= saveBox.x ||
      saveBox.y + saveBox.height <= box.y || box.y + box.height <= saveBox.y,
      `${await control.textContent()} stays clear of save status`).toBe(true);
  }
}

for (const view of [
  {name:'desktop', touch:false, coarse:false, width:1280, height:720},
  {name:'touch', touch:true, coarse:false, width:390, height:844},
  {name:'landscape touch', touch:true, coarse:false, width:844, height:390},
  {name:'coarse landscape touch', touch:true, coarse:true, width:932, height:430},
]) {
  const {touch} = view;
  test.describe(view.name, () => {
    test.use({hasTouch:view.coarse});
  test(`save completion, failure and retry on ${view.name}`, async ({page}, info) => {
    test.skip(!!process.env.SMOKE_URL, 'storage faults use an isolated local world');
    test.setTimeout(150_000);
    await page.setViewportSize({width:view.width, height:view.height});
    await ready(page, touch);
    expect(await page.evaluate(() => matchMedia('(pointer:coarse)').matches)).toBe(view.coarse);
    const panel = page.locator('#signal-save-status');
    // Hold the next real checkpoint, including one requested by autosave.
    // Capture its preceding receipt in the same callback to avoid idle-window races.
    const first = await page.evaluate(() => new Promise<{generation:number; completedAt:number; age:number}>((resolve, reject) => {
      const mod = (window as any).Module;
      const storage = mod.signalLocalPersistence;
      storage.originalFlush = storage.flush;
      const timeout = setTimeout(() => {
        storage.flush = storage.originalFlush;
        reject(new Error('Next checkpoint did not reach storage'));
      }, 30_000);
      storage.flush = (generation: number) => {
        storage.heldGeneration = generation;
        storage.state = 2;
        clearTimeout(timeout);
        resolve({generation:storage.completedGeneration, completedAt:storage.completedAt,
          age:(Date.now() - storage.completedAt) / 1000});
      };
      mod.ccall('signal_save_local_world', 'number', [], []);
    }));
    expect(first.generation).toBeGreaterThan(0);
    expect(first.completedAt).toBeGreaterThan(0);
    await expect(panel).toContainText('Saving...');
    await page.waitForTimeout(1200);
    const delayed = await status(page);
    expect(delayed.state).toBe(1);
    expect(delayed.generation).toBe(first.generation);
    expect(delayed.completedAt).toBe(first.completedAt);
    expect(delayed.age).toBeGreaterThanOrEqual(first.age + 1);
    await page.screenshot({path:info.outputPath('saving.png')});
    await page.evaluate(() => {
      const storage = (window as any).Module.signalLocalPersistence;
      storage.flush = storage.originalFlush;
      storage.state = 1;
      storage.flush(storage.heldGeneration);
    });
    await expect.poll(async () => (await status(page)).generation, {timeout:30_000}).toBeGreaterThan(first.generation);
    const completed = await status(page);
    expect(completed.completedAt).toBeGreaterThan(first.completedAt + 1000);
    await page.evaluate(() => {
      const storage = (window as any).Module.signalLocalPersistence;
      storage.flush = () => {
        storage.failedPreviousGeneration = storage.completedGeneration;
        storage.failedPreviousAt = storage.completedAt;
        storage.state = -1;
      };
    });
    // A real guide action changes persisted player progress.
    await page.locator('canvas').focus();
    await page.keyboard.press('h', {delay:80});
    const progress = await call(page, 'signal_debug_player_progress');
    expect(progress & (1 << 12)).toBe(1 << 12);
    await expect.poll(() => call(page, 'signal_save_local_world'), {timeout:30_000}).toBe(1);
    await expect(panel).toContainText('Retry needed');
    const failed = await status(page);
    const priorReceipt = await page.evaluate(() => {
      const storage = (window as any).Module.signalLocalPersistence;
      return {generation:storage.failedPreviousGeneration, completedAt:storage.failedPreviousAt};
    });
    expect(priorReceipt.generation).toBeGreaterThanOrEqual(completed.generation);
    expect(failed.generation).toBe(priorReceipt.generation);
    expect(failed.completedAt).toBe(priorReceipt.completedAt);
    const retry = page.getByRole('button', {name:'Retry save', exact:true});
    await expect(retry).toBeVisible();
    expect((await retry.boundingBox())!.height).toBeGreaterThanOrEqual(44);
    await page.screenshot({path:info.outputPath('retry-needed.png')});
    if (touch) await expectTouchClearance(page);
    const before = JSON.parse(await call(page, 'signal_debug_local_save_summary', 'string'));
    // Keep automatic retries failing until the real button click starts the retry.
    await retry.evaluate(button => button.addEventListener('click', () => {
      const storage = (window as any).Module.signalLocalPersistence;
      storage.flush = storage.originalFlush;
      storage.retryClicks = (storage.retryClicks || 0) + 1;
    }, {capture:true, once:true}));
    await retry.click();
    expect(await page.evaluate(() => (window as any).Module.signalLocalPersistence.retryClicks)).toBe(1);
    await expect.poll(async () => (await status(page)).generation, {timeout:30_000}).toBeGreaterThan(failed.generation);
    // The completed retry remains durable while a later autosave is active.
    await expect(panel).toHaveAttribute('data-state', /^[12]$/);
    await expect(panel.getByRole('status')).toHaveText(/^(Saved |Saving\.\.\. - last saved )/);
    await expect(retry).toBeHidden();
    await page.screenshot({path:info.outputPath('saved.png')});
    await page.reload();
    await page.waitForFunction(() => (window as any).SignalGameWasmReady === true);
    await expect(page.locator('#loading')).toHaveClass('done');
    expect(await call(page, 'signal_debug_player_progress')).toBe(progress);
    const restored = JSON.parse(await call(page, 'signal_debug_local_save_summary', 'string'));
    for (const key of ['seed', 'ship', 'credits', 'stations']) expect(restored[key]).toEqual(before[key]);
    expect(restored.time).toBeGreaterThanOrEqual(before.time);
    // A completed receipt remains valid when the next autosave is already active.
    await expect.poll(async () => (await status(page)).completedAt, {timeout:30_000}).toBeGreaterThan(0);
    if (touch) await page.getByRole('button', {name:'Launch', exact:true}).click();
    else {
      await page.locator('canvas').focus();
      await page.keyboard.press('e', {delay:80});
    }
    await expect.poll(() => call(page, 'get_player_docked')).toBe(0);
    await page.screenshot({path:info.outputPath('flight-saved.png')});
    await page.evaluate(() => {
      const storage = (window as any).Module.signalLocalPersistence;
      storage.flush = () => { storage.state = -1; };
    });
    await expect.poll(() => call(page, 'signal_save_local_world'), {timeout:30_000}).toBe(1);
    await expect(panel).toContainText('Retry needed');
    await page.screenshot({path:info.outputPath('flight-retry-needed.png')});
    if (touch) await expectTouchClearance(page);
  });
  });
}

test('online status follows server authority', async ({page}) => {
  test.skip(!!process.env.SMOKE_URL, 'connection fixture uses local browser assets');
  const server = 'ws://signal-save-status.invalid/ws';
  await page.routeWebSocket(server, ws => { ws.onMessage(() => {}); });
  await page.goto(`/play.html?smoke=1&server=${encodeURIComponent(server)}`);
  await page.waitForFunction(() => (window as any).SignalGameWasmReady === true);
  await expect(page.locator('#signal-save-status')).toHaveAttribute('data-mode', 'online');
  const online = await status(page);
  expect(online.generation).toBe(0);
  expect(online.completedAt).toBe(0);
  expect(online.retry).toBe(false);
  expect(online.text).toBe('Connecting to server...');
  await expect(page.getByRole('button', {name:'Retry save', exact:true})).toBeHidden();
});


test('storage receipts follow completed transactions and retain prior success on error', () => {
  const callbacks: ((error?: Error) => void)[] = [];
  let clock = 1000;
  let throwOnWrite = false;
  const module: any = {};
  runInNewContext(readFileSync('web/local-persistence.js', 'utf8'), {
    Module: module, URLSearchParams,
    Date: {now: () => clock}, console: {warn() {}, error() {}},
    window: {location: {search:'?singleplayer=1'}, addEventListener() {}},
    document: {addEventListener() {}},
    navigator: {locks: {request: (_name: string, _options: unknown, fn: Function) => {
      fn({}); return Promise.resolve();
    }}},
    addRunDependency() {}, removeRunDependency() {}, IDBFS: {},
    FS: {mkdir() {}, mount() {}, syncfs(read: boolean, callback: (error?: Error) => void) {
      if (read) callback();
      else if (throwOnWrite) throw new Error('write setup failed');
      else callbacks.push(callback);
    }},
  });
  module.preRun[0]();
  const storage = module.signalLocalPersistence;
  storage.flush(7);
  expect(storage.state).toBe(2);
  expect(storage.completedGeneration).toBe(0);
  expect(storage.completedAt).toBe(0);
  clock = 9000;
  callbacks[0]();
  expect(storage.completedGeneration).toBe(7);
  expect(storage.completedAt).toBe(9000);
  storage.flush(8);
  callbacks[1](new Error('transaction failed'));
  expect(storage.state).toBe(-1);
  expect(storage.completedGeneration).toBe(7);
  expect(storage.completedAt).toBe(9000);
  throwOnWrite = true;
  storage.flush(9);
  expect(storage.state).toBe(-1);
  expect(storage.completedGeneration).toBe(7);
  throwOnWrite = false;
  storage.flush(10);
  callbacks[0](); // An old callback cannot complete the retry.
  expect(storage.state).toBe(2);
  clock = 14000;
  callbacks[2]();
  expect(storage.state).toBe(1);
  expect(storage.completedGeneration).toBe(10);
  expect(storage.completedAt).toBe(14000);
});
