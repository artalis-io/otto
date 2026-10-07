import { mkdirSync, readdirSync, readFileSync, writeFileSync, existsSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { randomUUID } from 'node:crypto';
import { config } from './config.js';
import type { Plan } from './types.js';

/* Minimal durable store for scenarios, plans and jobs. JSON files under the
 * local data dir (not SQLite) - fastest reliable for a single-node demo, and
 * nothing here is client-private (plans are derived, scenarios are edits).
 * Source data stays immutable and external. */

/* Manual dispatcher overrides, applied to the Surge request at solve time:
 * a pin forces an order onto one vehicle (allowed_vehicles = [vehicleId]);
 * a forbid excludes a vehicle for an order (forbidden_vehicles). Pins win over
 * forbids for the same order. Orders are keyed by order_no (stable across days). */
export interface Pin { orderNo: string; vehicleId: number }
export interface Forbid { orderNo: string; vehicleId: number }

export interface Scenario {
  id: string;
  day: string;            // 'day1' | 'day2'
  kind: 'base' | 'copy';
  parentId: string | null;
  revision: number;       // bumped on each edit
  label: string;
  removedVehicleIds: number[];
  pins: Pin[];
  forbids: Forbid[];
  createdAt: string;
}

/* A single dispatcher edit applied to an editable (copy) scenario. */
export type ScenarioEdit =
  | { op: 'removeVehicle'; vehicleId: number }
  | { op: 'restoreVehicle'; vehicleId: number }
  | { op: 'pin'; orderNo: string; vehicleId: number }
  | { op: 'unpin'; orderNo: string }
  | { op: 'forbid'; orderNo: string; vehicleId: number }
  | { op: 'unforbid'; orderNo: string; vehicleId: number }
  | { op: 'clearOverrides' };

export type JobStatus = 'pending' | 'running' | 'completed' | 'failed' | 'cancelled';
export interface Job {
  id: string;
  scenarioId: string;
  scenarioRevision: number;
  status: JobStatus;
  createdAt: string;
  startedAt: string | null;
  finishedAt: string | null;
  elapsedSec: number;
  planId: string | null;
  termination: string | null;
  error: string | null;
}

function dir(sub: string): string {
  const d = resolve(config.dataDir, sub);
  mkdirSync(d, { recursive: true });
  return d;
}
function writeJson(path: string, obj: unknown): void {
  writeFileSync(path, JSON.stringify(obj));
}
function loadAll<T>(sub: string): Map<string, T> {
  const m = new Map<string, T>();
  const d = dir(sub);
  if (!existsSync(d)) return m;
  for (const f of readdirSync(d)) {
    if (!f.endsWith('.json')) continue;
    try { const o = JSON.parse(readFileSync(join(d, f), 'utf8')) as { id?: string }; if (o.id) m.set(o.id, o as T); } catch { /* skip */ }
  }
  return m;
}

export class Store {
  scenarios = loadAll<Scenario>('scenarios');
  jobs = loadAll<Job>('jobs');
  plans = new Map<string, Plan>();          // in-memory (baselines + live)
  private persistedPlans = loadAll<Plan>('plans');

  constructor() {
    for (const [id, p] of this.persistedPlans) this.plans.set(id, p);
    // Normalize scenarios persisted before overrides existed.
    for (const s of this.scenarios.values()) {
      if (!Array.isArray(s.pins)) s.pins = [];
      if (!Array.isArray(s.forbids)) s.forbids = [];
    }
  }

  newId(prefix: string): string { return `${prefix}_${randomUUID().slice(0, 8)}`; }

  /* Idempotent base scenario per day (immutable, revision 0). */
  ensureBaseScenario(day: string, label: string): Scenario {
    const id = `${day}-base`;
    let s = this.scenarios.get(id);
    if (!s) {
      s = { id, day, kind: 'base', parentId: null, revision: 0, label, removedVehicleIds: [], pins: [], forbids: [], createdAt: new Date().toISOString() };
      this.scenarios.set(id, s);
      writeJson(join(dir('scenarios'), `${id}.json`), s);
    }
    return s;
  }

  createCopy(parent: Scenario, edits: { removeVehicleId?: number }, label?: string): Scenario {
    const removed = [...parent.removedVehicleIds];
    if (edits.removeVehicleId != null && !removed.includes(edits.removeVehicleId)) removed.push(edits.removeVehicleId);
    const s: Scenario = {
      id: this.newId('scn'),
      day: parent.day,
      kind: 'copy',
      parentId: parent.id,
      revision: 1,
      label: label ?? `${parent.label} (edited)`,
      removedVehicleIds: removed,
      pins: parent.pins.map((p) => ({ ...p })),
      forbids: parent.forbids.map((f) => ({ ...f })),
      createdAt: new Date().toISOString(),
    };
    this.scenarios.set(s.id, s);
    writeJson(join(dir('scenarios'), `${s.id}.json`), s);
    return s;
  }

  /** Return an editable (copy) scenario for the given one: copies forked from an
   * immutable base so the baseline is never mutated; copies are edited in place. */
  forkForEdit(current: Scenario, label?: string): Scenario {
    if (current.kind === 'copy') return current;
    return this.createCopy(current, {}, label);
  }

  /** Apply a dispatcher edit to an editable scenario, bump its revision, persist.
   * Throws on a base scenario (immutable). Returns the updated scenario. */
  editScenario(id: string, edit: ScenarioEdit): Scenario {
    const s = this.scenarios.get(id);
    if (!s) throw new Error('scenario not found');
    if (s.kind === 'base') throw new Error('base scenario is immutable; fork a copy first');
    switch (edit.op) {
      case 'removeVehicle':
        if (!s.removedVehicleIds.includes(edit.vehicleId)) s.removedVehicleIds.push(edit.vehicleId);
        // an order cannot be pinned to a vehicle that is no longer available
        s.pins = s.pins.filter((p) => p.vehicleId !== edit.vehicleId);
        break;
      case 'restoreVehicle':
        s.removedVehicleIds = s.removedVehicleIds.filter((v) => v !== edit.vehicleId);
        break;
      case 'pin':
        s.pins = s.pins.filter((p) => p.orderNo !== edit.orderNo);
        s.pins.push({ orderNo: edit.orderNo, vehicleId: edit.vehicleId });
        s.forbids = s.forbids.filter((f) => f.orderNo !== edit.orderNo); // pin supersedes forbids
        break;
      case 'unpin':
        s.pins = s.pins.filter((p) => p.orderNo !== edit.orderNo);
        break;
      case 'forbid':
        if (!s.forbids.some((f) => f.orderNo === edit.orderNo && f.vehicleId === edit.vehicleId))
          s.forbids.push({ orderNo: edit.orderNo, vehicleId: edit.vehicleId });
        s.pins = s.pins.filter((p) => !(p.orderNo === edit.orderNo && p.vehicleId === edit.vehicleId));
        break;
      case 'unforbid':
        s.forbids = s.forbids.filter((f) => !(f.orderNo === edit.orderNo && f.vehicleId === edit.vehicleId));
        break;
      case 'clearOverrides':
        s.pins = []; s.forbids = [];
        break;
    }
    s.revision += 1;
    this.scenarios.set(s.id, s);
    writeJson(join(dir('scenarios'), `${s.id}.json`), s);
    return s;
  }

  getScenario(id: string): Scenario | undefined { return this.scenarios.get(id); }

  putPlan(plan: Plan, persist = true): void {
    this.plans.set(plan.id, plan);
    if (persist) writeJson(join(dir('plans'), `${plan.id}.json`), plan);
  }
  getPlan(id: string): Plan | undefined { return this.plans.get(id); }
  plansForScenario(scenarioId: string): Plan[] {
    return [...this.plans.values()].filter((p) => p.scenarioId === scenarioId);
  }

  createJob(scenarioId: string, scenarioRevision: number): Job {
    const j: Job = {
      id: this.newId('job'), scenarioId, scenarioRevision,
      status: 'pending', createdAt: new Date().toISOString(),
      startedAt: null, finishedAt: null, elapsedSec: 0,
      planId: null, termination: null, error: null,
    };
    this.jobs.set(j.id, j);
    this.saveJob(j);
    return j;
  }
  getJob(id: string): Job | undefined { return this.jobs.get(id); }
  saveJob(j: Job): void { writeJson(join(dir('jobs'), `${j.id}.json`), j); }
}

export const store = new Store();
