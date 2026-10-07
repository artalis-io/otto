import { Ban, CheckCircle2, AlertTriangle, Pin, PinOff, X, RotateCcw, SlidersHorizontal, ChevronRight } from 'lucide-react';
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs';
import { ScrollArea } from '@/components/ui/scroll-area';
import { Badge } from '@/components/ui/badge';
import { Separator } from '@/components/ui/separator';
import { hhmm, km, money, pct, ratio } from '@/lib/format';
import { NarrationPanel } from '@/components/NarrationPanel';
import { useT } from '@/i18n';
import { useState } from 'react';
import type { Plan, Scenario, Selection, Vehicle, Trip, Stop, ValidationViolation, VehicleConstraintPatch } from '@/types';

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

function TripDetail({ trip, vehicle, currency, onSelectStop }: { trip: Trip; vehicle: Vehicle; currency: string; onSelectStop: (seq: number) => void }) {
  const t = useT();
  const util = Math.max(ratio(trip.loadKg, vehicle.capacityKg), ratio(trip.loadPallets, vehicle.capacityPallets));
  const drive = trip.stops.reduce((n, s) => n + s.travelToSec, 0) + Math.max(0, trip.endSec - (trip.stops.at(-1)?.departureSec ?? trip.endSec));
  const wait = trip.stops.reduce((n, s) => n + s.waitSec, 0);
  return (
    <div className="space-y-2">
      <Field label={t('inspector.window')} value={`${hhmm(trip.startSec)}–${hhmm(trip.endSec)}`} />
      <Field label={t('inspector.distance')} value={km(trip.distanceKm)} />
      <Field label={t('inspector.load')} value={`${kg(trip.loadKg)} · ${plt(trip.loadPallets)} (${pct(util)})`} warn={util > 0.98} />
      <Field label={t('inspector.driveWait')} value={`${t('inspector.minN', { n: Math.round(drive / 60) })} · ${t('inspector.minN', { n: Math.round(wait / 60) })}`} />
      {trip.costFt != null && <Field label={t('inspector.cost')} value={money(trip.costFt, currency, true)} />}
      {trip.reloadSecAfter > 0 && <Field label={t('inspector.reloadAfter')} value={t('inspector.minN', { n: Math.round(trip.reloadSecAfter / 60) })} />}
      <Separator />
      <ol className="space-y-0.5">
        {trip.stops.map((s) => (
          <li key={s.seq}>
            <button type="button" onClick={() => onSelectStop(s.seq)} className="flex w-full items-baseline justify-between gap-2 rounded px-1 py-1 text-left text-xs hover:bg-accent">
              <span className="min-w-0 truncate"><span className="tnum text-muted-foreground">{s.seq}.</span> {s.customer ?? s.orderNo}
                <span className="tnum ml-1 text-[10px] text-muted-foreground">+{Math.round(s.travelToSec / 60)}m</span>
              </span>
              <span className="tnum shrink-0 text-muted-foreground">{hhmm(s.arrivalSec)}{s.lateBySec > 0 && <span className="ml-1 text-warning">+{Math.round(s.lateBySec / 60)}m</span>}</span>
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

/* Staged per-vehicle constraint editor: capacity, shift window, and max trips/
 * distance/duration. Blank fields keep the dataset default; changes stage on the
 * scenario and apply on the next Replan. */
function ConstraintsEditor({ vehicle, override, onSet, onClear }: {
  vehicle: Vehicle; override?: import('@/types').VehicleOverride;
  onSet: (patch: VehicleConstraintPatch) => void; onClear: () => void;
}) {
  const t = useT();
  const [open, setOpen] = useState(Boolean(override));
  const timeToSec = (v: string): number | undefined => { if (!v) return undefined; const [h, m] = v.split(':').map(Number); return h * 3600 + m * 60; };
  const secToTime = (s?: number): string => (s == null ? '' : hhmm(s));
  const has = Boolean(override);

  return (
    <div className="rounded-md border border-divider">
      <button type="button" onClick={() => setOpen((o) => !o)} className="flex w-full items-center gap-1.5 px-2 py-1.5 text-left text-[11px] font-semibold uppercase tracking-wide text-muted-foreground hover:bg-accent/40">
        <ChevronRight className={`h-3.5 w-3.5 transition-transform ${open ? 'rotate-90' : ''}`} />
        <SlidersHorizontal className="h-3.5 w-3.5" /> {t('inspector.constraints')}
        {has && <span className="ml-auto rounded bg-warning/15 px-1 text-[9px] text-warning">●</span>}
      </button>
      {open && (
        <div className="space-y-2 px-2 pb-2">
          <div className="grid grid-cols-2 gap-2">
            <Num label={t('inspector.capKg')} def={override?.capacityKg} ph={vehicle.capacityKg} onCommit={(v) => onSet({ capacityKg: v })} />
            <Num label={t('inspector.capPlt')} def={override?.capacityPallets} ph={vehicle.capacityPallets} onCommit={(v) => onSet({ capacityPallets: v })} />
            <Field2 label={t('inspector.shiftStart')}><input type="time" defaultValue={secToTime(override?.shiftEarlySec)} onChange={(e) => onSet({ shiftEarlySec: timeToSec(e.target.value) })} className="h-7 w-full rounded border border-divider bg-card px-1 text-xs" /></Field2>
            <Field2 label={t('inspector.shiftEnd')}><input type="time" defaultValue={secToTime(override?.shiftLateSec)} onChange={(e) => onSet({ shiftLateSec: timeToSec(e.target.value) })} className="h-7 w-full rounded border border-divider bg-card px-1 text-xs" /></Field2>
            <Num label={t('inspector.maxTrips')} def={override?.maxTrips} ph={vehicle.tripCount} onCommit={(v) => onSet({ maxTrips: v })} />
            <Num label={t('inspector.maxDist')} def={override?.maxDistanceKm} ph={Math.round(vehicle.distanceKm)} onCommit={(v) => onSet({ maxDistanceKm: v })} />
          </div>
          <p className="text-[10px] text-muted-foreground">{t('inspector.constraintsHint')}</p>
          {has && (
            <button type="button" onClick={onClear} className="flex w-full items-center justify-center gap-1.5 rounded border border-divider px-2 py-1 text-[11px] text-muted-foreground hover:bg-accent">
              <X className="h-3 w-3" /> {t('inspector.clearConstraints')}
            </button>
          )}
        </div>
      )}
    </div>
  );
}

function Field2({ label, children }: { label: string; children: React.ReactNode }) {
  return <label className="block"><span className="mb-0.5 block text-[10px] text-muted-foreground">{label}</span>{children}</label>;
}
function Num({ label, def, ph, onCommit }: { label: string; def?: number; ph: number; onCommit: (v: number | undefined) => void }) {
  return (
    <Field2 label={label}>
      <input type="number" defaultValue={def ?? ''} placeholder={String(ph)}
        onBlur={(e) => { const n = Number(e.target.value); onCommit(e.target.value.trim() === '' || Number.isNaN(n) ? undefined : n); }}
        onKeyDown={(e) => { if (e.key === 'Enter') (e.target as HTMLInputElement).blur(); }}
        className="h-7 w-full rounded border border-divider bg-card px-1.5 text-xs" />
    </Field2>
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

export function Inspector({ plan, scenario, selection, onSelect, onMarkUnavailable, onRestoreVehicle, onSetConstraint, onClearConstraint, solving, onPin, onUnpin, onForbid, onClearOverrides }: {
  plan: Plan; baseline: Plan; scenario: Scenario; selection: Selection; onSelect: (s: Selection) => void; onMarkUnavailable: (id: number) => void; solving: boolean;
  onRestoreVehicle: (id: number) => void;
  onSetConstraint: (vehicleId: number, patch: VehicleConstraintPatch) => void;
  onClearConstraint: (vehicleId: number) => void;
  onPin: (orderNo: string, vehicleId: number) => void;
  onUnpin: (orderNo: string) => void;
  onForbid: (orderNo: string, vehicleId: number) => void;
  onClearOverrides: () => void;
}) {
  const t = useT();
  const overrideCount = scenario.pins.length + scenario.forbids.length;
  const currency = plan.cost?.currency ?? 'HUF';
  // Unassigned order context.
  if (selection?.kind === 'unassigned') {
    const u = plan.unassigned.find((x) => x.orderNo === selection.orderNo);
    const pin = u ? scenario.pins.find((p) => p.orderNo === u.orderNo) : undefined;
    const refOf = (id: number): string => plan.vehicles.find((v) => v.id === id)?.ref ?? `#${id}`;
    return (
      <Panel>
        {u ? (
          <div className="space-y-2 p-3">
            <div className="text-sm font-semibold">{u.customer ?? `Order ${u.orderNo}`}</div>
            <div className="tnum text-xs text-muted-foreground">#{u.orderNo}{u.city ? ` · ${u.city}` : ''}</div>
            <Separator />
            <Badge variant="warning">{t('inspector.unassignedBadge')}</Badge>
            <p className="text-xs text-muted-foreground">{u.reason ?? t('inspector.unassignedReason')}</p>
            {/* Assign by hand: pin this unassigned order to a chosen vehicle, applied on Replan. */}
            <div className="space-y-2 rounded-md border border-divider bg-muted/30 p-2.5">
              <div className="text-[11px] font-semibold uppercase tracking-wide text-muted-foreground">{t('inspector.assignByHand')}</div>
              {pin ? (
                <div className="flex items-center justify-between gap-2">
                  <Badge variant="outline" className="gap-1"><Pin className="h-3 w-3" /> {t('inspector.pinned', { ref: refOf(pin.vehicleId) })}</Badge>
                  <button type="button" onClick={() => onUnpin(u.orderNo)} className="inline-flex items-center gap-1 rounded border border-divider px-1.5 py-0.5 text-[11px] hover:bg-accent"><PinOff className="h-3 w-3" /> {t('inspector.unpin')}</button>
                </div>
              ) : (
                <select defaultValue="" onChange={(e) => { const id = Number(e.target.value); if (id) onPin(u.orderNo, id); }}
                  className="h-7 w-full rounded border border-divider bg-card px-1.5 text-xs">
                  <option value="" disabled>{t('inspector.assignTo')}</option>
                  {plan.vehicles.map((v) => <option key={v.id} value={v.id}>{v.ref}{v.vehicleClass ? ` · ${v.vehicleClass.replace(/_/g, ' ')}` : ''}</option>)}
                </select>
              )}
              <p className="text-[10px] text-muted-foreground">{t('inspector.overrideHint')}</p>
            </div>
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
            <button type="button" onClick={() => onSelect({ kind: 'vehicle', vehicleId: vehicle.id })}
              className={`font-semibold ${trip ? 'hover:underline' : ''}`} title={trip ? t('inspector.backToVehicle') : undefined}>{vehicle.ref}</button>
            {vehicle.vehicleClass && <Badge variant="outline">{vehicle.vehicleClass.replace(/_/g, ' ')}</Badge>}
            {vehicle.isSubcontractor && <Badge variant="warning">{t('inspector.subcontractor')}</Badge>}
            {stop && trip ? (
              <button type="button" onClick={() => onSelect({ kind: 'trip', vehicleId: vehicle.id, tripIndex: trip.index })}
                className="tnum ml-auto text-xs text-muted-foreground hover:underline">{t('inspector.tripN', { n: trip.index + 1 })} · {t('inspector.stopN', { n: stop.seq })}</button>
            ) : trip ? <span className="tnum ml-auto text-xs text-muted-foreground">{t('inspector.tripN', { n: trip.index + 1 })}</span> : null}
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
                : trip ? <TripDetail trip={trip} vehicle={vehicle} currency={currency} onSelectStop={(seq) => onSelect({ kind: 'stop', vehicleId: vehicle.id, tripIndex: trip.index, seq })} />
                  : <VehicleSummary vehicle={vehicle} plan={plan} onSelectTrip={(i) => onSelect({ kind: 'trip', vehicleId: vehicle.id, tripIndex: i })} />}
              {stop && (
                <AssignmentSection plan={plan} scenario={scenario} orderNo={stop.orderNo} currentVehicleId={vehicle.id}
                  onPin={onPin} onUnpin={onUnpin} onForbid={onForbid} />
              )}
              <Separator />
              <ValidationRow plan={plan} vehicle={vehicle} />
              {!stop && !trip && (
                <ConstraintsEditor vehicle={vehicle}
                  override={scenario.vehicleOverrides.find((o) => o.vehicleId === vehicle.id)}
                  onSet={(patch) => onSetConstraint(vehicle.id, patch)} onClear={() => onClearConstraint(vehicle.id)} />
              )}
              {overrideCount > 0 && (
                <button type="button" onClick={onClearOverrides}
                  className="flex w-full items-center justify-center gap-1.5 rounded border border-divider px-2 py-1 text-[11px] text-muted-foreground hover:bg-accent">
                  <X className="h-3 w-3" /> {t('inspector.clearOverrides')} ({overrideCount})
                </button>
              )}
              {scenario.removedVehicleIds.includes(vehicle.id) ? (
                <button type="button" disabled={solving} onClick={() => onRestoreVehicle(vehicle.id)}
                  className="flex w-full items-center justify-center gap-1.5 rounded border border-divider px-2 py-1.5 text-xs font-medium text-muted-foreground hover:bg-accent disabled:opacity-40">
                  <RotateCcw className="h-3.5 w-3.5" /> {t('fleet.restore')}
                </button>
              ) : (
                <button type="button" disabled={solving} onClick={() => onMarkUnavailable(vehicle.id)}
                  className="flex w-full items-center justify-center gap-1.5 rounded border border-warning/40 px-2 py-1.5 text-xs font-medium text-warning hover:bg-warning/10 disabled:opacity-40">
                  <Ban className="h-3.5 w-3.5" /> {t('inspector.markUnavailable', { ref: vehicle.ref })}
                </button>
              )}
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
