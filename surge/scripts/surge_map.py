#!/usr/bin/env python3
"""
surge_map.py - render a Surge solution as GeoJSON + an HTML5 slippy map.

Consumes the two standard Surge JSON documents -- a solve request (the
POST /api/v1/solve body; see the HTTP API and surge_solve) and its solution
(sg_api_write_solution output) -- and emits, into an output directory:

  routes.geojson   FeatureCollection: one MultiLineString per vehicle route
                   (a trip = one line), styled by `color`, with route metadata.
  stops.geojson    FeatureCollection of stop / depot / unassigned Points.
  index.html       Leaflet page: a tile basemap + the two GeoJSON overlays,
                   with a per-route legend/toggle. (omit with --no-html)

Coordinates come from the request (locations[]/location_id, or a task's own
x/y). Labels are generic (task/request ids, vehicle id + capacity) -- no
domain-specific fields required, so this works for any Surge model.

Route lines are straight stop-to-stop legs by default. Pass --velo-graph to
route each leg over a Velo graph (velo/route_geometry) for real road-following
geometry. Pass --carta-graph to pre-render an offline PNG tile basemap into
<out>/tiles (carta/tiledump); the HTML then needs no tile server.

Example:
  surge_solve request.json > solution.json
  surge_map.py --request request.json --solution solution.json --out-dir out \\
      --velo-graph region.vlg --carta-graph region.osm.pbf
  ( cd out && python3 -m http.server 8090 )   # open http://127.0.0.1:8090/
"""
import argparse, colorsys, json, os, subprocess, sys, tempfile

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


# ---------------------------------------------------------------- pure core ---
def _hms(sec):
    try:
        sec = int(sec)
    except (TypeError, ValueError):
        return ""
    return f"{sec // 3600:02d}:{(sec % 3600) // 60:02d}"


def _color(i, n):
    r, g, b = colorsys.hsv_to_rgb((i / max(1, n)) % 1.0, 0.72, 0.85)
    return "#%02x%02x%02x" % (int(r * 255), int(g * 255), int(b * 255))


def _coord_lookup(request):
    """Return (task_coord, depot_coord, request_to_task) helpers as (lat, lon).

    Supports both request shapes: locations[] + location_id, or inline x/y on
    tasks/depots."""
    locs = request.get("locations")

    def loc_ll(i):
        l = locs[i]
        return (l["y"], l["x"])

    tasks = {t["id"]: t for t in request.get("tasks", [])}
    depots = {d["id"]: d for d in request.get("depots", [])}
    reqs = {r["id"]: r for r in request.get("requests", [])}
    vehicles = {v["id"]: v for v in request.get("vehicles", [])}

    def node_ll(obj):
        if locs is not None and obj.get("location_id") is not None:
            return loc_ll(obj["location_id"])
        if obj.get("y") is not None and obj.get("x") is not None:
            return (obj["y"], obj["x"])
        return None

    def task_ll(tid):
        t = tasks.get(tid)
        return node_ll(t) if t else None

    def depot_ll_for_vehicle(vid):
        v = vehicles.get(vid, {})
        did = v.get("start_depot_id", 0)
        d = depots.get(did) or (depots.get(0) if depots else None)
        return node_ll(d) if d else None

    def request_task(rid):
        r = reqs.get(rid, {})
        return r.get("delivery_task_id", r.get("pickup_task_id"))

    return task_ll, depot_ll_for_vehicle, request_task, tasks, vehicles


def build_geojson(request, solution, geometry=None):
    """Pure core: (request, solution[, {leg_id: [[lat,lon],...]}]) -> (routes_fc,
    stops_fc, legs). `legs` is the list of (leg_id, (latA,lonA), (latB,lonB))
    the caller can route to obtain `geometry`; call once without geometry to get
    the legs, then again with geometry for road-following lines."""
    task_ll, depot_ll_for_vehicle, request_task, tasks, vehicles = _coord_lookup(request)
    routes = solution.get("routes", [])
    route_feats, stop_feats, legs = [], [], []

    for i, rt in enumerate(routes):
        vid = rt.get("vehicle_id", i)
        color = _color(i, len(routes))
        depot = depot_ll_for_vehicle(vid) or (0.0, 0.0)
        stops = rt.get("stops", [])
        # group stops by trip
        trips = {}
        for s in stops:
            trips.setdefault(s.get("trip_index", 0), []).append(s)
        multiline = []
        for trip in sorted(trips):
            sts = trips[trip]
            seq = [depot] + [task_ll(s["task_id"]) or depot for s in sts] + [depot]
            line_ll = []
            for k in range(len(seq) - 1):
                a, b = seq[k], seq[k + 1]
                lid = f"R{i}_T{trip}_{k:03d}"
                legs.append((lid, a, b))
                pts = None
                if geometry is not None:
                    g = geometry.get(lid)
                    if g:
                        pts = g
                if pts is None:
                    pts = [list(a), list(b)]
                if line_ll and pts and abs(line_ll[-1][0] - pts[0][0]) < 1e-9 \
                        and abs(line_ll[-1][1] - pts[0][1]) < 1e-9:
                    pts = pts[1:]
                line_ll.extend([[la, lo] for la, lo in pts])
            if line_ll:
                multiline.append([[lo, la] for la, lo in line_ll])  # GeoJSON [lon,lat]
        v = vehicles.get(vid, {})
        cap = v.get("capacity")
        route_feats.append({
            "type": "Feature",
            "geometry": {"type": "MultiLineString", "coordinates": multiline},
            "properties": {
                "route": i, "vehicle_id": vid, "color": color,
                "label": f"vehicle {vid}" + (f" (cap {cap})" if cap is not None else ""),
                "n_stops": len(stops), "n_trips": len(trips),
                "distance": rt.get("distance"), "duration": rt.get("duration"),
                "on_duty": _hms(rt.get("duration")),
            },
        })
        for s in stops:
            ll = task_ll(s["task_id"])
            if not ll:
                continue
            stop_feats.append({
                "type": "Feature",
                "geometry": {"type": "Point", "coordinates": [ll[1], ll[0]]},
                "properties": {"kind": "stop", "route": i, "color": color,
                               "task_id": s["task_id"], "request_id": s.get("request_id"),
                               "type_": s.get("type"), "trip": s.get("trip_index", 0) + 1,
                               "arr": _hms(s.get("arrival")),
                               "label": f"task {s['task_id']}"},
            })

    # depots (one point each, deduped by coord)
    seen = set()
    locs = request.get("locations")
    for d in request.get("depots", []):
        ll = None
        if locs is not None and d.get("location_id") is not None:
            ll = (locs[d["location_id"]]["y"], locs[d["location_id"]]["x"])
        elif d.get("y") is not None:
            ll = (d["y"], d["x"])
        if ll and ll not in seen:
            seen.add(ll)
            stop_feats.append({"type": "Feature",
                               "geometry": {"type": "Point", "coordinates": [ll[1], ll[0]]},
                               "properties": {"kind": "depot", "label": f"depot {d.get('id', 0)}"}})

    # unassigned (request ids)
    for rid in solution.get("unassigned", []):
        tid = request_task(rid)
        ll = task_ll(tid) if tid is not None else None
        if ll:
            stop_feats.append({"type": "Feature",
                               "geometry": {"type": "Point", "coordinates": [ll[1], ll[0]]},
                               "properties": {"kind": "unassigned", "request_id": rid,
                                              "label": f"request {rid} (unassigned)"}})

    routes_fc = {"type": "FeatureCollection", "features": route_feats}
    stops_fc = {"type": "FeatureCollection", "features": stop_feats}
    return routes_fc, stops_fc, legs


def bbox_of(routes_fc, stops_fc):
    """(min_lat, min_lon, max_lat, max_lon) over all geometry, or None."""
    lats, lons = [], []
    for f in stops_fc["features"]:
        lo, la = f["geometry"]["coordinates"]
        lats.append(la); lons.append(lo)
    for f in routes_fc["features"]:
        for line in f["geometry"]["coordinates"]:
            for lo, la in line:
                lats.append(la); lons.append(lo)
    if not lats:
        return None
    return (min(lats), min(lons), max(lats), max(lons))


# ---------------------------------------------------------------- externals ---
def route_geometry(legs, graph, velo_bin, profile, weight):
    """Route each leg over a Velo graph -> {leg_id: [[lat,lon],...]}."""
    with tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False) as fh:
        for lid, a, b in legs:
            fh.write(f"{lid}\t{a[0]:.6f}\t{a[1]:.6f}\t{b[0]:.6f}\t{b[1]:.6f}\n")
        legs_path = fh.name
    try:
        with open(legs_path) as inp:
            out = subprocess.run([velo_bin, graph, "--profile", profile, "--weight", weight],
                                 stdin=inp, capture_output=True, text=True)
    finally:
        os.unlink(legs_path)
    if out.returncode != 0:
        sys.stderr.write(out.stderr)
        raise SystemExit(f"surge_map: route_geometry failed ({out.returncode})")
    geom, ok = {}, 0
    for line in out.stdout.splitlines():
        if not line.strip():
            continue
        o = json.loads(line)
        if o.get("ok") and o.get("coords"):
            geom[o["id"]] = o["coords"]; ok += 1
    sys.stderr.write(f"surge_map: routed {ok}/{len(legs)} legs on roads\n")
    return geom


def prerender_tiles(graph, carta_bin, out_dir, bbox, zmin, zmax, pad=0.05):
    tiles_dir = os.path.join(out_dir, "tiles")
    mnla, mnlo, mxla, mxlo = bbox
    dla = (mxla - mnla) * pad or 0.05
    dlo = (mxlo - mnlo) * pad or 0.05
    subprocess.run([carta_bin, graph, tiles_dir, str(zmin), str(zmax),
                    f"{mnla - dla:.5f}", f"{mnlo - dlo:.5f}",
                    f"{mxla + dla:.5f}", f"{mxlo + dlo:.5f}"], check=True)


HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>__TITLE__</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<style>
 html,body{margin:0;height:100%;font:13px/1.4 system-ui,sans-serif}
 #map{position:absolute;inset:0}
 #panel{position:absolute;top:10px;right:10px;z-index:1000;background:#fffe;border:1px solid #ccc;
   border-radius:8px;padding:10px 12px;max-height:88vh;overflow:auto;box-shadow:0 2px 12px #0003;width:280px}
 #panel h1{font-size:14px;margin:0 0 4px} #panel .sum{color:#444;margin-bottom:8px;font-size:12px}
 .row{display:flex;align-items:center;gap:6px;padding:2px 0;cursor:pointer;border-radius:4px}
 .row:hover{background:#f0f0f0} .sw{width:14px;height:4px;border-radius:2px;flex:0 0 auto}
 .row small{color:#666} .muted{color:#999}
 #banner{position:absolute;bottom:8px;left:8px;z-index:1000;background:#fffe;border:1px solid #ccc;
   border-radius:6px;padding:4px 8px;font-size:11px;color:#a00;display:none}
</style></head><body>
<div id="map"></div>
<div id="panel"><h1>__TITLE__</h1><div class="sum" id="sum">loading…</div><div id="list"></div></div>
<div id="banner">⚠ basemap tiles not loading (check the tile source / run a static server)</div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
const TILES=__TILES__, ATTR=__ATTR__, TZ=__TILEOPTS__;
const map=L.map('map',{preferCanvas:true}).setView([__CLAT__,__CLON__],__ZOOM__);
let te=0; const banner=document.getElementById('banner');
L.tileLayer(TILES,Object.assign({attribution:ATTR},TZ)).addTo(map)
  .on('tileerror',()=>{if(++te===4)banner.style.display='block'});
const layers={};const bounds=L.latLngBounds([]);
Promise.all([fetch('routes.geojson').then(r=>r.json()),fetch('stops.geojson').then(r=>r.json())])
.then(([routes,stops])=>{
  routes.features.forEach(f=>{const lyr=L.geoJSON(f,{style:{color:f.properties.color,weight:3,opacity:.85}}).addTo(map);
    layers[f.properties.route]=lyr; try{bounds.extend(lyr.getBounds())}catch(e){}});
  const sl={};
  stops.features.forEach(f=>{const [lo,la]=f.geometry.coordinates,p=f.properties;
    if(p.kind==='depot'){L.marker([la,lo]).bindPopup('<b>'+p.label+'</b>').addTo(map);bounds.extend([la,lo]);return;}
    if(p.kind==='unassigned'){(sl.__un=sl.__un||L.layerGroup().addTo(map)).addLayer(
       L.circleMarker([la,lo],{radius:5,color:'#c00',weight:2,fillColor:'#fff',fillOpacity:1}).bindPopup('<b>'+p.label+'</b>'));
       bounds.extend([la,lo]);return;}
    const m=L.circleMarker([la,lo],{radius:4,color:'#222',weight:1,fillColor:p.color,fillOpacity:1})
      .bindPopup('<b>'+p.label+'</b><br>route '+p.route+' · trip '+p.trip+(p.arr?(' · arr '+p.arr):''));
    (sl[p.route]=sl[p.route]||L.layerGroup().addTo(map)).addLayer(m);});
  if(bounds.isValid())map.fitBounds(bounds.pad(0.05));
  const totStops=routes.features.reduce((a,f)=>a+(f.properties.n_stops||0),0);
  const nun=stops.features.filter(f=>f.properties.kind==='unassigned').length;
  document.getElementById('sum').innerHTML=routes.features.length+' vehicles · '+totStops+
    ' stops · <span style="color:#c00">'+nun+' unassigned</span>';
  const list=document.getElementById('list');
  routes.features.forEach(f=>{const p=f.properties,d=document.createElement('div');d.className='row';
    d.innerHTML='<span class="sw" style="background:'+p.color+'"></span><span>#'+p.route+' <small>'+p.label+
      '</small><br><small class="muted">'+p.n_stops+' stops · '+p.n_trips+' trip(s)'+(p.on_duty?(' · '+p.on_duty):'')+'</small></span>';
    let on=true;d.onclick=()=>{on=!on;const g=sl[p.route];
      if(on){layers[p.route].addTo(map);g&&g.addTo(map);d.style.opacity=1;}
      else{map.removeLayer(layers[p.route]);g&&map.removeLayer(g);d.style.opacity=.4;}};
    list.appendChild(d);});
  if(nun){const d=document.createElement('div');d.className='row';
    d.innerHTML='<span class="sw" style="background:#c00"></span><span>Unassigned <small>('+nun+')</small></span>';
    let on=true;d.onclick=()=>{on=!on;const g=sl.__un;if(on){g&&g.addTo(map);d.style.opacity=1}else{g&&map.removeLayer(g);d.style.opacity=.4}};
    list.appendChild(d);}
}).catch(e=>{document.getElementById('sum').textContent='failed to load geojson: '+e;});
</script></body></html>"""


def render_html(out_dir, tiles, title, center, zoom, tile_opts, attribution):
    html = (HTML_TEMPLATE
            .replace("__TITLE__", title)
            .replace("__TILES__", json.dumps(tiles))
            .replace("__ATTR__", json.dumps(attribution))
            .replace("__TILEOPTS__", json.dumps(tile_opts))
            .replace("__CLAT__", repr(center[0])).replace("__CLON__", repr(center[1]))
            .replace("__ZOOM__", str(zoom)))
    with open(os.path.join(out_dir, "index.html"), "w") as fh:
        fh.write(html)


def main():
    ap = argparse.ArgumentParser(description="Render a Surge solution as GeoJSON + HTML map")
    ap.add_argument("--request", required=True, help="Surge solve request JSON")
    ap.add_argument("--solution", required=True, help="Surge solution JSON (surge_solve output)")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--title", default="Surge routes")
    ap.add_argument("--tiles", default="tiles/{z}/{x}/{y}.png",
                    help="Leaflet tile URL template (default: local pre-rendered tiles)")
    ap.add_argument("--tile-size", type=int, default=256)
    ap.add_argument("--max-native-zoom", type=int, default=11)
    ap.add_argument("--attribution", default="Carta (OTTO) · OSM data")
    ap.add_argument("--no-html", action="store_true")
    # road-following geometry (optional)
    ap.add_argument("--velo-graph", help="Velo .vlg/.osm.pbf: route legs on real roads")
    ap.add_argument("--velo-bin", default=os.path.join(REPO, "velo", "route_geometry"))
    ap.add_argument("--profile", default="truck", choices=["car", "truck", "bike", "foot", "any"])
    ap.add_argument("--weight", default="distance", choices=["distance", "duration"])
    # offline basemap pre-render (optional)
    ap.add_argument("--carta-graph", help="Carta .osm.pbf/.idx: pre-render tiles into <out>/tiles")
    ap.add_argument("--carta-bin", default=os.path.join(REPO, "carta", "tiledump"))
    ap.add_argument("--min-zoom", type=int, default=6)
    ap.add_argument("--max-zoom", type=int, default=11)
    a = ap.parse_args()

    request = json.load(open(a.request))
    solution = json.load(open(a.solution))
    os.makedirs(a.out_dir, exist_ok=True)

    routes_fc, stops_fc, legs = build_geojson(request, solution)
    if a.velo_graph:
        geom = route_geometry(legs, a.velo_graph, a.velo_bin, a.profile, a.weight)
        routes_fc, stops_fc, _ = build_geojson(request, solution, geometry=geom)

    json.dump(routes_fc, open(os.path.join(a.out_dir, "routes.geojson"), "w"))
    json.dump(stops_fc, open(os.path.join(a.out_dir, "stops.geojson"), "w"))

    bbox = bbox_of(routes_fc, stops_fc)
    if a.carta_graph and bbox:
        prerender_tiles(a.carta_graph, a.carta_bin, a.out_dir, bbox, a.min_zoom, a.max_zoom)

    if not a.no_html:
        center = ((bbox[0] + bbox[2]) / 2, (bbox[1] + bbox[3]) / 2) if bbox else (0.0, 0.0)
        tile_opts = {"tileSize": a.tile_size, "minZoom": a.min_zoom,
                     "maxZoom": max(a.max_zoom + 3, a.max_zoom),
                     "maxNativeZoom": a.max_native_zoom}
        render_html(a.out_dir, a.tiles, a.title, center, a.min_zoom + 2, tile_opts, a.attribution)

    npts = sum(len(l) for f in routes_fc["features"] for l in f["geometry"]["coordinates"])
    print(f"surge_map: {len(routes_fc['features'])} routes ({npts} geometry pts), "
          f"{len(stops_fc['features'])} points -> {a.out_dir}/", file=sys.stderr)


if __name__ == "__main__":
    main()
