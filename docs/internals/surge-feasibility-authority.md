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

/* THE eligibility check for a candidate request list on a vehicle (wraps the
 * existing inline sg_vehicle_allowed_for_request + sg_vehicle_qualifies). */
int sg_seg_eligible(const SGContext *ctx, uint32_t vehicle_id,
                    const uint32_t *request_ids, uint32_t n);
```

Eligibility already has a single inline source (`sg_vehicle_allowed_for_request`
/ `sg_vehicle_qualifies`); the problem there was call-site *coverage*, not
divergence. `sg_seg_eligible` gives the operators one helper to call, and the
apply chokepoint (below) is the hard backstop.

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
4. Re-point the delivery and PD insertion evals' O(L) fallback at it; keep the
   O(1) concat as the verified fast path.
5. Replace the operators' ad-hoc eligibility checks with `sg_seg_eligible`.
6. Make the final-commit feasibility guarantee explicit (repair-or-reject).

Each step is a pure consolidation with an existing safety net (the sweep, the
`SG_CONCAT_VERIFY` gate, the Solomon/Li&Lim byte-identity check, the 481-test
suite). The end state: **one capacity function, one eligibility function, the
fast path verified against them in CI, and two enforced chokepoints** — so this
class of bug cannot recur by drift.

## Known fast-path defects (found by the M2 gate)

Enabling the `SG_CONCAT_VERIFY` gate (M2) surfaced two concat discrepancies,
both distinct from #226/#227. Fix both in step 4 (concat derived from, and
verified equal to, the one canonical check):

**(a) Over-reject on an empty multi-dimension route (safe direction).** On a
**2-D capacity, first insertion into an empty route**,
`sg_route_check_capacity_concat` reports a spurious violation (`concat_ok=0`
while the O(L) reference correctly says feasible) — e.g.
`test_multi_dimension_capacity` (cap `[100,5]`, demand `[-50,-3]`: clearly fits,
yet concat reports viol 1.0 at `stop_len=0`). This is the **safe** direction (it
over-rejects a feasible insertion rather than admitting an infeasible one), so the
gate warns rather than failing — but it is a real quality loss: the fast path
prunes valid first-insertions on multi-dimension vehicles, latent on the 2-D/3-D
Gyermelyi model.

**(b) Unsafe admit on a PD route (dangerous direction).** The correctness sweep
(256 feature combos x seeds, built `-DSG_CONCAT_VERIFY`) aborts on a **pickup-
delivery** instance — feature bit `F_PD` alone, 1-D capacity — where concat
*admits* an insertion the O(L) reference rejects: `concat_ok=1 scan_ok=0 v=1
pos=5 stop_len=25 scan_viol=4.0` (sweep `f=0x40 seed=2`). This is the
**dangerous** direction and the gate *fails* (aborts) on it. Confirmed
**pre-existing** on the merged main (0a5a2aef) with the Step-2 refactor byte-
identical to base — i.e. a latent concat bug the M2 sweep exposes, not a refactor
regression. The downstream safety nets (guarded postprocess, eject, validator-
gated commit) prevent it from reaching a committed solution (the non-verify sweep
reports committed-invalid BUGS=0), so it is latent rather than shipped — but it is
the strongest argument yet for M1: an O(1) summary that silently admits infeasible
PD insertions is exactly the drift one source of truth removes. Step 4 must make
the concat PD capacity path derive from `sg_cap_trip_excess` and the sweep run to
completion with zero unsafe admits.

## Why not just "be careful"

The three bugs this cycle were all shipped by careful people and caught only by
an empirical sweep. Duplication of a safety invariant is the defect; centralizing
it is the fix. This is the single highest-leverage change for Surge to be a
*trustworthy* authoritative reference solver, not merely a capable one.
