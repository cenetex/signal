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
});

test('guide focus survives a normal local save and reload', async ({page}) => {
  await ready(page);
  await page.keyboard.press('g');
  await expect.poll(() => call(page, 'signal_guide_story_focus')).toBe(1);
  await expect.poll(() => call(page, 'signal_save_local_world')).toBe(1);
  await expect.poll(() => call(page, 'signal_debug_local_save_generation')).toBeGreaterThan(0);
  await page.reload();
  await page.waitForFunction(() => (window as any).SignalGameWasmReady === true);
  await expect.poll(() => call(page, 'signal_guide_story_focus')).toBe(1);
  await page.keyboard.press('g');
  await expect.poll(() => call(page, 'signal_guide_story_focus')).toBe(0);
});
