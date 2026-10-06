import type { StyleSpecification } from 'maplibre-gl';

/*
 * A deliberately QUIET custom basemap for Carta vector tiles: warm ivory
 * background, subdued landuse, pale blue water, a restrained road hierarchy with
 * casing and zoom-dependent widths, and readable place/road labels with subtle
 * halos. Kept muted so overlaid delivery routes dominate. Carta's own schema
 * (see carta/docs/vector-schema.md): layers roads/water/landuse/railways/
 * buildings/boundaries/labels. Glyphs are served locally (no CDN).
 */

const C = {
  ivory: '#f4f1e8',
  water: '#cfe2ef',
  waterLine: '#a9cbe0',
  forest: '#e2e8d6',
  park: '#e5ecd9',
  residential: '#efebe1',
  commercial: '#efe8e0',
  industrial: '#e9e6e0',
  farmland: '#eef0e2',
  grass: '#e8efdc',
  building: '#e7e1d4',
  buildingLine: '#d8d0bf',
  rail: '#cfc8bd',
  boundary: '#cdbfe0',
  casing: '#dcd5c6',
  motorway: '#f3c07a',
  trunk: '#f6cf9c',
  primary: '#f7d9ad',
  secondary: '#fae6c4',
  minor: '#ffffff',
  label: '#4a4636',
  labelHalo: '#f7f4ec',
  waterLabel: '#4a6a82',
};

// Zoom-interpolated road width (px) by class. Casing adds ~2px underneath.
function roadWidth(base: number[][]): any {
  return { stops: base, base: 1.2 } as any;
}

export function cartaStyle(): StyleSpecification {
  return {
    version: 8,
    name: 'Carta Quiet',
    glyphs: '/fonts/{fontstack}/{range}.pbf',
    sources: {
      carta: { type: 'vector', url: '/tiles.vector.json' },
    },
    layers: [
      { id: 'bg', type: 'background', paint: { 'background-color': C.ivory } },

      // --- Landuse (subdued fills) ---
      { id: 'landuse-forest', type: 'fill', source: 'carta', 'source-layer': 'landuse',
        filter: ['==', ['get', 'class'], 'forest'],
        paint: { 'fill-color': C.forest, 'fill-opacity': 0.7 } },
      { id: 'landuse-park', type: 'fill', source: 'carta', 'source-layer': 'landuse',
        filter: ['in', ['get', 'class'], ['literal', ['park', 'grass']]],
        paint: { 'fill-color': C.park, 'fill-opacity': 0.7 } },
      { id: 'landuse-farmland', type: 'fill', source: 'carta', 'source-layer': 'landuse',
        filter: ['==', ['get', 'class'], 'farmland'],
        paint: { 'fill-color': C.farmland, 'fill-opacity': 0.6 } },
      { id: 'landuse-residential', type: 'fill', source: 'carta', 'source-layer': 'landuse',
        filter: ['in', ['get', 'class'], ['literal', ['residential', 'commercial', 'industrial']]],
        paint: { 'fill-color': C.residential, 'fill-opacity': 0.5 } },

      // --- Water ---
      { id: 'water-fill', type: 'fill', source: 'carta', 'source-layer': 'water',
        paint: { 'fill-color': C.water, 'fill-outline-color': C.waterLine } },
      { id: 'water-line', type: 'line', source: 'carta', 'source-layer': 'water',
        filter: ['in', ['get', 'class'], ['literal', ['river', 'canal', 'stream']]],
        paint: { 'line-color': C.waterLine,
          'line-width': ['interpolate', ['linear'], ['zoom'], 9, 0.6, 14, 2.2] } },

      // --- Buildings (very quiet, high zoom) ---
      { id: 'buildings', type: 'fill', source: 'carta', 'source-layer': 'buildings',
        minzoom: 14,
        paint: { 'fill-color': C.building, 'fill-outline-color': C.buildingLine, 'fill-opacity': 0.6 } },

      // --- Railways ---
      { id: 'rail', type: 'line', source: 'carta', 'source-layer': 'railways',
        minzoom: 10,
        paint: { 'line-color': C.rail, 'line-dasharray': [3, 2],
          'line-width': ['interpolate', ['linear'], ['zoom'], 10, 0.5, 16, 1.6] } },

      // --- Road casing (under fill) ---
      { id: 'road-casing', type: 'line', source: 'carta', 'source-layer': 'roads',
        filter: ['in', ['get', 'class'], ['literal', ['motorway', 'trunk', 'primary', 'secondary', 'tertiary']]],
        layout: { 'line-cap': 'round', 'line-join': 'round' },
        paint: { 'line-color': C.casing,
          'line-width': ['interpolate', ['exponential', 1.4], ['zoom'],
            6, 1.2, 10, 3.0, 14, 7.0, 18, 18.0] } },

      // --- Road fills by class ---
      { id: 'road-minor', type: 'line', source: 'carta', 'source-layer': 'roads',
        minzoom: 12,
        filter: ['in', ['get', 'class'], ['literal', ['residential', 'service', 'tertiary', 'other']]],
        layout: { 'line-cap': 'round', 'line-join': 'round' },
        paint: { 'line-color': C.minor,
          'line-width': ['interpolate', ['exponential', 1.4], ['zoom'], 12, 0.8, 16, 3.5, 19, 10.0] } },
      { id: 'road-secondary', type: 'line', source: 'carta', 'source-layer': 'roads',
        filter: ['==', ['get', 'class'], 'secondary'],
        layout: { 'line-cap': 'round', 'line-join': 'round' },
        paint: { 'line-color': C.secondary,
          'line-width': ['interpolate', ['exponential', 1.4], ['zoom'], 8, 0.8, 12, 2.2, 16, 6.0] } },
      { id: 'road-primary', type: 'line', source: 'carta', 'source-layer': 'roads',
        filter: ['==', ['get', 'class'], 'primary'],
        layout: { 'line-cap': 'round', 'line-join': 'round' },
        paint: { 'line-color': C.primary,
          'line-width': ['interpolate', ['exponential', 1.4], ['zoom'], 7, 1.0, 12, 3.0, 16, 8.0] } },
      { id: 'road-trunk', type: 'line', source: 'carta', 'source-layer': 'roads',
        filter: ['==', ['get', 'class'], 'trunk'],
        layout: { 'line-cap': 'round', 'line-join': 'round' },
        paint: { 'line-color': C.trunk,
          'line-width': ['interpolate', ['exponential', 1.4], ['zoom'], 6, 1.0, 12, 3.4, 16, 9.0] } },
      { id: 'road-motorway', type: 'line', source: 'carta', 'source-layer': 'roads',
        filter: ['==', ['get', 'class'], 'motorway'],
        layout: { 'line-cap': 'round', 'line-join': 'round' },
        paint: { 'line-color': C.motorway,
          'line-width': ['interpolate', ['exponential', 1.4], ['zoom'], 5, 1.2, 12, 4.0, 16, 11.0] } },

      // --- Boundaries ---
      { id: 'boundary', type: 'line', source: 'carta', 'source-layer': 'boundaries',
        paint: { 'line-color': C.boundary, 'line-dasharray': [4, 2], 'line-opacity': 0.6,
          'line-width': ['interpolate', ['linear'], ['zoom'], 4, 0.6, 10, 1.4] } },

      // --- Road name labels ---
      { id: 'road-labels', type: 'symbol', source: 'carta', 'source-layer': 'roads',
        minzoom: 13,
        filter: ['has', 'name'],
        layout: {
          'symbol-placement': 'line',
          'text-field': ['get', 'name'],
          'text-font': ['IBM Plex Sans Regular'],
          'text-size': ['interpolate', ['linear'], ['zoom'], 13, 10, 18, 13],
        },
        paint: { 'text-color': C.label, 'text-halo-color': C.labelHalo, 'text-halo-width': 1.2 } },

      // --- Place labels (from the labels layer) ---
      { id: 'place-labels', type: 'symbol', source: 'carta', 'source-layer': 'labels',
        layout: {
          'text-field': ['get', 'name'],
          'text-font': ['IBM Plex Sans Regular'],
          'text-size': ['interpolate', ['linear'], ['get', 'rank'],
            0, 11, 5, 14, 20, 20],
          'text-anchor': 'center',
          'text-max-width': 7,
          'symbol-sort-key': ['-', 100, ['coalesce', ['get', 'rank'], 0]],
        },
        paint: { 'text-color': C.label, 'text-halo-color': C.labelHalo, 'text-halo-width': 1.6 } },
    ],
  } as StyleSpecification;
}
