import { test, expect, Page } from '@playwright/test';

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
  await expect.poll(async () => (await status(page)).state).toBe(2);
}

for (const touch of [false, true]) {
  test(`save completion, failure and retry on ${touch ? 'touch' : 'desktop'}`, async ({page}, info) => {
    test.skip(!!process.env.SMOKE_URL, 'storage faults use an isolated local world');
    test.setTimeout(90_000);
    await page.setViewportSize(touch ? {width:390, height:844} : {width:1280, height:720});
    await ready(page, touch);
    const panel = page.locator('#signal-save-status');
    const first = await status(page);
    expect(first.generation).toBeGreaterThan(0);
    expect(first.completedAt).toBeGreaterThan(0);
    await page.evaluate(() => {
      const storage = (window as any).Module.signalLocalPersistence;
      storage.originalFlush = storage.flush;
      storage.flush = (generation: number) => {
        storage.heldGeneration = generation;
        storage.state = 2;
      };
    });
    expect(await call(page, 'signal_save_local_world')).toBe(1);
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
      storage.state = 1;
      storage.originalFlush(storage.heldGeneration);
    });
    await expect.poll(async () => (await status(page)).generation).toBeGreaterThan(first.generation);
    const completed = await status(page);
    expect(completed.completedAt).toBeGreaterThan(first.completedAt + 1000);
    await page.evaluate(() => {
      const storage = (window as any).Module.signalLocalPersistence;
      storage.flush = () => { storage.state = -1; };
    });
    // A real guide action changes persisted player progress.
    await page.locator('canvas').focus();
    await page.keyboard.press('h', {delay:80});
    const progress = await call(page, 'signal_debug_player_progress');
    expect(progress & (1 << 12)).toBe(1 << 12);
    await expect.poll(() => call(page, 'signal_save_local_world')).toBe(1);
    await expect(panel).toContainText('Retry needed');
    const failed = await status(page);
    expect(failed.generation).toBe(completed.generation);
    expect(failed.completedAt).toBe(completed.completedAt);
    const retry = page.getByRole('button', {name:'Retry save', exact:true});
    await expect(retry).toBeVisible();
    expect((await retry.boundingBox())!.height).toBeGreaterThanOrEqual(44);
    await page.screenshot({path:info.outputPath('retry-needed.png')});
    const before = JSON.parse(await call(page, 'signal_debug_local_save_summary', 'string'));
    await page.evaluate(() => {
      const storage = (window as any).Module.signalLocalPersistence;
      storage.flush = storage.originalFlush;
    });
    await retry.click();
    await expect.poll(async () => (await status(page)).generation).toBeGreaterThan(completed.generation);
    await expect(panel).toContainText('Saved');
    await expect(retry).toBeHidden();
    await page.screenshot({path:info.outputPath('saved.png')});
    await page.reload();
    await page.waitForFunction(() => (window as any).SignalGameWasmReady === true);
    await expect(page.locator('#loading')).toHaveClass('done');
    expect(await call(page, 'signal_debug_player_progress')).toBe(progress);
    const restored = JSON.parse(await call(page, 'signal_debug_local_save_summary', 'string'));
    for (const key of ['seed', 'ship', 'credits', 'stations']) expect(restored[key]).toEqual(before[key]);
    expect(restored.time).toBeGreaterThanOrEqual(before.time);
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
  expect(online.text).toMatch(/Connecting to server|Online - server-managed saves/);
  await expect(page.getByRole('button', {name:'Retry save', exact:true})).toBeHidden();
});
