# Decision Cockpit (codename: Sage)

**Status:** Concept / pre-prototype (2026-10). Driving use case: the Gyermelyi
demo for the deputy CEO (deadline 2026-10-20). Codename *Sage* is provisional.

## One-liner

Not a routing screen: a **decision lab**. You ask a question in plain Hungarian,
and in seconds it re-plans the whole distribution for that question, shows the new
plan on the map against today's, and tells you what it costs and how risky it is.

The buyer (Gyermelyi's deputy CEO) is a paradigm-changer who wants to **close
depots** and rethink how they sell. Levente's brief frames OTTO as a *flexible
modelling tool we develop modules around*. The cockpit is exactly that: the seat
where he runs paradigm experiments himself.

## Why this framing

- He decides, and he thinks in paradigm shifts, not route tweaks. A "what-if"
  cockpit lets him test his own ideas (close Polgár, change the sales cadence)
  live, with numbers to defend the decision upward.
- Their business "will keep changing" (his words via Levente). A fixed optimizer
  rots; a modeller you talk to adapts. That is the product, not a one-off plan.
- It extends, does not replace, our existing positioning: TMS = admin,
  **Surge = the brain**, and now **Sage = the analyst seat** on top.

## Architecture (three layers)

```
   You (natural language, Hungarian)
        |
   [1] Sage  -- LLM modeller & narrator          (local, on the DGX Spark)
        |         NL intent  ->  scenario spec (JSON)
        |         solver result -> plain-language explanation + next experiment
        v
   [2] Surge / PyVRP  -- stochastic simulator & optimizer
        |         one re-solve per scenario; Monte-Carlo over uncertainty
        v
   [3] ClayShards cockpit  -- map (WebGL) + KPI deltas + before/after animation
```

**Division of labour is deliberate.** The LLM never invents routes or numbers;
it only (a) turns a fuzzy business question into a precise, validated scenario
spec, and (b) narrates the solver's output. The trustworthy, deterministic core
stays the solver. This kills the "the AI hallucinated a route" failure mode and
keeps the demo defensible.

### [1] Sage: the LLM modeller (local, on an NVIDIA DGX Spark)

- **Model:** Qwen3 (8B, "Flash" class) to start; the DGX Spark's 128 GB unified
  memory hosts far larger Qwen3 variants (e.g. 30B+ / Qwen3-Next) if we want more
  modelling quality. Small is enough because the model emits a *structured spec*,
  not prose math; the heavy lifting is the solver.
- **The local box is an argument, not a cost saving.** We physically bring the
  DGX Spark to Gyermely:
  - **Private:** confidential FMCG order/pricing data never leaves the room.
  - **Offline:** the demo works with no wifi on site.
  - **Edge-native:** matches the OTTO manifesto ("if it runs in WASM/on a box,
    it runs anywhere"). Same message as Surge, one layer up.
- **Job:** map intent -> `scenario spec` via tool/function-calling; explain the
  diff and infeasibility in Hungarian; propose the next experiment.

### [2] Surge / PyVRP: the stochastic simulator

- One scenario = one re-solve on the real matrix, fleet, and constraints.
- **Monte-Carlo** over uncertainty (demand +/- X%, vehicle drop-out, time-window
  slippage): hundreds of solves -> a *distribution*, not a single number.
  "95% of the time under 24 trucks; at +15% demand, 2-day slip risk near
  Nyiregyhaza." This is what makes it an **expert tool**, not a toy: it
  quantifies the risk a depot-closure decision has to survive.
- Surge is the target engine (C/WASM, embeddable). As of PR #225 the HGS
  quality gap is effectively closed on the real instance (population search in
  production: day1 12 veh, day2 23 veh, 0 unserved, vs PyVRP 11/24), the
  small-vehicle size guarantee is enforced natively (capacity dimension; the
  allowed_vehicles leak that let restricted orders ride big trucks is fixed),
  and solves are deterministic. So Surge is now a viable *authoritative* engine,
  not just a target. The cockpit stays engine-agnostic behind the scenario spec;
  PyVRP and cuOpt remain available (see the engine-status note below).

### [3] ClayShards cockpit

- Reuse the existing map + WebGL renderer + offline route-viz.
- The "wow" is the **before/after**: today's plan and the scenario side by side,
  KPIs (trucks, km, Ft, late deliveries) animating to their new values.

## The scenario-spec contract

The single interface between Sage and the solver. Generic shape (dataset fills in
the specifics):

```jsonc
{
  "base": "per_day_baseline",          // what we diff against
  "depots":   { "disable": ["Polgar"] },
  "fleet":    { "limit": {"nyerges": 10}, "add": [], "remove": [] },
  "demand":   { "scale": 1.15, "seed_spread": 200 },   // Monte-Carlo trigger
  "constraints": {
    "size_restriction": "hard",        // on|off|hard
    "hard_time_windows": true,
    "max_on_duty_min": 780
  },
  "objective": "vehicles_then_distance",
  "report": ["trucks","km","cost_huf","late","risk_band"]
}
```

Sage emits this; a thin driver turns it into a Surge/PyVRP solve (or a Monte-Carlo
sweep when `demand.scale`/`seed_spread` is present) and returns KPI deltas +
before/after geojson.

## Demo scenarios (tuned to the buyer)

1. **"Mi lenne, ha bezarnank a polgari depot?"** -> live re-solve, map redraws,
   +/- Ft, how many addresses' windows break. His paradigm, live.
2. **"Es ha a rendelesi ritmus 2 naprol 3-ra menne?"** -> the "sales will change"
   point, simulated.
3. **Size-restriction toggle** -> shows the price of our guarantee (fewer trucks
   vs guaranteed small-vehicle service). Honest, and our strength.
4. **Robustness band** on one plan -> "holds 24 trucks up to +15% demand."

## Scope & phases

**Oct 20 MVP (must be genuinely live, yet stable):**
- Cockpit over the Gyermelyi data; **3-4 what-if scenarios with real re-solves**
  (not canned numbers), animated map + KPI deltas.
- **2-3 driven by live NL parsing** (Sage on the DGX Spark -> scenario spec); the
  rest on buttons so the demo never hinges on model whim.
- **One Monte-Carlo risk band** precomputed, shown live.

**Non-goals for Oct 20 (state honestly):**
- Free-form live solve for arbitrary questions.
- **True 2L/3D load packing.** Neither Surge nor PyVRP does real 2D packing today
  (see `surge.md`); we approximate with a footprint dimension. Either soften the
  "2L-VRP" wording or build the loading-feasibility module later (Tier-2).

**Later:**
- Surge as the sole/default engine (PR #225 closed the quality gap and the size
  guarantee; remaining blocker is the multi-trip spurious-ERROR path, surge #182).
  Once that lands, drop the PyVRP dependency.
- Free-form NL; saved scenario library; multi-period (close depot *and* phase the
  transition over weeks); cost-objective solves feeding the value-share pricing.
- True loading feasibility (ties to the pallet-capacity work).

## Dependencies & open questions

- Qwen3 serving stack on the Spark (llama.cpp / vLLM / Ollama) + function-calling
  reliability for the spec. Decide one, pin it.
- Re-solve latency per scenario on the demo instance (budget the iteration count
  so a live click returns in a few seconds; precompute the heavy Monte-Carlo).
- Scenario-spec schema freeze (so Sage's function signature is stable).
- Gyermelyi-specific wiring lives in the dataset, not this repo (see the dataset's
  `DEMO_SCOPE.md`); this doc stays generic/product-level.

## cuOpt Phase-2 spike (2026-10, done)

Evaluated NVIDIA cuOpt as a GPU backend for the cockpit's interactive solves, on the
demo DGX Spark (GB10, sm_121). Findings:

- **Installs & runs on the Spark**: `pip install --extra-index-url
  https://pypi.nvidia.com cuopt-cu13` (aarch64/cu13/cp312); cuOpt **26.08.00** solves
  on the GB10. No container/docker needed.
- **Constraint parity is good**: 2-D capacity (`add_capacity_dimension`), order +
  vehicle time windows (shift), service times, per-vehicle max route time (on-duty),
  heterogeneous fleet with fixed costs, min-vehicle objective, and the size
  restriction cleanly via `add_order_vehicle_match` (restricted order -> small
  vehicles). All verified on the real instance.
- **Solver quality + speed**: on the fair single-trip benchmark (one day, 117 orders,
  10 s each), cuOpt reached **14 vehicles / 3,879 km, 0 unserved** vs PyVRP
  single-trip **16 / 4,469, infeasible**. Day-2 (146 orders) ~17-19 veh / ~5,250 km,
  0 unserved in 10-20 s. GPU has ~2.5 s fixed setup overhead per solve.
- **The one gap: native multi-trip / reload is missing** (only `set_drop_return_trips`
  = open routes), the same gap Surge had. The authoritative plan uses PyVRP reloads.
  Workaround for parity: solve trip-level with cuOpt, then a lightweight shift-packer
  assigns trips to physical trucks within the 13 h on-duty window.
- **Determinism**: cuOpt shows run-to-run variance (17 vs 19 veh on repeats), less
  reproducible than PyVRP seed=42. So: keep the **authoritative plan on PyVRP**
  (reproducible, multi-trip); use cuOpt only as the **fast interactive accelerator**.
  And it is NOT "our engine" (Surge is the positioning differentiator).

**Shift-packer (built) + 60 s head-to-head.** A first-fit-decreasing packer
consolidates cuOpt's single-trip routes onto physical trucks within the 13 h shift
(30 min reload), per vehicle class, recovering multi-trip. At 60 s:
- day1 (117): cuOpt+packer **11 trucks / 3,726 km (feasible)** == PyVRP multi-trip
  11 / 3,840 (and slightly fewer km).
- day2 (146): cuOpt+packer **19 trucks / 7,820 km (feasible)** vs PyVRP multi-trip
  24 / 6,730 (**infeasible at 60 s**) -> cuOpt+packer uses fewer trucks and stays
  feasible, at the cost of more km (single-trip+pack = bigger loads, longer routes).
So cuOpt+packer reaches multi-trip parity (beats it on trucks + feasibility at
interactive budget). **Seeding: cuOpt 26.08 has NO seed** (not in SolverSettings /
Solve / config dump) -> non-reproducible (day2 varied 17/19/23 trips across runs).
Therefore: authoritative plan stays PyVRP (seed=42, native reload); cuOpt+packer is
the fast interactive accelerator.

Spike artifacts (dataset): `scripts/export_cuopt_day.py`, `scripts/cuopt_spike.py`,
and `scripts/cuopt_solve.py` (Spark-side solver + packer).

**WIRED (2026-10): `SCENARIO_ENGINE=cuopt`.** scenario_solve now has a cuOpt engine:
for each day it builds a spec-applied bundle (depot disable / fleet limit / size rule /
pallet cap / demand scale / on-duty all honoured), scp+ssh's it to the Spark, runs
cuopt_solve.py (cuOpt + shift-packer), and rebuilds the same routes -> identical
per-day KPIs / diff / geojson as the PyVRP path. The baseline cache is keyed per
engine (cuopt-scenario diffs vs a cuopt baseline). Verified end-to-end. Env:
`CUOPT_SSH_HOST` (spark-7468), `CUOPT_REMOTE_PY` (/tmp/cuopt-venv/bin/python). Default
engine stays `pyvrp` (reproducible); the cockpit opts in by launching with
`SCENARIO_ENGINE=cuopt`. Falls back to PyVRP on any cuOpt error.

## Engine status (2026-10, post #225)

Three engines sit behind the scenario spec; the cockpit picks per role:

| Engine | Role | Reproducible | Multi-trip | Size guarantee | Notes |
|--------|------|--------------|-----------|----------------|-------|
| **Surge** | our differentiator; authoritative candidate | yes (seed, deterministic) | blocked on #182 | native (capacity dim) | parity on the real instance as of #225; C/WASM/embeddable |
| **PyVRP** | current authoritative plan | yes (seed=42) | native reload | via allowed-vehicles | the reproducible reference the demo is measured against |
| **cuOpt** | fast interactive accelerator (`SCENARIO_ENGINE=cuopt`) | no (no seed in 26.08) | via shift-packer | `add_order_vehicle_match` | GPU on the Spark; ~2.5 s setup/solve |

Direction: Surge becomes the default once #182 (multi-trip spurious ERROR) is
fixed, at which point PyVRP can be dropped and cuOpt stays as the optional GPU
accelerator. Until then the authoritative plan stays on PyVRP for reproducibility.

## Relationship to other components

- **Surge** (`surge.md`): the engine; cockpit is the seat above it.
- **ClayShards**: the cockpit UI.
- **Nexus**: feeds validated orders/matrix into the baseline the cockpit diffs.
- Positioning: TMS = admin, Surge = brain, **Sage = analyst**.
