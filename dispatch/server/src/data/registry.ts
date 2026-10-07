import { mkdirSync, readdirSync, readFileSync, writeFileSync, existsSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { config } from '../config.js';

/* Registry of admitted (uploaded) datasets. Each dataset is a folder under the
 * external uploads dir holding its Surge request, geocoded orders, and a
 * dataset.json manifest. Its days (by delivery date) become selectable
 * alongside the built-in days. Never committed. */

export interface RegDay { dayId: string; date: string; isoDate: string; label: string; orders: number }
export interface Dataset {
  id: string;
  label: string;
  createdAt: string;
  depot: { name: string; lon: number; lat: number };
  requestPath: string;
  geocodedPath: string;
  days: RegDay[];
}

/* A geocoded order is routable iff it has coordinates, a confident tier, and is
 * on the router's graph (Hungary). Shared by admit (building the matrix/tasks)
 * and loadDay (scoping) so task indices stay aligned. */
export function isRoutableOrder(o: Record<string, unknown>): boolean {
  return typeof o.lat === 'number' && typeof o.lon === 'number'
    && ['GREEN', 'YELLOW'].includes(String(o.geo_tier)) && String(o.geo_cc) === 'HU';
}

export function datasetsRoot(): string { const d = resolve(config.uploadsDir, 'datasets'); mkdirSync(d, { recursive: true }); return d; }
export function datasetDir(id: string): string { return resolve(datasetsRoot(), id); }

const registry = new Map<string, Dataset>();
function load(): void {
  registry.clear();
  const root = datasetsRoot();
  if (!existsSync(root)) return;
  for (const id of readdirSync(root)) {
    try { const ds = JSON.parse(readFileSync(join(root, id, 'dataset.json'), 'utf8')) as Dataset; if (ds.id) registry.set(ds.id, ds); } catch { /* skip */ }
  }
}
load();

export function registerDataset(ds: Dataset): void {
  mkdirSync(datasetDir(ds.id), { recursive: true });
  writeFileSync(join(datasetDir(ds.id), 'dataset.json'), JSON.stringify(ds));
  registry.set(ds.id, ds);
}
export function allDatasets(): Dataset[] { return [...registry.values()]; }
export function datasetForDay(dayId: string): { dataset: Dataset; day: RegDay } | null {
  for (const ds of registry.values()) {
    const day = ds.days.find((d) => d.dayId === dayId);
    if (day) return { dataset: ds, day };
  }
  return null;
}
