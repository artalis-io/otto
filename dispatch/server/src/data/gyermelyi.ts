import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { config } from '../config.js';
import type { SurgeRequest, SurgeSolution } from '../types.js';

/* The Gyermelyi dataset is immutable and lives outside the repo. the backend reads it
 * read-only. Each planning day is a separate VRP; the per-day request file
 * carries ALL in-scope locations + the full travel matrix but we scope the
 * served order set to that day's delivery date (verified exact: 117 tasks on
 * 2026-05-06 == the 117 the saved baseline serves). */

export interface DayDef {
  id: string;        // 'day1'
  date: string;      // canonical delivery_date token, e.g. '5/6/26'
  isoDate: string;   // '2026-05-06'
  label: string;
  requestFile: string;
  solutionFile: string;
}

export const DAYS: DayDef[] = [
  { id: 'day1', date: '5/6/26', isoDate: '2026-05-06', label: 'Wednesday 6 May 2026',
    requestFile: 'results/perday/day1_request.json', solutionFile: 'results/perday/day1_solution.json' },
  { id: 'day2', date: '5/7/26', isoDate: '2026-05-07', label: 'Thursday 7 May 2026',
    requestFile: 'results/perday/day2_request.json', solutionFile: 'results/perday/day2_solution.json' },
];

export interface OrderInfo {
  id: string | null;
  orderNo: string;
  customer: string | null;
  city: string | null;
  street: string | null;
  zip: string | null;
  lat: number | null;
  lon: number | null;
  pallets: number | null;    // fractional, from canonical
  weightKg: number | null;
  twStart: string | null;    // HH:MM
  twEnd: string | null;
  serviceMin: number | null;
  requiresTailLift: boolean;
  maxTonnage: number | null; // max vehicle GVW (t) that can access the address
}

export interface VehicleInfo {
  vehicleClass: string | null;
  plate: string | null;
  isSubcontractor: boolean;
  tonnage: number | null;    // vehicle gross weight (t)
  hasTailLift: boolean;
}

/* Parse a Hungarian special_req free-text field: a tail-lift flag ("emelőhátfal")
 * and/or a max access tonnage ("3,5t" / "7,5t" / "18t"). */
function parseTonnage(specialReq: string | null | undefined): number | null {
  if (!specialReq) return null;
  const m = /(\d+(?:[.,]\d+)?)\s*t/i.exec(specialReq);
  return m ? Number(m[1]!.replace(',', '.')) : null;
}
function hasTailLiftReq(specialReq: string | null | undefined): boolean {
  return typeof specialReq === 'string' && /emel[őo]h[áa]tfal/i.test(specialReq);
}

function readJson<T>(rel: string): T {
  return JSON.parse(readFileSync(resolve(config.gyermelyiRoot, rel), 'utf8')) as T;
}

function records(obj: any): any[] {
  return Array.isArray(obj) ? obj : (obj?.records ?? []);
}

/** order_no -> set of canonical delivery_date tokens (for day scoping). */
export function loadOrderDates(): Map<string, Set<string>> {
  const recs = records(readJson<any>('input/orders.json'));
  const m = new Map<string, Set<string>>();
  for (const r of recs) {
    const no = String(r.order_no);
    const set = m.get(no) ?? new Set<string>();
    set.add(String(r.delivery_date));
    m.set(no, set);
  }
  return m;
}

/* Stable pseudonym for a customer name when anonymizing (same name -> same label). */
function pseudonym(name: string): string {
  let h = 0;
  for (let i = 0; i < name.length; i++) h = (h * 31 + name.charCodeAt(i)) >>> 0;
  return `Customer ${(h % 900) + 100}`;
}

/** order_no -> OrderInfo (customer/city/coords/tw/pallets), from geocoded canonical. */
export function loadEnrichment(): Map<string, OrderInfo> {
  const recs = records(readJson<any>('input/orders.geocoded.json'));
  const m = new Map<string, OrderInfo>();
  for (const r of recs) {
    const no = String(r.order_no);
    if (m.has(no)) continue; // same-date duplicates are identical enough; keep first
    m.set(no, {
      id: r.id ?? null,
      orderNo: no,
      customer: r.customer ? (config.anonymize ? pseudonym(String(r.customer)) : String(r.customer)) : null,
      city: r.city ?? null,
      street: r.street ?? null,
      zip: r.zip != null ? String(r.zip) : null,
      lat: typeof r.lat === 'number' ? r.lat : null,
      lon: typeof r.lon === 'number' ? r.lon : null,
      pallets: typeof r.pallets === 'number' ? r.pallets : null,
      weightKg: typeof r.weight_kg === 'number' ? r.weight_kg : null,
      twStart: r.tw_start ?? null,
      twEnd: r.tw_end ?? null,
      serviceMin: typeof r.service_min === 'number' ? r.service_min : null,
      requiresTailLift: Boolean(r.requires_tail_lift) || hasTailLiftReq(r.special_req),
      maxTonnage: parseTonnage(r.special_req),
    });
  }
  return m;
}

const normKey = (s: string): string => s.replace(/[^a-z0-9]/gi, '').toUpperCase();

/** request-vehicle ref -> VehicleInfo (class/plate/subcontractor), best-effort join. */
export function loadVehicleInfo(): Map<string, VehicleInfo> {
  const recs = records(readJson<any>('input/vehicles.json'));
  const m = new Map<string, VehicleInfo>();
  for (const r of recs) {
    const info: VehicleInfo = {
      vehicleClass: r.vehicle_class ?? null,
      plate: r.plate ?? null,
      isSubcontractor: Boolean(r.is_subcontractor),
      tonnage: typeof r.gross_weight_kg === 'number' && r.gross_weight_kg > 0 ? r.gross_weight_kg / 1000 : null,
      hasTailLift: Boolean(r.requires_tail_lift) || hasTailLiftReq(r.special_req),
    };
    for (const k of [r.id, r.plate, r.ref].filter(Boolean)) m.set(normKey(String(k)), info);
  }
  return m;
}

export function vehicleInfoFor(ref: string, table: Map<string, VehicleInfo>): VehicleInfo | null {
  const n = normKey(ref);
  if (table.has(n)) return table.get(n)!;
  for (const [k, v] of table) if (k.includes(n) || n.includes(k)) return v;
  return null;
}

export interface LoadedDay {
  def: DayDef;
  request: SurgeRequest;
  solution: SurgeSolution;
  scopeTaskIds: Set<number>;   // tasks whose order falls on this day
  depot: { name: string; lon: number; lat: number };
}

export function loadDay(dayId: string): LoadedDay {
  const def = DAYS.find((d) => d.id === dayId);
  if (!def) throw new Error(`unknown day ${dayId}`);
  const request = readJson<SurgeRequest>(def.requestFile);
  const solution = readJson<SurgeSolution>(def.solutionFile);
  const dates = loadOrderDates();
  const scopeTaskIds = new Set<number>();
  for (const t of request.tasks) {
    const ds = dates.get(String(t.ref));
    if (ds && ds.size === 1 && ds.has(def.date)) scopeTaskIds.add(t.id);
  }
  const d0 = request.locations[0]!;
  return {
    def,
    request,
    solution,
    scopeTaskIds,
    depot: { name: 'Gyermely', lon: d0.x, lat: d0.y },
  };
}
