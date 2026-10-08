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

## Status
- [x] Toolchain gate: `wasmtime` + `wasmer` + `emcc` present; clang targets wasm32.
- [x] Native reference harness exists (`bench_li_lim`, 60 Li & Lim instances, BKS).
- [x] Pipeline validated + a preliminary native datapoint captured (above).
- [ ] Full native baseline (parallel K vs single, several T, all sizes) → CSV.
- [ ] `surge_run.c` single-run WASI CLI + standalone-WASM build.
- [ ] `run.sh` K-fan-out runner + compare.
- [ ] Results table + go/no-go call.
