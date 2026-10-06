/*
 * TypeScript mirror of the backend -> frontend Plan contract.
 * Source of truth: dispatch/docs/data-api-mapping.md ("Plan (backend -> frontend
 * contract)"). Keep these in sync with that document. All times are
 * seconds-from-midnight; distances are kilometers; coordinates are [lon, lat].
 */

export type PlanSource = 'saved' | 'live' | 'sample';

export interface SolverConfig {
  seed: number;
  maxIterations: number;
  maxTimeSeconds: number;
  lexicographic: boolean;
  hardCapacity: boolean;
  hardTimeWindows: boolean;
  hardMaxDuration: boolean;
}

export interface ValidationViolation {
  code: string;
  message: string;
  vehicleId?: number;
  orderId?: string;
}

export interface PlanProvenance {
  inputSha256: string;
  solverConfig: SolverConfig;
  termination: 'OK' | 'LIMIT';
  validation: { valid: boolean; violations: ValidationViolation[] };
}

export interface PlanStats {
  servedOrders: number;
  totalOrders: number;
  deliveryStops: number;
  trips: number;
  vehiclesUsed: number;
  totalDistanceKm: number;
  solveElapsedSeconds: number;
}

export interface Depot {
  name: string;
  lon: number;
  lat: number;
}

export type StopType = 'delivery' | 'pickup' | 'depot';

export interface Stop {
  orderId: string;
  orderNo: string;
  customer: string;
  city: string;
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
  lateBySec: number;
}

export interface Trip {
  index: number;
  startSec: number;
  endSec: number;
  distanceKm: number;
  reloadSecAfter: number;
  stops: Stop[];
  /** Road-following path from Velo, cached with the plan. [lon, lat] pairs. */
  geometry: GeoJSON.LineString;
}

export interface Vehicle {
  id: number;
  ref: string;
  class: string;
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
  orderId: string;
  orderNo: string;
  customer: string;
  reason: string;
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

/** A flattened reference to a selected item, for the right inspector. */
export type Selection =
  | { kind: 'vehicle'; vehicleId: number }
  | { kind: 'trip'; vehicleId: number; tripIndex: number }
  | { kind: 'stop'; vehicleId: number; tripIndex: number; seq: number }
  | null;
