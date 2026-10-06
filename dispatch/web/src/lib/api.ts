import type { DaySummary, Job, Plan, Scenario } from '@/types';

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

  job: (id: string) => jget<Job>(`/api/jobs/${id}`),
  cancelJob: (id: string) => jsend<{ cancelled: boolean; status: string }>(`/api/jobs/${id}`, 'DELETE'),
};

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
