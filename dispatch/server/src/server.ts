import Fastify, { type FastifyReply, type FastifyRequest } from 'fastify';
import fastifyStatic from '@fastify/static';
import { existsSync } from 'node:fs';
import { join } from 'node:path';
import { config } from './config.js';
import { DAYS, loadDay, loadEnrichment, loadVehicleInfo } from './data/gyermelyi.js';
import { mapSolutionToPlan } from './plan/mapper.js';
import { fillPlanGeometry, veloReachable } from './geometry/velo.js';
import { store, type ScenarioEdit } from './store.js';
import { startSolve, cancelJob, type Objective } from './solve.js';
import { comparePlans } from './plan/compare.js';
import { importSummary, rawSample, canonicalSample } from './data/import.js';
import { sageReachable, narratePlan, narrateComparison, type Lang } from './sage.js';
import { loadTariff } from './cost.js';
import { planToCsv, planToRouteSheetHtml } from './export.js';
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
app.get('/api/config', async () => {
  const tariff = loadTariff();
  return {
    carta: config.cartaOrigin, velo: config.veloOrigin, solveTimeSeconds: config.solveTimeSeconds,
    cost: { available: true, currency: tariff.currency, source: tariff.source },
  };
});

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

/* Apply a manual dispatcher edit (pin/forbid/remove-vehicle...). Forks an
 * editable copy from an immutable base so the baseline is never mutated; edits
 * an existing copy in place. Returns the (possibly new) scenario. */
app.post<{ Params: { id: string }; Body: { edit?: ScenarioEdit; label?: string } }>('/api/scenarios/:id/edit', async (req, reply) => {
  const cur = store.getScenario(req.params.id);
  if (!cur) return reply.code(404).send({ error: 'scenario not found' });
  const edit = req.body?.edit;
  if (!edit || typeof edit.op !== 'string') return reply.code(400).send({ error: 'edit op required' });
  try {
    const forked = cur.kind === 'base';
    const target = store.forkForEdit(cur, req.body?.label);
    const updated = store.editScenario(target.id, edit);
    return reply.code(forked ? 201 : 200).send(updated);
  } catch (e) {
    return reply.code(400).send({ error: (e as Error).message });
  }
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
app.post<{ Params: { id: string }; Body: { budgetSec?: number; objective?: string } }>('/api/scenarios/:id/solve', async (req, reply) => {
  const scenario = store.getScenario(req.params.id);
  if (!scenario) return reply.code(404).send({ error: 'scenario not found' });
  const budget = Math.max(5, Math.min(600, req.body?.budgetSec ?? config.solveTimeSeconds));
  const objective: Objective = req.body?.objective === 'distance' ? 'distance' : 'vehicles';
  const job = store.createJob(scenario.id, scenario.revision);
  startSolve(job, scenario, budget, objective); // off the event loop (child process)
  return reply.code(202).send({ jobId: job.id, scenarioId: scenario.id, scenarioRevision: scenario.revision, budgetSec: budget, objective });
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
// List saved plans (baseline + persisted live plans), optionally by day, for reopen.
app.get<{ Querystring: { day?: string } }>('/api/plans', async (req) => {
  const out = [];
  for (const p of store.plans.values()) {
    const sc = store.getScenario(p.scenarioId);
    if (req.query.day && sc?.day !== req.query.day) continue;
    out.push({
      id: p.id, scenarioId: p.scenarioId, day: sc?.day ?? null,
      label: sc?.label ?? p.scenarioId, source: p.source, createdAt: p.createdAt,
      termination: p.provenance.termination,
      servedOrders: p.stats.servedOrders, totalOrders: p.stats.totalOrders,
      vehiclesUsed: p.stats.vehiclesUsed, totalDistanceKm: p.stats.totalDistanceKm,
    });
  }
  out.sort((a, b) => (a.source === 'saved' ? 0 : 1) - (b.source === 'saved' ? 0 : 1) || a.createdAt.localeCompare(b.createdAt));
  return out;
});

app.get<{ Params: { id: string } }>('/api/plans/:id', async (req, reply) => {
  const plan = store.getPlan(req.params.id);
  if (!plan) return reply.code(404).send({ error: 'plan not found' });
  try { await fillPlanGeometry(plan); } catch (e) { app.log.warn(`geometry fill: ${(e as Error).message}`); }
  return plan;
});

/* ---- Operational exports (stops CSV, printable route sheets) ---- */
function dayLabelForPlan(plan: Plan): string {
  return DAYS.find((d) => d.isoDate === plan.day)?.label ?? plan.day;
}
app.get<{ Params: { id: string } }>('/api/plans/:id/export.csv', async (req, reply) => {
  const plan = store.getPlan(req.params.id);
  if (!plan) return reply.code(404).send({ error: 'plan not found' });
  reply.header('content-type', 'text/csv; charset=utf-8');
  reply.header('content-disposition', `attachment; filename="otto-plan-${plan.day}-${plan.id}.csv"`);
  return reply.send(planToCsv(plan));
});
app.get<{ Params: { id: string } }>('/api/plans/:id/routesheet.html', async (req, reply) => {
  const plan = store.getPlan(req.params.id);
  if (!plan) return reply.code(404).send({ error: 'plan not found' });
  reply.header('content-type', 'text/html; charset=utf-8');
  return reply.send(planToRouteSheetHtml(plan, dayLabelForPlan(plan)));
});

app.get<{ Querystring: { base?: string; revised?: string } }>('/api/compare', async (req, reply) => {
  const base = req.query.base ? store.getPlan(req.query.base) : undefined;
  const revised = req.query.revised ? store.getPlan(req.query.revised) : undefined;
  if (!base || !revised) return reply.code(404).send({ error: 'base and revised plan ids required' });
  return comparePlans(base, revised);
});

/* ---- Sage narration (optional LLM; degrades gracefully) ---- */
app.get('/api/sage/status', async () => ({ reachable: await sageReachable(), model: config.sageModel }));

app.post<{ Params: { id: string }; Body: { lang?: Lang } }>('/api/plans/:id/narrate', async (req, reply) => {
  const plan = store.getPlan(req.params.id);
  if (!plan) return reply.code(404).send({ error: 'plan not found' });
  try {
    return { text: await narratePlan(plan, req.body?.lang === 'hu' ? 'hu' : 'en') };
  } catch (e) {
    return reply.code(503).send({ error: `sage unavailable: ${(e as Error).message}`, unavailable: true });
  }
});

app.post<{ Querystring: { base?: string; revised?: string }; Body: { lang?: Lang } }>('/api/compare/narrate', async (req, reply) => {
  const base = req.query.base ? store.getPlan(req.query.base) : undefined;
  const revised = req.query.revised ? store.getPlan(req.query.revised) : undefined;
  if (!base || !revised) return reply.code(404).send({ error: 'base and revised plan ids required' });
  try {
    return { text: await narrateComparison(comparePlans(base, revised), req.body?.lang === 'hu' ? 'hu' : 'en') };
  } catch (e) {
    return reply.code(503).send({ error: `sage unavailable: ${(e as Error).message}`, unavailable: true });
  }
});

/* ---- Import / provenance (dedicated import view) ---- */
app.get('/api/import/summary', async () => importSummary());
app.get<{ Querystring: { file?: string; limit?: string } }>('/api/import/raw', async (req, reply) => {
  const s = rawSample(req.query.file ?? 'orders', req.query.limit ? Number(req.query.limit) : 8);
  if (!s) return reply.code(404).send({ error: 'raw file not available' });
  return s;
});
app.get<{ Querystring: { limit?: string } }>('/api/import/canonical', async (req, reply) => {
  const s = canonicalSample(req.query.limit ? Number(req.query.limit) : 8);
  if (!s) return reply.code(404).send({ error: 'canonical not available' });
  return s;
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
