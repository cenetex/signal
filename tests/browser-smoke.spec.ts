import { test, expect } from '@playwright/test';

test.describe('Browser smoke tests', () => {
  test('WASM module loads and canvas appears', async ({ page }) => {
    const errors: string[] = [];
    page.on('pageerror', (err) => errors.push(err.message));

    await page.goto('/space_miner.html');

    // Wait for the Emscripten canvas to appear
    const canvas = page.locator('canvas');
    await expect(canvas).toBeVisible({ timeout: 15_000 });

    // Canvas should have real dimensions
    const box = await canvas.boundingBox();
    expect(box).toBeTruthy();
    expect(box!.width).toBeGreaterThan(100);
    expect(box!.height).toBeGreaterThan(100);

    // No fatal JS errors during startup
    expect(errors.filter((e) => /abort|unreachable|RuntimeError/i.test(e))).toHaveLength(0);
  });

  test('keyboard input does not crash', async ({ page }) => {
    const errors: string[] = [];
    page.on('pageerror', (err) => errors.push(err.message));

    await page.goto('/space_miner.html');
    const canvas = page.locator('canvas');
    await expect(canvas).toBeVisible({ timeout: 15_000 });

    // Focus the canvas and send key presses
    await canvas.click();
    await page.keyboard.press('w');
    await page.keyboard.press('a');
    await page.keyboard.press('Space');
    await page.keyboard.press('Escape');

    // Wait a beat for any deferred crashes
    await page.waitForTimeout(500);

    expect(errors.filter((e) => /abort|unreachable|RuntimeError/i.test(e))).toHaveLength(0);
  });

  test('focus loss does not crash', async ({ page }) => {
    const errors: string[] = [];
    page.on('pageerror', (err) => errors.push(err.message));

    await page.goto('/space_miner.html');
    const canvas = page.locator('canvas');
    await expect(canvas).toBeVisible({ timeout: 15_000 });

    // Focus then blur
    await canvas.click();
    await page.keyboard.press('w');
    await page.evaluate(() => document.activeElement instanceof HTMLElement && document.activeElement.blur());
    await page.waitForTimeout(300);

    // Re-focus
    await canvas.click();
    await page.keyboard.press('w');
    await page.waitForTimeout(300);

    expect(errors.filter((e) => /abort|unreachable|RuntimeError/i.test(e))).toHaveLength(0);
  });
});
