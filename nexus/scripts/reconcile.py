#!/usr/bin/env python3
"""Schema-driven reconciler: backtrack Nexus canonical output to raw client cells.

Reads a transform schema, re-executes its grammar (row_merge is not handled --
CSV has no continuation rows -- multi_transforms: merge/split/regex/conditional/
coalesce, then column mapping/transforms/defaults), and diffs the recomputed
values against nx_pipeline's canonical output field-by-field.

Faithful by construction OR explicit: anything it cannot mechanically reproduce
(C compute functions other than coalesce; slugified row ids) is REPORTED as
unverified, never silently passed.

Usage:
  reconcile.py <input.csv> --schema <schema.json> [--nx <nx_pipeline>]

Exit: 0 = all verifiable fields match; 1 = mismatch; 2 = setup error.
Numeric compare uses rel_tol=1e-6 because Stage X re-serializes doubles at 6
significant figures (nx_validate.c ~L723), overriding column precision.
"""
import argparse, json, subprocess, sys, os, re, math, hashlib

VERIFIABLE_COMPUTE = {"coalesce"}          # reimplemented below
OPAQUE_COMPUTE = {"eov_to_wgs84", "dms_to_dd", "phone_normalize",
                  "zip_to_region", "opening_hours"}  # live in C, not reproduced


def apply_transforms(val, transforms):
    """Column/target transforms: 'trim' and {'replace': [from, to]}."""
    for t in transforms or []:
        if t == "trim":
            val = val.strip()
        elif isinstance(t, dict) and "replace" in t:
            frm, to = t["replace"][0], t["replace"][1]
            val = val.replace(frm, to)
    return val


def run_multi_transforms(raw_cells, multis):
    """Reproduce nx_xform virtual columns. Returns (combined_cells, unverifiable).

    combined_cells = raw_cells + virtual values (in production order).
    unverifiable = set of virtual indices (in combined space) we could not compute.
    """
    ncols = len(raw_cells)
    combined = list(raw_cells)
    unverifiable = set()

    def get(idx):
        return combined[idx] if 0 <= idx < len(combined) else ""

    for mt in multis or []:
        typ = mt.get("type")
        base = len(combined)              # index of this transform's first output
        srcs = mt.get("sources", [])
        targets = mt.get("targets", [])

        if typ == "merge":
            if isinstance(mt.get("template"), str):
                out = re.sub(r"\{(\d)\}",
                             lambda m: get(srcs[int(m.group(1))]) if int(m.group(1)) < len(srcs) else "",
                             mt["template"])
            else:
                sep = mt.get("separator", " ")
                out = sep.join(get(s) for s in srcs)
            combined.append(out)

        elif typ == "split":
            src = get(mt.get("source", 0))
            parts = src.split(mt.get("delimiter", ","))
            tlist = targets or [{}]
            for j, t in enumerate(tlist):
                idx = t.get("index", j)
                v = parts[idx] if 0 <= idx < len(parts) else ""
                combined.append(apply_transforms(v, t.get("transforms")))

        elif typ == "regex":
            src = get(mt.get("source", 0))
            m = re.search(mt.get("pattern", ""), src)
            for j, t in enumerate(targets):
                grp = t.get("group", t.get("index", j))
                v = ""
                if m and grp is not None:
                    try:
                        g = m.group(grp)
                        v = g if g is not None else ""
                    except (IndexError, error):
                        v = ""
                combined.append(apply_transforms(v, t.get("transforms")))

        elif typ == "conditional":
            src = get(mt.get("source", 0))
            out = ""
            for cond in mt.get("conditions", []):
                if re.search(cond.get("match", ".*"), src):
                    out = cond.get("set", {}).get("value", "").replace("{0}", src)
                    break
            combined.append(out)

        elif typ == "compute":
            fn = mt.get("function", "")
            tlist = targets or [{}]
            if fn == "coalesce":
                v = next((s for s in (get(x) for x in srcs) if s), "")
                combined.append(v)
                for _ in tlist[1:]:
                    combined.append("")
            else:                          # opaque C function -> can't reproduce
                for k in range(len(tlist)):
                    combined.append("")
                    unverifiable.add(base + k)
        else:
            continue  # unknown transform type: nx_xform skips it too

    return combined, unverifiable


error = re.error  # for the except above


def coerce_expect(val, col):
    """Mimic nx_xform column emit for comparison purposes."""
    typ = col.get("type", "string")
    if typ in ("int", "double"):
        try:
            return ("num", float(val)) if val.strip() != "" else ("num", 0.0)
        except ValueError:
            return ("num", None)   # nx would reject; shouldn't reach accepted rows
    if typ == "bool":
        return ("bool", val in ("true", "1", "yes"))
    return ("str", val)


def default_nx_path():
    """Where the built nx_pipeline lives.

    The Makefile writes `nx_pipeline`, and on Windows the compiler appends
    `.exe` to that, so a bare name is only correct on POSIX. This is why
    `make -C nexus test-reconcile` failed on Windows and nowhere else: CI runs
    it on Linux, so nothing ever saw it.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    for name in ("nx_pipeline", "nx_pipeline.exe"):
        candidate = os.path.join(here, "..", name)
        if os.path.exists(candidate):
            return candidate
    return os.path.join(here, "..", "nx_pipeline")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--schema", required=True)
    ap.add_argument("--nx", default=default_nx_path(),
                    help="path to the nx_pipeline binary")
    ap.add_argument("--show", type=int, default=40, help="max mismatches to print")
    args = ap.parse_args()

    nx = os.path.abspath(args.nx)
    if not os.path.exists(nx) and os.path.exists(nx + ".exe"):
        nx += ".exe"          # an explicit --nx may also omit the suffix
    if not os.path.exists(nx):
        print(f"nx_pipeline not found: {nx}", file=sys.stderr); return 2

    schema = json.load(open(args.schema, encoding="utf-8"))
    raw = json.loads(subprocess.check_output([nx, args.input, "--raw"], stderr=subprocess.DEVNULL))
    canon = json.loads(subprocess.check_output([nx, args.input, "--schema", args.schema], stderr=subprocess.DEVNULL))

    # --- provenance ---
    file_sha = hashlib.sha256(open(args.input, "rb").read()).hexdigest()
    prov_ok = (canon.get("source_sha256") == file_sha)

    # --- position join, skipping Stage B rejects (by raw row number) ---
    audit = canon.get("audit", {})
    rejected = {d["row"] for d in audit.get("rejections", [])}
    raw_rows = [(r["row"], r["cells"]) for r in raw["tables"][0]["rows"]
                if r["row"] not in rejected]
    recs = canon["records"]
    if len(raw_rows) != len(recs):
        print(f"[{os.path.basename(args.input)}] JOIN FAIL: raw kept {len(raw_rows)} "
              f"vs canon {len(recs)} (Stage X may have removed error-severity records)",
              file=sys.stderr)
        return 1

    cols = schema.get("columns", [])
    derived = schema.get("derived", [])
    multis = schema.get("multi_transforms", [])

    mism = []
    unverified_fields = set()
    verified_fields = set()

    for (rownum, raw_cells), rec in zip(raw_rows, recs):
        combined, unverifiable = run_multi_transforms(raw_cells, multis)
        ncols = len(raw_cells)

        for col in cols:
            tgt = col["target"]
            src = col.get("source", 0)
            raw_val = combined[src] if 0 <= src < len(combined) else ""

            if src in unverifiable:
                unverified_fields.add(f"{tgt} (compute)")
                continue

            val = apply_transforms(raw_val, col.get("transforms"))
            if val == "" and "default" in col:
                val = str(col["default"])

            kind, exp = coerce_expect(val, col)
            got = rec.get(tgt)
            verified_fields.add(tgt)

            if kind == "num":
                if exp is None or got is None or not math.isclose(
                        float(got), exp, rel_tol=1e-6, abs_tol=1e-9):
                    mism.append((rec.get("id", rownum), tgt, f"{got!r} != raw->{exp!r}"))
            elif kind == "bool":
                if bool(got) != exp:
                    mism.append((rec.get("id", rownum), tgt, f"{got!r} != raw->{exp!r}"))
            else:
                if got != exp:
                    mism.append((rec.get("id", rownum), tgt, f"{got!r} != raw->{exp!r}"))

        for d in derived:
            if rec.get(d["target"]) != d["value"]:
                mism.append((rec.get("id", rownum), d["target"],
                             f"{rec.get(d['target'])!r} != derived {d['value']!r}"))
            else:
                verified_fields.add(d["target"])

    name = os.path.basename(args.input)
    print(f"=== {name}  (schema {schema.get('version')}) ===")
    print(f"  provenance sha256 : {'OK' if prov_ok else 'MISMATCH'}")
    print(f"  rows reconciled   : {len(recs)}  (Stage B rejects skipped: {len(rejected)})")
    print(f"  fields verified   : {len(verified_fields)}  {sorted(verified_fields)}")
    if recs and "id" in recs[0]:
        print(f"  NOT verified      : id (slugified) " +
              (", ".join(sorted(unverified_fields)) if unverified_fields else ""))
    print(f"  mismatches        : {len(mism)}")
    for m in mism[:args.show]:
        print("    MISMATCH", m)
    return 1 if (mism or not prov_ok) else 0


if __name__ == "__main__":
    sys.exit(main())
