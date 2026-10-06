import { useEffect, useRef } from 'react';
import maplibregl from 'maplibre-gl';
import 'maplibre-gl/dist/maplibre-gl.css';
import { cartaStyle } from './style';
import type { RoutePlan } from './types';

function routesFC(plan: RoutePlan): GeoJSON.FeatureCollection {
  return {
    type: 'FeatureCollection',
    features: plan.routes.map((r) => ({
      type: 'Feature',
      id: r.id,
      properties: { id: r.id, color: r.color, label: r.label },
      geometry: r.geometry,
    })),
  };
}
function stopsFC(plan: RoutePlan): GeoJSON.FeatureCollection {
  const feats: GeoJSON.Feature[] = [];
  for (const r of plan.routes)
    for (const s of r.stops)
      feats.push({ type: 'Feature', properties: { route: r.id, color: r.color, seq: s.seq, name: s.name },
        geometry: { type: 'Point', coordinates: [s.lon, s.lat] } });
  return { type: 'FeatureCollection', features: feats };
}
function bounds(coords: [number, number][]): maplibregl.LngLatBounds {
  const b = new maplibregl.LngLatBounds();
  coords.forEach((c) => b.extend(c));
  return b;
}

export function MapView(props: { plan: RoutePlan; selected: string | null }) {
  const { plan, selected } = props;
  const ref = useRef<HTMLDivElement>(null);
  const mapRef = useRef<maplibregl.Map | null>(null);
  const ready = useRef(false);

  useEffect(() => {
    if (!ref.current) return;
    const map = new maplibregl.Map({
      container: ref.current,
      style: cartaStyle(),
      center: [plan.depot.lon, plan.depot.lat],
      zoom: 12,
      attributionControl: { compact: true },
      // Carta serves same-origin-relative tile/glyph templates ("/tiles/..."),
      // which is correct server-side. MapLibre builds tile requests inside a
      // Web Worker with no document base, so a root-relative URL fails to parse
      // there. Resolve it against the page origin on the way out.
      transformRequest: (url) => ({
        url: url.startsWith('/') ? window.location.origin + url : url,
      }),
    });
    mapRef.current = map;
    map.addControl(new maplibregl.NavigationControl({ showCompass: false }), 'top-right');

    map.on('load', () => {
      map.addSource('routes', { type: 'geojson', data: routesFC(plan) });
      map.addSource('stops', { type: 'geojson', data: stopsFC(plan) });
      map.addSource('depot', { type: 'geojson', data: {
        type: 'Feature', properties: {},
        geometry: { type: 'Point', coordinates: [plan.depot.lon, plan.depot.lat] } } as any });

      map.addLayer({ id: 'route-casing', type: 'line', source: 'routes',
        layout: { 'line-cap': 'round', 'line-join': 'round' },
        paint: { 'line-color': '#1f3d2f', 'line-width': 6.5, 'line-opacity': 0.9 } });
      map.addLayer({ id: 'route-line', type: 'line', source: 'routes',
        layout: { 'line-cap': 'round', 'line-join': 'round' },
        paint: { 'line-color': ['get', 'color'], 'line-width': 4, 'line-opacity': 0.95 } });
      map.addLayer({ id: 'stops', type: 'circle', source: 'stops',
        paint: { 'circle-radius': 5, 'circle-color': ['get', 'color'],
          'circle-stroke-color': '#ffffff', 'circle-stroke-width': 1.5 } });
      map.addLayer({ id: 'depot', type: 'circle', source: 'depot',
        paint: { 'circle-radius': 8, 'circle-color': '#1f3d2f',
          'circle-stroke-color': '#ffffff', 'circle-stroke-width': 2.5 } });

      ready.current = true;
      fitAll();
      applySelection();
      // Expose the instance and a readiness flag for screenshot automation only.
      (window as any).__carta_map = map;
      map.once('idle', () => { (window as any).__carta_ready = true; });
    });

    return () => { map.remove(); mapRef.current = null; ready.current = false; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  function fitAll() {
    const map = mapRef.current; if (!map) return;
    const all: [number, number][] = [[plan.depot.lon, plan.depot.lat]];
    plan.routes.forEach((r) => (r.geometry.coordinates as [number, number][]).forEach((c) => all.push(c)));
    if (all.length > 1) map.fitBounds(bounds(all), { padding: 60, duration: 0 });
  }

  function applySelection() {
    const map = mapRef.current; if (!map || !ready.current) return;
    const sel = selected;
    const lineOpacity: any = sel
      ? ['case', ['==', ['get', 'id'], sel], 0.98, 0.12]
      : 0.95;
    const casingOpacity: any = sel
      ? ['case', ['==', ['get', 'id'], sel], 0.9, 0.06]
      : 0.9;
    const lineWidth: any = sel
      ? ['case', ['==', ['get', 'id'], sel], 6, 3]
      : 4;
    map.setPaintProperty('route-line', 'line-opacity', lineOpacity);
    map.setPaintProperty('route-line', 'line-width', lineWidth);
    map.setPaintProperty('route-casing', 'line-opacity', casingOpacity);
    const stopOpacity: any = sel ? ['case', ['==', ['get', 'route'], sel], 1, 0.12] : 1;
    map.setPaintProperty('stops', 'circle-opacity', stopOpacity);
    map.setPaintProperty('stops', 'circle-stroke-opacity', stopOpacity);

    if (sel) {
      const r = plan.routes.find((x) => x.id === sel);
      if (r) map.fitBounds(bounds(r.geometry.coordinates as [number, number][]), { padding: 90, duration: 600 });
    } else {
      fitAll();
    }
  }

  useEffect(() => { applySelection(); /* eslint-disable-next-line */ }, [selected]);

  return <div className="map" ref={ref} />;
}
