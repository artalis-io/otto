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

  // Pseudonymize customer names (cities/coords kept). For shippable screenshots
  // of real plans without exposing client PII. Off by default (real demo).
  anonymize: env('DISPATCH_ANONYMIZE', '0') === '1',

  // ---- Robustness limits ----
  // Max solver subprocesses at once; further solves queue (pending -> running).
  maxConcurrentSolves: Number(env('DISPATCH_MAX_CONCURRENT_SOLVES', '2')),
  // Hard-kill a solve this many seconds past its budget (SIGTERM, then SIGKILL).
  solveWatchdogGraceSec: Number(env('DISPATCH_SOLVE_GRACE_SEC', '20')),
  // Cap captured solver stdout; beyond this the job fails instead of OOMing.
  maxSolveOutputMB: Number(env('DISPATCH_MAX_SOLVE_OUTPUT_MB', '128')),
  // Retention: keep at most this many persisted plans / jobs (oldest pruned).
  retainPlans: Number(env('DISPATCH_RETAIN_PLANS', '200')),
  retainJobs: Number(env('DISPATCH_RETAIN_JOBS', '200')),
  // Cap the on-disk/in-memory road-geometry leg cache (oldest evicted). Bounds
  // what was an unbounded, fully-rewritten-per-persist file.
  geocacheMaxLegs: Number(env('DISPATCH_GEOCACHE_MAX_LEGS', '200000')),
  // Per-IP API rate limit (token bucket): sustained rps and burst.
  rateLimitRps: Number(env('DISPATCH_RATE_RPS', '40')),
  rateLimitBurst: Number(env('DISPATCH_RATE_BURST', '120')),

  // ---- Data onboarding (upload) ----
  // Where uploaded datasets live (external, never committed).
  uploadsDir: env('DISPATCH_UPLOADS_DIR', resolve(env('GYERMELYI_ROOT', '/Users/mark/artalis.io/data/gyermelyi'), 'uploads')),
  // Nexus engines (ingest.sh, reconcile.py, nx_pipeline) in the main checkout.
  nexusDir: env('NEXUS_DIR', resolve(env('OTTO_ROOT', '/Users/mark/artalis.io/src/otto'), 'nexus')),
  // Upload limits.
  maxUploadMB: Number(env('DISPATCH_MAX_UPLOAD_MB', '25')),
  maxUploadRows: Number(env('DISPATCH_MAX_UPLOAD_ROWS', '50000')),
  ingestTimeoutSec: Number(env('DISPATCH_INGEST_TIMEOUT_SEC', '90')),
  // Geocoding (reuse the dataset cache; keys-if-present else offline).
  geocodeCacheDir: env('GEOCODE_CACHE_DIR', resolve(env('GYERMELYI_ROOT', '/Users/mark/artalis.io/data/gyermelyi'), '.geocode_cache')),
  geocodePbf: env('GEOCODE_PBF', resolve(env('OTTO_ROOT', '/Users/mark/artalis.io/src/otto'), 'data/hungary-latest.osm.pbf')),
  geocodeEnv: env('GEOCODE_ENV', resolve(env('OTTO_ROOT', '/Users/mark/artalis.io/src/otto'), '.env')),
  geocodeForceOffline: env('DISPATCH_GEOCODE_OFFLINE', '0') === '1', // force cache-only even if keys exist
  geocodeTimeoutSec: Number(env('DISPATCH_GEOCODE_TIMEOUT_SEC', '180')),
  // Travel matrix (Velo all-pairs) for admitting an uploaded dataset.
  matrixBuildBin: env('MATRIX_BUILD_BIN', resolve(env('OTTO_ROOT', '/Users/mark/artalis.io/src/otto'), 'velo/matrix_build')),
  veloGraph: env('VELO_GRAPH', resolve(env('OTTO_ROOT', '/Users/mark/artalis.io/src/otto'), 'data/index/hungary-velo.vlg')),
  matrixTimeoutSec: Number(env('DISPATCH_MATRIX_TIMEOUT_SEC', '300')),
  maxMatrixLocations: Number(env('DISPATCH_MAX_MATRIX_LOCATIONS', '1500')),
} as const;

export type Config = typeof config;
