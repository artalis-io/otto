import { spawn, type ChildProcess } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { config } from './config.js';
import { loadDay, loadEnrichment, loadVehicleInfo, DAYS } from './data/gyermelyi.js';
import { mapSolutionToPlan } from './plan/mapper.js';
import { fillPlanGeometry } from './geometry/velo.js';
import { store, type Job, type Scenario } from './store.js';
import type { SurgeRequest, SurgeSolution } from './types.js';

const enrichment = loadEnrichment();
const vehicleInfo = loadVehicleInfo();
const running = new Map<string, ChildProcess>();

export interface BuiltRequest { request: SurgeRequest; day: ReturnType<typeof loadDay> }

export type Objective = 'vehicles' | 'distance';

/** Build the exact Surge request for a scenario: day-scoped requests, edited
 * fleet, manual pins/forbids, solve budget and objective. The base day's
 * vehicles are already day-specific.
 *
 * Objective maps to Surge's lexicographic_objective: 'vehicles' (true) serves
 * then minimizes fleet size then distance; 'distance' (false) minimizes the
 * single cost objective (fixed + distance), i.e. the least-distance plan. */
export function buildRequest(scenario: Scenario, budgetSec: number, objective: Objective = 'vehicles'): BuiltRequest {
  const day = loadDay(scenario.day);
  const request = structuredClone(day.request) as SurgeRequest;
  request.requests = request.requests.filter((r) => day.scopeTaskIds.has(r.delivery_task_id));

  const available = new Set(request.vehicles.map((v) => v.id));
  if (scenario.removedVehicleIds.length) {
    const drop = new Set(scenario.removedVehicleIds);
    request.vehicles = request.vehicles.filter((v) => !drop.has(v.id));
    for (const id of drop) available.delete(id);
  }

  applyOverrides(request, scenario, available);
  applyVehicleOverrides(request, scenario);

  request.config = {
    ...request.config,
    max_time_seconds: budgetSec,
    seed: 42,
    lexicographic_objective: objective !== 'distance',
    hard_capacity: true,
    hard_time_windows: true,
    hard_max_duration: true,
  };
  return { request, day };
}

/** Apply a scenario's manual pins/forbids onto a request (mutates request). Orders
 * are keyed by order_no (task.ref); pins set allowed_vehicles=[v] and supersede
 * forbids on the same order; forbids append to forbidden_vehicles. Overrides
 * naming a vehicle not in `available` (e.g. one that was removed) are skipped. */
export function applyOverrides(request: SurgeRequest, scenario: Scenario, available?: Set<number>): void {
  if (!scenario.pins.length && !scenario.forbids.length) return;
  const avail = available ?? new Set(request.vehicles.map((v) => v.id));
  const taskRefById = new Map(request.tasks.map((t) => [t.id, t.ref]));
  const reqByOrderNo = new Map<string, SurgeRequest['requests'][number]>();
  for (const r of request.requests) {
    const ref = taskRefById.get(r.delivery_task_id);
    if (ref != null) reqByOrderNo.set(String(ref), r);
  }
  for (const p of scenario.pins) {
    const r = reqByOrderNo.get(p.orderNo);
    if (r && avail.has(p.vehicleId)) r.allowed_vehicles = [p.vehicleId];
  }
  for (const f of scenario.forbids) {
    const r = reqByOrderNo.get(f.orderNo);
    if (!r || !avail.has(f.vehicleId)) continue;
    if (r.allowed_vehicles) continue; // a pin on this order supersedes forbids
    r.forbidden_vehicles = [...(r.forbidden_vehicles ?? []), f.vehicleId];
  }
}

/** Apply per-vehicle constraint overrides (capacity, shift, max trips/distance/
 * duration) onto the request vehicles (mutates request). Unset fields are left
 * at the vehicle's dataset defaults. Distance is km->m, duration is min->s. */
export function applyVehicleOverrides(request: SurgeRequest, scenario: Scenario): void {
  if (!scenario.vehicleOverrides.length) return;
  const byId = new Map(request.vehicles.map((v) => [v.id, v]));
  for (const o of scenario.vehicleOverrides) {
    const v = byId.get(o.vehicleId);
    if (!v) continue; // vehicle removed / not in scope
    if (o.capacityKg != null) v.capacity[0] = o.capacityKg;
    if (o.capacityPallets != null) v.capacity[1] = o.capacityPallets;
    if (o.shiftEarlySec != null) v.shift_early = o.shiftEarlySec;
    if (o.shiftLateSec != null) v.shift_late = o.shiftLateSec;
    if (o.maxTrips != null) v.max_trips = o.maxTrips;
    if (o.maxDistanceKm != null) v.max_distance = o.maxDistanceKm * 1000;
    if (o.maxDurationMin != null) v.max_duration = o.maxDurationMin * 60;
  }
}

function dayLabel(dayId: string): string {
  return DAYS.find((d) => d.id === dayId)?.label ?? dayId;
}

/** Spawn surge_solve for a job (off the event loop), then map + geometry-fill +
 * store the resulting plan. Updates job status throughout. */
export function startSolve(job: Job, scenario: Scenario, budgetSec: number, objective: Objective = 'vehicles'): void {
  const built = buildRequest(scenario, budgetSec, objective);
  const jobsDir = resolve(config.dataDir, 'jobs');
  mkdirSync(jobsDir, { recursive: true });
  const reqFile = resolve(jobsDir, `${job.id}_request.json`);
  writeFileSync(reqFile, JSON.stringify(built.request));

  job.status = 'running';
  job.startedAt = new Date().toISOString();
  store.saveJob(job);

  const t0 = Date.now();
  const child = spawn(config.surgeBin, [reqFile], { stdio: ['ignore', 'pipe', 'pipe'] });
  running.set(job.id, child);

  const out: Buffer[] = [];
  const err: Buffer[] = [];
  child.stdout.on('data', (c: Buffer) => out.push(c));
  child.stderr.on('data', (c: Buffer) => err.push(c));

  child.on('error', (e) => {
    job.status = 'failed';
    job.error = `spawn failed: ${e.message}`;
    job.finishedAt = new Date().toISOString();
    job.elapsedSec = (Date.now() - t0) / 1000;
    running.delete(job.id);
    store.saveJob(job);
  });

  child.on('close', (code, signal) => {
    running.delete(job.id);
    job.elapsedSec = (Date.now() - t0) / 1000;
    job.finishedAt = new Date().toISOString();

    if (job.status === 'cancelled' || signal === 'SIGTERM' || signal === 'SIGKILL') {
      job.status = 'cancelled';
      store.saveJob(job);
      return;
    }

    let solution: SurgeSolution | null = null;
    try { solution = JSON.parse(Buffer.concat(out).toString('utf8')) as SurgeSolution; } catch { /* below */ }

    if (!solution || !Array.isArray(solution.routes) || solution.routes.length === 0) {
      job.status = 'failed';
      job.termination = solution?.status ?? null;
      job.error = (Buffer.concat(err).toString('utf8').trim() || `surge_solve exited ${code} with no usable solution`).slice(0, 500);
      store.saveJob(job);
      return;
    }

    const planId = store.newId('pln');
    const plan = mapSolutionToPlan({
      day: built.day,
      request: built.request,
      solution,
      enrichment,
      vehicleInfo,
      scenarioId: scenario.id,
      scenarioRevision: scenario.revision,
      planId,
      source: 'live',
      createdAt: new Date().toISOString(),
    });
    // surge_solve does not emit elapsed; record the measured wall-clock.
    plan.stats.solveElapsedSeconds = Math.round(job.elapsedSec * 10) / 10;

    // Fill geometry before publishing so the plan renders immediately.
    fillPlanGeometry(plan)
      .catch(() => false)
      .finally(() => {
        store.putPlan(plan);
        job.status = 'completed';
        job.planId = planId;
        job.termination = solution!.status;
        store.saveJob(job);
      });
  });
}

export function cancelJob(job: Job): boolean {
  const child = running.get(job.id);
  if (!child) return false;
  job.status = 'cancelled';
  store.saveJob(job);
  child.kill('SIGTERM');
  return true;
}

export { dayLabel };
