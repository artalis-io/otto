import { useState } from 'react';
import { ChevronRight, Search } from 'lucide-react';
import { Input } from '@/components/ui/input';
import { ScrollArea } from '@/components/ui/scroll-area';
import { Card } from '@/components/ui/card';
import { Separator } from '@/components/ui/separator';
import { hhmm, km, ratio, pct } from '@/lib/format';
import type { Plan, Vehicle, Selection } from '@/types';

function CapacityBar({ label, used, cap, color }: { label: string; used: number; cap: number; color: string }) {
  const r = ratio(used, cap);
  return (
    <div className="flex items-center gap-2">
      <span className="w-8 text-[10px] uppercase text-muted-foreground">{label}</span>
      <div className="h-1.5 flex-1 overflow-hidden rounded-full bg-muted">
        <div className="h-full rounded-full" style={{ width: `${r * 100}%`, backgroundColor: color }} />
      </div>
      <span className="tnum w-9 text-right text-[10px] text-muted-foreground">{pct(r)}</span>
    </div>
  );
}

function VehicleCard({
  vehicle,
  selected,
  onSelect,
}: {
  vehicle: Vehicle;
  selected: boolean;
  onSelect: () => void;
}) {
  return (
    <Card
      onClick={onSelect}
      className={`cursor-pointer p-2.5 transition-colors hover:bg-accent/60 ${
        selected ? 'ring-1 ring-ring' : ''
      }`}
    >
      <div className="flex items-center gap-2">
        <span className="h-3 w-3 shrink-0 rounded-sm" style={{ backgroundColor: vehicle.color }} />
        <span className="font-semibold">{vehicle.ref}</span>
        <span className="text-[11px] text-muted-foreground">{vehicle.class.replace(/_/g, ' ')}</span>
        <span className="tnum ml-auto text-[11px] text-muted-foreground">
          {vehicle.tripCount} {vehicle.tripCount === 1 ? 'trip' : 'trips'}
        </span>
      </div>
      <div className="mt-2 space-y-1">
        <CapacityBar label="kg" used={vehicle.peakKg} cap={vehicle.capacityKg} color={vehicle.color} />
        <CapacityBar label="plt" used={vehicle.peakPallets} cap={vehicle.capacityPallets} color={vehicle.color} />
      </div>
      <div className="mt-2 flex items-center justify-between text-[11px] text-muted-foreground">
        <span className="tnum">{km(vehicle.distanceKm)}</span>
        <span className="tnum">finish {hhmm(vehicle.finishTimeSec)}</span>
      </div>
    </Card>
  );
}

/*
 * Left fleet panel: search, scrollable vehicle cards, and a collapsible
 * "Unassigned orders" section (collapsed by default). Search filters by ref /
 * class locally; selection is lifted to App.
 */
export function FleetPanel({
  plan,
  selection,
  onSelect,
}: {
  plan: Plan;
  selection: Selection;
  onSelect: (sel: Selection) => void;
}) {
  const [query, setQuery] = useState('');
  const [unassignedOpen, setUnassignedOpen] = useState(false);

  const q = query.trim().toLowerCase();
  const vehicles = q
    ? plan.vehicles.filter(
        (v) => v.ref.toLowerCase().includes(q) || v.class.toLowerCase().includes(q)
      )
    : plan.vehicles;

  const selectedId = selection?.kind === 'vehicle' ? selection.vehicleId : null;

  return (
    <div className="flex h-full flex-col bg-card">
      <div className="shrink-0 p-3">
        <h2 className="mb-2 text-xs font-semibold uppercase tracking-wide text-muted-foreground">
          Fleet
        </h2>
        <div className="relative">
          <Search className="pointer-events-none absolute left-2.5 top-1/2 h-3.5 w-3.5 -translate-y-1/2 text-muted-foreground" />
          <Input
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            placeholder="Search vehicles"
            className="h-8 pl-8"
          />
        </div>
      </div>
      <Separator />
      <ScrollArea className="min-h-0 flex-1">
        <div className="space-y-2 p-3">
          {vehicles.map((v) => (
            <VehicleCard
              key={v.id}
              vehicle={v}
              selected={selectedId === v.id}
              onSelect={() => onSelect({ kind: 'vehicle', vehicleId: v.id })}
            />
          ))}
          {vehicles.length === 0 && (
            <p className="px-1 py-6 text-center text-xs text-muted-foreground">No vehicles match.</p>
          )}
        </div>
      </ScrollArea>
      <Separator />
      <div className="shrink-0">
        <button
          type="button"
          onClick={() => setUnassignedOpen((o) => !o)}
          className="flex w-full items-center gap-2 px-3 py-2 text-left text-xs font-semibold uppercase tracking-wide text-muted-foreground hover:bg-accent/40"
        >
          <ChevronRight
            className={`h-3.5 w-3.5 transition-transform ${unassignedOpen ? 'rotate-90' : ''}`}
          />
          Unassigned orders
          <span className="tnum ml-auto rounded bg-warning/15 px-1.5 text-warning">
            {plan.unassigned.length}
          </span>
        </button>
        {unassignedOpen && (
          <div className="max-h-40 overflow-y-auto px-3 pb-3">
            {plan.unassigned.map((u) => (
              <div key={u.orderId} className="border-t border-divider py-1.5 text-xs">
                <div className="font-medium">{u.customer}</div>
                <div className="text-muted-foreground">{u.reason}</div>
              </div>
            ))}
            {plan.unassigned.length === 0 && (
              <p className="py-2 text-xs text-muted-foreground">All orders assigned.</p>
            )}
          </div>
        )}
      </div>
    </div>
  );
}
