import Fastify from 'fastify';
import { config } from './config.js';
import { DAYS, loadDay, loadEnrichment, loadVehicleInfo } from './data/gyermelyi.js';
import { mapSolutionToPlan } from './plan/mapper.js';
import { fillPlanGeometry, veloReachable } from './geometry/velo.js';
import type { Plan } from './types.js';

const app = Fastify({ logger: { level: 'info' } });

// Enrichment + vehicle metadata are static for the dataset; load once.
const enrichment = loadEnrichment();
const vehicleInfo = loadVehicleInfo();

/* Build the immutable baseline plan for a day from its SAVED real solution.
 * This is a genuine prior Surge result, not a placeholder. */
function baselinePlan(dayId: string): Plan {
  const day = loadDay(dayId);
  return mapSolutionToPlan({
    day,
    request: day.request,
    solution: day.solution,
    enrichment,
    vehicleInfo,
    scenarioId: `${dayId}-base`,
    scenarioRevision: 0,
    planId: `${dayId}-baseline`,
    source: 'saved',
    createdAt: new Date(0).toISOString(),
  });
}

const baselines = new Map<string, Plan>();
for (const d of DAYS) {
  try {
    baselines.set(`${d.id}-baseline`, baselinePlan(d.id));
    app.log.info(`baseline ${d.id}: ${baselines.get(`${d.id}-baseline`)!.stats.servedOrders} served`);
  } catch (e) {
    app.log.error(`failed to load baseline ${d.id}: ${(e as Error).message}`);
  }
}

// Precompute baseline road geometry in the background so the first UI load is
// complete and instant (results are cached in-memory + on disk).
void (async () => {
  if (!(await veloReachable())) {
    app.log.warn('velo not reachable; route geometry will fill lazily on request');
    return;
  }
  for (const [id, plan] of baselines) {
    try {
      await fillPlanGeometry(plan);
      const trips = plan.vehicles.reduce((n, v) => n + v.trips.length, 0);
      const filled = plan.vehicles.reduce((n, v) => n + v.trips.filter((t) => t.geometry).length, 0);
      app.log.info(`geometry ${id}: ${filled}/${trips} trips`);
    } catch (e) { app.log.warn(`geometry precompute ${id} failed: ${(e as Error).message}`); }
  }
})();

app.get('/api/health', async () => ({
  status: 'ok',
  service: 'dispatch-api',
  days: DAYS.map((d) => d.id),
  baselines: [...baselines.keys()],
}));

app.get('/api/config', async () => ({
  carta: config.cartaOrigin,
  velo: config.veloOrigin,
  solveTimeSeconds: config.solveTimeSeconds,
}));

app.get('/api/days', async () => DAYS.map((d) => {
  const plan = baselines.get(`${d.id}-baseline`);
  return {
    id: d.id,
    isoDate: d.isoDate,
    label: d.label,
    orders: plan?.stats.totalOrders ?? null,
    vehicles: plan?.vehicles.length ?? null,
    baselinePlanId: `${d.id}-baseline`,
  };
}));

app.get<{ Params: { id: string } }>('/api/plans/:id', async (req, reply) => {
  const plan = baselines.get(req.params.id);
  if (!plan) return reply.code(404).send({ error: 'plan not found' });
  // Fill road geometry once (cached in-memory + on disk); never per selection.
  try { await fillPlanGeometry(plan); } catch (e) { app.log.warn(`geometry fill failed: ${(e as Error).message}`); }
  return plan;
});

const start = async () => {
  try {
    await app.listen({ port: config.port, host: config.host });
  } catch (err) {
    app.log.error(err);
    process.exit(1);
  }
};
start();
