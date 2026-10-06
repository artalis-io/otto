import { useEffect, useState } from 'react';
import { ArrowRight, TriangleAlert } from 'lucide-react';
import { Dialog, DialogContent, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import { ScrollArea } from '@/components/ui/scroll-area';
import { api } from '@/lib/api';
import { km } from '@/lib/format';
import type { Comparison } from '@/types';

function DeltaCell({ value, goodWhenNegative, unit = '' }: { value: number; goodWhenNegative?: boolean; unit?: string }) {
  if (value === 0) return <span className="tnum text-muted-foreground">±0{unit}</span>;
  const good = goodWhenNegative ? value < 0 : value > 0;
  return <span className={`tnum font-medium ${good ? 'text-primary' : 'text-warning'}`}>{value > 0 ? '+' : ''}{value}{unit}</span>;
}

function Row({ label, base, revised, delta }: { label: string; base: string; revised: string; delta: React.ReactNode }) {
  return (
    <div className="grid grid-cols-[1.4fr_1fr_1fr_1fr] items-baseline gap-2 border-t border-divider py-1.5 text-sm">
      <span className="text-muted-foreground">{label}</span>
      <span className="tnum text-right">{base}</span>
      <span className="tnum text-right font-medium">{revised}</span>
      <span className="text-right">{delta}</span>
    </div>
  );
}

export function CompareDialog({ open, onOpenChange, basePlanId, revisedPlanId, scenarioLabel }: {
  open: boolean; onOpenChange: (o: boolean) => void; basePlanId: string; revisedPlanId: string; scenarioLabel: string;
}) {
  const [cmp, setCmp] = useState<Comparison | null>(null);
  const [err, setErr] = useState<string | null>(null);

  useEffect(() => {
    if (!open) return;
    setCmp(null); setErr(null);
    api.compare(basePlanId, revisedPlanId).then(setCmp).catch((e) => setErr(String(e)));
  }, [open, basePlanId, revisedPlanId]);

  const b = cmp?.base.stats, r = cmp?.revised.stats;
  const servedDrop = cmp ? cmp.deltas.servedOrders < 0 : false;

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-2xl">
        <DialogHeader>
          <DialogTitle>Plan comparison — {scenarioLabel}</DialogTitle>
        </DialogHeader>
        {err && <p className="text-sm text-destructive">{err}</p>}
        {!cmp ? <p className="py-8 text-center text-sm text-muted-foreground">Comparing…</p> : (
          <div className="space-y-4">
            {servedDrop && (
              <div className="flex items-start gap-2 rounded-md border border-warning/40 bg-warning/10 p-2.5 text-sm text-warning">
                <TriangleAlert className="mt-0.5 h-4 w-4 shrink-0" />
                <span>
                  <span className="font-semibold">{Math.abs(cmp.deltas.servedOrders)} fewer order(s) served</span>{' '}
                  with the reduced fleet{cmp.deltas.totalDistanceKm < 0 ? `, saving ${km(Math.abs(cmp.deltas.totalDistanceKm))}` : ''}.
                  Distance savings here come at the cost of service, not efficiency.
                </span>
              </div>
            )}

            <div>
              <div className="grid grid-cols-[1.4fr_1fr_1fr_1fr] gap-2 pb-1 text-[11px] uppercase tracking-wide text-muted-foreground">
                <span>Metric</span><span className="text-right">Baseline</span><span className="text-right">Revised</span><span className="text-right">Δ</span>
              </div>
              <Row label="Served orders" base={`${b!.servedOrders}/${b!.totalOrders}`} revised={`${r!.servedOrders}/${r!.totalOrders}`} delta={<DeltaCell value={cmp.deltas.servedOrders} />} />
              <Row label="Unassigned" base={`${b!.totalOrders - b!.servedOrders}`} revised={`${r!.totalOrders - r!.servedOrders}`} delta={<DeltaCell value={cmp.deltas.unassigned} goodWhenNegative />} />
              <Row label="Vehicles used" base={`${b!.vehiclesUsed}`} revised={`${r!.vehiclesUsed}`} delta={<DeltaCell value={cmp.deltas.vehiclesUsed} goodWhenNegative />} />
              <Row label="Trips" base={`${b!.trips}`} revised={`${r!.trips}`} delta={<DeltaCell value={cmp.deltas.trips} goodWhenNegative />} />
              <Row label="Total distance" base={km(b!.totalDistanceKm)} revised={km(r!.totalDistanceKm)} delta={<DeltaCell value={cmp.deltas.totalDistanceKm} goodWhenNegative unit=" km" />} />
            </div>

            <ScrollArea className="max-h-64">
              <div className="space-y-3 pr-3">
                {cmp.newlyUnassigned.length > 0 && (
                  <Section title={`Newly unassigned (${cmp.newlyUnassigned.length})`} tone="warn">
                    {cmp.newlyUnassigned.map((o) => (
                      <li key={o.orderNo} className="tnum flex justify-between py-0.5"><span>{o.customer ?? o.orderNo}</span><span className="text-muted-foreground">was {o.fromVehicleRef}</span></li>
                    ))}
                  </Section>
                )}
                {cmp.nowServed.length > 0 && (
                  <Section title={`Now served (${cmp.nowServed.length})`}>
                    {cmp.nowServed.map((o) => <li key={o.orderNo} className="tnum flex justify-between py-0.5"><span>{o.customer ?? o.orderNo}</span><span className="text-muted-foreground">→ {o.toVehicleRef}</span></li>)}
                  </Section>
                )}
                {cmp.movedOrders.length > 0 && (
                  <Section title={`Reassigned (${cmp.movedOrders.length})`}>
                    {cmp.movedOrders.map((o) => (
                      <li key={o.orderNo} className="flex items-center justify-between py-0.5">
                        <span>{o.customer ?? o.orderNo}</span>
                        <span className="tnum inline-flex items-center gap-1 text-muted-foreground">{o.fromVehicleRef} <ArrowRight className="h-3 w-3" /> {o.toVehicleRef}</span>
                      </li>
                    ))}
                  </Section>
                )}
                {cmp.removedVehicles.length > 0 && (
                  <Section title={`Vehicles no longer used (${cmp.removedVehicles.length})`}>
                    <li className="tnum text-muted-foreground">{cmp.removedVehicles.join(', ')}</li>
                  </Section>
                )}
              </div>
            </ScrollArea>
          </div>
        )}
      </DialogContent>
    </Dialog>
  );
}

function Section({ title, tone, children }: { title: string; tone?: 'warn'; children: React.ReactNode }) {
  return (
    <div>
      <div className={`mb-1 text-xs font-semibold uppercase tracking-wide ${tone === 'warn' ? 'text-warning' : 'text-muted-foreground'}`}>{title}</div>
      <ul className="text-xs">{children}</ul>
    </div>
  );
}
