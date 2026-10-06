import { useEffect, useMemo, useRef, useState } from 'react';
import { hhmm } from '@/lib/format';
import type { Plan, Selection, Vehicle } from '@/types';

/* Vehicle timeline. With a vehicle selected: that vehicle's full working day -
 * explicit trips, per-stop travel/service/wait segments, reload periods and
 * depot returns, on a common hourly scale. With nothing selected: a compact
 * fleet Gantt (one row per vehicle), clickable to select. Custom SVG. */

function useWidth<T extends HTMLElement>() {
  const ref = useRef<T | null>(null);
  const [w, setW] = useState(900);
  useEffect(() => {
    const el = ref.current; if (!el) return;
    const ro = new ResizeObserver(() => setW(el.clientWidth));
    ro.observe(el); setW(el.clientWidth);
    return () => ro.disconnect();
  }, []);
  return [ref, w] as const;
}

const GRID = 'hsl(42 14% 82%)';
const AXIS_TXT = 'hsl(40 8% 42%)';

function hourTicks(lo: number, hi: number): number[] {
  const t: number[] = [];
  for (let s = Math.floor(lo / 3600) * 3600; s <= Math.ceil(hi / 3600) * 3600; s += 3600) t.push(s);
  return t;
}

function Header({ title, hint }: { title: string; hint: string }) {
  return (
    <div className="flex shrink-0 items-center gap-2 px-3 py-1.5">
      <h2 className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Timeline</h2>
      <span className="text-[11px] text-foreground/70">{title}</span>
      <span className="ml-auto text-[11px] text-muted-foreground">{hint}</span>
    </div>
  );
}

function VehicleTimeline({ vehicle, selection, onSelect }: { vehicle: Vehicle; selection: Selection; onSelect: (s: Selection) => void }) {
  const [ref, width] = useWidth<HTMLDivElement>();
  const padL = 12, padR = 16, laneY = 46, laneH = 22;
  const { lo, hi } = useMemo(() => {
    let a = Infinity, b = -Infinity;
    for (const t of vehicle.trips) { a = Math.min(a, t.startSec); b = Math.max(b, t.endSec); }
    if (!Number.isFinite(a)) { a = 18000; b = 64800; }
    return { lo: Math.floor(a / 3600) * 3600, hi: Math.ceil(b / 3600) * 3600 };
  }, [vehicle]);
  const span = Math.max(1, hi - lo);
  const plotW = Math.max(50, width - padL - padR);
  const xOf = (s: number) => padL + ((s - lo) / span) * plotW;
  const H = 78;
  const selStopSeq = selection?.kind === 'stop' && selection.vehicleId === vehicle.id ? selection.seq : null;
  const selTrip = selection && (selection.kind === 'trip' || selection.kind === 'stop') && selection.vehicleId === vehicle.id ? selection.tripIndex : null;

  return (
    <div ref={ref} className="h-full w-full px-2">
      <svg width={width} height={H} role="img" aria-label={`Timeline for ${vehicle.ref}`}>
        {hourTicks(lo, hi).map((s) => (
          <g key={s}>
            <line x1={xOf(s)} x2={xOf(s)} y1={16} y2={H - 6} stroke={GRID} strokeWidth={1} />
            <text x={xOf(s)} y={11} textAnchor="middle" fontSize={10} fill={AXIS_TXT} style={{ fontVariantNumeric: 'tabular-nums' }}>{hhmm(s)}</text>
          </g>
        ))}
        {/* reload periods between consecutive trips */}
        {vehicle.trips.slice(0, -1).map((t, i) => {
          const next = vehicle.trips[i + 1]!;
          const x = xOf(t.endSec), w = Math.max(1, xOf(next.startSec) - x);
          return <rect key={`r${t.index}`} x={x} y={laneY} width={w} height={laneH} fill="url(#reload)" stroke={GRID} />;
        })}
        {/* trips */}
        {vehicle.trips.map((t) => {
          const dim = selTrip != null && selTrip !== t.index;
          const op = dim ? 0.25 : 1;
          return (
            <g key={t.index} opacity={op} onClick={() => onSelect({ kind: 'trip', vehicleId: vehicle.id, tripIndex: t.index })} style={{ cursor: 'pointer' }}>
              {/* travel baseline across the whole trip */}
              <rect x={xOf(t.startSec)} y={laneY + laneH / 2 - 2} width={Math.max(1, xOf(t.endSec) - xOf(t.startSec))} height={4} rx={2} fill={vehicle.color} fillOpacity={0.28} />
              {/* depot start/end markers */}
              <rect x={xOf(t.startSec) - 1.5} y={laneY} width={3} height={laneH} fill="#223a2e" />
              <rect x={xOf(t.endSec) - 1.5} y={laneY} width={3} height={laneH} fill="#223a2e" />
              {t.stops.map((s) => {
                const sx = xOf(s.serviceStartSec);
                const sw = Math.max(2, xOf(s.departureSec) - sx);
                const ww = Math.max(0, xOf(s.serviceStartSec) - xOf(s.arrivalSec));
                const isSel = selStopSeq === s.seq;
                return (
                  <g key={s.seq} onClick={(e) => { e.stopPropagation(); onSelect({ kind: 'stop', vehicleId: vehicle.id, tripIndex: t.index, seq: s.seq }); }} style={{ cursor: 'pointer' }}>
                    {ww > 0.5 && <rect x={xOf(s.arrivalSec)} y={laneY + 3} width={ww} height={laneH - 6} fill="#d97706" fillOpacity={0.6} />}
                    <rect x={sx} y={laneY} width={sw} height={laneH} rx={2} fill={vehicle.color} stroke={isSel ? '#111' : 'none'} strokeWidth={isSel ? 1.5 : 0} />
                  </g>
                );
              })}
            </g>
          );
        })}
        <defs>
          <pattern id="reload" width={6} height={6} patternTransform="rotate(45)" patternUnits="userSpaceOnUse">
            <rect width={6} height={6} fill="hsl(42 14% 88%)" />
            <line x1={0} y1={0} x2={0} y2={6} stroke="hsl(40 8% 60%)" strokeWidth={1} />
          </pattern>
        </defs>
      </svg>
      <div className="flex items-center gap-3 px-1 text-[10px] text-muted-foreground">
        <Legend color={vehicle.color} label="service" />
        <Legend color="#d97706" label="wait" />
        <span className="inline-flex items-center gap-1"><span className="inline-block h-2 w-3" style={{ background: 'repeating-linear-gradient(45deg,#e8e3d6,#e8e3d6 2px,#aaa 3px)' }} /> reload</span>
        <span className="inline-flex items-center gap-1"><span className="inline-block h-2 w-[3px] bg-[#223a2e]" /> depot</span>
      </div>
    </div>
  );
}

function Legend({ color, label }: { color: string; label: string }) {
  return <span className="inline-flex items-center gap-1"><span className="inline-block h-2 w-3 rounded-sm" style={{ backgroundColor: color }} /> {label}</span>;
}

function FleetOverview({ plan, onSelect }: { plan: Plan; onSelect: (s: Selection) => void }) {
  const [ref, width] = useWidth<HTMLDivElement>();
  const rowH = 16, labelW = 56, padR = 12, axisH = 16;
  const { lo, hi } = useMemo(() => {
    let a = Infinity, b = -Infinity;
    for (const v of plan.vehicles) for (const t of v.trips) { a = Math.min(a, t.startSec); b = Math.max(b, t.endSec); }
    if (!Number.isFinite(a)) { a = 18000; b = 72000; }
    return { lo: Math.floor(a / 3600) * 3600, hi: Math.ceil(b / 3600) * 3600 };
  }, [plan]);
  const span = Math.max(1, hi - lo);
  const plotW = Math.max(50, width - labelW - padR);
  const xOf = (s: number) => labelW + ((s - lo) / span) * plotW;
  const H = axisH + plan.vehicles.length * rowH + 6;

  return (
    <div ref={ref} className="h-full w-full overflow-y-auto px-2">
      <svg width={width} height={H} role="img" aria-label="Fleet timeline overview">
        {hourTicks(lo, hi).map((s) => (
          <g key={s}>
            <line x1={xOf(s)} x2={xOf(s)} y1={axisH} y2={H - 4} stroke={GRID} strokeWidth={1} />
            <text x={xOf(s)} y={11} textAnchor="middle" fontSize={9} fill={AXIS_TXT} style={{ fontVariantNumeric: 'tabular-nums' }}>{hhmm(s)}</text>
          </g>
        ))}
        {plan.vehicles.map((v, i) => {
          const y = axisH + i * rowH + 2;
          return (
            <g key={v.id} onClick={() => onSelect({ kind: 'vehicle', vehicleId: v.id })} style={{ cursor: 'pointer' }}>
              <text x={4} y={y + rowH - 6} fontSize={10} fill="hsl(40 10% 18%)" style={{ fontVariantNumeric: 'tabular-nums' }}>{v.ref}</text>
              {v.trips.map((t) => (
                <rect key={t.index} x={xOf(t.startSec)} y={y} width={Math.max(2, xOf(t.endSec) - xOf(t.startSec))} height={rowH - 5} rx={2} fill={v.color} fillOpacity={0.85} />
              ))}
            </g>
          );
        })}
      </svg>
    </div>
  );
}

export function Timeline({ plan, selection, onSelect }: { plan: Plan; selection: Selection; onSelect: (s: Selection) => void }) {
  const vehId = selection && selection.kind !== 'unassigned' ? selection.vehicleId : null;
  const vehicle = vehId != null ? plan.vehicles.find((v) => v.id === vehId) : null;
  return (
    <div className="flex h-full flex-col bg-card">
      <Header
        title={vehicle ? `${vehicle.ref} · working day` : 'Fleet overview'}
        hint={vehicle ? `${vehicle.tripCount} trips · finish ${hhmm(vehicle.finishTimeSec)}` : 'select a vehicle for its day'}
      />
      <div className="min-h-0 flex-1 overflow-hidden pb-1">
        {vehicle ? <VehicleTimeline vehicle={vehicle} selection={selection} onSelect={onSelect} /> : <FleetOverview plan={plan} onSelect={onSelect} />}
      </div>
    </div>
  );
}
