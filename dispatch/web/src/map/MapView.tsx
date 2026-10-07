import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import Map, {
  Layer, Source, type MapRef, type LayerProps, type MapLayerMouseEvent,
} from 'react-map-gl/maplibre';
import maplibregl from 'maplibre-gl';
import type { FeatureCollection, LineString } from 'geojson';
import { Minus, Plus, Maximize2 } from 'lucide-react';
import 'maplibre-gl/dist/maplibre-gl.css';
import { cartaStyle } from './cartaStyle';
import type { Plan, Selection } from '@/types';

/*
 * The dispatch map. Basemap is the reused "Carta Quiet" style; routes, stops and
 * the depot are declarative overlays. Selection drives styling (highlight the
 * selected vehicle/trip, fade the rest) and the camera (fit on deliberate
 * selection; preserved across background plan updates like geometry fills).
 *
 * transformRequest is load-bearing (copied from the Carta viewer): the style's
 * root-relative tile/glyph URLs fail to parse inside MapLibre's Web Worker, so
 * we resolve them against the page origin.
 */

const transformRequest = (url: string) => ({ url: url.startsWith('/') ? window.location.origin + url : url });

function routesFC(plan: Plan): FeatureCollection {
  return {
    type: 'FeatureCollection',
    features: plan.vehicles.flatMap((v) =>
      v.trips.filter((t) => t.geometry).map((t) => ({
        type: 'Feature' as const,
        id: v.id * 100 + t.index,
        properties: { vehicleId: v.id, color: v.color, tripIndex: t.index },
        geometry: t.geometry as LineString,
      }))),
  };
}
function stopsFC(plan: Plan): FeatureCollection {
  return {
    type: 'FeatureCollection',
    features: plan.vehicles.flatMap((v) =>
      v.trips.flatMap((t) =>
        t.stops.map((s) => ({
          type: 'Feature' as const,
          properties: { vehicleId: v.id, color: v.color, tripIndex: t.index, seq: s.seq },
          geometry: { type: 'Point' as const, coordinates: [s.lon, s.lat] },
        })))),
  };
}
function pointFC(lon: number, lat: number, props: Record<string, unknown> = {}): FeatureCollection {
  return { type: 'FeatureCollection', features: [{ type: 'Feature', properties: props, geometry: { type: 'Point', coordinates: [lon, lat] } }] };
}

function selVehicle(sel: Selection): number | null {
  return sel && sel.kind !== 'unassigned' ? sel.vehicleId : null;
}
function selTripIndex(sel: Selection): number | null {
  return sel && (sel.kind === 'trip' || sel.kind === 'stop') ? sel.tripIndex : null;
}

/* Point at fraction `frac` (0..1) along a polyline, by cumulative segment length. */
function pointAlong(coords: [number, number][], frac: number): [number, number] {
  if (coords.length === 0) return [0, 0];
  if (coords.length === 1 || frac <= 0) return coords[0]!;
  if (frac >= 1) return coords[coords.length - 1]!;
  const seg: number[] = []; let total = 0;
  for (let i = 1; i < coords.length; i++) {
    const dx = coords[i]![0] - coords[i - 1]![0], dy = coords[i]![1] - coords[i - 1]![1];
    const d = Math.hypot(dx, dy); seg.push(d); total += d;
  }
  let target = frac * total;
  for (let i = 0; i < seg.length; i++) {
    if (target <= seg[i]!) {
      const r = seg[i]! === 0 ? 0 : target / seg[i]!;
      return [coords[i]![0] + (coords[i + 1]![0] - coords[i]![0]) * r, coords[i]![1] + (coords[i + 1]![1] - coords[i]![1]) * r];
    }
    target -= seg[i]!;
  }
  return coords[coords.length - 1]!;
}

/* Active vehicle positions at `clockSec`: interpolated along each trip's road
 * geometry by the trip's elapsed-time fraction. */
function playheadFC(plan: Plan, clockSec: number | null): FeatureCollection {
  const features: FeatureCollection['features'] = [];
  if (clockSec == null) return { type: 'FeatureCollection', features };
  for (const v of plan.vehicles) {
    for (const t of v.trips) {
      if (clockSec < t.startSec || clockSec > t.endSec || !t.geometry) continue;
      const frac = (clockSec - t.startSec) / Math.max(1, t.endSec - t.startSec);
      const [lon, lat] = pointAlong(t.geometry.coordinates as [number, number][], frac);
      features.push({ type: 'Feature', properties: { color: v.color, ref: v.ref }, geometry: { type: 'Point', coordinates: [lon, lat] } });
      break;
    }
  }
  return { type: 'FeatureCollection', features };
}

export function MapView({ plan, selection, onSelect, clockSec }: { plan: Plan; selection: Selection; onSelect: (s: Selection) => void; clockSec: number | null }) {
  const mapRef = useRef<MapRef | null>(null);
  // Respect reduced-motion: snap the camera instead of animating.
  const reduced = useRef(typeof window !== 'undefined' && window.matchMedia('(prefers-reduced-motion: reduce)').matches);
  const dur = (ms: number) => (reduced.current ? 0 : ms);
  const style = useMemo(() => cartaStyle(), []);
  const routes = useMemo(() => routesFC(plan), [plan]);
  const stops = useMemo(() => stopsFC(plan), [plan]);
  const depot = useMemo(() => pointFC(plan.depot.lon, plan.depot.lat), [plan.depot.lon, plan.depot.lat]);
  const playhead = useMemo(() => playheadFC(plan, clockSec), [plan, clockSec]);
  const [hovering, setHovering] = useState(false);

  const vId = selVehicle(selection);
  const tIdx = selTripIndex(selection);

  // Match expression for the selected subset (vehicle, or vehicle+trip).
  const match = useMemo(() => {
    if (vId == null) return null;
    return tIdx == null
      ? (['==', ['get', 'vehicleId'], vId] as unknown)
      : (['all', ['==', ['get', 'vehicleId'], vId], ['==', ['get', 'tripIndex'], tIdx]] as unknown);
  }, [vId, tIdx]);

  const routeCasing: LayerProps = useMemo(() => ({
    id: 'route-casing', type: 'line', layout: { 'line-cap': 'round', 'line-join': 'round' },
    paint: {
      'line-color': '#223a2e',
      'line-width': match ? (['case', match, 8, 4] as never) : 6,
      'line-opacity': match ? (['case', match, 0.9, 0.05] as never) : 0.85,
    },
  }), [match]);
  const routeLine: LayerProps = useMemo(() => ({
    id: 'route-line', type: 'line', layout: { 'line-cap': 'round', 'line-join': 'round' },
    paint: {
      'line-color': ['get', 'color'],
      'line-width': match ? (['case', match, 5, 2.5] as never) : 4,
      'line-opacity': match ? (['case', match, 0.98, 0.1] as never) : 0.92,
    },
  }), [match]);
  const stopLayer: LayerProps = useMemo(() => ({
    id: 'stops', type: 'circle',
    paint: {
      'circle-radius': 4.5,
      'circle-color': ['get', 'color'],
      'circle-stroke-color': '#fbfaf5',
      'circle-stroke-width': 1.2,
      'circle-opacity': match ? (['case', match, 1, 0.1] as never) : 0.95,
      'circle-stroke-opacity': match ? (['case', match, 1, 0.1] as never) : 0.9,
    },
  }), [match]);

  const depotLayer: LayerProps = { id: 'depot', type: 'circle', paint: { 'circle-radius': 7, 'circle-color': '#223a2e', 'circle-stroke-color': '#fbfaf5', 'circle-stroke-width': 2.5 } };

  // Selected stop highlight ring.
  const selectedStop = useMemo(() => {
    if (!selection || selection.kind !== 'stop') return null;
    const v = plan.vehicles.find((x) => x.id === selection.vehicleId);
    const t = v?.trips.find((x) => x.index === selection.tripIndex);
    const s = t?.stops.find((x) => x.seq === selection.seq);
    return s ? pointFC(s.lon, s.lat, { color: v!.color }) : null;
  }, [selection, plan]);

  // Unassigned orders (if coords available) as muted hollow markers.
  const unassignedFC = useMemo<FeatureCollection>(() => ({
    type: 'FeatureCollection',
    features: plan.unassigned
      .filter((u) => typeof (u as { lon?: number }).lon === 'number')
      .map((u) => ({ type: 'Feature', properties: { orderNo: u.orderNo },
        geometry: { type: 'Point', coordinates: [(u as { lon: number }).lon, (u as { lat: number }).lat] } })),
  }), [plan.unassigned]);

  const fitTo = useCallback((coords: [number, number][], padding = 70) => {
    const map = mapRef.current; if (!map || coords.length === 0) return;
    const b = new maplibregl.LngLatBounds();
    for (const c of coords) b.extend(c);
    map.fitBounds(b, { padding, duration: dur(500), maxZoom: 15 });
  }, []);

  const allCoords = useCallback((): [number, number][] => {
    const cs: [number, number][] = [[plan.depot.lon, plan.depot.lat]];
    for (const v of plan.vehicles) for (const t of v.trips) if (t.geometry) for (const c of t.geometry.coordinates) cs.push(c as [number, number]);
    return cs;
  }, [plan]);

  const fitAll = useCallback(() => fitTo(allCoords(), 60), [fitTo, allCoords]);

  // Camera: fit to the selection on deliberate selection change; fit all when a
  // new plan is loaded. Not re-run on background geometry fills (dep on plan.id).
  // eslint-disable-next-line react-hooks/exhaustive-deps
  useEffect(() => {
    const map = mapRef.current; if (!map) return;
    if (!selection || selection.kind === 'unassigned') { fitAll(); return; }
    const v = plan.vehicles.find((x) => x.id === selection.vehicleId);
    if (!v) return;
    if (selection.kind === 'vehicle') {
      fitTo(v.trips.flatMap((t) => (t.geometry?.coordinates ?? []) as [number, number][]));
    } else if (selection.kind === 'trip') {
      const t = v.trips.find((x) => x.index === selection.tripIndex);
      fitTo((t?.geometry?.coordinates ?? []) as [number, number][]);
    } else {
      const t = v.trips.find((x) => x.index === selection.tripIndex);
      const s = t?.stops.find((x) => x.seq === selection.seq);
      if (s) map.easeTo({ center: [s.lon, s.lat], zoom: Math.max(map.getZoom(), 13), duration: dur(500) });
    }
  }, [selectionKey(selection), plan.id]);

  const onClick = useCallback((e: MapLayerMouseEvent) => {
    const f = e.features?.[0];
    if (!f) { onSelect(null); return; }
    const p = f.properties as { vehicleId?: number; tripIndex?: number; seq?: number } | null;
    if (f.layer.id === 'stops' && p?.vehicleId != null) {
      onSelect({ kind: 'stop', vehicleId: p.vehicleId, tripIndex: p.tripIndex ?? 0, seq: p.seq ?? 1 });
    } else if ((f.layer.id === 'route-line' || f.layer.id === 'route-hit') && p?.vehicleId != null) {
      onSelect({ kind: 'trip', vehicleId: p.vehicleId, tripIndex: p.tripIndex ?? 0 });
    }
  }, [onSelect]);

  const zoomBy = (d: number) => { const m = mapRef.current; if (m) m.easeTo({ zoom: m.getZoom() + d, duration: dur(200) }); };

  return (
    <div className="relative h-full w-full">
      <Map
        ref={mapRef}
        initialViewState={{ longitude: plan.depot.lon, latitude: plan.depot.lat, zoom: 9 }}
        mapStyle={style}
        transformRequest={transformRequest}
        attributionControl={false}
        interactiveLayerIds={['stops', 'route-hit', 'route-line']}
        cursor={hovering ? 'pointer' : 'default'}
        onMouseEnter={() => setHovering(true)}
        onMouseLeave={() => setHovering(false)}
        onClick={onClick}
        onLoad={fitAll}
        style={{ position: 'absolute', inset: 0 }}
      >
        <Source id="routes" type="geojson" data={routes}>
          {/* wide transparent hit target so thin routes are easy to click */}
          <Layer id="route-hit" type="line" paint={{ 'line-color': '#000', 'line-opacity': 0, 'line-width': 16 }} />
          <Layer {...routeCasing} />
          <Layer {...routeLine} />
        </Source>
        {unassignedFC.features.length > 0 && (
          <Source id="unassigned" type="geojson" data={unassignedFC}>
            <Layer id="unassigned" type="circle" paint={{ 'circle-radius': 5, 'circle-color': '#b4572a', 'circle-opacity': 0.25, 'circle-stroke-color': '#b4572a', 'circle-stroke-width': 1.5, 'circle-stroke-opacity': 0.8 }} />
          </Source>
        )}
        <Source id="stops" type="geojson" data={stops}>
          <Layer {...stopLayer} />
        </Source>
        {selectedStop && (
          <Source id="selstop" type="geojson" data={selectedStop}>
            <Layer id="selstop-ring" type="circle" paint={{ 'circle-radius': 9, 'circle-color': 'rgba(0,0,0,0)', 'circle-stroke-color': ['get', 'color'], 'circle-stroke-width': 2.5 }} />
          </Source>
        )}
        <Source id="depot" type="geojson" data={depot}>
          <Layer {...depotLayer} />
        </Source>
        {playhead.features.length > 0 && (
          <Source id="playhead" type="geojson" data={playhead}>
            <Layer id="playhead-halo" type="circle" paint={{ 'circle-radius': 9, 'circle-color': ['get', 'color'], 'circle-opacity': 0.25 }} />
            <Layer id="playhead-dot" type="circle" paint={{ 'circle-radius': 5, 'circle-color': ['get', 'color'], 'circle-stroke-color': '#111', 'circle-stroke-width': 1.5 }} />
          </Source>
        )}
      </Map>

      <div className="maplibregl-ctrl-group absolute right-3 top-3 flex flex-col overflow-hidden text-graphite-foreground">
        <button type="button" aria-label="Zoom in" onClick={() => zoomBy(1)} className="flex h-8 w-8 items-center justify-center hover:bg-white/10"><Plus className="h-4 w-4" /></button>
        <button type="button" aria-label="Zoom out" onClick={() => zoomBy(-1)} className="flex h-8 w-8 items-center justify-center border-t border-white/10 hover:bg-white/10"><Minus className="h-4 w-4" /></button>
        <button type="button" aria-label="Fit plan" onClick={() => { onSelect(null); fitAll(); }} className="flex h-8 w-8 items-center justify-center border-t border-white/10 hover:bg-white/10"><Maximize2 className="h-4 w-4" /></button>
      </div>
    </div>
  );
}

function selectionKey(s: Selection): string {
  if (!s) return 'none';
  if (s.kind === 'unassigned') return `u:${s.orderNo}`;
  if (s.kind === 'vehicle') return `v:${s.vehicleId}`;
  if (s.kind === 'trip') return `t:${s.vehicleId}:${s.tripIndex}`;
  return `s:${s.vehicleId}:${s.tripIndex}:${s.seq}`;
}
