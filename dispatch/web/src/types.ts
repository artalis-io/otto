/*
 * TypeScript mirror of the backend -> frontend Plan contract.
 * Source of truth: dispatch/docs/data-api-mapping.md and dispatch/server/src/types.ts.
 * All times are seconds-from-midnight; distances are kilometers; coords [lon, lat].
 */

export type PlanSource = 'saved' | 'live' | 'sample';

export interface PlanProvenance {
  inputSha256: string;
  solverConfig: Record<string, unknown>; // raw Surge config (seed, max_time_seconds, hard_*, ...)
  termination: string;                    // OK | LIMIT | ...
  validation: { valid: boolean; violations: ValidationViolation[] };
}

export interface ValidationViolation {
  type: string;            // HARD_TW | CAPACITY | ...
  dimension?: string;
  vehicle_id?: number;
  request_id?: number;
  trip?: number;
  actual?: number;
  limit?: number;
}

export interface PlanStats {
  servedOrders: number;
  totalOrders: number;
  deliveryStops: number;
  trips: number;
  vehiclesUsed: number;
  totalDistanceKm: number;
  solveElapsedSeconds: number | null;
}

export interface Depot { name: string; lon: number; lat: number }

export type StopType = 'delivery' | 'pickup' | 'service' | 'depot';

export interface Stop {
  orderId: string | null;
  orderNo: string;
  customer: string | null;
  city: string | null;
  lon: number;
  lat: number;
  seq: number;
  type: StopType;
  arrivalSec: number;
  serviceStartSec: number;
  departureSec: number;
  twStartSec: number;
  twEndSec: number;
  pallets: number;
  weightKg: number;
  serviceMin: number;
  waitSec: number;
  travelToSec: number;
  lateBySec: number;
}

export interface Trip {
  index: number;
  startSec: number;
  endSec: number;
  distanceKm: number;
  reloadSecAfter: number;
  loadKg: number;
  loadPallets: number;
  stops: Stop[];
  /** Road-following path from Velo, cached with the plan. [lon, lat] pairs. */
  geometry: GeoJSON.LineString | null;
}

export interface Vehicle {
  id: number;
  ref: string;
  vehicleClass: string | null;
  /** Assigned by the backend from a fixed distinct palette; stable within a plan. */
  color: string;
  capacityKg: number;
  capacityPallets: number;
  tripCount: number;
  finishTimeSec: number;
  peakKg: number;
  peakPallets: number;
  distanceKm: number;
  trips: Trip[];
}

export interface UnassignedOrder {
  orderId: string | null;
  orderNo: string;
  customer: string | null;
  city: string | null;
  lon: number | null;
  lat: number | null;
  reason: string | null;
}

export interface Plan {
  id: string;
  scenarioId: string;
  scenarioRevision: number;
  day: string;
  createdAt: string;
  source: PlanSource;
  provenance: PlanProvenance;
  stats: PlanStats;
  depot: Depot;
  vehicles: Vehicle[];
  unassigned: UnassignedOrder[];
}

/* ---- API shapes ---- */
export interface DaySummary {
  id: string;
  isoDate: string;
  label: string;
  orders: number | null;
  vehicles: number | null;
  baseScenarioId: string;
  baselinePlanId: string;
}

export interface Scenario {
  id: string;
  day: string;
  kind: 'base' | 'copy';
  parentId: string | null;
  revision: number;
  label: string;
  removedVehicleIds: number[];
  createdAt: string;
}

export type JobStatus = 'pending' | 'running' | 'completed' | 'failed' | 'cancelled';
export interface Job {
  id: string;
  scenarioId: string;
  scenarioRevision: number;
  status: JobStatus;
  elapsedSec: number;
  planId: string | null;
  termination: string | null;
  error: string | null;
}

export interface Comparison {
  base: { planId: string; stats: PlanStats };
  revised: { planId: string; stats: PlanStats };
  deltas: { servedOrders: number; vehiclesUsed: number; trips: number; totalDistanceKm: number; unassigned: number };
  newlyUnassigned: { orderNo: string; customer: string | null; city: string | null; fromVehicleRef: string }[];
  nowServed: { orderNo: string; customer: string | null; city: string | null; toVehicleRef: string }[];
  movedOrders: { orderNo: string; customer: string | null; fromVehicleRef: string; toVehicleRef: string }[];
  removedVehicles: string[];
  addedVehicles: string[];
}

/** A flattened reference to a selected item, driving map/timeline/inspector. */
export type Selection =
  | { kind: 'vehicle'; vehicleId: number }
  | { kind: 'trip'; vehicleId: number; tripIndex: number }
  | { kind: 'stop'; vehicleId: number; tripIndex: number; seq: number }
  | { kind: 'unassigned'; orderNo: string }
  | null;
