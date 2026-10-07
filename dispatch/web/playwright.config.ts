import { defineConfig } from '@playwright/test';

/*
 * UI tests run against an already-running dispatch app (Vite dev on :5179 by
 * default, or the backend-served SPA via VIEWER_URL). The backend (:8091),
 * Carta (:8097) and Velo (:8082) must be up - the tests exercise real solves
 * and geometry. Software WebGL (SwiftShader) renders the MapLibre map headless.
 *
 *   npm run test:ui                       # against http://localhost:5179
 *   VIEWER_URL=http://localhost:8091 npm run test:ui
 */
export default defineConfig({
  testDir: './tests',
  timeout: 90_000,
  expect: { timeout: 20_000 },
  fullyParallel: false,
  workers: 1,
  retries: 0,
  reporter: [['list']],
  use: {
    baseURL: process.env.VIEWER_URL || 'http://localhost:5179',
    viewport: { width: 1440, height: 900 },
    actionTimeout: 20_000,
    launchOptions: {
      ...(process.env.PW_CHROMIUM ? { executablePath: process.env.PW_CHROMIUM } : {}),
      args: ['--use-gl=angle', '--use-angle=swiftshader', '--ignore-gpu-blocklist', '--enable-unsafe-swiftshader'],
    },
  },
});
