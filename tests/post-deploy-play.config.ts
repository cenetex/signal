import { defineConfig } from '@playwright/test';
import { resolve } from 'node:path';

// A dedicated command keeps live actions separate from the local smoke suite.
export default defineConfig({
  testDir: '.',
  testMatch: 'post-deploy-play.live.ts',
  timeout: 90_000,
  workers: 1,
  retries: 0,
  outputDir: resolve(__dirname, '../test-results/post-deploy-play'),
  reporter: [['list'], ['html', { outputFolder: resolve(__dirname, '../playwright-report/post-deploy-play'), open: 'never' }]],
  use: {
    browserName: 'chromium',
    headless: true,
    trace: 'off', // The test starts a trace after private identity setup.
    screenshot: 'off',
    storageState: { cookies: [], origins: [] },
  },
  projects: [
    { name: 'desktop', use: { viewport: { width: 1280, height: 720 } } },
    { name: 'touch', use: { viewport: { width: 390, height: 760 }, hasTouch: true, isMobile: true } },
  ],
});
