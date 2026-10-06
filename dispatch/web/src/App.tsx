import { useCallback, useEffect, useRef, useState } from 'react';
import { PanelLeftClose, PanelLeftOpen, PanelRightClose, PanelRightOpen } from 'lucide-react';
import { TooltipProvider } from '@/components/ui/tooltip';
import { Button } from '@/components/ui/button';
import { TopBar } from '@/components/TopBar';
import { KpiStrip } from '@/components/KpiStrip';
import { FleetPanel } from '@/components/FleetPanel';
import { Inspector } from '@/components/Inspector';
import { Timeline } from '@/components/Timeline';
import { MapView } from '@/map/MapView';
import { CompareDialog } from '@/components/CompareDialog';
import { ImportDialog } from '@/components/ImportDialog';
import { HistoryDialog } from '@/components/HistoryDialog';
import { api, pollJob } from '@/lib/api';
import { useT } from '@/i18n';
import type { DaySummary, Job, Plan, Scenario, Selection } from '@/types';

/* The live solve budget for UI-triggered solves. The saved baseline used ~240s
 * to reach day-1 117/0; 60s gives a believable, genuinely-elapsed optimize/replan
 * for the demo (status is reported honestly whatever the budget). Override with
 * VITE_SOLVE_SECONDS. */
const SOLVE_BUDGET_SEC = Number(import.meta.env.VITE_SOLVE_SECONDS ?? 60);

export interface JobView { id: string; status: Job['status']; elapsedSec: number; scenarioId: string; error: string | null }

export default function App() {
  const t = useT();
  const [days, setDays] = useState<DaySummary[]>([]);
  const [dayId, setDayId] = useState<string>('day1');
  const [scenario, setScenario] = useState<Scenario | null>(null);
  const [plan, setPlan] = useState<Plan | null>(null);
  const [baseline, setBaseline] = useState<Plan | null>(null);
  const [selection, setSelection] = useState<Selection>(null);
  const [job, setJob] = useState<JobView | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [compareOpen, setCompareOpen] = useState(false);
  const [importOpen, setImportOpen] = useState(false);
  const [historyOpen, setHistoryOpen] = useState(false);
  const [leftOpen, setLeftOpen] = useState(true);
  const [rightOpen, setRightOpen] = useState(true);

  const activeJobId = useRef<string | null>(null);
  const pollAbort = useRef<AbortController | null>(null);

  // Initial load: days + day-1 baseline.
  useEffect(() => {
    (async () => {
      try {
        const ds = await api.days();
        setDays(ds);
        await loadDay(ds.find((d) => d.id === 'day1') ?? ds[0]!);
      } catch (e) { setError(e instanceof Error ? e.message : String(e)); }
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const loadDay = useCallback(async (d: DaySummary) => {
    pollAbort.current?.abort();
    activeJobId.current = null;
    setJob(null);
    setSelection(null);
    setDayId(d.id);
    const base = await api.plan(d.baselinePlanId);
    setBaseline(base);
    setPlan(base);
    setScenario({ id: d.baseScenarioId, day: d.id, kind: 'base', parentId: null, revision: 0, label: d.label, removedVehicleIds: [], createdAt: '' });
  }, []);

  // Run a live solve of a scenario; apply + return the plan only if still current.
  const runSolve = useCallback(async (scenarioId: string): Promise<Plan | null> => {
    pollAbort.current?.abort();
    const ctrl = new AbortController();
    pollAbort.current = ctrl;
    setError(null);
    try {
      const { jobId } = await api.solve(scenarioId, SOLVE_BUDGET_SEC);
      activeJobId.current = jobId;
      setJob({ id: jobId, status: 'running', elapsedSec: 0, scenarioId, error: null });
      const final = await pollJob(jobId, (j) => {
        if (activeJobId.current !== jobId) return;
        setJob({ id: j.id, status: j.status, elapsedSec: j.elapsedSec, scenarioId, error: j.error });
      }, { signal: ctrl.signal });
      // Stale-job guard: ignore if a newer job/day superseded this one.
      if (activeJobId.current !== jobId) return null;
      if (final.status === 'completed' && final.planId) {
        const revised = await api.plan(final.planId);
        setPlan(revised);
        setSelection(null);
        return revised;
      }
      return null;
    } catch (e) {
      if ((e as Error).name === 'AbortError') return null;
      setError(e instanceof Error ? e.message : String(e));
      setJob((j) => (j ? { ...j, status: 'failed', error: String(e) } : j));
      return null;
    }
  }, []);

  // Make a vehicle unavailable: create a scenario copy and solve it (replan).
  const replanWithout = useCallback(async (vehicleId: number) => {
    if (!scenario) return;
    const parentId = scenario.kind === 'base' ? scenario.id : (scenario.parentId ?? scenario.id);
    const ref = plan?.vehicles.find((v) => v.id === vehicleId)?.ref ?? `#${vehicleId}`;
    const copy = await api.createCopy(parentId, vehicleId, t('scn.unavailable', { ref }));
    setScenario(copy);
    setSelection(null);
    const revised = await runSolve(copy.id);
    if (revised) setCompareOpen(true);
  }, [scenario, plan, runSolve, t]);

  const resetToBaseline = useCallback(() => {
    const d = days.find((x) => x.id === dayId);
    if (d) void loadDay(d);
  }, [days, dayId, loadDay]);

  // Reopen a saved plan faithfully, restoring its scenario context.
  const reopenPlan = useCallback(async (planId: string) => {
    pollAbort.current?.abort();
    activeJobId.current = null;
    setJob(null); setSelection(null); setError(null);
    try {
      const p = await api.plan(planId);
      const sc = await api.scenario(p.scenarioId);
      const d = days.find((x) => x.id === sc.day);
      if (d && baseline?.day !== d.isoDate) { setBaseline(await api.plan(d.baselinePlanId)); setDayId(d.id); }
      setScenario(sc);
      setPlan(p);
    } catch (e) { setError(e instanceof Error ? e.message : String(e)); }
  }, [days, baseline]);

  if (error && !plan) return <div className="flex h-full items-center justify-center text-sm text-destructive">{t('app.failed', { e: error })}</div>;
  if (!plan || !baseline || !scenario) return <div className="flex h-full items-center justify-center text-sm text-muted-foreground">{t('app.loading')}</div>;

  const isReplan = scenario.kind === 'copy';
  const solving = job?.status === 'running' || job?.status === 'pending';

  return (
    <TooltipProvider delayDuration={200}>
      <div className="flex h-full flex-col overflow-hidden">
        <TopBar
          days={days} dayId={dayId} onDayChange={(id) => { const d = days.find((x) => x.id === id); if (d) void loadDay(d); }}
          scenario={scenario} plan={plan} job={job} solving={!!solving}
          onOptimize={() => void runSolve(scenario.id)}
          onReset={resetToBaseline}
          showCompare={isReplan && plan.source === 'live'}
          onCompare={() => setCompareOpen(true)}
          onOpenImport={() => setImportOpen(true)}
          onOpenHistory={() => setHistoryOpen(true)}
        />
        <KpiStrip plan={plan} baseline={baseline} compare={isReplan} job={job} />

        <div className="flex min-h-0 flex-1">
          {leftOpen && (
            <aside className="w-[310px] shrink-0 border-r border-divider">
              <FleetPanel plan={plan} selection={selection} onSelect={setSelection} onMarkUnavailable={(id) => void replanWithout(id)} solving={!!solving} />
            </aside>
          )}

          <main className="flex min-w-0 flex-1 flex-col">
            <div className="relative min-h-0 flex-1">
              <div className="absolute left-3 top-3 z-10 flex gap-1.5">
                <Button variant="graphite" size="icon" className="h-8 w-8 opacity-90" aria-label={leftOpen ? 'Collapse fleet panel' : 'Open fleet panel'} onClick={() => setLeftOpen((o) => !o)}>
                  {leftOpen ? <PanelLeftClose className="h-4 w-4" /> : <PanelLeftOpen className="h-4 w-4" />}
                </Button>
                <Button variant="graphite" size="icon" className="h-8 w-8 opacity-90" aria-label={rightOpen ? 'Collapse inspector' : 'Open inspector'} onClick={() => setRightOpen((o) => !o)}>
                  {rightOpen ? <PanelRightClose className="h-4 w-4" /> : <PanelRightOpen className="h-4 w-4" />}
                </Button>
              </div>
              <MapView plan={plan} selection={selection} onSelect={setSelection} />
            </div>
            <div className="h-48 shrink-0 border-t border-divider">
              <Timeline plan={plan} selection={selection} onSelect={setSelection} />
            </div>
          </main>

          {rightOpen && (
            <aside className="w-[340px] shrink-0 border-l border-divider">
              <Inspector plan={plan} baseline={baseline} selection={selection} onMarkUnavailable={(id) => void replanWithout(id)} solving={!!solving} />
            </aside>
          )}
        </div>
      </div>

      {isReplan && plan.source === 'live' && (
        <CompareDialog
          open={compareOpen}
          onOpenChange={setCompareOpen}
          basePlanId={baseline.id}
          revisedPlanId={plan.id}
          scenarioLabel={scenario.label}
        />
      )}
      <ImportDialog open={importOpen} onOpenChange={setImportOpen} />
      <HistoryDialog open={historyOpen} onOpenChange={setHistoryOpen} dayId={dayId} currentPlanId={plan.id} onReopen={(id) => void reopenPlan(id)} />
    </TooltipProvider>
  );
}
