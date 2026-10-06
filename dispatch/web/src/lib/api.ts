import type { Comparison, DaySummary, Job, Plan, Scenario } from '@/types';

/* Same-origin API client. In dev, Vite proxies /api and /tiles to the backend
 * (VITE_API_ORIGIN); in production the backend serves the SPA and these paths. */

async function jget<T>(url: string): Promise<T> {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return (await r.json()) as T;
}
async function jsend<T>(url: string, method: string, body?: unknown): Promise<T> {
  const r = await fetch(url, {
    method,
    headers: body ? { 'content-type': 'application/json' } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return (await r.json()) as T;
}

export const api = {
  days: () => jget<DaySummary[]>('/api/days'),
  plan: (id: string) => jget<Plan>(`/api/plans/${id}`),
  scenario: (id: string) => jget<Scenario & { ordersInScope: number; vehicleCount: number; isoDate: string }>(`/api/scenarios/${id}`),

  createBaseScenario: (day: string) => jsend<Scenario>('/api/scenarios', 'POST', { day }),
  createCopy: (from: string, removeVehicleId: number, label?: string) =>
    jsend<Scenario>('/api/scenarios', 'POST', { from, removeVehicleId, label }),

  solve: (scenarioId: string, budgetSec?: number) =>
    jsend<{ jobId: string; scenarioId: string; scenarioRevision: number; budgetSec: number }>(
      `/api/scenarios/${scenarioId}/solve`, 'POST', budgetSec ? { budgetSec } : {}),

  plans: (day?: string) => jget<PlanSummary[]>(`/api/plans${day ? `?day=${day}` : ''}`),

  job: (id: string) => jget<Job>(`/api/jobs/${id}`),
  cancelJob: (id: string) => jsend<{ cancelled: boolean; status: string }>(`/api/jobs/${id}`, 'DELETE'),

  compare: (base: string, revised: string) => jget<Comparison>(`/api/compare?base=${base}&revised=${revised}`),

  sageStatus: () => jget<{ reachable: boolean; model: string }>('/api/sage/status'),
  narratePlan: (id: string, lang: string) => jsend<{ text: string }>(`/api/plans/${id}/narrate`, 'POST', { lang }),
  narrateCompare: (base: string, revised: string, lang: string) =>
    jsend<{ text: string }>(`/api/compare/narrate?base=${base}&revised=${revised}`, 'POST', { lang }),

  importSummary: () => jget<ImportSummary>('/api/import/summary'),
  importRaw: (file: string) => jget<RawSample>(`/api/import/raw?file=${file}&limit=8`),
  importCanonical: () => jget<CanonicalSample>('/api/import/canonical?limit=8'),
};

export interface ImportSummary {
  sources: { kind: string; label: string; file: string; present: boolean; sha256: string | null; rawRows: number }[];
  canonical: { orders: { count: number | null; sourceSha256: string | null; audit: { rows_processed?: number; rows_accepted?: number; rows_rejected?: number; validation?: { errors?: number; warnings?: number } } | null } };
  pipeline: { stage: string; detail: string }[];
  rules: string[];
  scope: { rawOrderLines: number; inScopeHungarian: number; day1: number; day2: number };
}
export interface PlanSummary {
  id: string; scenarioId: string; day: string | null; label: string;
  source: 'saved' | 'live' | 'sample'; createdAt: string; termination: string;
  servedOrders: number; totalOrders: number; vehiclesUsed: number; totalDistanceKm: number;
}
export interface RawSample { label: string; headers: string[]; rows: string[][]; totalRows: number }
export interface CanonicalSample { fields: string[]; rows: Record<string, unknown>[]; count: number; sha256: string | null }

/** Poll a job to a terminal state, reporting each tick. Honors an AbortSignal. */
export async function pollJob(
  jobId: string,
  onTick: (job: Job) => void,
  opts: { intervalMs?: number; signal?: AbortSignal } = {},
): Promise<Job> {
  const interval = opts.intervalMs ?? 1200;
  for (;;) {
    if (opts.signal?.aborted) throw new DOMException('aborted', 'AbortError');
    const job = await api.job(jobId);
    onTick(job);
    if (job.status === 'completed' || job.status === 'failed' || job.status === 'cancelled') return job;
    await new Promise((res) => setTimeout(res, interval));
  }
}
