import { km, seconds } from '@/lib/format';
import { useT } from '@/i18n';
import type { Plan } from '@/types';
import type { JobView } from '@/App';

function Delta({ value, goodWhenNegative, unit = '' }: { value: number; goodWhenNegative?: boolean; unit?: string }) {
  const t = useT();
  if (value === 0) return <span className="tnum text-[11px] text-muted-foreground">±0{unit}</span>;
  const good = goodWhenNegative ? value < 0 : value > 0;
  const sign = value > 0 ? '+' : '';
  return (
    <span className={`tnum text-[11px] font-medium ${good ? 'text-primary' : 'text-warning'}`}>
      {sign}{value % 1 === 0 ? value : value.toFixed(0)}{unit} {t('kpi.vsBaseline')}
    </span>
  );
}

function Kpi({ label, value, sub, delta }: { label: string; value: string; sub?: React.ReactNode; delta?: React.ReactNode }) {
  return (
    <div className="flex min-w-[9rem] flex-col px-5 py-2">
      <span className="text-[11px] uppercase tracking-wide text-muted-foreground">{label}</span>
      <span className="tnum text-lg font-semibold leading-tight">{value}</span>
      {delta ?? (sub && <span className="tnum text-[11px] text-muted-foreground">{sub}</span>)}
    </div>
  );
}

/* Compact KPI strip with fine dividers; shows deltas vs the baseline on replan. */
export function KpiStrip({ plan, baseline, compare, job }: { plan: Plan; baseline: Plan; compare: boolean; job: JobView | null }) {
  const t = useT();
  const s = plan.stats;
  const b = baseline.stats;
  const solveT = s.solveElapsedSeconds ?? (job?.status === 'running' ? job.elapsedSec : null);

  return (
    <div className="flex shrink-0 items-stretch overflow-x-auto border-b border-divider bg-card [&>*+*]:border-l [&>*+*]:border-divider">
      <Kpi
        label={t('kpi.servedTotal')}
        value={`${s.servedOrders} / ${s.totalOrders}`}
        sub={t('kpi.unassignedN', { n: plan.unassigned.length })}
        delta={compare ? <Delta value={s.servedOrders - b.servedOrders} goodWhenNegative={false} /> : undefined}
      />
      <Kpi
        label={t('kpi.vehiclesUsed')}
        value={`${s.vehiclesUsed}`}
        sub={t('kpi.tripsN', { n: s.trips })}
        delta={compare ? <Delta value={s.vehiclesUsed - b.vehiclesUsed} goodWhenNegative /> : undefined}
      />
      <Kpi
        label={t('kpi.totalDistance')}
        value={km(s.totalDistanceKm)}
        delta={compare ? <Delta value={Math.round(s.totalDistanceKm - b.totalDistanceKm)} goodWhenNegative unit=" km" /> : undefined}
      />
      <Kpi label={t('kpi.deliveryStops')} value={`${s.deliveryStops}`} />
      <Kpi
        label={t('kpi.solveTime')}
        value={solveT != null ? seconds(solveT, 1) : '—'}
        sub={plan.source === 'saved' ? t('kpi.savedResult') : plan.provenance.termination}
      />
    </div>
  );
}
