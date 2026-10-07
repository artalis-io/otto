import { Ban, CheckCircle2, AlertTriangle, Pin, PinOff, X } from 'lucide-react';
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs';
import { ScrollArea } from '@/components/ui/scroll-area';
import { Badge } from '@/components/ui/badge';
import { Separator } from '@/components/ui/separator';
import { hhmm, km, money, pct, ratio } from '@/lib/format';
import { NarrationPanel } from '@/components/NarrationPanel';
import { useT } from '@/i18n';
import type { Plan, Scenario, Selection, Vehicle, Trip, Stop, ValidationViolation } from '@/types';

function Field({ label, value, warn }: { label: string; value: string; warn?: boolean }) {
  return (
    <div className="flex items-baseline justify-between py-1 text-sm">
      <span className="text-muted-foreground">{label}</span>
      <span className={`tnum font-medium ${warn ? 'text-warning' : ''}`}>{value}</span>
    </div>
  );
}
const kg = (n: number) => `${Math.round(n).toLocaleString('en-US')} kg`;
const plt = (n: number) => `${(Math.round(n * 10) / 10).toLocaleString('en-US')} plt`;

function StopDetail({ stop }: { stop: Stop }) {
  const t = useT();
  const late = stop.lateBySec > 0;
  return (
    <div className="space-y-2">
      <div>
        <div className="text-sm font-semibold">{stop.customer ?? `Order ${stop.orderNo}`}</div>
        <div className="tnum text-xs text-muted-foreground">#{stop.orderNo}{stop.city ? ` · ${stop.city}` : ''}</div>
      </div>
      <Separator />
      <Field label={t('inspector.arrival')} value={hhmm(stop.arrivalSec)} warn={late} />
      <Field label={t('inspector.service')} value={`${hhmm(stop.serviceStartSec)}–${hhmm(stop.departureSec)}`} />
      <Field label={t('inspector.timeWindow')} value={`${hhmm(stop.twStartSec)}–${hhmm(stop.twEndSec)}`} />
      {stop.waitSec > 0 && <Field label={t('inspector.wait')} value={t('inspector.minN', { n: Math.round(stop.waitSec / 60) })} />}
      {late && <Field label={t('inspector.lateBy')} value={t('inspector.minN', { n: Math.round(stop.lateBySec / 60) })} warn />}
      <Separator />
      <Field label={t('inspector.weight')} value={kg(stop.weightKg)} />
      <Field label={t('inspector.pallets')} value={plt(stop.pallets)} />
      <Field label={t('inspector.serviceTime')} value={t('inspector.minN', { n: stop.serviceMin })} />
      <Field label={t('inspector.travelTo')} value={t('inspector.minN', { n: Math.round(stop.travelToSec / 60) })} />
    </div>
  );
}

function TripDetail({ trip, onSelectStop }: { trip: Trip; onSelectStop: (seq: number) => void }) {
  const t = useT();
  return (
    <div className="space-y-2">
      <Field label={t('inspector.window')} value={`${hhmm(trip.startSec)}–${hhmm(trip.endSec)}`} />
      <Field label={t('inspector.distance')} value={km(trip.distanceKm)} />
      <Field label={t('inspector.load')} value={`${kg(trip.loadKg)} · ${plt(trip.loadPallets)}`} />
      {trip.reloadSecAfter > 0 && <Field label={t('inspector.reloadAfter')} value={t('inspector.minN', { n: Math.round(trip.reloadSecAfter / 60) })} />}
      <Separator />
      <ol className="space-y-0.5">
        {trip.stops.map((s) => (
          <li key={s.seq}>
            <button type="button" onClick={() => onSelectStop(s.seq)} className="flex w-full items-baseline justify-between rounded px-1 py-0.5 text-left text-xs hover:bg-accent">
              <span><span className="tnum text-muted-foreground">{s.seq}.</span> {s.customer ?? s.orderNo}</span>
              <span className="tnum text-muted-foreground">{hhmm(s.arrivalSec)}{s.lateBySec > 0 && <span className="ml-1 text-warning">late</span>}</span>
            </button>
          </li>
        ))}
      </ol>
    </div>
  );
}

/* Per-stop manual assignment overrides: pin an order to its current vehicle,
 * move it to another, or forbid a vehicle. These accumulate on the scenario and
 * take effect on the next Replan (they do not solve immediately). */
function AssignmentSection({ plan, scenario, orderNo, currentVehicleId, onPin, onUnpin, onForbid }: {
  plan: Plan; scenario: Scenario; orderNo: string; currentVehicleId: number;
  onPin: (orderNo: string, vehicleId: number) => void;
  onUnpin: (orderNo: string) => void;
  onForbid: (orderNo: string, vehicleId: number) => void;
}) {
  const t = useT();
  const refOf = (id: number): string => plan.vehicles.find((v) => v.id === id)?.ref ?? `#${id}`;
  const pin = scenario.pins.find((p) => p.orderNo === orderNo);
  const forbidden = scenario.forbids.filter((f) => f.orderNo === orderNo).map((f) => f.vehicleId);
  const curRef = refOf(currentVehicleId);
  const others = plan.vehicles.filter((v) => v.id !== currentVehicleId);

  return (
    <div className="space-y-2 rounded-md border border-divider bg-muted/30 p-2.5">
      <div className="text-[11px] font-semibold uppercase tracking-wide text-muted-foreground">{t('inspector.assignment')}</div>
      {pin ? (
        <div className="flex items-center justify-between gap-2">
          <Badge variant="outline" className="gap-1"><Pin className="h-3 w-3" /> {t('inspector.pinned', { ref: refOf(pin.vehicleId) })}</Badge>
          <button type="button" onClick={() => onUnpin(orderNo)} className="inline-flex items-center gap-1 rounded border border-divider px-1.5 py-0.5 text-[11px] hover:bg-accent">
            <PinOff className="h-3 w-3" /> {t('inspector.unpin')}
          </button>
        </div>
      ) : (
        <button type="button" onClick={() => onPin(orderNo, currentVehicleId)}
          className="flex w-full items-center justify-center gap-1.5 rounded border border-divider px-2 py-1 text-xs hover:bg-accent">
          <Pin className="h-3.5 w-3.5" /> {t('inspector.pinHere', { ref: curRef })}
        </button>
      )}

      <div className="flex items-center gap-1.5">
        <select defaultValue="" onChange={(e) => { const id = Number(e.target.value); if (id) { onPin(orderNo, id); e.target.value = ''; } }}
          className="h-7 min-w-0 flex-1 rounded border border-divider bg-card px-1.5 text-xs">
          <option value="" disabled>{t('inspector.moveTo')}</option>
          {others.map((v) => <option key={v.id} value={v.id}>{v.ref}{v.vehicleClass ? ` · ${v.vehicleClass.replace(/_/g, ' ')}` : ''}</option>)}
        </select>
        <button type="button" onClick={() => onForbid(orderNo, currentVehicleId)} title={t('inspector.forbidHere', { ref: curRef })}
          className="inline-flex items-center gap-1 rounded border border-warning/40 px-1.5 py-1 text-[11px] text-warning hover:bg-warning/10">
          <Ban className="h-3 w-3" /> {t('inspector.forbidHere', { ref: curRef })}
        </button>
      </div>

      {forbidden.length > 0 && (
        <div className="text-[11px] text-muted-foreground">{t('inspector.forbiddenN', { refs: forbidden.map(refOf).join(', ') })}</div>
      )}
      <p className="text-[10px] text-muted-foreground">{t('inspector.overrideHint')}</p>
    </div>
  );
}

function VehicleSummary({ vehicle, plan, onSelectTrip }: { vehicle: Vehicle; plan: Plan; onSelectTrip: (i: number) => void }) {
  const t = useT();
  const vcost = plan.cost?.perVehicle.find((c) => c.vehicleId === vehicle.id);
  return (
    <div className="space-y-2">
      <Field label={t('inspector.trips')} value={`${vehicle.tripCount}`} />
      <Field label={t('inspector.distance')} value={km(vehicle.distanceKm)} />
      <Field label={t('inspector.finish')} value={hhmm(vehicle.finishTimeSec)} />
      <Field label={t('inspector.capacity')} value={`${kg(vehicle.capacityKg)} · ${plt(vehicle.capacityPallets)}`} />
      {vcost && plan.cost && <Field label={t('inspector.cost')} value={money(vcost.totalCost, plan.cost.currency, true)} />}
      <Separator />
      <div className="space-y-1">
        {vehicle.trips.map((trip) => (
          <button key={trip.index} type="button" onClick={() => onSelectTrip(trip.index)} className="flex w-full items-center justify-between rounded border border-divider px-2 py-1 text-left text-xs hover:bg-accent">
            <span className="font-medium">{t('fleet.tripLabel', { n: trip.index + 1 })} · <span className="text-muted-foreground">{t('fleet.stopsN', { n: trip.stops.length })}</span></span>
            <span className="tnum text-muted-foreground">{hhmm(trip.startSec)}–{hhmm(trip.endSec)}</span>
          </button>
        ))}
      </div>
    </div>
  );
}

function ValidationRow({ plan, vehicle }: { plan: Plan; vehicle: Vehicle }) {
  const t = useT();
  const vios = (plan.provenance.validation.violations as ValidationViolation[]).filter((v) => v.vehicle_id === vehicle.id);
  if (vios.length === 0) return (
    <div className="flex items-center gap-1.5 text-xs text-primary"><CheckCircle2 className="h-3.5 w-3.5" /> {t('inspector.noViolations')}</div>
  );
  return (
    <div className="space-y-0.5 text-xs text-warning">
      {vios.map((v, i) => (
        <div key={i} className="flex items-center gap-1.5"><AlertTriangle className="h-3.5 w-3.5" /> {v.type}{v.dimension ? ` (${v.dimension})` : ''}</div>
      ))}
    </div>
  );
}

function LoadTab({ vehicle }: { vehicle: Vehicle }) {
  const t = useT();
  return (
    <div className="space-y-3">
      <p className="rounded border border-divider bg-muted/40 p-2 text-[11px] text-muted-foreground">
        {t('inspector.loadNote')}
      </p>
      {vehicle.trips.map((trip) => (
        <div key={trip.index} className="space-y-1.5">
          <div className="text-xs font-semibold">{t('fleet.tripLabel', { n: trip.index + 1 })}</div>
          {[{ label: t('inspector.weight'), used: trip.loadKg, cap: vehicle.capacityKg, fmt: kg },
            { label: t('inspector.pallets'), used: trip.loadPallets, cap: vehicle.capacityPallets, fmt: plt }].map((d) => {
            const r = ratio(d.used, d.cap);
            return (
              <div key={d.label}>
                <div className="mb-0.5 flex items-baseline justify-between text-[11px]">
                  <span className="text-muted-foreground">{d.label}</span>
                  <span className="tnum">{d.fmt(d.used)} / {d.fmt(d.cap)} ({pct(r)})</span>
                </div>
                <div className="h-1.5 overflow-hidden rounded-full bg-muted">
                  <div className="h-full rounded-full" style={{ width: `${r * 100}%`, backgroundColor: r > 0.98 ? '#d97706' : vehicle.color }} />
                </div>
              </div>
            );
          })}
        </div>
      ))}
    </div>
  );
}

export function Inspector({ plan, scenario, selection, onMarkUnavailable, solving, onPin, onUnpin, onForbid, onClearOverrides }: {
  plan: Plan; baseline: Plan; scenario: Scenario; selection: Selection; onMarkUnavailable: (id: number) => void; solving: boolean;
  onPin: (orderNo: string, vehicleId: number) => void;
  onUnpin: (orderNo: string) => void;
  onForbid: (orderNo: string, vehicleId: number) => void;
  onClearOverrides: () => void;
}) {
  const t = useT();
  const overrideCount = scenario.pins.length + scenario.forbids.length;
  // Unassigned order context.
  if (selection?.kind === 'unassigned') {
    const u = plan.unassigned.find((x) => x.orderNo === selection.orderNo);
    return (
      <Panel>
        {u ? (
          <div className="space-y-2 p-3">
            <div className="text-sm font-semibold">{u.customer ?? `Order ${u.orderNo}`}</div>
            <div className="tnum text-xs text-muted-foreground">#{u.orderNo}{u.city ? ` · ${u.city}` : ''}</div>
            <Separator />
            <Badge variant="warning">{t('inspector.unassignedBadge')}</Badge>
            <p className="text-xs text-muted-foreground">{u.reason ?? t('inspector.unassignedReason')}</p>
          </div>
        ) : <Empty planId={plan.id} />}
      </Panel>
    );
  }

  const vehicleId = selection ? selection.vehicleId : null;
  const vehicle = vehicleId != null ? plan.vehicles.find((v) => v.id === vehicleId) : undefined;
  if (!vehicle) return <Panel><Empty planId={plan.id} /></Panel>;

  const trip = selection && (selection.kind === 'trip' || selection.kind === 'stop')
    ? vehicle.trips.find((t) => t.index === selection.tripIndex) : undefined;
  const stop = selection?.kind === 'stop' && trip ? trip.stops.find((s) => s.seq === selection.seq) : undefined;

  return (
    <Panel>
      <Tabs defaultValue="details" className="flex min-h-0 flex-1 flex-col">
        <div className="px-3 pt-3">
          <div className="mb-2 flex items-center gap-2">
            <span className="h-3 w-3 rounded-sm" style={{ backgroundColor: vehicle.color }} />
            <span className="font-semibold">{vehicle.ref}</span>
            {vehicle.vehicleClass && <Badge variant="outline">{vehicle.vehicleClass.replace(/_/g, ' ')}</Badge>}
            {vehicle.isSubcontractor && <Badge variant="warning">{t('inspector.subcontractor')}</Badge>}
            {stop ? <span className="tnum ml-auto text-xs text-muted-foreground">{t('inspector.stopN', { n: stop.seq })}</span>
              : trip ? <span className="tnum ml-auto text-xs text-muted-foreground">{t('inspector.tripN', { n: trip.index + 1 })}</span> : null}
          </div>
          <TabsList className="w-full">
            <TabsTrigger value="details" className="flex-1">{t('inspector.tabDetails')}</TabsTrigger>
            <TabsTrigger value="load" className="flex-1">{t('inspector.tabLoad')}</TabsTrigger>
          </TabsList>
        </div>
        <ScrollArea className="min-h-0 flex-1">
          <div className="space-y-3 p-3">
            <TabsContent value="details" className="mt-0 space-y-3">
              {stop ? <StopDetail stop={stop} />
                : trip ? <TripDetail trip={trip} onSelectStop={() => { /* handled by timeline/map */ }} />
                  : <VehicleSummary vehicle={vehicle} plan={plan} onSelectTrip={() => { /* handled by fleet */ }} />}
              {stop && (
                <AssignmentSection plan={plan} scenario={scenario} orderNo={stop.orderNo} currentVehicleId={vehicle.id}
                  onPin={onPin} onUnpin={onUnpin} onForbid={onForbid} />
              )}
              <Separator />
              <ValidationRow plan={plan} vehicle={vehicle} />
              {overrideCount > 0 && (
                <button type="button" onClick={onClearOverrides}
                  className="flex w-full items-center justify-center gap-1.5 rounded border border-divider px-2 py-1 text-[11px] text-muted-foreground hover:bg-accent">
                  <X className="h-3 w-3" /> {t('inspector.clearOverrides')} ({overrideCount})
                </button>
              )}
              <button type="button" disabled={solving} onClick={() => onMarkUnavailable(vehicle.id)}
                className="flex w-full items-center justify-center gap-1.5 rounded border border-warning/40 px-2 py-1.5 text-xs font-medium text-warning hover:bg-warning/10 disabled:opacity-40">
                <Ban className="h-3.5 w-3.5" /> {t('inspector.markUnavailable', { ref: vehicle.ref })}
              </button>
            </TabsContent>
            <TabsContent value="load" className="mt-0">
              <LoadTab vehicle={vehicle} />
            </TabsContent>
          </div>
        </ScrollArea>
      </Tabs>
    </Panel>
  );
}

function Panel({ children }: { children: React.ReactNode }) {
  const t = useT();
  return (
    <div className="flex h-full flex-col bg-card">
      <div className="shrink-0 px-3 py-2"><h2 className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">{t('inspector.title')}</h2></div>
      <Separator />
      {children}
    </div>
  );
}
function Empty({ planId }: { planId: string }) {
  const t = useT();
  return (
    <div className="flex h-full flex-col gap-4 p-3">
      <NarrationPanel planId={planId} />
      <p className="px-3 text-center text-sm text-muted-foreground">{t('inspector.empty')}</p>
    </div>
  );
}
