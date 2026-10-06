import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const serverRoot = resolve(here, '..'); // dispatch/server

function env(name: string, fallback: string): string {
  const v = process.env[name];
  return v && v.length > 0 ? v : fallback;
}

/*
 * the backend composes existing OTTO services; it does not reimplement them.
 *
 * - OTTO_ROOT holds the built C binaries (surge_solve) and dataset indexes.
 *   The dispatch worktree reuses the main checkout's artifacts by default, so we
 *   don't rebuild them here.
 * - GYERMELYI_ROOT is the immutable client dataset (never committed).
 * - CARTA/VELO/SAGE origins are reached over HTTP.
 */
export const config = {
  port: Number(env('DISPATCH_PORT', '8091')),
  host: env('DISPATCH_HOST', '127.0.0.1'),

  ottoRoot: env('OTTO_ROOT', '/Users/mark/artalis.io/src/otto'),
  surgeBin: env('SURGE_BIN', resolve(env('OTTO_ROOT', '/Users/mark/artalis.io/src/otto'), 'surge/surge_solve')),

  gyermelyiRoot: env('GYERMELYI_ROOT', '/Users/mark/artalis.io/data/gyermelyi'),

  cartaOrigin: env('CARTA_ORIGIN', 'http://127.0.0.1:8097'),
  veloOrigin: env('VELO_ORIGIN', 'http://127.0.0.1:8082'),
  sageOrigin: env('SAGE_ORIGIN', 'http://127.0.0.1:8084'),
  sageModel: env('SAGE_MODEL', 'sgdflash'),

  // Built SPA to serve. Absent in dev (Vite serves the app and proxies /api).
  webDist: env('WEB_DIST', resolve(serverRoot, '../web/dist')),

  // Local runtime store (scenarios/plans/jobs/geometry cache).
  dataDir: env('DISPATCH_DATA_DIR', resolve(serverRoot, 'data')),

  // Velo profile + polyline precision for road geometry (matches the matrices).
  veloProfile: env('VELO_PROFILE', 'truck'),
  polylinePrecision: 5,

  // Demo solve budget (seconds). The saved baseline used ~240s to reach 117/0;
  // live solves are honest about status/elapsed whatever the budget.
  solveTimeSeconds: Number(env('DISPATCH_SOLVE_SECONDS', '240')),
} as const;

export type Config = typeof config;
