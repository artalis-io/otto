#!/usr/bin/env python3
"""Generic per-file semantic sanity checks for a canonical Nexus output.

Field-inferring and dataset-agnostic: it looks at which fields exist by name and
applies the invariants that apply. Unlike reconcile.py (which proves the
transform is FAITHFUL to raw), this asks whether values make real-world sense.
It is NON-BLOCKING by design -- meant to print a warnings summary during ingest,
not to reject data. Exit is always 0 unless --strict and an ERROR is found.

For cross-file referential/fleet checks (carrier<->vehicle joins, per-trip
capacity), use a dataset-specific audit; this covers only single-file invariants.

Usage: semantic_checks.py <input> --schema <schema.json> [--nx <nx_pipeline>] [--strict]
"""
import argparse, json, subprocess, sys, os, re
from collections import Counter

def find(fields, pat):
    rx = re.compile(pat, re.I)
    return [f for f in fields if rx.search(f) and not f.endswith("_raw")]

def as_num(v):
    try: return float(v)
    except (TypeError, ValueError): return None

def norm_hhmm(s):
    m = re.match(r"^(\d{1,2}):(\d{2})$", (s or "").strip())
    if not m: return None
    h, mi = int(m.group(1)), int(m.group(2))
    return h*60+mi if h <= 24 and mi < 60 else None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input"); ap.add_argument("--schema", required=True)
    ap.add_argument("--nx", default=os.path.join(os.path.dirname(__file__), "..", "nx_pipeline"))
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--show", type=int, default=4)
    args = ap.parse_args()

    nx = os.path.abspath(args.nx)
    recs = json.loads(subprocess.check_output([nx, args.input, "--schema", args.schema],
                                              stderr=subprocess.DEVNULL))["records"]
    if not recs:
        print("  semantic: no records"); return 0
    fields = list(recs[0].keys())
    findings = []
    def add(sev, chk, msg): findings.append((sev, chk, msg))

    weight_f = (find(fields, r"weight") or [None])[0]
    pallet_f = (find(fields, r"pallet") or [None])[0]
    cap_f    = (find(fields, r"capacit.*kg|capacity_kg") or [None])[0]
    gross_f  = (find(fields, r"gross") or [None])[0]
    zip_f    = "zip" if "zip" in fields else None
    # time-window pairs: <p>_start / <p>_end
    pairs = [(f, f[:-6] + "_end") for f in fields
             if f.endswith("_start") and (f[:-6] + "_end") in fields]

    key_f = next((k for k in ("order_no", "plate", "id") if k in fields), None)

    for r in recs:
        rid = r.get(key_f, "?")
        if weight_f is not None:
            w = as_num(r.get(weight_f))
            if w is not None and w < 0: add("ERROR", "negative", f"{rid}: {weight_f}={w}")
        if cap_f and gross_f:
            c, g = as_num(r.get(cap_f)), as_num(r.get(gross_f))
            if c is not None and g is not None and c > g:
                add("ERROR", "gvw", f"{rid}: {cap_f} {c} > {gross_f} {g}")
        if weight_f and pallet_f:
            w, p = as_num(r.get(weight_f)), as_num(r.get(pallet_f))
            if w and p and p > 0:
                d = w / p
                if d > 1500: add("WARN", "density", f"{rid}: {d:.0f} kg per unit ({w}/{p})")
                elif d < 1:  add("WARN", "density", f"{rid}: {d:.2f} kg per unit ({w}/{p})")
            if (w == 0 or w is None) and (p == 0 or p is None):
                add("WARN", "empty", f"{rid}: zero {weight_f} and {pallet_f}")
        for s_f, e_f in pairs:
            s, e = r.get(s_f), r.get(e_f)
            if s and e:
                ms, me = norm_hhmm(s), norm_hhmm(e)
                if ms is None or me is None: add("WARN", "time-window", f"{rid}: unparseable {s}-{e}")
                elif me <= ms: add("ERROR", "time-window", f"{rid}: end {e} <= start {s}")
        if zip_f:
            z = (r.get(zip_f) or "").strip()
            if z and not re.match(r"^\d{4}$|^\d{5}$|^\d{6}$", z):
                add("INFO", "zip-shape", f"{rid}: non-standard zip {z!r}")

    if key_f:
        dups = [k for k, c in Counter(r.get(key_f) for r in recs).items() if c > 1]
        if dups: add("WARN", "duplicate-key", f"{len(dups)} duplicate {key_f}: {dups[:6]}")

    counts = Counter(f[0] for f in findings)
    print(f"  semantic: {counts.get('ERROR',0)} ERROR, {counts.get('WARN',0)} WARN, "
          f"{counts.get('INFO',0)} INFO  ({len(recs)} records, key={key_f})")
    order = {"ERROR": 0, "WARN": 1, "INFO": 2}
    by = {}
    for sev, chk, msg in sorted(findings, key=lambda x: order[x[0]]):
        by.setdefault((order[sev], sev, chk), []).append(msg)
    for (_, sev, chk), msgs in sorted(by.items()):
        print(f"    [{sev}] {chk} ({len(msgs)})")
        for m in msgs[:args.show]:
            print(f"        - {m}")
        if len(msgs) > args.show: print(f"        ... +{len(msgs)-args.show} more")
    return 1 if (args.strict and counts.get("ERROR")) else 0

if __name__ == "__main__":
    sys.exit(main())
