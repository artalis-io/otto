import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs';
import { ScrollArea } from '@/components/ui/scroll-area';
import { Badge } from '@/components/ui/badge';
import { Separator } from '@/components/ui/separator';
import { hhmm, km, pct, ratio } from '@/lib/format';
import type { Plan, Selection, Vehicle } from '@/types';

function Field({ label, value }: { label: string; value: string }) {
  return (
    <div className="flex items-baseline justify-between py-1 text-sm">
      <span className="text-muted-foreground">{label}</span>
      <span className="tnum font-medium">{value}</span>
    </div>
  );
}

function Empty() {
  return (
    <div className="flex h-full items-center justify-center p-6 text-center text-sm text-muted-foreground">
      Select a vehicle to inspect its trips and load.
    </div>
  );
}

function DetailsTab({ vehicle }: { vehicle: Vehicle }) {
  return (
    <div className="space-y-3">
      <div className="flex items-center gap-2">
        <span className="h-3 w-3 rounded-sm" style={{ backgroundColor: vehicle.color }} />
        <span className="font-semibold">{vehicle.ref}</span>
        <Badge variant="outline">{vehicle.class.replace(/_/g, ' ')}</Badge>
      </div>
      <Separator />
      <div>
        <Field label="Trips" value={`${vehicle.tripCount}`} />
        <Field label="Distance" value={km(vehicle.distanceKm)} />
        <Field label="Finish" value={hhmm(vehicle.finishTimeSec)} />
      </div>
      <Separator />
      <div className="space-y-2">
        {vehicle.trips.map((t) => (
          <div key={t.index} className="rounded-md border border-divider p-2">
            <div className="mb-1 flex items-center justify-between text-xs">
              <span className="font-semibold">Trip {t.index + 1}</span>
              <span className="tnum text-muted-foreground">
                {hhmm(t.startSec)}–{hhmm(t.endSec)} · {km(t.distanceKm)}
              </span>
            </div>
            <ol className="space-y-0.5">
              {t.stops.map((s) => (
                <li key={s.seq} className="flex items-baseline justify-between text-xs">
                  <span>
                    <span className="tnum text-muted-foreground">{s.seq}.</span> {s.customer}
                  </span>
                  <span className="tnum text-muted-foreground">
                    {hhmm(s.arrivalSec)}
                    {s.lateBySec > 0 && <span className="ml-1 text-warning">late</span>}
                  </span>
                </li>
              ))}
            </ol>
          </div>
        ))}
      </div>
    </div>
  );
}

function LoadTab({ vehicle }: { vehicle: Vehicle }) {
  return (
    <div className="space-y-3">
      <p className="text-xs text-muted-foreground">
        Aggregate scalar capacity only. Physical (3L) loading feasibility has not been checked.
      </p>
      <div className="space-y-2">
        {[
          { label: 'Weight', used: vehicle.peakKg, cap: vehicle.capacityKg, unit: 'kg' },
          { label: 'Pallets', used: vehicle.peakPallets, cap: vehicle.capacityPallets, unit: 'plt' },
        ].map((d) => {
          const r = ratio(d.used, d.cap);
          return (
            <div key={d.label}>
              <div className="mb-1 flex items-baseline justify-between text-sm">
                <span className="text-muted-foreground">{d.label}</span>
                <span className="tnum font-medium">
                  {d.used.toLocaleString('en-US')} / {d.cap.toLocaleString('en-US')} {d.unit} ({pct(r)})
                </span>
              </div>
              <div className="h-2 overflow-hidden rounded-full bg-muted">
                <div
                  className="h-full rounded-full"
                  style={{ width: `${r * 100}%`, backgroundColor: vehicle.color }}
                />
              </div>
            </div>
          );
        })}
      </div>
    </div>
  );
}

/*
 * Right inspector: contextual Details / Load tabs for the selected vehicle.
 * Scaffold shows the selected vehicle only (trip/stop selection to be wired).
 */
export function Inspector({ plan, selection }: { plan: Plan; selection: Selection }) {
  const vehicleId = selection?.kind ? selection.vehicleId : null;
  const vehicle = vehicleId != null ? plan.vehicles.find((v) => v.id === vehicleId) : undefined;

  return (
    <div className="flex h-full flex-col bg-card">
      <div className="shrink-0 px-3 py-2">
        <h2 className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">
          Inspector
        </h2>
      </div>
      <Separator />
      {!vehicle ? (
        <Empty />
      ) : (
        <Tabs defaultValue="details" className="flex min-h-0 flex-1 flex-col">
          <div className="px-3 pt-3">
            <TabsList className="w-full">
              <TabsTrigger value="details" className="flex-1">
                Details
              </TabsTrigger>
              <TabsTrigger value="load" className="flex-1">
                Load
              </TabsTrigger>
            </TabsList>
          </div>
          <ScrollArea className="min-h-0 flex-1">
            <div className="p-3">
              <TabsContent value="details" className="mt-0">
                <DetailsTab vehicle={vehicle} />
              </TabsContent>
              <TabsContent value="load" className="mt-0">
                <LoadTab vehicle={vehicle} />
              </TabsContent>
            </div>
          </ScrollArea>
        </Tabs>
      )}
    </div>
  );
}
