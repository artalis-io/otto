#!/usr/bin/env python3
"""Geocode an nx_pipeline orders JSON with defense-in-depth verification.

Authoritative source: Google Geocoding. Independent third source: HERE. In-house
cross-check: Locus (OSM, via the batch tool, where a PBF is supplied). Plus checks
that need no third party: country-bbox envelope, expected-country-from-zip,
provider precision tier, partial-match flag, GeoNames postcode envelope + RED
fallback, and a routes_fact baseline-cluster check (stops on one trip cluster).

Every order is tiered GREEN / YELLOW / APPROX / RED so nothing wrong is silently
used. Google/HERE responses are cached (--cache-dir) so re-runs don't re-bill;
GEOCODE_OFFLINE=1 forbids live calls entirely (cache-only).

Dataset-agnostic: all paths are CLI args; keys come from a .env (--env). See -h.
"""
import os, sys, json, time, math, re, subprocess, urllib.parse, urllib.request, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from normalize_address import expand, parse, looks_like_poi, HAVE_LIBPOSTAL

# Dataset-agnostic. All paths come from CLI args (see main); these are set there.
# Reads an nx_pipeline "orders" canonical JSON (records with address_geocode/zip/
# city/street/order_no) and, optionally, a routes_fact JSON for the trip-cluster
# cross-check. Keys come from the environment (.env). Client-specific paths live
# in the caller's driver, never here.
ORDERS=FACT=OUT=REPORT=CACHE_DIR=CACHE=GEO_DIR=None
PBF=""
LC_BATCH = os.environ.get("LC_GEOCODE_BATCH", "/tmp/lc_geocode_batch")

# rough country bounding boxes (min_lat,max_lat,min_lon,max_lon) for envelope sanity
BBOX = {
    "HU": (45.7,48.6,16.1,22.9), "RO": (43.6,48.3,20.2,29.7), "BG": (41.2,44.2,22.3,28.6),
    "SK": (47.7,49.6,16.8,22.6), "AT": (46.3,49.1,9.5,17.2),  "NL": (50.7,53.6,3.3,7.3),
    "LV": (55.6,58.1,20.9,28.3), "RS": (42.2,46.2,18.8,23.0), "CZ": (48.5,51.1,12.0,18.9),
    "PL": (49.0,54.9,14.1,24.2), "DE": (47.2,55.1,5.8,15.1),  "HR": (42.3,46.6,13.4,19.5),
    "SI": (45.4,46.9,13.4,16.6), "FI": (59.7,70.1,20.5,31.6), "IT": (36.6,47.1,6.6,18.6),
    "SE": (55.3,69.1,11.1,24.2), "UA": (44.3,52.4,22.1,40.2),
}

def load_env(path=None):
    # default: repo-root .env (script is at nexus/scripts/), or $GEOCODE_ENV
    if not path:
        repo=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        path=os.environ.get("GEOCODE_ENV", os.path.join(repo,".env"))
    if not os.path.exists(path): return
    for line in open(path):
        line=line.strip()
        if line and not line.startswith("#") and "=" in line:
            k,v=line.split("=",1); os.environ.setdefault(k,v)

def haversine(a,b):
    R=6371000.0
    la1,lo1,la2,lo2=map(math.radians,[a[0],a[1],b[0],b[1]])
    d=math.sin((la2-la1)/2)**2+math.cos(la1)*math.cos(la2)*math.sin((lo2-lo1)/2)**2
    return 2*R*math.asin(math.sqrt(d))

def norm_pc(z):
    """Normalize a postcode for cross-source matching: drop country prefix
    (A-, LV-) and spaces, uppercase. e.g. 'A-2355'->'2355', '010 09'->'01009'."""
    z=(z or "").strip().upper()
    z=re.sub(r"^[A-Z]{1,3}-","",z)
    return z.replace(" ","")

_GEO={}   # (cc, normed_pc) -> (lat,lon);  plus (cc, leading4digits) fallback index
_GEO4={}
def load_geonames():
    if _GEO: return
    if not os.path.isdir(GEO_DIR): return
    for fn in os.listdir(GEO_DIR):
        if not fn.endswith(".txt"): continue
        for line in open(f"{GEO_DIR}/{fn}",encoding="utf-8"):
            f=line.rstrip("\n").split("\t")
            if len(f)<11 or not f[9] or not f[10]: continue
            cc=f[0]; pc=norm_pc(f[1])
            try: coord=(float(f[9]),float(f[10]))
            except ValueError: continue
            _GEO.setdefault((cc,pc),coord)
            d4=re.match(r"\d{1,4}",pc)
            if d4: _GEO4.setdefault((cc,d4.group()),coord)

def pc_centroid(cc,zip_):
    """GeoNames postcode centroid for (country, postcode), with digit-prefix
    fallback (handles NL '1624 NR'->'1624'). Returns (lat,lon) or None."""
    load_geonames()
    if not cc: return None
    z=norm_pc(zip_)
    if (cc,z) in _GEO: return _GEO[(cc,z)]
    d4=re.match(r"\d{1,4}",z)
    if d4 and (cc,d4.group()) in _GEO4: return _GEO4[(cc,d4.group())]
    return None

def expected_country(zip_):
    z=(zip_ or "").strip()
    if z.startswith("A-"): return "AT"
    if z.startswith("LV-"): return "LV"
    if len(z)==6 and z.isdigit(): return "RO"
    # 4-digit is ambiguous HU/BG; 5-digit SK/other -> leave None (don't over-claim)
    return None

# statuses that are a FINAL answer and safe to cache forever; transient errors
# (network, quota) are NOT cached so a re-run retries them without losing money
# on the ones that already succeeded.
TERMINAL = {"OK", "ZERO_RESULTS", "INVALID_REQUEST"}
_stats = {"hits": 0, "calls": 0}
# GEOCODE_OFFLINE=1 -> never hit any API; a cache miss returns a sentinel instead
# of a billable call. Reproduce runs set this so caches are reused, never recomputed.
OFFLINE = os.environ.get("GEOCODE_OFFLINE") == "1"

def google_geocode(addr, key, cache):
    if addr in cache:                    # cache hit -> no API call, no cost
        _stats["hits"] += 1
        return cache[addr]
    if OFFLINE:                          # reproduce mode: do not spend / recompute
        return {"status": "CACHE_MISS_OFFLINE"}
    _stats["calls"] += 1                 # a real, billable request
    u="https://maps.googleapis.com/maps/api/geocode/json?"+urllib.parse.urlencode({"address":addr,"key":key})
    for attempt in range(3):
        try:
            d=json.load(urllib.request.urlopen(u,timeout=20)); break
        except Exception:
            time.sleep(1.0)
    else:
        d={"status":"NETWORK_ERROR","results":[]}
    out={"status":d.get("status")}
    if d.get("results"):
        r=d["results"][0]; loc=r["geometry"]["location"]
        out.update(lat=loc["lat"], lon=loc["lng"],
                   location_type=r["geometry"].get("location_type"),
                   partial=r.get("partial_match",False),
                   cc=next((c["short_name"] for c in r["address_components"] if "country" in c["types"]),None),
                   formatted=r.get("formatted_address"))
    if out["status"] in TERMINAL:        # only persist final answers
        cache[addr]=out
    return out

def google_components(order, key, cache):
    """Targeted re-geocode using libpostal components + Google's `components`
    filter (country + postal_code). Aims to lift centroid/partial matches to
    ROOFTOP. Cached separately so it's a one-time spend."""
    p = parse(order["address_geocode"])
    road = p.get("road",""); hn = p.get("house_number") or p.get("house","")
    city = order.get("city","") or p.get("city","")
    addr = ", ".join(x for x in [f"{hn} {road}".strip(), city] if x) or order["address_geocode"]
    comps=[]
    if order.get("zip","").strip(): comps.append("postal_code:"+order["zip"].strip())
    if order.get("geo_cc"): comps.append("country:"+order["geo_cc"])
    ckey=f"RG|{addr}|{'|'.join(comps)}"
    if ckey in cache: _stats["hits"]+=1; return cache[ckey]
    if OFFLINE: return {"status":"CACHE_MISS_OFFLINE"}
    _stats["calls"]+=1
    q={"address":addr,"key":key}
    if comps: q["components"]="|".join(comps)
    u="https://maps.googleapis.com/maps/api/geocode/json?"+urllib.parse.urlencode(q)
    try:
        d=json.load(urllib.request.urlopen(u,timeout=20))
    except Exception:
        d={"status":"NETWORK_ERROR","results":[]}
    out={"status":d.get("status")}
    if d.get("results"):
        r=d["results"][0]; loc=r["geometry"]["location"]
        out.update(lat=loc["lat"],lon=loc["lng"],location_type=r["geometry"].get("location_type"),
                   partial=r.get("partial_match",False),
                   cc=next((c["short_name"] for c in r["address_components"] if "country" in c["types"]),None))
    if out["status"] in TERMINAL: cache[ckey]=out
    return out

# HERE returns ISO-3 country codes; map the ones in play to ISO-2 for comparison.
ISO3={"HUN":"HU","SVK":"SK","ROU":"RO","BGR":"BG","AUT":"AT","NLD":"NL","LVA":"LV",
      "SRB":"RS","SVN":"SI","FIN":"FI","CZE":"CZ","POL":"PL","DEU":"DE","HRV":"HR",
      "ITA":"IT","SWE":"SE","UKR":"UA"}
_hstats={"hits":0,"calls":0}

def here_geocode(addr, key, cache):
    """Independent third source (independent of both Google and OSM/Locus)."""
    if addr in cache: _hstats["hits"]+=1; return cache[addr]
    if OFFLINE: return {"status":"CACHE_MISS_OFFLINE"}
    _hstats["calls"]+=1
    u="https://geocode.search.hereapi.com/v1/geocode?"+urllib.parse.urlencode({"q":addr,"apiKey":key})
    for _ in range(3):
        try: d=json.load(urllib.request.urlopen(u,timeout=20)); break
        except Exception: time.sleep(1.0)
    else: d={"items":[]}
    items=d.get("items",[])
    out={"status":"OK" if items else "ZERO_RESULTS"}
    if items:
        r=items[0]; p=r["position"]
        out.update(lat=p["lat"], lon=p["lng"], result_type=r.get("resultType"),
                   cc=ISO3.get((r.get("address") or {}).get("countryCode"),
                               (r.get("address") or {}).get("countryCode")),
                   qs=(r.get("scoring") or {}).get("queryScore"))
    cache[addr]=out                      # HERE returns a definite answer; safe to cache
    return out

def main():
    ap=argparse.ArgumentParser(description="3-way geocode + verify an nx_pipeline orders JSON")
    ap.add_argument("--orders", required=True, help="orders canonical JSON (nx_pipeline output)")
    ap.add_argument("--fact", help="routes_fact JSON for the trip-cluster cross-check")
    ap.add_argument("--out", help="output geocoded JSON (default: <orders-dir>/orders.geocoded.json)")
    ap.add_argument("--report", help="text report path (default: <orders-dir>/geocode_report.txt)")
    ap.add_argument("--cache-dir", help="geocode cache dir (default: <orders-dir>/../.geocode_cache)")
    ap.add_argument("--pbf", default="", help="OSM PBF for the Locus cross-check (optional)")
    ap.add_argument("--geonames", help="GeoNames postal dir (default: <cache-dir>/geonames)")
    ap.add_argument("--env", help="path to .env with API keys (default: repo-root .env)")
    a=ap.parse_args()
    global ORDERS,FACT,OUT,REPORT,CACHE_DIR,CACHE,GEO_DIR,PBF
    ORDERS=a.orders; base=os.path.dirname(os.path.abspath(a.orders))
    FACT=a.fact or os.path.join(base,"routes_fact.json")
    OUT=a.out or os.path.join(base,"orders.geocoded.json")
    REPORT=a.report or os.path.join(base,"geocode_report.txt")
    CACHE_DIR=a.cache_dir or os.path.join(os.path.dirname(base),".geocode_cache")
    CACHE=os.path.join(CACHE_DIR,"google.json")
    GEO_DIR=a.geonames or os.path.join(CACHE_DIR,"geonames")
    PBF=a.pbf

    load_env(a.env)
    key=os.environ.get("GOOGLE_MAPS_API_KEY")
    if not key: print("no GOOGLE_MAPS_API_KEY in .env",file=sys.stderr); return 2
    orders=json.load(open(ORDERS))["records"]
    fact=json.load(open(FACT))["records"] if os.path.exists(FACT) else []
    os.makedirs(CACHE_DIR,exist_ok=True)
    cache=json.load(open(CACHE)) if os.path.exists(CACHE) else {}

    # --- Google (authoritative), cached ---
    print(f"geocoding {len(orders)} orders via Google (cache has {len(cache)})...",file=sys.stderr)
    for i,o in enumerate(orders):
        google_geocode(o["address_geocode"], key, cache)
        if i%50==0:
            json.dump(cache,open(CACHE,"w")); time.sleep(0.02)
    json.dump(cache,open(CACHE,"w"))

    print(f"Google: {_stats['calls']} live calls, {_stats['hits']} cache hits",file=sys.stderr)

    # --- HERE (independent third source), cached by address ---
    here_key=os.environ.get("HERE_API_KEY")
    HERE_CACHE=f"{CACHE_DIR}/here.json"
    here={}
    if here_key:
        here_cache=json.load(open(HERE_CACHE)) if os.path.exists(HERE_CACHE) else {}
        for i,o in enumerate(orders):
            here[o["address_geocode"]]=here_geocode(o["address_geocode"], here_key, here_cache)
            if i%50==0: json.dump(here_cache,open(HERE_CACHE,"w"),ensure_ascii=False); time.sleep(0.02)
        json.dump(here_cache,open(HERE_CACHE,"w"),ensure_ascii=False)
        print(f"HERE: {_hstats['calls']} live calls, {_hstats['hits']} cache hits",file=sys.stderr)
    else:
        print("no HERE_API_KEY; skipping third source",file=sys.stderr)

    # --- Locus (cross-check), one PBF load for all queries; cached by query ---
    LOCUS_CACHE=f"{CACHE_DIR}/locus.json"
    locus={}
    lc_cache=json.load(open(LOCUS_CACHE)) if os.path.exists(LOCUS_CACHE) else {}
    # feed Locus: libpostal-parsed ROAD as the search query, CITY as an
    # independent bbox anchor, and HOUSE NUMBER for house-level post-filtering
    # (city-anchored two-stage search + house-number match). Google stays on the
    # original cached string. Query key includes all three so the cache
    # invalidates if any changes.
    def lq(o):
        p=parse(o.get("address_geocode","")) if o.get("address_geocode") else {}
        road=p.get("road") or o.get("street","")
        hn=p.get("house_number") or p.get("house") or ""
        return (expand(road)[0] if road else "", o.get("city",""), hn, (o.get("zip","") or "").strip())
    want={o["order_no"]: lq(o) for o in orders}
    qkey={oid:f"{s}|{c}|{h}|{z}" for oid,(s,c,h,z) in want.items()}
    stale = any(lc_cache.get(oid,{}).get("_q")!=qkey[oid] for oid in want) or set(want)-set(lc_cache)
    if not stale and lc_cache:
        locus={k:v for k,v in lc_cache.items() if v.get("found")}
        print(f"Locus: {len(locus)} hits from cache",file=sys.stderr)
    elif os.path.exists(LC_BATCH) and os.path.exists(PBF):
        tsv="".join(f'{oid}\t{s}\t{c}\t{h}\t{z}\n' for oid,(s,c,h,z) in want.items())
        print("running Locus batch (building HU index from PBF)...",file=sys.stderr)
        p=subprocess.run([LC_BATCH,PBF],input=tsv,capture_output=True,text=True,timeout=600)
        newc={}
        for line in p.stdout.splitlines():
            try:
                r=json.loads(line); r["_q"]=qkey.get(r.get("id"),"")
                newc[r["id"]]=r
                if r.get("found"): locus[r["id"]]=r
            except json.JSONDecodeError: pass
        json.dump(newc,open(LOCUS_CACHE,"w"),ensure_ascii=False)
    else:
        print("Locus batch or PBF missing; skipping cross-check",file=sys.stderr)

    # --- fuse + 3-way vote (Google / HERE / Locus) ---
    STRONG=250.0      # two commercial sources on the same building/block
    SOFT=1500.0       # same neighbourhood — minor rooftop-vs-centroid difference
    LAGREE=2000.0     # anything involving Locus (coarser)
    def inbox(cc,lat,lon): return cc in BBOX and BBOX[cc][0]<=lat<=BBOX[cc][1] and BBOX[cc][2]<=lon<=BBOX[cc][3]

    out=[]; upgraded=0
    for o in orders:
        rec=dict(o); flags=[]; checks=[]

        # Google: primary; if not precise, try components re-geocode and keep the better
        g=cache[o["address_geocode"]]
        g_ok = g.get("status")=="OK" and "lat" in g
        g_prec = g_ok and g.get("location_type") in ("ROOFTOP","RANGE_INTERPOLATED")
        if g_ok and not g_prec:
            g2=google_components({**o,"geo_cc":g.get("cc")}, key, cache)
            if g2.get("status")=="OK" and "lat" in g2 and g2.get("location_type") in ("ROOFTOP","RANGE_INTERPOLATED"):
                g=g2; g_prec=True; flags.append("regeocoded"); upgraded+=1
        gp=(g["lat"],g["lon"]) if g_ok else None; g_cc=g.get("cc") if g_ok else None

        # HERE
        h=here.get(o["address_geocode"],{})
        h_ok = h.get("status")=="OK" and "lat" in h
        h_prec = h_ok and h.get("result_type")=="houseNumber"
        hp=(h["lat"],h["lon"]) if h_ok else None; h_cc=h.get("cc") if h_ok else None

        # Locus. Only a postcode-matched Locus point is trusted as a vote:
        # pc_match => right town, 97% within 2km, 0 gross errors; +hn => house level.
        l=locus.get(o["order_no"]); lp=(l["lat"],l["lon"]) if (l and "lat" in l) else None
        l_trust = bool(l and l.get("pc_match"))
        l_house = l_trust and bool(l.get("hn_match"))

        d_gh=haversine(gp,hp) if gp and hp else None
        d_gl=haversine(gp,lp) if gp and lp else None
        d_hl=haversine(hp,lp) if hp and lp else None
        if gp: rec["google"]={"lat":gp[0],"lon":gp[1],"type":g.get("location_type"),"cc":g_cc}
        if hp: rec["here"]={"lat":hp[0],"lon":hp[1],"type":h.get("result_type"),"cc":h_cc,"qs":h.get("qs")}
        if lp: rec["locus"]={"lat":lp[0],"lon":lp[1],"class":l.get("class"),"dist_m":round(d_gl,1) if d_gl is not None else None,
                             "in_box":bool(l.get("in_box")),"hn_match":bool(l.get("hn_match")),
                             "pc_match":bool(l.get("pc_match"))}

        used=None; tier="RED"
        if g_ok and h_ok and g_cc and h_cc and g_cc!=h_cc:
            tier="RED"; checks.append(f"country conflict G={g_cc} H={h_cc}")
            used=gp; cc=g_cc
        elif d_gh is not None and d_gh<=STRONG:
            used=gp; cc=g_cc; flags.append("consensus_gh")          # 2 independent commercial sources agree
            three = (d_gl is not None and d_gl<=LAGREE) or (d_hl is not None and d_hl<=LAGREE)
            if three: flags.append("consensus_3way")
            tier="GREEN" if (g_prec or h_prec) else "YELLOW"
        elif d_gh is not None and d_gh<=SOFT:                        # minor difference, same area
            used=gp; cc=g_cc; flags.append("minor_gh_diff")
            checks.append(f"G↔H differ {d_gh:.0f}m (minor)")
            tier="GREEN" if (g_prec and h_prec) else "YELLOW"
        elif g_ok and h_ok:                                          # real conflict (>1.5km) -> only a TRUSTED Locus breaks the tie
            hl_ok=l_trust and d_hl is not None and d_hl<=LAGREE
            gl_ok=l_trust and d_gl is not None and d_gl<=LAGREE
            if hl_ok and not gl_ok:
                used=hp; cc=h_cc; tier="YELLOW"; flags.append("google_outlier")  # HERE+Locus outvote Google
                checks.append(f"G↔H differ {d_gh/1000:.1f}km; Locus sides HERE")
            elif gl_ok and not hl_ok:
                used=gp; cc=g_cc; tier="YELLOW"; flags.append("here_outlier")
                checks.append(f"G↔H differ {d_gh/1000:.1f}km; Locus sides Google")
            else:
                used=gp if g_prec else (hp if h_prec else gp); cc=g_cc if used is gp else h_cc
                tier="YELLOW" if (g_prec or h_prec) else "RED"
                checks.append(f"G↔H conflict {d_gh/1000:.1f}km, no tie-break")
        elif g_ok:
            used=gp; cc=g_cc; tier="GREEN" if g_prec else "YELLOW"; flags.append("google_only")
        elif h_ok:
            used=hp; cc=h_cc; tier="GREEN" if h_prec else "YELLOW"; flags.append("here_only")
        else:
            checks.append(f"no geocode (G={g.get('status')} H={h.get('status')})")

        # Trusted Locus (postcode matched) is a first-class consensus vote:
        #   precise commercial point + Locus agree (<=2km) -> independent confirmation -> GREEN
        #   imprecise commercial + house-level Locus (<=1.5km) -> adopt the Locus house point -> GREEN
        if l_trust and lp is not None and used is not None and tier=="YELLOW":
            d_ul=haversine(used,lp)
            used_prec=(used is gp and g_prec) or (used is hp and h_prec)
            if used_prec and d_ul is not None and d_ul<=LAGREE:
                tier="GREEN"; flags.append("consensus_locus")
            elif l_house and d_ul is not None and d_ul<=SOFT:
                used=lp; cc=(g_cc or h_cc); tier="GREEN"; flags.append("consensus_locus_house")

        if used is not None:
            lat,lon=used
            if cc and not inbox(cc,lat,lon):
                tier="RED"; checks.append(f"point outside {cc} bbox")
            gt = g.get("location_type") if used is gp else h.get("result_type") if used is hp \
                 else "locus_"+(l.get("class") or "address")
            rec.update(lat=lat, lon=lon, geo_cc=cc, geo_type=gt)
        if looks_like_poi(o["address_geocode"]): flags.append("poi_name")
        rec["geo_tier"]=tier; rec["geo_checks"]=checks
        if flags: rec["flags"]=flags
        out.append(rec)

    json.dump(cache,open(CACHE,"w"),ensure_ascii=False)
    print(f"targeted re-geocode upgrades: {upgraded}",file=sys.stderr)

    # --- baseline cluster check via routes_fact: stops per fuvar should cluster ---
    from collections import defaultdict
    coords={o["order_no"]:(o["lat"],o["lon"]) for o in out if o.get("lat") is not None}
    trip=defaultdict(list)
    for f in fact:
        if f["order_no"] in coords: trip[f["fuvar_no"]].append(f["order_no"])
    outlier=0
    idx={o["order_no"]:o for o in out}
    for fv,ids in trip.items():
        pts=[coords[i] for i in ids]
        if len(pts)<3: continue
        clat=sum(p[0] for p in pts)/len(pts); clon=sum(p[1] for p in pts)/len(pts)
        for i in ids:
            d=haversine(coords[i],(clat,clon))
            if d>300000:  # >300km from its trip centroid
                idx[i]["geo_checks"].append(f"cluster-outlier {d/1000:.0f}km from trip {fv}")
                if idx[i]["geo_tier"]=="GREEN": idx[i]["geo_tier"]="YELLOW"
                outlier+=1

    # --- GeoNames postcode-centroid: RED fallback + all-orders envelope check ---
    # trip clusters from routes_fact (only from CONFIDENT stops) for cross-check
    o2fuvar={}
    for f in fact: o2fuvar.setdefault(f["order_no"], f["fuvar_no"])
    trip_pts=defaultdict(list)
    for o in out:
        if o.get("lat") is not None and o["geo_tier"] in ("GREEN","YELLOW"):
            fv=o2fuvar.get(o["order_no"])
            if fv: trip_pts[fv].append((o["lat"],o["lon"]))
    def trip_centroid(order_no):
        fv=o2fuvar.get(order_no); pts=trip_pts.get(fv,[])
        if len(pts)<2: return None
        return (sum(p[0] for p in pts)/len(pts), sum(p[1] for p in pts)/len(pts))

    approx=far_pc=trip_conflict=0
    for o in out:
        cc=o.get("geo_cc") or expected_country(o.get("zip",""))
        pc=pc_centroid(cc, o.get("zip",""))
        tc=trip_centroid(o["order_no"])
        # (1) envelope check: a located point far from its postcode is untrustworthy
        far=False
        if pc and o.get("lat") is not None and o["geo_tier"] in ("GREEN","YELLOW"):
            dpc=haversine((o["lat"],o["lon"]), pc)
            if dpc>30000:
                far=True; far_pc+=1
                o.setdefault("flags",[]).append("far_from_postcode")
                o["geo_checks"].append(f"chosen point {dpc/1000:.0f}km from postcode centroid")
        # (2) approximate: RED (no trustworthy point) OR far-from-postcode -> postcode
        #     centroid (or trip cluster), cross-checked against the historical trip.
        if o["geo_tier"]=="RED" or far:
            pt,src,unc = (pc,"postcode_centroid",5000) if pc else \
                         ((tc,"trip_cluster",15000) if tc else (None,None,None))
            if pt:
                if pc and tc:
                    dt=haversine(pc,tc)
                    o["geo_checks"].append(f"postcode↔trip-cluster {dt/1000:.0f}km")
                    if dt>30000:
                        o.setdefault("flags",[]).append("approx_conflicts_trip"); trip_conflict+=1
                o.update(lat=pt[0], lon=pt[1], geo_tier="APPROX", geo_source=src, uncertainty_m=unc)
                o.setdefault("flags",[]).append("approx_"+src)
                approx+=1

    json.dump({"nx_canonical":1,"record_count":len(out),"records":out},open(OUT,"w"),ensure_ascii=False,indent=1)

    # --- report ---
    from collections import Counter
    tiers=Counter(o["geo_tier"] for o in out)
    by_cc=Counter(o.get("geo_cc","?") for o in out)
    gtype=Counter(o.get("geo_type","-") for o in out)
    def has(f): return lambda o: f in o.get("flags",[])
    cons_gh=sum(1 for o in out if has("consensus_gh")(o))
    cons_3=sum(1 for o in out if has("consensus_3way")(o))
    g_out=[o for o in out if has("google_outlier")(o)]
    h_out=sum(1 for o in out if has("here_outlier")(o))
    regeo=sum(1 for o in out if has("regeocoded")(o))
    here_cov=sum(1 for o in out if "here" in o)
    locus_cov=sum(1 for o in out if "locus" in o)
    poi=sum(1 for o in out if has("poi_name")(o))
    lines=[]
    lines.append("=== Geocode verification report (3-way: Google / HERE / Locus) ===")
    lines.append(f"libpostal normalization: {'ON' if HAVE_LIBPOSTAL else 'OFF (regex fallback)'}  |  POI/facility-name inputs: {poi}")
    lines.append(f"Google API this run: {_stats['calls']} live, {_stats['hits']} cached  |  "
                 f"HERE: {_hstats['calls']} live, {_hstats['hits']} cached  (cost only on live)")
    lines.append(f"orders={len(out)}  GREEN={tiers['GREEN']} YELLOW={tiers['YELLOW']} APPROX={tiers['APPROX']} RED={tiers['RED']}")
    lines.append(f"coverage: Google {sum(1 for o in out if 'google' in o)}, HERE {here_cov}, Locus {locus_cov}")
    apx=sum(1 for o in out if o.get("geo_source")=="postcode_centroid")
    apt=sum(1 for o in out if o.get("geo_source")=="trip_cluster")
    farpc=sum(1 for o in out if has("far_from_postcode")(o))
    conf=sum(1 for o in out if has("approx_conflicts_trip")(o))
    lines.append(f"GeoNames postcode-centroid: {apx} RED→APPROX (postcode) + {apt} (trip-cluster); "
                 f"envelope check flagged {farpc} located points >30km from their postcode (potential wrong-town); "
                 f"{conf} approx conflict their trip cluster")
    cl=sum(1 for o in out if has("consensus_locus")(o))
    clh=sum(1 for o in out if has("consensus_locus_house")(o))
    lines.append(f"Google↔HERE consensus (<=250m): {cons_gh}   3-way consensus (+Locus): {cons_3}")
    lines.append(f"GREEN via trusted Locus (pc_match): {cl} precise-commercial+Locus, {clh} Locus house point adopted")
    lines.append(f"conflicts resolved by Locus vote: google_outlier={len(g_out)}, here_outlier={h_out}")
    lines.append(f"targeted re-geocode upgrades: {regeo}")
    lines.append(f"Google location_type: {dict(gtype)}")
    lines.append(f"country distribution: {dict(by_cc)}")
    lines.append(f"baseline cluster-outliers (>300km from trip centroid): {outlier}")
    if g_out:
        lines.append("")
        lines.append("--- GOOGLE-OUTLIER (HERE+Locus agree, Google differs — highest-value catches) ---")
        for o in g_out[:10]:
            lines.append(f"  {o['order_no']} [{o.get('geo_cc','?')}] {o['city']}: {', '.join(o['geo_checks'])}")
    lines.append("")
    lines.append("--- RED (do not auto-use; review/fallback) ---")
    reds=[o for o in out if o["geo_tier"]=="RED"]
    for o in reds[:25]:
        lines.append(f"  {o['order_no']} [{o.get('geo_cc','?')}] {o['city']}: {', '.join(o['geo_checks'])}")
    if len(reds)>25: lines.append(f"  ... +{len(reds)-25} more")
    rpt="\n".join(lines)
    open(REPORT,"w").write(rpt+"\n")
    print(rpt)

if __name__=="__main__":
    sys.exit(main())
