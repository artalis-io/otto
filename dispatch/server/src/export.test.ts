import { test } from 'node:test';
import assert from 'node:assert/strict';

import { planToCsv, planToRouteSheetHtml } from './export.js';
import type { Plan, PlanStop, PlanTrip, PlanVehicle } from './types.js';

function mkStop(orderNo: string, over: Partial<PlanStop> = {}): PlanStop {
  return {
    orderId: null, orderNo, customer: `Cust ${orderNo}`, city: 'Town',
    lon: 19.1, lat: 47.5, seq: 1, type: 'delivery',
    arrivalSec: 8 * 3600, serviceStartSec: 8 * 3600, departureSec: 8 * 3600 + 600,
    twStartSec: 7 * 3600, twEndSec: 12 * 3600, pallets: 2.5, weightKg: 1234,
    serviceMin: 10, waitSec: 0, travelToSec: 600, lateBySec: 0, ...over,
  };
}
function mkTrip(stops: PlanStop[]): PlanTrip {
  return { index: 0, startSec: 7 * 3600, endSec: 10 * 3600, distanceKm: 42.5, reloadSecAfter: 0, loadKg: 2000, loadPallets: 5, stops, geometry: null };
}
function mkVehicle(): PlanVehicle {
  return {
    id: 1, ref: 'TRK-1', vehicleClass: 'SEMI_TRAILER', isSubcontractor: true, color: '#123456',
    capacityKg: 24000, capacityPallets: 33, tripCount: 1, finishTimeSec: 10 * 3600,
    peakKg: 2000, peakPallets: 5, distanceKm: 42.5, trips: [mkTrip([mkStop('O1'), mkStop('O2', { seq: 2, lateBySec: 900 })])],
  };
}
function mkPlan(): Plan {
  const vehicles = [mkVehicle()];
  return {
    id: 'pln_1', scenarioId: 'scn', scenarioRevision: 1, day: '2026-05-06',
    createdAt: '2026-05-06T08:00:00.000Z', source: 'live', objective: 'distance',
    provenance: { inputSha256: 'x', solverConfig: {}, termination: 'OK', validation: { valid: true, violations: [] } },
    stats: { servedOrders: 2, totalOrders: 3, deliveryStops: 2, trips: 1, vehiclesUsed: 1, totalDistanceKm: 42.5, solveElapsedSeconds: 60 },
    cost: {
      currency: 'HUF', source: 'demo', total: 70000, fixed: 52000, variable: 18000,
      perVehicle: [{ vehicleId: 1, ref: 'TRK-1', vehicleClass: 'SEMI_TRAILER', distanceKm: 42.5, fixedCost: 52000, variableCost: 18000, totalCost: 70000 }],
    },
    depot: { name: 'Gyermely', lon: 18.7, lat: 47.5 },
    vehicles,
    unassigned: [{ orderId: null, orderNo: 'U9', customer: 'Cust U9', city: 'Elsewhere', lon: 20, lat: 47, reason: null }],
  };
}

test('planToCsv: header, assigned rows, and an unassigned row', () => {
  const csv = planToCsv(mkPlan());
  const lines = csv.trim().split('\n');
  assert.ok(lines[0]!.startsWith('status,vehicle_ref,vehicle_class,trip,seq,order_no'));
  assert.equal(lines.length, 1 + 2 + 1); // header + 2 stops + 1 unassigned
  assert.ok(lines[1]!.startsWith('assigned,TRK-1,SEMI_TRAILER,1,1,O1,'));
  assert.ok(lines.at(-1)!.startsWith('unassigned,,,,,U9,'));
});

test('planToCsv: late minutes computed from lateBySec', () => {
  const csv = planToCsv(mkPlan());
  const o2 = csv.split('\n').find((l) => l.includes(',O2,'))!;
  assert.ok(o2.endsWith(',15')); // 900s -> 15 min late, last column
});

test('planToRouteSheetHtml: contains vehicle, subcontractor tag, maps link, cost', () => {
  const html = planToRouteSheetHtml(mkPlan(), 'Tuesday 6 May 2026');
  assert.ok(html.includes('TRK-1'));
  assert.ok(html.includes('subcontractor'));
  assert.ok(html.includes('https://www.google.com/maps/dir/'));
  assert.ok(html.includes('HUF'));
  assert.ok(html.includes('least distance'));         // objective rendered
  assert.ok(html.includes('Unassigned (1)'));
});

test('planToRouteSheetHtml: escapes HTML in customer names', () => {
  const plan = mkPlan();
  plan.vehicles[0]!.trips[0]!.stops[0]!.customer = 'A & <script>B</script>';
  const html = planToRouteSheetHtml(plan, 'Day');
  assert.ok(html.includes('A &amp; &lt;script&gt;'));
  assert.ok(!html.includes('<script>B'));
});
