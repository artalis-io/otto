import { km, seconds } from '@/lib/format';
import type { Plan } from '@/types';

function Kpi({ label, value, sub }: { label: string; value: string; sub?: string }) {
  return (
    <div className="flex flex-col px-5 py-2">
      <span className="text-[11px] uppercase tracking-wide text-muted-foreground">{label}</span>
      <span className="tnum text-lg font-semibold leading-tight">{value}</span>
      {sub && <span className="tnum text-[11px] text-muted-foreground">{sub}</span>}
    </div>
  );
}

/* Compact KPI strip with fine 1px dividers between cells. Tabular numerals. */
export function KpiStrip({ plan }: { plan: Plan }) {
  const s = plan.stats;
  return (
    <div className="flex shrink-0 items-stretch border-b border-divider bg-card [&>*+*]:border-l [&>*+*]:border-divider">
      <Kpi
        label="Served / Total"
        value={`${s.servedOrders} / ${s.totalOrders}`}
        sub={`${plan.unassigned.length} unassigned`}
      />
      <Kpi label="Vehicles used" value={`${s.vehiclesUsed}`} sub={`${s.trips} trips`} />
      <Kpi label="Total distance" value={km(s.totalDistanceKm)} />
      <Kpi label="Solve time" value={seconds(s.solveElapsedSeconds)} />
      <Kpi label="Delivery stops" value={`${s.deliveryStops}`} />
    </div>
  );
}
