import { defineConfig } from '@playwright/test';

const liveUrl = process.env.SMOKE_URL;

export default defineConfig({
  testDir: './tests',
  timeout: 30_000,
  use: {
    headless: true,
    baseURL: liveUrl || 'http://localhost:3000',
  },
  projects: [{ name: 'chromium', use: { browserName: 'chromium' } }],
  // Only start local server if not testing live URL
  ...(liveUrl
    ? {}
    : {
        webServer: {
          command: 'npx serve build-web -l 3000 --no-clipboard',
          port: 3000,
          reuseExistingServer: !process.env.CI,
        },
      }),
});
