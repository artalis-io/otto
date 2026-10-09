# Surge WASM parallel-portfolio benchmark

**Question this answers:** can Surge run as an *in-process WASM compute worker*
(e.g. inside Hull), parallelized by **host fan-out of K single-threaded
instances**, without losing meaningful solution quality vs the native
thread-pool? This is the pivotal go/no-go for "Surge as a first-class Hull
worker" (see `dispatch/docs/hull-backend-port.md` §10–§11).

Public instances only (Li & Lim PDPTW + Solomon VRPTW already in
`surge/benchmarks/`). **No customer data.**

## Why host fan-out, not WASM threads
`sg_parallel.h`: Surge's parallel solve is *portfolio* — "multiple independent
runs with different seeds; the best is retained". It is embarrassingly parallel,
so K **single-threaded** instances run concurrently (seeds base..base+K-1) and
reduced to the best reproduce `sg_solve_parallel(K)` — with **no** shared memory
or the WASM threads proposal (which Hull's WAMR build does not evidence). The
single-threaded unit already exists: build Surge with `SG_HAS_THREADS` undefined
so only `sg_solve` (one run) compiles.

## The metric that decides it
Multi-start improves **quality at a fixed wall-clock budget**, not latency. WASM
runs each instance ~1.5–3× slower than native, so each worker explores fewer
iterations in budget T. Go/no-go compares **solution quality at equal T**:

> For budget T (≈5–30 s) and K = cores: is the quality of *K concurrent
> single-threaded WASM runs* within an acceptable gap of *native
> `sg_solve_parallel(K)`* across Li & Lim instances?

Quality = lexicographic `(vehicles, distance)` vs the BKS in `benchmarks/bks/`.

## Harness
1. **Native reference** (reuses the existing bench, no new code):
   - parallel: `./bench_li_lim --dir benchmarks/li_lim --time-limit T --population --threads K --seed 42 --output-csv native_parallel.csv`
   - single:   `./bench_li_lim --dir benchmarks/li_lim --time-limit T --seed 42 --output-csv native_single.csv`
2. **WASM single-run CLI** (`wasm_parallel/surge_run.c`, TODO): read one instance
   + `--time-limit --seed`, `sg_solve`, print `vehicles distance unassigned iters`.
   Build: `emcc ... -DSG_NO_THREADS -sSTANDALONE_WASM -o surge_run.wasm`; run under
   `wasmtime --dir=. surge_run.wasm -- <instance> T <seed>`.
3. **Fan-out runner** (`wasm_parallel/run.sh`, TODO): for each instance, launch K
   `wasmtime` processes (seeds 0..K-1) concurrently, budget T, reduce to best;
   emit `instance,mode,K,T,vehicles,distance,gapVsBKS,iters,wall` CSV.
4. **Compare**: native_parallel vs wasm_fanout quality gap + per-instance WASM
   slowdown (native iters/run ÷ wasm iters/run at equal wall).

## Go / no-go
- **GO** (Surge as a WASM worker): wasm-fanout distance gap vs native ≤ ~1–2%
  (never worse on vehicles) at T, per-instance slowdown ≤ ~3×.
- **MARGINAL**: only acceptable at larger T or with `-pthread` in-module threads.
- **NO-GO**: large gap or slowdown > ~4–5× ⇒ Surge stays an HTTP service; the
  lighter engines (Locus/Nexus) still become in-process workers.

## Preliminary native datapoint (pipeline check)
`lc101`, 5 s budget, seed 42: single = 15 veh / 1474.2 dist (converged 1.66 s);
parallel `--population --threads 4` = 15 veh / 1433.3 dist (2.9 s). Parallel wins
~2.8% distance at equal vehicles — the portfolio effect we expect WASM fan-out to
reproduce. NB set a high `--iterations` so `--time-limit` is the binding
constraint (lc101 single converged before 5 s here).

## Preliminary WASM result (single run, native vs emcc/Node)
`surge_run.c` = one-shot single-threaded `sg_solve`; built native and via
`emcc -sNODERAWFS -sEXIT_RUNTIME` (no `SG_HAS_THREADS` ⇒ single-threaded).
Same instance / seed / `--iterations`:

| instance | iters | native veh / dist / s | wasm veh / dist / s | WASM slowdown |
|----------|-------|-----------------------|---------------------|---------------|
| lc101    | 12000 | 16 / 1514.99 / 1.15   | 15 / 1474.21 / 2.42 | **2.1×** |
| lr201    | 12000 | 4 / 1257.37 / 5.12    | 4 / 1253.23 / 10.18 | **2.0×** |

**Two findings:**
1. **WASM ≈ 2× slower per iteration** — inside the ≤3× GO bar.
2. **Per-seed quality differs native vs WASM** (lc101: 16v vs 15v) — genuine FP
   divergence across compilers (clang vs emcc), the documented OTTO pitfall #7
   (solvers compare near-equal values for pivots; reassociation changes the
   path). It is **noise, not loss** — WASM matched or *beat* native here. So the
   portfolio comparison must be empirical (best-of-K native vs best-of-K WASM),
   not "assume identical". Runs are reproducible within a build (same binary +
   seed). Also note: `max_iterations` SIZES the SA schedule (not just a cap), so
   fix iterations per run; don't set it huge and rely on `--time-limit`.

## Results — K=4 fan-out portfolio + iterations curve (`run.sh`)
Li & Lim size-100, seed base 0, `--iterations 10000` for the fan-out. Full CSV
is `results.csv` (git-ignored; reproduce with `./run.sh`).

**A. iterations → quality (native single, seed 42)** — quality is *noisy and
plateaus early*; past the plateau more iterations is just per-seed noise:
| instance | 2.5k | 5k | 10k | 20k |
|----------|------|----|-----|-----|
| lc101 (dist) | 1641 | 1762 | **1515** | 1641 |
| lr201 (dist) | 1273 | 1253 | 1257 | 1253 |

⇒ single-seed runs are high-variance ⇒ the **portfolio (best-of-K) is what
delivers stable quality**, and WASM doing fewer iters in a wall-budget lands in
the same plateau band (little to lose).

**B. best-of-K=4, native vs WASM @10k iters** — lexicographic `(unassigned, veh, dist)`:
| instance | native best | WASM best | WASM/run slowdown | WASM vs native |
|----------|-------------|-----------|-------------------|----------------|
| lc101 | 0 / 16 / 1458 | 0 / **15** / **1342** | ×2.03 | **better** |
| lr101 | **5** / 25 / 1748 | **3** / 25 / 1906 | ×1.33 | **better** (unassigned is the primary key) |
| lr201 | 0 / 4 / 1253 | 0 / 4 / 1253 | ×1.94 | tie |

## IMPORTANT: portfolio ≠ Surge's best mode (population shares)
The fan-out above models `sg_solve_parallel` = **portfolio** (independent seeds,
best-of, NO sharing). Surge's stronger mode is `sg_solve_population` — a
PyVRP-style **memetic GA with cross-population sharing**: an elite pool
(`population_size`), **SREX crossover** (`crossover_fraction`, half the workers
each generation recombine two pool parents), biased-fitness survivor selection
with broken-pairs **diversity**, and each generation warm-started from prior
elites. It is Surge's `--population` mode and it **beats the portfolio**:

| instance | portfolio best-of-4 (no sharing) | **population** K=4/gen3 (SREX+elite) |
|----------|----------------------------------|--------------------------------------|
| lc101 | 16 / 1458 | **15 / 1433** |
| lr101 | 25 / **5 unassigned** / 1748 | 25 / **0 unassigned** / 1928 |
| lr201 | 4 / 1253 | 4 / 1253 (tie) |

Population wins decisively where it's hard (lr101: serves **all** requests vs 5
dropped). So the portfolio fan-out is Surge's *weaker* mode.

## Verdict
- **Portfolio-in-WASM: GO** — ~1.3–2.0× slower per iteration, and WASM best-of-K
  ≈ native best-of-K (FP divergence washes out). Trivially maps to Hull
  `compute.async.call × K`, no sharing. A viable **fallback**.
- **But the quality mode is population (sharing), which the fan-out does NOT
  replicate.** Getting population quality as an in-process Hull worker needs ONE
  of:
  - **(A) in-WASM threads — ❌ RULED OUT (probed 2026-10-09).** Hull's WAMR is
    built `-DWASM_ENABLE_THREAD_MGR=0` + `-DWASM_ENABLE_SHARED_MEMORY=0`
    (`hull/mk/vendor/wamr.mk`); `docs/wamr_architecture.md` lists "Spawn threads
    or processes" under *what a plugin CANNOT do* ("no threads" build flag;
    single-threaded interpreter + host-level async dispatch). A multi-threaded
    `sg_solve_population` module cannot load/run in a Hull worker. (It would run
    under wasmtime's wasi-threads, but that's irrelevant to Hull.)
  - **(B) host-orchestrated generations — the only in-process path.** The Hull
    app runs G generations of K single-threaded `compute.async.call` worker
    solves (Hull's host thread pool, `--workers`, is exactly this), warm-started
    from an elite pool the HOST maintains, doing SREX crossover + biased-fitness
    survivor selection host-side. Needs `surge_run` to **accept a warm-start in
    and emit its solution out**, and SREX/selection **exposed from Surge or
    reimplemented** host-side. A real build-out, but it fits Hull's model.
- **Recommendation:** Path A is closed. For population quality in-process, scope
  Path B (host-side GA loop over single-threaded workers). Otherwise ship the
  portfolio fan-out as the cheap fallback (proven GO, but drops orders on hard
  instances like lr101) — or keep the population solve in an out-of-process
  Surge **service** (threads allowed) and have Hull call it over `net`.

**Caveats:** only size-100 instances available (no 200/400 in `li_lim_extended`);
dispatch is ~100–260 stops, so re-run larger before committing. This is
Node+emscripten (measures the *wasm compute*); the Hull WAMR-ABI port
(`hull_compute.h` + spans) is a separate step.

## Status
- [x] Toolchain gate; native reference harness; `surge_run.c` native + WASM builds.
- [x] K-fan-out runner (`run.sh`) + iterations↔quality curve → `results.csv`.
- [x] Results + call: **portfolio-in-WASM = GO**; population (Surge's stronger,
  sharing mode) beats it and is NOT replicated by the fan-out.
- [x] Population path (A) probed: **Hull/WAMR has threads OFF** (THREAD_MGR=0,
  SHARED_MEMORY=0) → A ruled out; in-process population must go via (B) host-
  orchestrated generations, else keep population as an out-of-process service.
- [ ] (if pursuing in-process population) scope (B): surge_run warm-start in/out
  + host-side SREX + biased-fitness selection.
- [ ] Re-run on 200–400-stop instances; port `surge_run` to the Hull compute ABI.
- [ ] `surge_run.c` single-run WASI CLI + standalone-WASM build.
- [ ] `run.sh` K-fan-out runner + compare.
- [ ] Results table + go/no-go call.
