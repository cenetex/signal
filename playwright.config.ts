import { defineConfig } from '@playwright/test';

const liveUrl = process.env.SMOKE_URL;
const port = Number(process.env.SMOKE_PORT || 3000);

export default defineConfig({
  testDir: './tests',
  timeout: 45_000,
  use: {
    headless: true,
    baseURL: liveUrl || `http://127.0.0.1:${port}`,
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
  },
  projects: [{ name: 'chromium', use: { browserName: 'chromium' } }],
  // Only start a local static server if not testing the deployed URL.
  // `make smoke` builds build-web first; the smoke spec adds
  // ?singleplayer=1 so local runs do not require docker/ws://:9091.
  ...(liveUrl
    ? {}
    : {
        webServer: {
          command: `python3 -m http.server ${port} --directory build-web`,
          port,
          reuseExistingServer: !process.env.CI,
        },
      }),
});
