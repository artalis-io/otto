import { useMemo } from 'react';
import { hhmm } from '@/lib/format';
import type { Plan } from '@/types';

/*
 * Vehicle timeline placeholder: a hand-rolled SVG horizontal time axis with one
 * row per vehicle and a block per trip (reload gaps left as whitespace). Custom
 * SVG by design (no chart library). Lays out against a shared [min,max] second
 * window so rows align. The lead can layer interaction/tooltips on top.
 */

const ROW_H = 26;
const LABEL_W = 64;
const PAD_X = 12;
const AXIS_H = 18;

export function Timeline({ plan }: { plan: Plan }) {
  const { minSec, maxSec, ticks } = useMemo(() => {
    let lo = Infinity;
    let hi = -Infinity;
    for (const v of plan.vehicles)
      for (const t of v.trips) {
        lo = Math.min(lo, t.startSec);
        hi = Math.max(hi, t.endSec);
      }
    if (!Number.isFinite(lo)) {
      lo = 18000;
      hi = 64800;
    }
    // Round to the hour for readable ticks.
    const loH = Math.floor(lo / 3600) * 3600;
    const hiH = Math.ceil(hi / 3600) * 3600;
    const t: number[] = [];
    for (let s = loH; s <= hiH; s += 3600) t.push(s);
    return { minSec: loH, maxSec: hiH, ticks: t };
  }, [plan]);

  const span = Math.max(1, maxSec - minSec);
  const rowsH = plan.vehicles.length * ROW_H;
  const height = AXIS_H + rowsH + 8;

  // viewBox width in abstract units; CSS scales it to the container.
  const VW = 1000;
  const plotW = VW - LABEL_W - PAD_X * 2;
  const xOf = (sec: number) => LABEL_W + PAD_X + ((sec - minSec) / span) * plotW;

  return (
    <div className="flex h-full flex-col bg-card">
      <div className="flex shrink-0 items-center px-3 py-1.5">
        <h2 className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">
          Timeline
        </h2>
        <span className="ml-2 text-[11px] text-muted-foreground">
          placeholder — trip blocks per vehicle
        </span>
      </div>
      <div className="min-h-0 flex-1 overflow-hidden px-2 pb-2">
        <svg
          viewBox={`0 0 ${VW} ${height}`}
          preserveAspectRatio="none"
          className="h-full w-full"
          role="img"
          aria-label="Vehicle timeline"
        >
          {/* Hour gridlines + labels */}
          {ticks.map((s) => (
            <g key={s}>
              <line
                x1={xOf(s)}
                x2={xOf(s)}
                y1={AXIS_H}
                y2={height - 4}
                stroke="hsl(42 14% 82%)"
                strokeWidth={1}
                vectorEffect="non-scaling-stroke"
              />
              <text
                x={xOf(s)}
                y={12}
                textAnchor="middle"
                fontSize={10}
                fill="hsl(40 8% 42%)"
                style={{ fontVariantNumeric: 'tabular-nums' }}
              >
                {hhmm(s)}
              </text>
            </g>
          ))}

          {/* One row per vehicle */}
          {plan.vehicles.map((v, i) => {
            const y = AXIS_H + i * ROW_H + 4;
            return (
              <g key={v.id}>
                <text
                  x={PAD_X}
                  y={y + (ROW_H - 10)}
                  fontSize={11}
                  fill="hsl(40 10% 18%)"
                  style={{ fontVariantNumeric: 'tabular-nums' }}
                >
                  {v.ref}
                </text>
                {v.trips.map((t) => {
                  const x = xOf(t.startSec);
                  const w = Math.max(2, xOf(t.endSec) - x);
                  return (
                    <rect
                      key={t.index}
                      x={x}
                      y={y}
                      width={w}
                      height={ROW_H - 10}
                      rx={3}
                      fill={v.color}
                      fillOpacity={0.85}
                    />
                  );
                })}
              </g>
            );
          })}
        </svg>
      </div>
    </div>
  );
}
