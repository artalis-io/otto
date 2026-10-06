/*
 * Build public/routes/plan.json from REAL Velo road geometry (Monaco graph).
 * Requires a running velo-route-server (VELO_ORIGIN, default 127.0.0.1:8082).
 * For each route it chains depot -> stops -> depot, requesting each leg from
 * Velo, decoding the polyline and stitching one road-following LineString.
 * Stops that Velo cannot reach on this extract are skipped (logged), so the
 * emitted geometry always follows real roads - never straight depot-stop lines.
 */
import { writeFile, mkdir } from 'node:fs/promises';
const VELO = process.env.VELO_ORIGIN || 'http://127.0.0.1:8082';

function decodePolyline(str, precision = 5) {
  let index = 0, lat = 0, lng = 0; const coords = [], factor = Math.pow(10, precision);
  while (index < str.length) {
    let result = 1, shift = 0, b;
    do { b = str.charCodeAt(index++) - 63 - 1; result += b << shift; shift += 5; } while (b >= 0x1f);
    lat += (result & 1) ? ~(result >> 1) : (result >> 1);
    result = 1; shift = 0;
    do { b = str.charCodeAt(index++) - 63 - 1; result += b << shift; shift += 5; } while (b >= 0x1f);
    lng += (result & 1) ? ~(result >> 1) : (result >> 1);
    coords.push([lng / factor, lat / factor]);
  }
  return coords;
}
async function leg(from, to) {
  const url = `${VELO}/api/v1/route?from=${from[0]},${from[1]}&to=${to[0]},${to[1]}&profile=car&mode=fastest`;
  const r = await fetch(url);
  if (!r.ok) return null;
  const j = await r.json();
  if (j.status !== 'ok' || !j.route?.geometry) return null;
  return { coords: decodePolyline(j.route.geometry), dist: j.route.distance };
}

const depot = { name: 'Port Hercule depot', lat: 43.7347, lon: 7.4256 };
const defs = [
  { id: 'r1', label: 'Monte-Carlo loop', color: '#2e7d5b', stops: [
    { name: 'Casino de Monte-Carlo', lat: 43.7395, lon: 7.4278 },
    { name: 'Larvotto Beach', lat: 43.7472, lon: 7.4330 },
    { name: 'La Rousse', lat: 43.7490, lon: 7.4360 } ] },
  { id: 'r2', label: 'Old town & port', color: '#b4572a', stops: [
    { name: 'Monaco-Ville', lat: 43.7308, lon: 7.4190 },
    { name: 'Fontvieille', lat: 43.7267, lon: 7.4120 },
    { name: 'Jardin Exotique', lat: 43.7326, lon: 7.4160 } ] },
  { id: 'r3', label: 'Condamine run', color: '#3b6ea5', stops: [
    { name: 'Condamine Market', lat: 43.7352, lon: 7.4215 },
    { name: 'Moneghetti', lat: 43.7380, lon: 7.4180 } ] },
];

const routes = [];
for (const d of defs) {
  let cursor = [depot.lat, depot.lon];
  let coords = [], meters = 0; const kept = [];
  for (const s of d.stops) {
    const L = await leg(cursor, [s.lat, s.lon]);
    if (!L) { console.warn(`  skip unreachable: ${d.label} -> ${s.name}`); continue; }
    if (coords.length && L.coords.length) L.coords.shift();
    coords = coords.concat(L.coords); meters += L.dist; cursor = [s.lat, s.lon]; kept.push(s);
  }
  const back = await leg(cursor, [depot.lat, depot.lon]);
  if (back) { if (coords.length && back.coords.length) back.coords.shift(); coords = coords.concat(back.coords); meters += back.dist; }
  if (coords.length < 2 || kept.length === 0) { console.warn(`  drop empty route ${d.label}`); continue; }
  routes.push({ id: d.id, label: d.label, color: d.color, distance_km: meters / 1000,
    stops: kept.map((s, i) => ({ name: s.name, lon: s.lon, lat: s.lat, seq: i + 1 })),
    geometry: { type: 'LineString', coordinates: coords } });
}
if (routes.length === 0) { console.error('no routes built - is velo-route-server running?'); process.exit(2); }
const plan = { source: 'velo', depot: { name: depot.name, lon: depot.lon, lat: depot.lat }, routes };
await mkdir('public/routes', { recursive: true });
await writeFile('public/routes/plan.json', JSON.stringify(plan, null, 1));
const tot = routes.reduce((a, r) => a + r.geometry.coordinates.length, 0);
console.log(`plan.json: ${routes.length} Velo routes, ${tot} geometry vertices`);
