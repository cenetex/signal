import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: './tests',
  timeout: 30_000,
  use: {
    headless: true,
    baseURL: 'http://localhost:3000',
  },
  projects: [{ name: 'chromium', use: { browserName: 'chromium' } }],
  webServer: {
    command: 'npx serve build-web -l 3000 --no-clipboard',
    port: 3000,
    reuseExistingServer: !process.env.CI,
  },
});
