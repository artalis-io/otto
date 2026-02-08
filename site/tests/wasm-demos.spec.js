/**
 * WASM API Demo Tests
 *
 * Validates that all WASM demos in api.html work correctly and return
 * expected responses matching the C API backend annotations.
 *
 * Run: npx playwright test tests/wasm-demos.spec.js
 */

const { test, expect } = require('@playwright/test');
const path = require('path');

const API_HTML_PATH = 'file://' + path.resolve(__dirname, '../api.html');

// Timeout for WASM module initialization
const WASM_INIT_TIMEOUT = 30000;

test.describe('WASM API Demos', () => {

  test.describe('Carta (Map Tiles)', () => {
    test.beforeEach(async ({ page }) => {
      await page.goto(API_HTML_PATH);
      await page.waitForFunction(() =>
        typeof cartaDemo !== 'undefined' && cartaDemo.isReady(),
        { timeout: WASM_INIT_TIMEOUT }
      );
    });

    test('health endpoint returns healthy status', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await cartaDemo.fetch('/api/v1/health');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      expect(result.body.status).toBe('healthy');
      expect(result.body.service).toContain('carta');
      expect(result.body.version).toMatch(/^\d+\.\d+\.\d+$/);
    });

    test('stats endpoint returns graph info', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await cartaDemo.fetch('/api/v1/stats');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      // Check for any stats fields (structure may vary)
      expect(Object.keys(result.body).length).toBeGreaterThan(0);
    });

    test('PNG tile generation returns valid image', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await cartaDemo.fetch('/tiles/14/8529/5974.png');
        const blob = await resp.blob();
        return {
          status: resp.status,
          contentType: resp.headers.get('content-type'),
          size: blob.size
        };
      });

      expect(result.status).toBe(200);
      expect(result.contentType).toBe('image/png');
      expect(result.size).toBeGreaterThan(100); // Non-empty PNG
    });

    test('MVT tile generation returns valid protobuf', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await cartaDemo.fetch('/tiles/14/8529/5974.mvt');
        const buffer = await resp.arrayBuffer();
        return {
          status: resp.status,
          contentType: resp.headers.get('content-type'),
          size: buffer.byteLength
        };
      });

      expect(result.status).toBe(200);
      // MVT can be application/x-protobuf or application/vnd.mapbox-vector-tile
      expect(result.contentType).toMatch(/protobuf|vector-tile/);
      expect(result.size).toBeGreaterThan(0);
    });

    test('ASCII tile generation returns text', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await cartaDemo.fetch('/tiles/14/8529/5974.txt');
        const text = await resp.text();
        return {
          status: resp.status,
          contentType: resp.headers.get('content-type'),
          length: text.length,
          hasContent: text.trim().length > 0
        };
      });

      expect(result.status).toBe(200);
      expect(result.contentType).toMatch(/text\/plain/);
      expect(result.hasContent).toBe(true);
    });

    test('TileJSON returns valid metadata', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await cartaDemo.fetch('/tiles.json');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      expect(result.body.tilejson).toBe('3.0.0');
      expect(result.body.tiles).toBeDefined();
      expect(Array.isArray(result.body.tiles)).toBe(true);
      expect(result.body.minzoom).toBeDefined();
      expect(result.body.maxzoom).toBeDefined();
    });
  });

  test.describe('Velo (Routing)', () => {
    test.beforeEach(async ({ page }) => {
      await page.goto(API_HTML_PATH);
      await page.waitForFunction(() =>
        typeof veloDemo !== 'undefined' && veloDemo.isReady(),
        { timeout: WASM_INIT_TIMEOUT }
      );
    });

    test('health endpoint returns healthy status', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await veloDemo.fetch('/api/v1/health');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      expect(result.body.status).toBe('healthy');
      expect(result.body.version).toMatch(/^\d+\.\d+\.\d+$/);
    });

    test('stats endpoint returns graph info', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await veloDemo.fetch('/api/v1/stats');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      expect(result.body.num_nodes).toBeGreaterThan(0);
      expect(result.body.num_edges).toBeGreaterThan(0);
    });

    test('route calculation returns valid route', async ({ page }) => {
      const result = await page.evaluate(async () => {
        return await veloDemo.route(
          { lat: 43.7384, lon: 7.4246 },  // Monaco Casino
          { lat: 43.7311, lon: 7.4197 }   // Port
        );
      });

      expect(result.status).toBe('ok');
      expect(result.route.distance).toBeGreaterThan(0);
      expect(result.route.duration).toBeGreaterThan(0);
      expect(result.route.profile).toBe('car');
      expect(result.route.mode).toBe('fastest');
    });

    test('route with geometry returns coordinates', async ({ page }) => {
      const result = await page.evaluate(async () => {
        return await veloDemo.route(
          { lat: 43.7384, lon: 7.4246 },
          { lat: 43.7311, lon: 7.4197 },
          { geometry: true }
        );
      });

      expect(result.status).toBe('ok');
      expect(result.route.geometry).toBeDefined();
      expect(Array.isArray(result.route.geometry)).toBe(true);
      expect(result.route.geometry.length).toBeGreaterThan(0);
    });

    test('route without geometry omits coordinates', async ({ page }) => {
      const result = await page.evaluate(async () => {
        return await veloDemo.route(
          { lat: 43.7384, lon: 7.4246 },
          { lat: 43.7311, lon: 7.4197 },
          { geometry: false }
        );
      });

      expect(result.status).toBe('ok');
      expect(result.route.geometry).toBeUndefined();
    });

    test('route accepts profile parameter', async ({ page }) => {
      // Test that profile parameter is accepted and returned
      const result = await page.evaluate(async () => {
        return await veloDemo.route(
          { lat: 43.7384, lon: 7.4246 },
          { lat: 43.7311, lon: 7.4197 },
          { profile: 'car' }
        );
      });

      expect(result.status).toBe('ok');
      expect(result.route.profile).toBe('car');
    });

    test('route with shortest mode', async ({ page }) => {
      const result = await page.evaluate(async () => {
        return await veloDemo.route(
          { lat: 43.7384, lon: 7.4246 },
          { lat: 43.7311, lon: 7.4197 },
          { mode: 'shortest' }
        );
      });

      expect(result.status).toBe('ok');
      expect(result.route.mode).toBe('shortest');
    });
  });

  test.describe('Locus (Geocoding)', () => {
    test.beforeEach(async ({ page }) => {
      await page.goto(API_HTML_PATH);
      await page.waitForFunction(() =>
        typeof locusDemo !== 'undefined' && locusDemo.isReady(),
        { timeout: WASM_INIT_TIMEOUT }
      );
    });

    test('health endpoint returns healthy status', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await locusDemo.fetch('/api/v1/health');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      expect(result.body.status).toBe('healthy');
      expect(result.body.version).toMatch(/^\d+\.\d+\.\d+$/);
    });

    test('stats endpoint returns index info', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await locusDemo.fetch('/api/v1/stats');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      // Check for any stats fields (structure may vary)
      expect(Object.keys(result.body).length).toBeGreaterThan(0);
    });

    test('search returns results for Monaco query', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await locusDemo.fetch('/api/v1/search?q=casino');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      expect(result.body.results).toBeDefined();
      expect(Array.isArray(result.body.results)).toBe(true);
    });

    test('autocomplete returns valid response', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await locusDemo.fetch('/api/v1/autocomplete?q=mon');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      expect(result.body).toBeDefined();
      expect(Object.keys(result.body).length).toBeGreaterThan(0);
    });

    test('reverse geocoding returns valid response', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await locusDemo.fetch('/api/v1/reverse?lat=43.7384&lon=7.4246');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      expect(result.body).toBeDefined();
    });
  });

  test.describe('FuelWise (Fuel Optimization)', () => {
    test.beforeEach(async ({ page }) => {
      await page.goto(API_HTML_PATH);
      await page.waitForFunction(() =>
        typeof fuelwiseDemo !== 'undefined' && fuelwiseDemo.isReady(),
        { timeout: WASM_INIT_TIMEOUT }
      );
    });

    test('health endpoint returns healthy status', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await fuelwiseDemo.fetch('/api/v1/health');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      expect(result.body.status).toBe('healthy');
      expect(result.body.version).toMatch(/^\d+\.\d+\.\d+$/);
    });

    test('stats endpoint returns solver info', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await fuelwiseDemo.fetch('/api/v1/stats');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
    });

    test('optimize endpoint returns response', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await fuelwiseDemo.fetch('/api/v1/optimize?' + new URLSearchParams({
          tank_capacity: '500',
          current_fuel: '100',
          min_arrival_fuel: '50',
          mpg: '6.5'
        }));
        return { status: resp.status, body: await resp.json() };
      });

      // May return 400 if stations not provided, but should return valid JSON
      expect([200, 400]).toContain(result.status);
      expect(result.body).toBeDefined();
    });

    test('solve endpoint returns optimization result', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const problem = {
          total_distance: 500,
          tank_capacity: 200,
          current_fuel: 50,
          consumption_mpg: 6.5,
          minimum_fuel: 25,
          stations: [
            { id: 1, distance: 100, price: 3.50 },
            { id: 2, distance: 250, price: 3.25 },
            { id: 3, distance: 400, price: 3.75 }
          ]
        };
        const resp = await fuelwiseDemo.fetch('/api/v1/solve', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(problem)
        });
        return { status: resp.status, body: await resp.json() };
      });

      // Should return 200 with solution or 400/422 if problem is infeasible
      expect([200, 400, 422]).toContain(result.status);
      expect(result.body).toBeDefined();
      if (result.status === 200) {
        expect(result.body.status).toBe('optimal');
        expect(result.body.total_cost).toBeDefined();
      }
    });
  });

  test.describe('Ralph (LP/MIP Solver)', () => {
    test.beforeEach(async ({ page }) => {
      await page.goto(API_HTML_PATH);
      await page.waitForFunction(() =>
        typeof ralphDemo !== 'undefined' && ralphDemo.isReady(),
        { timeout: WASM_INIT_TIMEOUT }
      );
    });

    test('health endpoint returns healthy status', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await ralphDemo.fetch('/api/v1/health');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      expect(result.body.status).toBe('ok');
      expect(result.body.version).toMatch(/^\d+\.\d+\.\d+$/);
    });

    test('formats endpoint returns supported formats', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const resp = await ralphDemo.fetch('/api/v1/formats');
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      expect(result.body.formats).toBeDefined();
      expect(Array.isArray(result.body.formats)).toBe(true);
      expect(result.body.formats.length).toBeGreaterThan(0);
    });

    test('solve endpoint solves LP problem', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const problem = {
          format: 'lp',
          problem: `max: 5 x + 3 y
subject to
wood: 2 x + 4 y <= 40
labor: 3 x + 2 y <= 24
bounds
x >= 0
y >= 0
end`
        };
        const resp = await ralphDemo.fetch('/api/v1/solve', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(problem)
        });
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(200);
      expect(result.body.status).toBe('optimal');
      expect(result.body.objective).toBeCloseTo(40, 1);
      expect(result.body.variables).toBeDefined();
      expect(result.body.variables.x).toBeCloseTo(8, 1);
      expect(result.body.variables.y).toBeCloseTo(0, 1);
    });

    test('solve endpoint handles invalid LP syntax', async ({ page }) => {
      const result = await page.evaluate(async () => {
        const problem = {
          format: 'lp',
          problem: 'this is not valid LP syntax'
        };
        const resp = await ralphDemo.fetch('/api/v1/solve', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(problem)
        });
        return { status: resp.status, body: await resp.json() };
      });

      expect(result.status).toBe(400);
      expect(result.body.error).toBeDefined();
    });
  });

});
