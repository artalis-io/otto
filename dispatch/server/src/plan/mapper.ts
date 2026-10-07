import { createHash } from 'node:crypto';
import type {
  Plan, PlanStop, PlanTrip, PlanVehicle, PlanUnassigned, PlanCost, PlanVehicleCost,
  SurgeRequest, SurgeSolution, SurgeTask,
} from '../types.js';
import type { LoadedDay, OrderInfo, VehicleInfo } from '../data/gyermelyi.js';
import { vehicleInfoFor } from '../data/gyermelyi.js';
import { colorForIndex } from '../colors.js';
import { metersToKm } from '../units.js';
import { loadTariff, vehicleCostFor, tripCost, type Tariff } from '../cost.js';

export interface MapInputs {
  day: LoadedDay;
  request: SurgeRequest;     // the exact request solved (may be scope-filtered)
  solution: SurgeSolution;
  enrichment: Map<string, OrderInfo>;
  vehicleInfo: Map<string, VehicleInfo>;
  tariff?: Tariff;           // cost tariff; defaults to the resolved tariff
  scenarioId: string;
  scenarioRevision: number;
  planId: string;
  source: 'saved' | 'live';
  createdAt: string;
}

export function sha256(s: string): string {
  return createHash('sha256').update(s).digest('hex');
}

export function mapSolutionToPlan(inp: MapInputs): Plan {
  const { request, solution } = inp;
  const N = request.travel.location_count;
  const dur = request.travel.durations;
  const dist = request.travel.distances;
  const legDur = (a: number, b: number): number => dur[a * N + b] ?? 0;
  const legDist = (a: number, b: number): number => dist[a * N + b] ?? 0;

  const taskById = new Map<number, SurgeTask>(request.tasks.map((t) => [t.id, t]));
  const vehById = new Map(request.vehicles.map((v) => [v.id, v]));
  const reqById = new Map<number, number>(); // request_id -> delivery_task_id
  for (const r of request.requests) reqById.set(r.id, r.delivery_task_id);

  const depotLoc = 0;
  const violations: unknown[] = [];
  const servedOrderNos = new Set<string>();
  const tariff = inp.tariff ?? loadTariff();

  const vehicles: PlanVehicle[] = solution.routes.map((route, ri) => {
    const rv = vehById.get(route.vehicle_id);
    const capKg = rv?.capacity[0] ?? 0;
    const capPal = rv?.capacity[1] ?? 0;
    const ref = rv?.ref ?? `veh-${route.vehicle_id}`;
    const vinfo = vehicleInfoFor(ref, inp.vehicleInfo);

    // group stops by trip_index preserving visiting order
    const byTrip = new Map<number, typeof route.stops>();
    for (const s of route.stops) {
      const arr = byTrip.get(s.trip_index) ?? [];
      arr.push(s);
      byTrip.set(s.trip_index, arr);
    }
    const tripIdx = [...byTrip.keys()].sort((a, b) => a - b);

    const trips: PlanTrip[] = tripIdx.map((ti) => {
      const stopsRaw = byTrip.get(ti)!;
      let prevLoc = depotLoc;
      let loadKg = 0, loadPallets = 0, tripMeters = 0;
      const stops: PlanStop[] = stopsRaw.map((s, i) => {
        const task = taskById.get(s.task_id);
        const loc = task ? request.locations[task.location_id] : undefined;
        const lon = loc?.x ?? 0, lat = loc?.y ?? 0;
        const orderNo = task?.ref ?? String(s.task_id);
        const info = inp.enrichment.get(orderNo) ?? null;
        const weightKg = task?.demand[0] ?? 0;
        const pallets = task?.demand[1] ?? 0;
        loadKg += weightKg; loadPallets += pallets;
        const travelToSec = task ? legDur(prevLoc, task.location_id) : 0;
        tripMeters += task ? legDist(prevLoc, task.location_id) : 0;
        if (task) prevLoc = task.location_id;
        servedOrderNos.add(orderNo);
        const twLate = task?.tw_late ?? 0;
        const lateBySec = Math.max(0, s.arrival - twLate);
        if (lateBySec > 1) violations.push({ type: 'HARD_TW', vehicle_id: route.vehicle_id, request_id: s.request_id, actual: s.arrival, limit: twLate });
        return {
          orderId: info?.id ?? null,
          orderNo,
          customer: info?.customer ?? null,
          city: info?.city ?? null,
          lon, lat, seq: i + 1,
          type: s.type,
          arrivalSec: s.arrival,
          serviceStartSec: s.service_start,
          departureSec: s.departure,
          twStartSec: task?.tw_early ?? 0,
          twEndSec: twLate,
          pallets, weightKg,
          serviceMin: task ? Math.round(task.service_seconds / 60) : 0,
          waitSec: Math.max(0, s.service_start - s.arrival),
          travelToSec,
          lateBySec,
        };
      });
      // close the trip back to depot
      tripMeters += prevLoc !== depotLoc ? legDist(prevLoc, depotLoc) : 0;
      const first = stops[0];
      const last = stops[stops.length - 1];
      const startSec = first ? first.arrivalSec - first.travelToSec : 0;
      const endSec = last ? last.departureSec + legDur(prevLoc, depotLoc) : startSec;
      if (loadKg > capKg + 1) violations.push({ type: 'CAPACITY', dimension: 'kg', vehicle_id: route.vehicle_id, trip: ti, actual: loadKg, limit: capKg });
      if (loadPallets > capPal + 1e-6) violations.push({ type: 'CAPACITY', dimension: 'pallets', vehicle_id: route.vehicle_id, trip: ti, actual: loadPallets, limit: capPal });
      const tripKm = metersToKm(tripMeters);
      return {
        index: ti, startSec, endSec,
        distanceKm: tripKm,
        reloadSecAfter: 0, // filled below once we know the next trip's start
        loadKg, loadPallets,
        costFt: tripCost(tariff, { vehicleClass: vinfo?.vehicleClass ?? null, isSubcontractor: Boolean(vinfo?.isSubcontractor) }, { distanceKm: tripKm, drops: stops.length }, tripIdx.length),
        stops,
        geometry: null,
      };
    });
    // reload dwell = gap between a trip's depot return and the next trip's departure
    for (let i = 0; i < trips.length - 1; i++) {
      trips[i]!.reloadSecAfter = Math.max(0, trips[i + 1]!.startSec - trips[i]!.endSec);
    }

    const peakKg = trips.reduce((m, t) => Math.max(m, t.loadKg), 0);
    const peakPallets = trips.reduce((m, t) => Math.max(m, t.loadPallets), 0);
    const finishTimeSec = trips.length ? trips[trips.length - 1]!.endSec : 0;

    return {
      id: route.vehicle_id,
      ref,
      vehicleClass: vinfo?.vehicleClass ?? null,
      isSubcontractor: Boolean(vinfo?.isSubcontractor),
      color: colorForIndex(ri),
      capacityKg: capKg,
      capacityPallets: capPal,
      tripCount: route.trip_count,
      finishTimeSec,
      peakKg, peakPallets,
      distanceKm: metersToKm(route.distance),
      trips,
    };
  });

  const unassigned: PlanUnassigned[] = solution.unassigned.map((rid) => {
    const taskId = reqById.get(rid);
    const task = taskId != null ? taskById.get(taskId) : undefined;
    const loc = task ? request.locations[task.location_id] : undefined;
    const orderNo = task?.ref ?? String(rid);
    const info = inp.enrichment.get(orderNo) ?? null;
    return {
      orderId: info?.id ?? null,
      orderNo,
      customer: info?.customer ?? null,
      city: info?.city ?? null,
      lon: loc?.x ?? info?.lon ?? null,
      lat: loc?.y ?? info?.lat ?? null,
      reason: null, // Surge does not emit per-order reasons; left explicit
    };
  });

  const deliveryStops = vehicles.reduce((n, v) => n + v.trips.reduce((m, t) => m + t.stops.length, 0), 0);
  const status = solution.status;
  const valid = (status === 'OK' || status === 'LIMIT') && violations.length === 0;

  // Estimated operating cost under the tariff (only used vehicles are charged).
  const perVehicle: PlanVehicleCost[] = vehicles.map((v) => {
    const c = vehicleCostFor(tariff, {
      vehicleClass: v.vehicleClass, isSubcontractor: v.isSubcontractor, distanceKm: v.distanceKm,
      trips: v.trips.map((t) => ({ distanceKm: t.distanceKm, drops: t.stops.length })),
    });
    return { vehicleId: v.id, ref: v.ref, vehicleClass: v.vehicleClass, distanceKm: v.distanceKm, fixedCost: c.fixed, variableCost: c.variable, totalCost: c.total };
  });
  const cost: PlanCost = {
    currency: tariff.currency,
    source: tariff.source,
    total: perVehicle.reduce((n, c) => n + c.totalCost, 0),
    fixed: perVehicle.reduce((n, c) => n + c.fixedCost, 0),
    variable: perVehicle.reduce((n, c) => n + c.variableCost, 0),
    perVehicle,
  };
  const objective: 'vehicles' | 'distance' = request.config.lexicographic_objective === false ? 'distance' : 'vehicles';

  return {
    id: inp.planId,
    scenarioId: inp.scenarioId,
    scenarioRevision: inp.scenarioRevision,
    day: inp.day.def.isoDate,
    createdAt: inp.createdAt,
    source: inp.source,
    objective,
    provenance: {
      inputSha256: sha256(JSON.stringify(request)),
      solverConfig: request.config,
      termination: status,
      validation: { valid, violations },
    },
    stats: {
      servedOrders: servedOrderNos.size,
      totalOrders: inp.day.scopeTaskIds.size,
      deliveryStops,
      trips: solution.stats.trips ?? vehicles.reduce((n, v) => n + v.tripCount, 0),
      vehiclesUsed: solution.stats.vehicles_used ?? vehicles.length,
      totalDistanceKm: metersToKm(solution.stats.total_distance ?? 0),
      solveElapsedSeconds: solution.stats.elapsed_seconds ?? null,
    },
    cost,
    depot: inp.day.depot,
    vehicles,
    unassigned,
  };
}
