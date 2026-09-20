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


def _ref(obj):
    """Human-readable identity carried on a request node (order no, plate, name),
    if the request author supplied one. Generic: first non-empty of ref/label/
    name/plate. None when absent -- callers keep the generic id as a fallback."""
    if not obj:
        return None
    for k in ("ref", "label", "name", "plate"):
        v = obj.get(k)
        if v not in (None, ""):
            return str(v)
    return None


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

    # demand + travel matrix let us report a running load and a cumulative
    # odometer per stop (the per-route breakdown). All optional: absent -> None.
    travel = request.get("travel") or {}
    tmatrix, tL = travel.get("distances"), travel.get("location_count")

    def _matrix_dist(a_loc, b_loc):
        if tmatrix and tL and a_loc is not None and b_loc is not None:
            return tmatrix[a_loc * tL + b_loc]
        return None

    def _task_loc(tid):
        t = tasks.get(tid)
        return t.get("location_id") if t else None

    def _depot_loc(vid):
        v = vehicles.get(vid, {})
        did = v.get("start_depot_id", 0)
        for d in request.get("depots", []):
            if d.get("id") == did:
                return d.get("location_id")
        return (request.get("depots") or [{}])[0].get("location_id")

    def _demand(tid):
        t = tasks.get(tid) or {}
        return t.get("demand") or []

    def _late_min(tid, arr):
        """Minutes a stop's committed arrival runs past its window close; 0 if on
        time or the task has no window. Reflects the solver's own schedule."""
        t = tasks.get(tid) or {}
        twl = t.get("tw_late")
        if twl is not None and arr is not None and arr > twl:
            return round((arr - twl) / 60.0, 1)
        return 0

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
        detail, peak, seq_n, odo, dloc = [], [], 0, 0.0, _depot_loc(vid)
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
            # per-stop breakdown for this trip: the truck departs the depot
            # loaded with the trip's total delivery demand and sheds it stop by
            # stop; `load` is what remains on board after each drop.
            trip_total = []
            for s in sts:
                for j, dv in enumerate(_demand(s["task_id"])):
                    if j >= len(trip_total):
                        trip_total.append(0.0)
                    trip_total[j] += dv
            for j, tv in enumerate(trip_total):
                if j >= len(peak):
                    peak.append(0.0)
                peak[j] = max(peak[j], tv)
            onboard = list(trip_total)
            prev_loc = dloc
            for s in sts:
                seq_n += 1
                tid = s["task_id"]
                dem = _demand(tid)
                for j, dv in enumerate(dem):
                    if j < len(onboard):
                        onboard[j] -= dv
                leg = _matrix_dist(prev_loc, _task_loc(tid))
                if leg is not None:
                    odo += leg
                prev_loc = _task_loc(tid)
                detail.append({
                    "seq": seq_n, "trip": trip + 1, "task_id": tid,
                    "ref": _ref(tasks.get(tid)),
                    "arr": _hms(s.get("arrival")), "dep": _hms(s.get("departure")),
                    "demand": dem, "load": [round(x, 3) for x in onboard],
                    "dist_cum": round(odo) if (tmatrix and tL) else None,
                    "late_min": _late_min(tid, s.get("arrival")),
                })
            leg = _matrix_dist(prev_loc, dloc)   # return to depot closes the odometer
            if leg is not None:
                odo += leg
        v = vehicles.get(vid, {})
        cap = v.get("capacity")
        vref = _ref(v)
        route_feats.append({
            "type": "Feature",
            "geometry": {"type": "MultiLineString", "coordinates": multiline},
            "properties": {
                "route": i, "vehicle_id": vid, "color": color,
                "label": f"vehicle {vid}" + (f" (cap {cap})" if cap is not None else ""),
                "vehicle_ref": vref,
                "n_stops": len(stops), "n_trips": len(trips),
                "distance": rt.get("distance"), "duration": rt.get("duration"),
                "on_duty": _hms(rt.get("duration")),
                "capacity": cap, "peak": [round(x, 3) for x in peak] if peak else None,
                "stops": detail,
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
                               "ref": _ref(tasks.get(s["task_id"])),
                               "type_": s.get("type"), "trip": s.get("trip_index", 0) + 1,
                               "arr": _hms(s.get("arrival")),
                               "late_min": _late_min(s["task_id"], s.get("arrival")),
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
                                              "ref": _ref(tasks.get(tid)),
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


def _downsample(pts, cap):
    """Keep at most `cap` points, evenly spaced, always including first + last."""
    n = len(pts)
    if n <= cap:
        return pts
    step = (n - 1) / (cap - 1)
    out = [pts[int(round(i * step))] for i in range(cap)]
    out[-1] = pts[-1]
    return out


def build_timeline(request, solution, geometry=None, max_pts_per_leg=48):
    """Per-vehicle animation timeline: for each moving leg, (t0, t1, [[lat,lon],...])
    where t0/t1 are the leg's clock times (seconds). A vehicle interpolates along
    the leg between t0 and t1, dwells at a stop in the gaps, is absent before its
    first departure and after its final return. Uses road geometry when supplied,
    straight legs otherwise. Returns {"span": [tmin, tmax], "vehicles": [...]}."""
    task_ll, depot_ll_for_vehicle, _req, tasks, vehicles = _coord_lookup(request)
    travel = request.get("travel") or {}
    L = travel.get("location_count"); dur = travel.get("durations")
    def leg_dur(a_loc, b_loc):
        if dur and L is not None and a_loc is not None and b_loc is not None:
            return dur[a_loc * L + b_loc]
        return 0.0
    def task_loc(tid):
        t = tasks.get(tid); return t.get("location_id") if t else None
    def depot_loc(vid):
        v = vehicles.get(vid, {}); did = v.get("start_depot_id", 0)
        for d in request.get("depots", []):
            if d.get("id") == did:
                return d.get("location_id")
        return request.get("depots", [{}])[0].get("location_id")

    tmin, tmax = float("inf"), float("-inf")
    veh_anim = []
    for i, rt in enumerate(solution.get("routes", [])):
        vid = rt.get("vehicle_id", i)
        depot = depot_ll_for_vehicle(vid) or (0.0, 0.0)
        dloc = depot_loc(vid)
        stops = rt.get("stops", [])
        trips = {}
        for s in stops:
            trips.setdefault(s.get("trip_index", 0), []).append(s)
        segs = []
        for trip in sorted(trips):
            sts = trips[trip]
            for k in range(len(sts) + 1):
                lid = f"R{i}_T{trip}_{k:03d}"
                if k == 0:                                   # depot -> first stop
                    nxt = sts[0]; t1 = nxt.get("arrival", 0)
                    t0 = t1 - leg_dur(dloc, task_loc(nxt["task_id"]))
                    fallback = [list(depot), list(task_ll(nxt["task_id"]) or depot)]
                elif k < len(sts):                           # stop[k-1] -> stop[k]
                    t0 = sts[k - 1].get("departure", 0); t1 = sts[k].get("arrival", 0)
                    fallback = [list(task_ll(sts[k - 1]["task_id"]) or depot),
                                list(task_ll(sts[k]["task_id"]) or depot)]
                else:                                        # last stop -> depot
                    prev = sts[-1]; t0 = prev.get("departure", 0)
                    t1 = t0 + leg_dur(task_loc(prev["task_id"]), dloc)
                    fallback = [list(task_ll(prev["task_id"]) or depot), list(depot)]
                pts = (geometry or {}).get(lid) or fallback
                if t1 <= t0 or len(pts) < 2:
                    continue
                segs.append([round(t0, 1), round(t1, 1), _downsample(pts, max_pts_per_leg)])
                tmin = min(tmin, t0); tmax = max(tmax, t1)
        if segs:
            veh_anim.append({"route": i, "vehicle_id": vid,
                             "vehicle_ref": _ref(vehicles.get(vid)),
                             "color": _color(i, len(solution.get("routes", []))), "segs": segs})
    if tmin == float("inf"):
        return {"span": [0, 0], "vehicles": []}
    return {"span": [round(tmin, 1), round(tmax, 1)], "vehicles": veh_anim}


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


# --- shared panel widgets (single-sourced; the standalone emitter imports these) ---
# CSS + JS for the per-route foldable breakdown panel and the route legend rows.
# Both this module's served page and the client offline standalone inject these,
# so the panel behaves identically in both.
ROUTE_DETAIL_CSS = r"""
 .caret{width:12px;flex:0 0 auto;color:#999;cursor:pointer;user-select:none;text-align:center;font-size:11px}
 .caret:hover{color:#000}
 .detail{margin:0 2px 6px 20px;overflow:auto}
 .capline{font-size:11px;color:#555;margin:2px 0 3px 0}
 table.det{border-collapse:collapse;font-size:11px;width:100%}
 table.det th,table.det td{padding:1px 5px;text-align:right;white-space:nowrap}
 table.det th{color:#999;font-weight:600;border-bottom:1px solid #ddd}
 table.det td:nth-child(2),table.det th:nth-child(2){text-align:left}
 table.det tr.trip td{text-align:left;color:#555;font-weight:600;padding-top:4px;background:#f4f4f4}
"""

ROUTE_DETAIL_JS = r"""
function _fmtN(x){return (x==null||x==='')?'':Number(x).toLocaleString(undefined,{maximumFractionDigits:0});}
// Foldable per-route breakdown: exact stop order with this-stop demand, load left
// on board after each drop, and the cumulative odometer. DIMS labels the demand
// dimensions (e.g. ["kg","pallets"]); falls back to d0,d1,... when unlabelled.
function routeDetailHTML(p, DIMS){
  var stops=p.stops||[]; if(!stops.length) return '<div class="capline">no stop detail</div>';
  var dims=DIMS&&DIMS.length?DIMS:((p.capacity||[]).map(function(_,i){return 'd'+i;}));
  var cap=p.capacity||[], peak=p.peak||[];
  var capline='';
  if(cap.length){capline='<div class="capline">peak load '+dims.map(function(L,i){
    return _fmtN(peak[i])+' / '+_fmtN(cap[i])+' '+L;}).join(' · ')+'</div>';}
  var head='<tr><th>#</th><th>order</th><th>arr</th>';
  dims.forEach(function(L){head+='<th>'+L+'</th><th title="on board after this stop">'+L+' load</th>';});
  head+='<th>km</th></tr>';
  var body='', curTrip=0, span=3+dims.length*2+1;
  stops.forEach(function(s){
    if((s.trip||1)!==curTrip){curTrip=s.trip||1;
      body+='<tr class="trip"><td colspan="'+span+'">trip '+curTrip+'</td></tr>';}
    var arr=(s.arr||'');
    if(s.late_min>0) arr='<span style="color:#c00" title="past delivery window">'+arr+' +'+s.late_min+'m</span>';
    var row='<td>'+s.seq+'</td><td>'+(s.ref!=null?s.ref:('#'+s.task_id))+'</td><td>'+arr+'</td>';
    dims.forEach(function(L,i){row+='<td>'+_fmtN(s.demand&&s.demand[i])+'</td><td>'+_fmtN(s.load&&s.load[i])+'</td>';});
    row+='<td>'+(s.dist_cum!=null?_fmtN(s.dist_cum/1000):'')+'</td>';
    body+='<tr>'+row+'</tr>';});
  return capline+'<table class="det">'+head+body+'</table>';
}
// Build one legend row + its (hidden) detail panel. The caret folds the detail;
// clicking the rest of the row is left to the caller (route show/hide toggle).
function makeRouteRow(f, DIMS){
  var p=f.properties, d=document.createElement('div'); d.className='row';
  var nlate=(p.stops||[]).filter(function(s){return s.late_min>0;}).length;
  var lateTag=nlate?(' · <span style="color:#c00">'+nlate+' late</span>'):'';
  d.innerHTML='<span class="caret">&#9656;</span><span class="sw" style="background:'+p.color+'"></span>'+
    '<span>#'+p.route+' <small>'+(p.vehicle_ref?('<b>'+p.vehicle_ref+'</b> '):'')+p.label+'</small>'+
    '<br><small class="muted">'+p.n_stops+' stops · '+p.n_trips+' trip(s)'+(p.on_duty?(' · '+p.on_duty):'')+lateTag+'</small></span>';
  var det=document.createElement('div'); det.className='detail'; det.style.display='none';
  det.innerHTML=routeDetailHTML(p, DIMS);
  var car=d.querySelector('.caret');
  car.onclick=function(e){e.stopPropagation(); var open=det.style.display==='none';
    det.style.display=open?'block':'none'; car.innerHTML=open?'&#9662;':'&#9656;';};
  return {row:d, detail:det};
}
"""


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
 #stats{display:grid;grid-template-columns:auto 1fr;gap:1px 10px;margin:6px 0 8px;font-size:12px}
 #stats .k{color:#888} #stats .v{text-align:right;font-variant-numeric:tabular-nums;font-weight:600}
 #toggleAll{margin:0 0 8px;font-size:11px;padding:3px 9px;cursor:pointer;border:1px solid #bbb;
   border-radius:4px;background:#f7f7f7}
 #toggleAll:hover{background:#eee}
 .row{display:flex;align-items:center;gap:6px;padding:2px 0;cursor:pointer;border-radius:4px}
 .row:hover{background:#f0f0f0} .sw{width:14px;height:4px;border-radius:2px;flex:0 0 auto}
 .row small{color:#666} .muted{color:#999}
 #tlToggle{margin:0 0 8px 6px;font-size:11px;padding:3px 9px;cursor:pointer;border:1px solid #bbb;border-radius:4px;background:#f7f7f7}
 #tlToggle:hover{background:#eee}
 #timeline{position:absolute;left:10px;right:300px;bottom:10px;z-index:1000;background:#fffe;border:1px solid #ccc;
   border-radius:8px;padding:8px 12px;box-shadow:0 2px 12px #0003;display:none;align-items:center;gap:10px;font-size:12px}
 #timeline.on{display:flex}
 #timeline input[type=range]{flex:1;min-width:120px}
 #tlPlay{cursor:pointer;border:1px solid #bbb;border-radius:4px;background:#f7f7f7;width:30px;padding:2px 0}
 #tlClock{font-variant-numeric:tabular-nums;font-weight:600;min-width:46px;text-align:center}
 #banner{position:absolute;bottom:8px;left:8px;z-index:1000;background:#fffe;border:1px solid #ccc;
   border-radius:6px;padding:4px 8px;font-size:11px;color:#a00;display:none}
__DETAIL_CSS__</style></head><body>
<div id="map"></div>
<div id="panel"><h1>__TITLE__</h1><div id="stats">loading…</div>
<button id="toggleAll">hide all</button><button id="tlToggle">&#9654; timeline</button><div id="list"></div></div>
<div id="timeline"><button id="tlPlay">&#9654;</button>
 <input id="tlRange" type="range" min="0" max="100" value="0" step="0.1"/>
 <span id="tlClock">--:--</span>
 <select id="tlSpeed"><option value="60">1&times;</option><option value="300" selected>5&times;</option>
  <option value="900">15&times;</option><option value="1800">30&times;</option></select></div>
<div id="banner">⚠ basemap tiles not loading (check the tile source / run a static server)</div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<script>
__DETAIL_JS__
const TILES=__TILES__, ATTR=__ATTR__, TZ=__TILEOPTS__, BOUNDS=__BOUNDS__, DSCALE=__DSCALE__, DUNIT=__DUNIT__, DIMS=__DIMS__;
const map=L.map('map',Object.assign({preferCanvas:true,minZoom:TZ.minZoom,maxZoom:TZ.maxZoom},
  BOUNDS?{maxBounds:BOUNDS,maxBoundsViscosity:0.7}:{})).setView([__CLAT__,__CLON__],__ZOOM__);
let te=0; const banner=document.getElementById('banner');
L.tileLayer(TILES,Object.assign({attribution:ATTR},TZ)).addTo(map)
  .on('tileerror',()=>{if(++te===4)banner.style.display='block'});
const layers={};const bounds=L.latLngBounds([]);let tlRefresh=null;
Promise.all([fetch('routes.geojson').then(r=>r.json()),fetch('stops.geojson').then(r=>r.json()),
  fetch('anim.json').then(r=>r.json()).catch(()=>null)])
.then(([routes,stops,anim])=>{
  routes.features.forEach(f=>{const lyr=L.geoJSON(f,{style:{color:f.properties.color,weight:3,opacity:.85}}).addTo(map);
    layers[f.properties.route]=lyr; try{bounds.extend(lyr.getBounds())}catch(e){}});
  const sl={};
  stops.features.forEach(f=>{const [lo,la]=f.geometry.coordinates,p=f.properties;
    if(p.kind==='depot'){L.marker([la,lo]).bindPopup('<b>'+p.label+'</b>').addTo(map);bounds.extend([la,lo]);return;}
    if(p.kind==='unassigned'){(sl.__un=sl.__un||L.layerGroup().addTo(map)).addLayer(
       L.circleMarker([la,lo],{radius:5,color:'#c00',weight:2,fillColor:'#fff',fillOpacity:1}).bindPopup('<b>'+p.label+'</b>'));
       bounds.extend([la,lo]);return;}
    const m=L.circleMarker([la,lo],{radius:4,color:'#222',weight:1,fillColor:p.color,fillOpacity:1})
      .bindPopup('<b>'+(p.ref!=null?p.ref:p.label)+'</b><br>'+p.label+' · route '+p.route+' · trip '+p.trip+(p.arr?(' · arr '+p.arr):'')+(p.late_min>0?(' <span style="color:#c00">(+'+p.late_min+'m late)</span>'):''));
    (sl[p.route]=sl[p.route]||L.layerGroup().addTo(map)).addLayer(m);});
  if(bounds.isValid())map.fitBounds(bounds.pad(0.05));
  // --- summary stats ---
  const nveh=routes.features.length;
  const totStops=routes.features.reduce((a,f)=>a+(f.properties.n_stops||0),0);
  const totTrips=routes.features.reduce((a,f)=>a+(f.properties.n_trips||0),0);
  const totDist=routes.features.reduce((a,f)=>a+(f.properties.distance||0),0)*DSCALE;
  const nun=stops.features.filter(f=>f.properties.kind==='unassigned').length;
  const served=totStops, total=served+nun;
  const fmt=x=>x.toLocaleString(undefined,{maximumFractionDigits:0});
  const st=[['vehicles',nveh],['trips',totTrips],['stops served',fmt(served)],
    ['unassigned','<span style="color:#c00">'+nun+'</span>'],
    ['served %',total?(100*served/total).toFixed(1)+'%':'-'],
    ['total distance',fmt(totDist)+' '+DUNIT],
    ['avg stops/veh',nveh?(served/nveh).toFixed(1):'-'],
    ['avg dist/veh',nveh?fmt(totDist/nveh)+' '+DUNIT:'-']];
  document.getElementById('stats').innerHTML=
    st.map(([k,v])=>'<div class="k">'+k+'</div><div class="v">'+v+'</div>').join('');
  const list=document.getElementById('list');
  routes.features.forEach(f=>{const p=f.properties;const {row:d,detail}=makeRouteRow(f,DIMS);
    d.onclick=()=>{const g=sl[p.route];const shown=map.hasLayer(layers[p.route]);
      if(shown){map.removeLayer(layers[p.route]);g&&map.removeLayer(g);d.style.opacity=.4;}
      else{layers[p.route].addTo(map);g&&g.addTo(map);d.style.opacity=1;}
      tlRefresh&&tlRefresh();};
    list.appendChild(d);list.appendChild(detail);});
  if(nun){const d=document.createElement('div');d.className='row';
    d.innerHTML='<span class="sw" style="background:#c00"></span><span>Unassigned <small>('+nun+')</small></span>';
    d.onclick=()=>{const g=sl.__un;const shown=g&&map.hasLayer(g);
      if(shown){map.removeLayer(g);d.style.opacity=.4}else{g&&g.addTo(map);d.style.opacity=1}};
    list.appendChild(d);}
  // toggle ALL routes + their stops on/off
  let allOn=true; const tgl=document.getElementById('toggleAll');
  tgl.onclick=()=>{allOn=!allOn; tgl.textContent=allOn?'hide all':'show all';
    Object.values(layers).forEach(l=>allOn?l.addTo(map):map.removeLayer(l));
    Object.values(sl).forEach(g=>allOn?g.addTo(map):map.removeLayer(g));
    document.querySelectorAll('#list .row').forEach(r=>r.style.opacity=allOn?1:.4);
    tlRefresh&&tlRefresh();};
  tlRefresh = initTimeline(anim, map, layers);
}).catch(e=>{document.getElementById('stats').textContent='failed to load geojson: '+e;});

// --- timeline animation: move a marker per vehicle along its road path by clock ---
// Returns a refresh() the route toggles call so markers appear/disappear in sync
// with their route lines. Markers show only when the timeline is on AND the
// route is visible on the map.
function initTimeline(anim, map, layers){
  const tlToggle=document.getElementById('tlToggle');
  if(!anim || !anim.vehicles || !anim.vehicles.length){ tlToggle.style.display='none'; return null; }
  const [T0,T1]=anim.span, moveLayer=L.layerGroup().addTo(map), markers={};
  anim.vehicles.forEach(v=>{markers[v.route]=L.circleMarker([0,0],
    {radius:6,color:'#111',weight:2,fillColor:v.color,fillOpacity:1,pane:'markerPane'})
    .bindTooltip(v.vehicle_ref?('#'+v.route+' '+v.vehicle_ref):('#'+v.route),{permanent:false});});
  function posAt(v,T){
    let last=null;
    for(const s of v.segs){const a=s[0],b=s[1],pts=s[2];
      if(T<a) break;
      if(T<=b){const f=(T-a)/((b-a)||1);
        let tot=0; for(let i=1;i<pts.length;i++) tot+=Math.hypot(pts[i][0]-pts[i-1][0],pts[i][1]-pts[i-1][1]);
        if(tot===0) return pts[0];
        let target=f*tot,acc=0;
        for(let i=1;i<pts.length;i++){const d=Math.hypot(pts[i][0]-pts[i-1][0],pts[i][1]-pts[i-1][1]);
          if(acc+d>=target){const g=(target-acc)/((d)||1);
            return [pts[i-1][0]+g*(pts[i][0]-pts[i-1][0]), pts[i-1][1]+g*(pts[i][1]-pts[i-1][1])];}
          acc+=d;}
        return pts[pts.length-1];}
      last=pts[pts.length-1];}   // between legs: dwelling at last stop
    return last;                 // before first departure: not yet on the road
  }
  const range=document.getElementById('tlRange'),clock=document.getElementById('tlClock'),
    play=document.getElementById('tlPlay'),speed=document.getElementById('tlSpeed'),bar=document.getElementById('timeline');
  // marker shows only when the timeline is on AND its route line is visible
  function render(T){ const on=bar.classList.contains('on');
    anim.vehicles.forEach(v=>{const m=markers[v.route];
      const p=(on && map.hasLayer(layers[v.route]))?posAt(v,T):null;
      if(p){m.setLatLng(p); if(!moveLayer.hasLayer(m))moveLayer.addLayer(m);}
      else if(moveLayer.hasLayer(m))moveLayer.removeLayer(m);});}
  const curT=()=>T0+(T1-T0)*(range.value/100);
  function upd(){const T=curT(),s=Math.round(T);
    clock.textContent=String(Math.floor(s/3600)).padStart(2,'0')+':'+String(Math.floor((s%3600)/60)).padStart(2,'0');
    render(T);}
  range.oninput=upd;
  let timer=null; const stop=()=>{if(timer){clearInterval(timer);timer=null;play.innerHTML='&#9654;';}};
  play.onclick=()=>{ if(timer){stop();return;} play.innerHTML='&#10073;&#10073;';
    timer=setInterval(()=>{let T=curT()+(+speed.value)*0.1;
      if(T>=T1){range.value=100;upd();stop();return;} range.value=(T-T0)/((T1-T0)||1)*100;upd();},100);};
  tlToggle.onclick=()=>{ bar.classList.toggle('on'); if(!bar.classList.contains('on')) stop(); upd(); };
  return upd;   // route toggles call this to sync markers with route visibility
}
</script></body></html>"""


def render_html(out_dir, tiles, title, center, zoom, tile_opts, attribution,
                distance_scale, distance_unit, maxbounds=None, dim_labels=None):
    html = (HTML_TEMPLATE
            .replace("__DETAIL_CSS__", ROUTE_DETAIL_CSS)
            .replace("__DETAIL_JS__", ROUTE_DETAIL_JS)
            .replace("__TITLE__", title)
            .replace("__TILES__", json.dumps(tiles))
            .replace("__ATTR__", json.dumps(attribution))
            .replace("__TILEOPTS__", json.dumps(tile_opts))
            .replace("__BOUNDS__", json.dumps(maxbounds))
            .replace("__DIMS__", json.dumps(dim_labels or []))
            .replace("__DSCALE__", repr(distance_scale))
            .replace("__DUNIT__", json.dumps(distance_unit))
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
    ap.add_argument("--distance-scale", type=float, default=0.001,
                    help="multiply solution route distances by this for the stats panel "
                         "(default 0.001: meters -> km)")
    ap.add_argument("--distance-unit", default="km", help="unit label for the distance stat")
    ap.add_argument("--demand-labels", default="",
                    help="comma-separated names for the demand dimensions shown in the per-route "
                         "breakdown (e.g. 'kg,pallets'); defaults to d0,d1,... ")
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
    geom = None
    if a.velo_graph:
        geom = route_geometry(legs, a.velo_graph, a.velo_bin, a.profile, a.weight)
        routes_fc, stops_fc, _ = build_geojson(request, solution, geometry=geom)

    json.dump(routes_fc, open(os.path.join(a.out_dir, "routes.geojson"), "w"))
    json.dump(stops_fc, open(os.path.join(a.out_dir, "stops.geojson"), "w"))
    anim = build_timeline(request, solution, geometry=geom)
    json.dump(anim, open(os.path.join(a.out_dir, "anim.json"), "w"))

    bbox = bbox_of(routes_fc, stops_fc)
    if a.carta_graph and bbox:
        prerender_tiles(a.carta_graph, a.carta_bin, a.out_dir, bbox, a.min_zoom, a.max_zoom)

    if not a.no_html:
        center = ((bbox[0] + bbox[2]) / 2, (bbox[1] + bbox[3]) / 2) if bbox else (0.0, 0.0)
        # cap zoom at the deepest available tile level (no upscaling past it) so
        # the map never requests tiles that were not rendered.
        top_z = min(a.max_native_zoom, a.max_zoom)
        tile_opts = {"tileSize": a.tile_size, "minZoom": a.min_zoom, "maxZoom": top_z,
                     "maxNativeZoom": top_z}
        maxbounds = None
        if bbox:
            dla = (bbox[2] - bbox[0]) * 0.08 or 0.1
            dlo = (bbox[3] - bbox[1]) * 0.08 or 0.1
            maxbounds = [[bbox[0] - dla, bbox[1] - dlo], [bbox[2] + dla, bbox[3] + dlo]]
        dim_labels = [c for c in a.demand_labels.split(",") if c]
        if not dim_labels:
            ndim = request.get("dimension_count") or len((request.get("tasks") or [{}])[0].get("demand", []))
            dim_labels = [f"d{j}" for j in range(ndim)]
        render_html(a.out_dir, a.tiles, a.title, center, a.min_zoom + 2, tile_opts,
                    a.attribution, a.distance_scale, a.distance_unit, maxbounds, dim_labels)

    npts = sum(len(l) for f in routes_fc["features"] for l in f["geometry"]["coordinates"])
    print(f"surge_map: {len(routes_fc['features'])} routes ({npts} geometry pts), "
          f"{len(stops_fc['features'])} points -> {a.out_dir}/", file=sys.stderr)


if __name__ == "__main__":
    main()
