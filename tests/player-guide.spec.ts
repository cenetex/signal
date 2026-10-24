import { test, expect, Page } from '@playwright/test';

async function call(page: Page, name: string, result = 'number', values: number[] = []) {
  return page.evaluate(({ name, result, values }) => {
    const mod = (window as any).Module;
    return mod.ccall(name, result, values.map(() => 'number'), values);
  }, { name, result, values });
}
async function ready(page: Page, touch = false) {
  await page.goto(`/play.html?singleplayer=1&smoke=1${touch ? '&touch=1' : ''}`);
  await page.waitForFunction(() => (window as any).SignalGameWasmReady === true);
  await expect(page.locator('#loading')).toHaveClass('done');
  await page.locator('canvas').focus();
}
async function fixture(page: Page, state: number) {
  expect(await call(page, 'set_smoke_loop_state', 'number', [state])).toBe(1);
}
async function hint(page: Page) { return call(page, 'get_hud_hint_text', 'string'); }

// Observe the IDBFS completion assignment in the page. A later autosave can
// return storage to busy before a cross-process generation poll sees ready.
async function checkpointInPage(): Promise<{ before: number; saved: number }> {
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


for (const viewport of [{width: 1280, height: 720}, {width: 390, height: 844}]) {
  test(`chosen work and story guide at ${viewport.width}px`, async ({ page }, info) => {
    await page.setViewportSize(viewport);
    await ready(page, viewport.width < 500);
    await fixture(page, 43);
    await fixture(page, -1);
    expect(await hint(page)).toContain('requires L2 laser');
    expect(await hint(page)).toContain(viewport.width < 500 ? '[Story guide]' : '[G] Story');
    if (viewport.width < 500) await page.getByRole('button', { name: 'Story guide', exact: true }).click();
    else await page.keyboard.press('g');
    await expect.poll(() => call(page, 'signal_guide_story_focus')).toBe(1);
    await expect.poll(() => hint(page), {timeout: 8000}).toContain('STORY // THE CALL');
    await page.screenshot({path: info.outputPath(`story-${viewport.width}.png`)});
    // Selecting a station job is the explicit choice to follow Work again.
    await page.keyboard.press('1');
    await expect.poll(() => call(page, 'signal_guide_story_focus')).toBe(0);
    await expect.poll(() => hint(page), {timeout: 8000}).toContain('requires L2 laser');
    await page.screenshot({path: info.outputPath(`work-${viewport.width}.png`)});
    await page.keyboard.press('g');
    await expect.poll(() => call(page, 'signal_guide_story_focus')).toBe(1);
    await fixture(page, 45);
    expect(await call(page, 'signal_station_credit_perception_summary', 'string')).toContain('Prospect: buy > haul');
    await page.screenshot({path: info.outputPath(`credit-guide-${viewport.width}.png`)});
  });
}

test('touch guide follows toggle controls and switches to keyboard prompts', async ({page}, info) => {
  await page.setViewportSize({width: 390, height: 844});
  await ready(page, true);
  await expect.poll(() => hint(page)).toContain('[Launch]');
  await fixture(page, 44);
  expect(await hint(page)).toContain('TAP [Tow]');
  await page.screenshot({path: info.outputPath('touch-tow.png')});
  await page.keyboard.press('ArrowLeft');
  expect(await hint(page)).toContain('HOLD [SPACE]');
  await page.getByRole('button', {name: 'Tow', exact: true}).click();
  await expect(page.getByRole('button', {name: 'Tow On', exact: true})).toBeVisible();
  expect(await hint(page)).toContain('TAP [Tow]');
  await page.getByRole('button', {name: 'Tow On', exact: true}).click();
  await fixture(page, 7);
  expect(await hint(page)).toContain('[Lock]');
  expect(await hint(page)).toContain('[Exit]');
  await page.setViewportSize({width: 1280, height: 720});
  await expect.poll(() => hint(page)).toContain('[E]');
  await page.setViewportSize({width: 390, height: 844});
  await expect.poll(() => hint(page)).toContain('[Lock]');
});

test('guide focus survives a normal local save and reload', async ({page}) => {
  test.setTimeout(90_000);
  await ready(page);
  await page.keyboard.press('g');
  await expect.poll(() => call(page, 'signal_guide_story_focus')).toBe(1);
  await expect.poll(() => call(page, 'signal_debug_auth_available'), {timeout: 20_000}).toBe(1);
  const checkpoint = await page.evaluate(checkpointInPage);
  expect(checkpoint.saved).toBeGreaterThan(checkpoint.before);
  await page.reload();
  await page.waitForFunction(() => (window as any).SignalGameWasmReady === true);
  await expect.poll(() => call(page, 'signal_guide_story_focus')).toBe(1);
  await page.keyboard.press('g');
  await expect.poll(() => call(page, 'signal_guide_story_focus')).toBe(0);
});

// Reproduce the ready window closing before the next external browser poll.
for (const fail of [false, true]) {
  test(`guide checkpoint completion ${fail ? 'reports storage failure' : 'survives an immediate next autosave'}`, async ({ page }) => {
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

for (const touch of [false, true]) {
  test(`first haul keeps a usable target and early-return step on ${touch ? 'touch' : 'desktop'}`, async ({page}, info) => {
    await page.setViewportSize(touch ? {width: 390, height: 844} : {width: 1280, height: 720});
    await ready(page, touch);
    expect(await hint(page)).toContain('LAUNCH FROM DOCK');
    expect(await hint(page)).toContain('TOW ORE TO A FURNACE');
    await page.screenshot({path: info.outputPath('first-haul-dock.png')});
    for (const state of [46, 47]) {
      await fixture(page, state);
      await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
      expect(await hint(page)).toContain('RED COMPASS PIP: Ferrite M (FE)');
      expect(await hint(page)).toContain(touch ? 'HOLD [Mine]' : 'HOLD [M]');
    }
    await page.screenshot({path: info.outputPath('first-haul-target.png')});
    if (touch) {
      await page.setViewportSize({width: 844, height: 390});
      await page.screenshot({path: info.outputPath('first-haul-landscape.png')});
      await page.setViewportSize({width: 390, height: 844});
    }
    await fixture(page, 49);
    expect(await hint(page)).toContain('ROCK TARGETED');
    await fixture(page, 48);
    expect(await hint(page)).toContain('LAUNCH FROM DOCK');
    expect(await hint(page)).toContain('FOLLOW THE RED COMPASS PIP');
    expect(await hint(page)).toContain(touch ? '[Launch]' : '[E]');
    await page.screenshot({path: info.outputPath('first-haul-return.png')});
    await fixture(page, 44);
    expect(await hint(page)).toContain(touch ? 'TAP [Tow]' : 'HOLD [SPACE]');
    await fixture(page, 20);
    expect(await hint(page)).toContain('GLOWING FURNACE');
  });
}

test('first scan keeps the first-haul action visible', async ({page}) => {
  await ready(page);
  await page.keyboard.press('e');
  await page.keyboard.down('w');
  await page.waitForTimeout(1200);
  await page.keyboard.up('w');
  await page.keyboard.press('h');
  await expect.poll(() => hint(page), {timeout: 2000}).toMatch(/RED COMPASS PIP|ROCK TARGETED/);
});
