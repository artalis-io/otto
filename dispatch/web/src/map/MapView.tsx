import { useCallback, useMemo, useRef } from 'react';
import Map, {
  Layer,
  Source,
  type MapRef,
  type LayerProps,
} from 'react-map-gl/maplibre';
import maplibregl from 'maplibre-gl';
import type { FeatureCollection, LineString } from 'geojson';
import 'maplibre-gl/dist/maplibre-gl.css';
import { Minus, Plus, Maximize2 } from 'lucide-react';
import { cartaStyle } from './cartaStyle';
import type { Plan } from '@/types';

/*
 * MapLibre map for the dispatch workspace, via react-map-gl's maplibre
 * entrypoint. The basemap is the reused "Carta Quiet" style; route lines, stop
 * dots, and the depot marker are overlaid from the plan as declarative
 * Source/Layer children. Selection dimming and fit-to-route are left for the
 * lead to wire against real interaction state (see note in README).
 *
 * The transformRequest below is load-bearing and copied from the Carta viewer:
 * the style uses root-relative tile/glyph URLs ("/tiles.vector.json",
 * "/fonts/..."), which are correct for a same-origin host but fail to parse
 * inside MapLibre's Web Worker (no document base). We resolve them against the
 * page origin on the way out.
 */

function routesFC(plan: Plan): FeatureCollection {
  return {
    type: 'FeatureCollection',
    features: plan.vehicles.flatMap((v) =>
      v.trips.map((t) => ({
        type: 'Feature' as const,
        id: v.id * 100 + t.index,
        properties: { vehicleId: v.id, color: v.color, tripIndex: t.index },
        geometry: t.geometry as LineString,
      }))
    ),
  };
}

function stopsFC(plan: Plan): FeatureCollection {
  return {
    type: 'FeatureCollection',
    features: plan.vehicles.flatMap((v) =>
      v.trips.flatMap((t) =>
        t.stops.map((s) => ({
          type: 'Feature' as const,
          properties: { vehicleId: v.id, color: v.color, seq: s.seq, customer: s.customer },
          geometry: { type: 'Point' as const, coordinates: [s.lon, s.lat] },
        }))
      )
    ),
  };
}

const routeCasing: LayerProps = {
  id: 'route-casing',
  type: 'line',
  layout: { 'line-cap': 'round', 'line-join': 'round' },
  paint: { 'line-color': '#1f3d2f', 'line-width': 6.5, 'line-opacity': 0.9 },
};
const routeLine: LayerProps = {
  id: 'route-line',
  type: 'line',
  layout: { 'line-cap': 'round', 'line-join': 'round' },
  paint: { 'line-color': ['get', 'color'], 'line-width': 4, 'line-opacity': 0.95 },
};
const stopLayer: LayerProps = {
  id: 'stops',
  type: 'circle',
  paint: {
    'circle-radius': 5,
    'circle-color': ['get', 'color'],
    'circle-stroke-color': '#ffffff',
    'circle-stroke-width': 1.5,
  },
};
const depotLayer: LayerProps = {
  id: 'depot',
  type: 'circle',
  paint: {
    'circle-radius': 8,
    'circle-color': '#1f3d2f',
    'circle-stroke-color': '#ffffff',
    'circle-stroke-width': 2.5,
  },
};

const transformRequest = (url: string) => ({
  url: url.startsWith('/') ? window.location.origin + url : url,
});

export function MapView({ plan }: { plan: Plan }) {
  const mapRef = useRef<MapRef | null>(null);
  const routes = useMemo(() => routesFC(plan), [plan]);
  const stops = useMemo(() => stopsFC(plan), [plan]);
  const depot = useMemo<FeatureCollection>(
    () => ({
      type: 'FeatureCollection',
      features: [
        {
          type: 'Feature',
          properties: {},
          geometry: { type: 'Point', coordinates: [plan.depot.lon, plan.depot.lat] },
        },
      ],
    }),
    [plan]
  );

  const style = useMemo(() => cartaStyle(), []);

  const fitAll = useCallback(() => {
    const map = mapRef.current;
    if (!map) return;
    const b = new maplibregl.LngLatBounds();
    b.extend([plan.depot.lon, plan.depot.lat]);
    for (const v of plan.vehicles)
      for (const t of v.trips)
        for (const c of t.geometry.coordinates) b.extend(c as [number, number]);
    map.fitBounds(b, { padding: 60, duration: 400 });
  }, [plan]);

  const zoomBy = useCallback((delta: number) => {
    const map = mapRef.current;
    if (!map) return;
    map.easeTo({ zoom: map.getZoom() + delta, duration: 200 });
  }, []);

  return (
    <div className="relative h-full w-full">
      <Map
        ref={mapRef}
        initialViewState={{
          longitude: plan.depot.lon,
          latitude: plan.depot.lat,
          zoom: 9,
        }}
        mapStyle={style}
        transformRequest={transformRequest}
        attributionControl={false}
        onLoad={fitAll}
        style={{ position: 'absolute', inset: 0 }}
      >
        <Source id="routes" type="geojson" data={routes}>
          <Layer {...routeCasing} />
          <Layer {...routeLine} />
        </Source>
        <Source id="stops" type="geojson" data={stops}>
          <Layer {...stopLayer} />
        </Source>
        <Source id="depot" type="geojson" data={depot}>
          <Layer {...depotLayer} />
        </Source>
      </Map>

      {/* Translucent floating map controls (graphite, blurred via index.css). */}
      <div className="maplibregl-ctrl-group absolute right-3 top-3 flex flex-col overflow-hidden text-graphite-foreground">
        <button
          type="button"
          aria-label="Zoom in"
          onClick={() => zoomBy(1)}
          className="flex h-8 w-8 items-center justify-center hover:bg-white/10"
        >
          <Plus className="h-4 w-4" />
        </button>
        <button
          type="button"
          aria-label="Zoom out"
          onClick={() => zoomBy(-1)}
          className="flex h-8 w-8 items-center justify-center border-t border-white/10 hover:bg-white/10"
        >
          <Minus className="h-4 w-4" />
        </button>
        <button
          type="button"
          aria-label="Fit plan"
          onClick={fitAll}
          className="flex h-8 w-8 items-center justify-center border-t border-white/10 hover:bg-white/10"
        >
          <Maximize2 className="h-4 w-4" />
        </button>
      </div>
    </div>
  );
}
