# Axle-load implementation progress

The [approved design](design-surge-axle-loads.md) and
[C/JSON contract](roadmaps/surge-loading-v2-api.md) remain the implementation target.
This record distinguishes a tested internal kernel from an advertised capability.

| Stage | State |
| --- | --- |
| A1: static reaction/interval validator | Implemented internally; exact integer comparisons including front-share ratios |
| A2: joint packing/unloading witness search | Implemented internally; shared node budget, cancellation and independent replay |
| A3: version-2 context, C/JSON, HGS and reports | Pending; no capability advertised yet |
| A4: Cursus admission/publication/approval/export | Pending; no Cursus pin or runtime changes |
| A5: measured customer pilot | Pending verified physical facts and separate activation approval |

The kernel is in `surge/src/sg_axle.c`. It supports the approved rigid
2-individual-axle model only. Interval reactions and gross mass are compared
without floating-point rounding. Continued-fraction comparison avoids overflowing
front-load fraction cross-products; no compiler-specific wide integer is needed.
The caps are validated before arithmetic and bound scaled reactions inside int64.

Joint search explores both placements and same-stop unit-removal orders. It does
not discard a route after its first stack-feasible arrangement fails axles. All
work shares one node budget. A valid removal sequence yields the depot loading
sequence by reversal, through the identical static states and valid access/support.
The independent validator replays the returned arrangement and sequence.

Version-1 functions retain their input rules, node counting and witness behavior.
The internal joint path permits literal zero payload/stack limits, which were
invalid descriptors in V1, without silently broadening the V1 contract. V2 unit
operations require topmost removal even with side access; that stronger explicit
operations domain must not be confused with V1's order-free stacking abstraction.
Equal-feasible-set performance fixtures must respect that distinction.

Executed at the internal-kernel checkpoint:

- Complete `make -C surge test`: 501/501 main tests, 27 time-budget, 19 neighbour,
  22 profile-matrix and existing transport/loading checks passed.
- Native C boundary/joint tests: counterweight delivery, alternative placements
  and same-stop removal orders, intervals, fraction boundaries, malformed geometry,
  zero limits, maximum numeric values, cancellation and UNKNOWN passed.
- Independent Python Fraction oracle: 4,291 state/boundary comparisons passed.
- Independent exhaustive joint oracle: 512 small cases (182 feasible, 330 infeasible)
  matched; every positive native witness independently replayed.
- Linux ARM64 GCC 14 ASan/UBSan with leak detection: new C boundary/joint tests passed
  in an isolated read-only source container. The controller macOS sanitizer process
  stalled without reaching test output and was stopped; it is not a sanitizer pass.

The examples are public/synthetic. No measured Gyermelyi axle or stacking validation
is claimed. No reshuffling, production configuration, deployment, customer dataset
or authoritative plan was changed. Performance/cancellation release gates remain
required once the native/HGS interface is connected; kernel correctness alone is
not an activation or throughput certificate.
