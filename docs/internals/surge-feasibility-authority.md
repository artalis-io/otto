# Surge: Centralizing the Feasibility Authority (design)

**Status:** Design (M1 from the 2026-10 c-audit). Not yet implemented.
**Motivation:** every correctness bug fixed this cycle was the *same* bug — a
hand-rolled copy of the feasibility invariant drifting out of sync with the
others.

## The problem

The two core invariants of a committed Surge route —

1. **Per-trip capacity** (signed-load span): along a trip, the running load
   `prefix[i] = Σ demand[0..i)` must satisfy `max(prefix) − min(prefix) ≤ cap`
   per dimension (with the `initial_load` branch for the first trip); the load
   resets at each `trip_start`.
2. **Eligibility**: every request on a vehicle must pass
   `sg_vehicle_allowed_for_request` **and** `sg_vehicle_qualifies`.

— are each implemented *independently in many places*:

| Path | File:fn | Capacity | Eligibility |
|------|---------|:---:|:---:|
| Authoritative full check | `sg_feasibility.c:535 sg_route_stop_sequence_feasible` | ✅ (reference) | ✅ |
| Delivery insertion eval | `sg_feasibility.c:1361 sg_route_eval_insertion_cached` | own O(1)/O(L) copy | ✅ |
| PD insertion eval | `sg_feasibility.c:2142 sg_route_eval_pd_best_insertion_cached` | own copy | ✅ |
| Sequence feasibility (moves) | `sg_feasibility.c:1237 sg_route_sequence_feasible_distance` | delegates to 535 ✅ | — |
| O(1) concat capacity | `sg_concat.c:167 sg_route_check_capacity_concat` | own O(1) summary | — |
| Concat delta evaluators | `sg_concat.c` or-opt/2opt*/cross | via above | own copy (`sg_seg_eligible_on_vehicle`) |
| Postprocess eject | `sg_postprocess.c:2060 sg_route_postprocess_eject_over_capacity` | own copy | — |
| Postprocess candidate check | `sg_postprocess.c:9 sg_route_candidate_compat_ok` | — | own copy |
| Solution validator | `sg_solution.c:1222 sg_route_solution_validate` | own copy | ✅ |
| Apply chokepoint | `sg_feasibility.c:3000/3131 sg_route_apply_insertion / sg_route_apply_pd_insertion` | — | ✅ (backstop, #225) |

The audit found the capacity invariant referenced across **9 files** (89 in
`sg_feasibility.c` alone). Each copy is a place to drift, and each drift was a
shipped bug:

- **#226** — the O(1) concat (`sg_concat.c`) disagreed with the O(L) reference on
  multi-trip (prefix/suffix combined across trip boundaries).
- **#227** — the eject (`sg_postprocess.c`) measured a trip's load as the *net
  demand sum* instead of the span; mixed PD+delivery slipped through.
- **#228** — postprocess move operators bypassed the accept-gate entirely and
  committed infeasible routes; only the (then-buggy) eject + fallback caught it.

The common root is architectural: **no single source of truth.** Correctness
depends on keeping N hand-written copies identical, which is not a property you
can maintain by review.

## The design: one authority, derived fast paths, enforced chokepoints

### 1. One canonical primitive per invariant

Factor the per-trip capacity math out of `sg_route_stop_sequence_feasible` into a
small, pure, well-tested core that *every* path calls:

```c
/* THE per-trip signed-load capacity check. Given a load profile (prefix sums of
 * demand, resetting at trip_start) over [lo, hi) and the vehicle's caps, returns
 * the total capacity excess (0.0 == feasible). The single source of truth;
 * sg_route_stop_sequence_feasible, the insertion evals, the eject pass and the
 * validator all call this instead of re-deriving span math. */
double sg_cap_trip_excess(const SGContext *ctx, uint32_t vehicle_id,
                          const double *load_profile, uint32_t lo, uint32_t hi,
                          int is_first_trip);

/* THE "can this vehicle serve this request" predicate (wraps the existing inline
 * sg_vehicle_qualifies + sg_vehicle_allowed_for_request). */
static inline int sg_vehicle_can_serve(const SGContext *ctx,
                                       uint32_t vehicle_id, uint32_t request_id);
```

Eligibility already had a single inline source for each half
(`sg_vehicle_qualifies`, `sg_vehicle_allowed_for_request`), but the *pairing* of
the two was copy-pasted at ~10 sites (and once combined as `allowed || qualifies`
at the apply chokepoints). `sg_vehicle_can_serve` is the one predicate every
assignment-gating site now calls; the segment wrapper `sg_seg_eligible_on_vehicle`
(the move operators' helper) loops it over a run of stops, and the apply
chokepoint (below) is the hard backstop. The validator deliberately keeps the two
halves separate so it can report `SG_VIOLATION_FORBIDDEN_VEHICLE` vs
`SG_VIOLATION_QUALIFICATION` distinctly -- that is diagnostic granularity, not
duplicated decision logic.

### 2. Fast paths are *derived*, never parallel implementations

The O(1) concat summary (`sg_route_check_capacity_concat`) is a legitimate
performance optimization, but it must be provably equal to the canonical O(L)
check, not a second independent implementation. Keep it, but:

- It stays a **fast path to the same answer**, with the O(L) canonical as the
  definition of correctness.
- The existing `SG_CONCAT_VERIFY` cross-check (runs both, aborts on disagreement)
  becomes a **CI gate** (this is M2), so any future drift fails the build the day
  it is introduced instead of shipping and being found by a sweep months later.
- New fast paths are only allowed if they come with the same verify harness.

### 3. Enforced chokepoints (postconditions, not conventions)

Two invariants are promoted from "every operator is careful" to "the system
cannot violate them":

- **Assignment eligibility** — already done in #225: `apply_insertion` /
  `sg_route_apply_pd_insertion` reject an ineligible placement. *Every* assignment flows
  through these, so no operator (present or future) can place a request on a
  vehicle it may not serve.
- **Committed-solution feasibility** — formalize the rule "sg_solve never commits
  a solution the validator rejects." #228's `sg_postprocess_guarded` is the first
  half (postprocess can't degrade feasibility); the second half is making the
  final commit path's guarantee explicit and total (repair-or-reject, never a
  silent fallback that depends on `initial` happening to be valid).

## Migration plan (incremental, each step independently shippable)

1. **Land M2 first** (the CI verify gate) so the refactor can't silently regress
   the fast path. *(This PR.)*
2. Extract `sg_cap_trip_excess` from `sg_route_stop_sequence_feasible`; re-point
   `sg_route_stop_sequence_feasible` at it (pure refactor, benchmark-identical).
3. Re-point the eject pass (`sg_route_postprocess_eject_over_capacity`) and the validator
   (`sg_route_solution_validate`) at `sg_cap_trip_excess`; delete their copies.
   Verify Solomon/Li&Lim byte-identical + sweep clean.
4. (a) Re-point the delivery and PD insertion evals' O(L) fallback at the one
   formula; keep the O(1) concat as the verified fast path. (b) Make the
   `-DSG_CONCAT_VERIFY` sweep run fully clean — see "Fast-path defects" below for
   the root cause (two derived caches, `route_stop_load` vs the capacity segment
   summaries, drifting after stop-reordering moves) and the fix.
5. Replace the ~10 copy-pasted `qualifies && allowed` eligibility pairs with the
   one `sg_vehicle_can_serve` predicate (the move operators' `sg_seg_eligible_on_vehicle`
   loops it over a run of stops). Byte-identical consolidation.
6. Make the final-commit feasibility guarantee explicit (repair-or-reject): the
   eject passes repair an infeasible `best`, `initial` is the fallback, and if
   neither validates the commit is skipped (`ctx->final_solution` is cleared) so
   an infeasible plan is never presented as the answer. The guarantee: `sg_solve`
   returns `SG_STATUS_OK` only when the committed solution passes the validator;
   otherwise `SG_STATUS_ERROR` and nothing is committed. (`sg_solve.c`, final
   block.)

Each step is a pure consolidation with an existing safety net (the sweep, the
`SG_CONCAT_VERIFY` gate, the Solomon/Li&Lim byte-identity check, the 481-test
suite). The end state: **one capacity function, one eligibility function, the
fast path verified against them in CI, and two enforced chokepoints** — so this
class of bug cannot recur by drift.

## Fast-path defects the M2 gate found — root cause and fix (step 4b, done)

Enabling the `SG_CONCAT_VERIFY` gate (M2) surfaced two concat discrepancies on
PD routes — one "unsafe admit" (`concat_ok=1 scan_ok=0`, dangerous direction, the
sweep *aborts*, e.g. `f=0x40 seed=2`: `v=1 pos=5 stop_len=25 scan_viol=4.0`) and a
cluster of "over-rejects" (`concat_ok=0 scan_ok=1`, safe direction, the gate
*warns*, e.g. `f=0x40 seed=1`, 13 of them). Both were confirmed pre-existing on
merged main (byte-identical to the step 2-4a refactor), and both turned out to be
the **same bug** — and, importantly, **not** a flaw in the concat algebra at all.

**Root cause: two derived caches drifting.** A committed route carries two
O(L)-derived caches of the same prefix-sum data: `route_stop_load` (the load
profile the O(L) capacity paths read) and the `route_seg_cap_*` prefix/suffix
segment summaries (the O(1) concat fast path). `sg_route_update_timing` rebuilt
the segments (via `sg_route_build_segments`) but **not** the load profile, while
`sg_route_update_load` rebuilt the load profile but not the segments. Every
mutation path that reordered stops and called *only* `update_timing` — the 2-opt
intra-route reversal (`sg_postprocess.c`) is the clearest — left `route_stop_load`
stale (reflecting an older stop order) while the segments were fresh. The
`SG_CONCAT_VERIFY` reference scan reads `route_stop_load`, so it computed a span
from the stale order and disagreed with the (correct) concat summary — in *both*
directions depending on whether the stale order over- or under-stated the span.
The concat fast path was right the whole time; the O(L) reference was reading
stale data. The same stale `route_stop_load` is read in production by the PD
insertion eval and the non-concat O(L) fallback, so this was a real latent
capacity-evaluation bug, not only a harness artifact.

**Fix:** couple the two rebuilds. `sg_route_update_timing` now also calls
`sg_route_update_load`, so "this route's stops changed, recompute derived state"
refreshes load *and* segments together and they can no longer drift; and
`sg_route_solution_copy` copies the capacity segment arrays alongside
`route_stop_load` so a clone is fully consistent. With these, the
`-DSG_CONCAT_VERIFY` correctness sweep (256 feature combos x 6 seeds, 1536
solves) runs to completion with **0 unsafe admits and 0 over-rejects**,
committed-invalid BUGS=0; Solomon(100) and Li & Lim(100) remain byte-identical to
pre-fix (the classic single-trip benchmarks' concat path never depended on the
stale load), and the 480-test suite passes. The concat summary stays the verified
fast path; the M2 gate now holds as a true invariant rather than a warning.

## Why not just "be careful"

The three bugs this cycle were all shipped by careful people and caught only by
an empirical sweep. Duplication of a safety invariant is the defect; centralizing
it is the fix. This is the single highest-leverage change for Surge to be a
*trustworthy* authoritative reference solver, not merely a capable one.
