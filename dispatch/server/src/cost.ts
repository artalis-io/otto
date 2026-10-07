import { readFileSync, existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { config } from './config.js';

/*
 * Plan cost estimation under a carrier tariff.
 *
 * Real Gyermelyi rate cards are confidential and live outside the repo (per
 * provider, zone-based). If a resolved tariff is present at
 * `${GYERMELYI_ROOT}/rates/tariff.json` we use it (source 'external'); otherwise
 * we fall back to an illustrative in-repo demo tariff (source 'demo') so the
 * economics view is always populated without committing anything confidential.
 *
 * The model is deliberately simple and transparent: each used vehicle incurs a
 * fixed standing cost plus a per-km rate (both optionally overridable per
 * vehicle class), with a multiplier for subcontracted vehicles.
 */

export interface Tariff {
  currency: string;
  source: 'demo' | 'external';
  perKmDefault: number;
  fixedPerVehicle: number;
  perKmByClass: Record<string, number>;   // keyed by UPPERCASED class token
  fixedByClass: Record<string, number>;
  subcontractorMultiplier: number;
}

/* Illustrative tariff (Hungarian forint). NOT a real rate card - replace by
 * dropping a rates/tariff.json into the dataset dir. Values are round numbers
 * chosen to be plausible for regional truck haulage, not sourced from any
 * carrier. */
const DEMO_TARIFF: Tariff = {
  currency: 'HUF',
  source: 'demo',
  perKmDefault: 260,
  fixedPerVehicle: 38000,
  perKmByClass: { SOLO: 210, RIGID: 230, SEMI: 300, TRAILER: 300, PONYVA: 260 },
  fixedByClass: { SOLO: 28000, SEMI: 52000, TRAILER: 52000 },
  subcontractorMultiplier: 1.12,
};

let cached: Tariff | null = null;

/** Load the tariff once: external rate card if resolved, else the demo tariff. */
export function loadTariff(): Tariff {
  if (cached) return cached;
  const path = resolve(config.gyermelyiRoot, 'rates/tariff.json');
  if (existsSync(path)) {
    try {
      const ext = JSON.parse(readFileSync(path, 'utf8')) as Partial<Tariff>;
      cached = {
        currency: ext.currency ?? 'HUF',
        source: 'external',
        perKmDefault: ext.perKmDefault ?? DEMO_TARIFF.perKmDefault,
        fixedPerVehicle: ext.fixedPerVehicle ?? DEMO_TARIFF.fixedPerVehicle,
        perKmByClass: upperKeys(ext.perKmByClass ?? {}),
        fixedByClass: upperKeys(ext.fixedByClass ?? {}),
        subcontractorMultiplier: ext.subcontractorMultiplier ?? 1,
      };
      return cached;
    } catch { /* fall through to demo */ }
  }
  cached = DEMO_TARIFF;
  return cached;
}

function upperKeys(o: Record<string, number>): Record<string, number> {
  const out: Record<string, number> = {};
  for (const [k, v] of Object.entries(o)) out[k.toUpperCase()] = v;
  return out;
}

function rateForClass(table: Record<string, number>, vehicleClass: string | null, fallback: number): number {
  if (!vehicleClass) return fallback;
  const key = vehicleClass.toUpperCase();
  if (table[key] != null) return table[key];
  for (const [k, v] of Object.entries(table)) if (key.includes(k) || k.includes(key)) return v;
  return fallback;
}

export interface VehicleCost { fixed: number; variable: number; total: number }

/** Cost of one used vehicle given its class, subcontractor flag, and route km. */
export function vehicleCost(
  tariff: Tariff, vehicleClass: string | null, isSubcontractor: boolean, distanceKm: number,
): VehicleCost {
  const perKm = rateForClass(tariff.perKmByClass, vehicleClass, tariff.perKmDefault);
  const fixedBase = rateForClass(tariff.fixedByClass, vehicleClass, tariff.fixedPerVehicle);
  const mult = isSubcontractor ? tariff.subcontractorMultiplier : 1;
  const fixed = Math.round(fixedBase * mult);
  const variable = Math.round(perKm * distanceKm * mult);
  return { fixed, variable, total: fixed + variable };
}
