import Fastify, { type FastifyReply, type FastifyRequest } from 'fastify';
import fastifyStatic from '@fastify/static';
import { existsSync } from 'node:fs';
import { join } from 'node:path';
import { config } from './config.js';
import { DAYS, loadDay, loadEnrichment, loadVehicleInfo } from './data/gyermelyi.js';
import { mapSolutionToPlan } from './plan/mapper.js';
import { fillPlanGeometry, veloReachable } from './geometry/velo.js';
import { store } from './store.js';
import { startSolve, cancelJob } from './solve.js';
import type { Plan } from './types.js';

const app = Fastify({ logger: { level: 'info' }, bodyLimit: 4 * 1024 * 1024 });

const enrichment = loadEnrichment();
const vehicleInfo = loadVehicleInfo();

/* ---- Baselines: immutable base scenario + saved real plan per day ---- */
function buildBaseline(dayId: string, label: string): Plan {
  const scenario = store.ensureBaseScenario(dayId, label);
  const day = loadDay(dayId);
  const plan = mapSolutionToPlan({
    day, request: day.request, solution: day.solution,
    enrichment, vehicleInfo,
    scenarioId: scenario.id, scenarioRevision: 0,
    planId: `${dayId}-baseline`, source: 'saved',
    createdAt: new Date(0).toISOString(),
  });
  store.putPlan(plan, false); // derived at startup; not persisted
  return plan;
}
for (const d of DAYS) {
  try { const p = buildBaseline(d.id, d.label); app.log.info(`baseline ${d.id}: ${p.stats.servedOrders}/${p.stats.totalOrders} served`); }
  catch (e) { app.log.error(`baseline ${d.id} failed: ${(e as Error).message}`); }
}

// Background: precompute baseline geometry so the first UI load is complete.
void (async () => {
  if (!(await veloReachable())) { app.log.warn('velo not reachable; geometry fills lazily'); return; }
  for (const d of DAYS) {
    const p = store.getPlan(`${d.id}-baseline`);
    if (!p) continue;
    try {
      await fillPlanGeometry(p);
      const trips = p.vehicles.reduce((n, v) => n + v.trips.length, 0);
      const filled = p.vehicles.reduce((n, v) => n + v.trips.filter((t) => t.geometry).length, 0);
      app.log.info(`geometry ${d.id}: ${filled}/${trips} trips`);
    } catch (e) { app.log.warn(`geometry ${d.id} failed: ${(e as Error).message}`); }
  }
})();

/* ---- Meta ---- */
app.get('/api/health', async () => ({ status: 'ok', service: 'dispatch-api', days: DAYS.map((d) => d.id) }));
app.get('/api/config', async () => ({ carta: config.cartaOrigin, velo: config.veloOrigin, solveTimeSeconds: config.solveTimeSeconds }));

app.get('/api/days', async () => DAYS.map((d) => {
  const plan = store.getPlan(`${d.id}-baseline`);
  return {
    id: d.id, isoDate: d.isoDate, label: d.label,
    orders: plan?.stats.totalOrders ?? null,
    vehicles: plan?.vehicles.length ?? null,
    baseScenarioId: `${d.id}-base`,
    baselinePlanId: `${d.id}-baseline`,
  };
}));

/* ---- Scenarios ---- */
app.post<{ Body: { day?: string; from?: string; removeVehicleId?: number; label?: string } }>('/api/scenarios', async (req, reply) => {
  const { day, from, removeVehicleId, label } = req.body ?? {};
  if (from) {
    const parent = store.getScenario(from);
    if (!parent) return reply.code(404).send({ error: 'parent scenario not found' });
    const s = store.createCopy(parent, { removeVehicleId }, label);
    return reply.code(201).send(s);
  }
  if (day) {
    const def = DAYS.find((d) => d.id === day);
    if (!def) return reply.code(400).send({ error: 'unknown day' });
    return store.ensureBaseScenario(day, def.label);
  }
  return reply.code(400).send({ error: 'day or from required' });
});

app.get<{ Params: { id: string } }>('/api/scenarios/:id', async (req, reply) => {
  const s = store.getScenario(req.params.id);
  if (!s) return reply.code(404).send({ error: 'scenario not found' });
  const day = loadDay(s.day);
  const availableVehicles = day.request.vehicles.filter((v) => !s.removedVehicleIds.includes(v.id));
  return {
    ...s,
    ordersInScope: day.scopeTaskIds.size,
    vehicleCount: availableVehicles.length,
    depot: day.depot,
    isoDate: day.def.isoDate,
  };
});

app.get<{ Params: { id: string } }>('/api/scenarios/:id/plans', async (req, reply) => {
  if (!store.getScenario(req.params.id)) return reply.code(404).send({ error: 'scenario not found' });
  return store.plansForScenario(req.params.id).map((p) => ({
    id: p.id, source: p.source, createdAt: p.createdAt,
    termination: p.provenance.termination, stats: p.stats,
  }));
});

/* ---- Solve jobs ---- */
app.post<{ Params: { id: string }; Body: { budgetSec?: number } }>('/api/scenarios/:id/solve', async (req, reply) => {
  const scenario = store.getScenario(req.params.id);
  if (!scenario) return reply.code(404).send({ error: 'scenario not found' });
  const budget = Math.max(5, Math.min(600, req.body?.budgetSec ?? config.solveTimeSeconds));
  const job = store.createJob(scenario.id, scenario.revision);
  startSolve(job, scenario, budget); // off the event loop (child process)
  return reply.code(202).send({ jobId: job.id, scenarioId: scenario.id, scenarioRevision: scenario.revision, budgetSec: budget });
});

app.get<{ Params: { id: string } }>('/api/jobs/:id', async (req, reply) => {
  const j = store.getJob(req.params.id);
  if (!j) return reply.code(404).send({ error: 'job not found' });
  // Report genuine elapsed while running (measured, not fabricated progress).
  if (j.status === 'running' && j.startedAt) {
    return { ...j, elapsedSec: Math.round(((Date.now() - Date.parse(j.startedAt)) / 1000) * 10) / 10 };
  }
  return j;
});

app.delete<{ Params: { id: string } }>('/api/jobs/:id', async (req, reply) => {
  const j = store.getJob(req.params.id);
  if (!j) return reply.code(404).send({ error: 'job not found' });
  const ok = cancelJob(j);
  return { cancelled: ok, status: j.status };
});

/* ---- Plans ---- */
app.get<{ Params: { id: string } }>('/api/plans/:id', async (req, reply) => {
  const plan = store.getPlan(req.params.id);
  if (!plan) return reply.code(404).send({ error: 'plan not found' });
  try { await fillPlanGeometry(plan); } catch (e) { app.log.warn(`geometry fill: ${(e as Error).message}`); }
  return plan;
});

/* ---- Carta tile proxy (same-origin so the reused style's relative URLs work) ---- */
async function proxyCarta(req: FastifyRequest, reply: FastifyReply): Promise<void> {
  const target = config.cartaOrigin + req.url;
  try {
    const res = await fetch(target, { signal: AbortSignal.timeout(15000) });
    reply.code(res.status);
    for (const h of ['content-type', 'cache-control', 'etag', 'access-control-allow-origin']) {
      const v = res.headers.get(h); if (v) reply.header(h, v);
    }
    reply.send(Buffer.from(await res.arrayBuffer()));
  } catch (e) {
    reply.code(502).send({ error: `carta proxy failed: ${(e as Error).message}` });
  }
}
app.get('/tiles.vector.json', proxyCarta);
app.get('/tiles/*', proxyCarta);

/* ---- Static SPA (production: backend serves the built frontend) ---- */
const webDist = config.webDist;
if (existsSync(join(webDist, 'index.html'))) {
  await app.register(fastifyStatic, { root: webDist, wildcard: false });
  app.setNotFoundHandler((req, reply) => {
    // SPA fallback for GET navigations only; everything else is a real 404.
    if (req.method === 'GET' && !req.url.startsWith('/api') && !req.url.startsWith('/tiles') && !req.url.startsWith('/fonts')) {
      return reply.sendFile('index.html');
    }
    return reply.code(404).send({ error: 'not found' });
  });
  app.log.info(`serving SPA from ${webDist}`);
} else {
  app.log.info('no web build found; run the frontend via Vite in dev');
}

const start = async () => {
  try { await app.listen({ port: config.port, host: config.host }); }
  catch (err) { app.log.error(err); process.exit(1); }
};
start();
