/*
 * Capture real-browser screenshots of the Carta MapLibre viewer.
 *
 * Drives a headless Chromium (Playwright) against the running Vite dev server,
 * which proxies /tiles.vector.json and /tiles/* to the Carta tile server. These
 * are screenshots of the ACTUAL rendered map - not mockups.
 *
 * Prereqs (see README):
 *   - carta-tile-server on :8097 (Monaco PBF)
 *   - vite dev on VIEWER_URL (default http://127.0.0.1:5178)
 *   - public/routes/plan.json generated from Velo (npm run plan)
 *
 * Output: screenshots/*.png
 */
import { chromium } from 'playwright';
import { mkdir } from 'node:fs/promises';

const URL = process.env.VIEWER_URL || 'http://127.0.0.1:5178';
const OUT = 'screenshots';

async function settle(page) {
  await page.waitForFunction(() => (window).__carta_ready === true, { timeout: 30000 });
  // Give the tile network + label layout a moment beyond first idle.
  await page.waitForTimeout(400);
}
async function view(page, center, zoom) {
  await page.evaluate(([c, z]) => {
    const m = (window).__carta_map;
    return new Promise((resolve) => { m.once('idle', resolve); m.jumpTo({ center: c, zoom: z }); });
  }, [center, zoom]);
  await page.waitForTimeout(300);
}

const main = async () => {
  await mkdir(OUT, { recursive: true });
  const launch = { headless: true,
    args: ['--use-gl=angle', '--use-angle=swiftshader', '--ignore-gpu-blocklist', '--enable-unsafe-swiftshader'] };
  // Use a browser already on disk when provided (avoids a network download).
  if (process.env.PW_CHROMIUM) launch.executablePath = process.env.PW_CHROMIUM;
  const browser = await chromium.launch(launch);
  const page = await browser.newPage({ viewport: { width: 1280, height: 860 }, deviceScaleFactor: 2 });
  page.on('console', (m) => { if (m.type() === 'error') console.log('  [page error]', m.text()); });

  await page.goto(URL, { waitUntil: 'networkidle' });
  await settle(page);

  // 1. Regional: the viewer's default fit over all demo routes (Monaco).
  await page.screenshot({ path: `${OUT}/01-regional.png` });
  console.log('wrote 01-regional.png');

  // 2. Wide/"country" context: zoom out to show the coastline extent.
  await view(page, [7.42, 43.70], 9);
  await page.screenshot({ path: `${OUT}/02-wide.png` });
  console.log('wrote 02-wide.png');

  // 3. Street level: Monte-Carlo, roads + accented labels legible.
  await view(page, [7.4280, 43.7400], 16);
  await page.screenshot({ path: `${OUT}/03-street.png` });
  console.log('wrote 03-street.png');

  // 4. Route selection: click the first route; map highlights + fits + details.
  await view(page, [7.42, 43.735], 13);
  await page.click('.route-item');
  await page.waitForFunction(() => (window).__carta_map && !(window).__carta_map.isMoving(), { timeout: 15000 });
  await page.waitForTimeout(500);
  await page.screenshot({ path: `${OUT}/04-route-selected.png` });
  console.log('wrote 04-route-selected.png');

  await browser.close();
};
main().catch((e) => { console.error(e); process.exit(1); });
