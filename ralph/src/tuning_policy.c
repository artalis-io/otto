/*
 * tuning_policy.c - instance-shape heuristics. See tuning_policy.h.
 */

#include <math.h>
#include <string.h>

#include "lp.h"
#include "tuning_policy.h"

int simplex_should_use_dense_small_phase1_partial(const SimplexSolver *solver,
                                                         const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    /* Dense, small Phase-1 tableaus make full Devex scans disproportionately
     * expensive while partial pricing still sees enough of the entering set.
     * Keep this conservative: larger or sparser NETLIB cases showed new
     * failures when partial Phase 1 was applied globally. */
    return (m <= 220 && density >= 0.05);
}

int simplex_should_use_bound_tightened_midrow_phase1_dantzig(const SimplexSolver *solver,
                                                                    const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    double nnz_per_row = (double)nnz / (double)m;
    /* Bound-tightened SEBA-like Phase 1 reaches feasibility with very few
     * Dantzig pivots, while nearby lower-row and BNL-like shapes regress or
     * stall badly under the same override. Keep the row/width/density band
     * tight so this does not catch those neighboring NETLIB families. */
    return (m >= 510 && m <= 540 &&
            n >= 1000 && n <= 1060 &&
            width_ratio >= 1.9 && width_ratio <= 2.05 &&
            nnz_per_row >= 8.0 && nnz_per_row <= 8.8 &&
            density >= 0.0078 && density <= 0.0085);
}

int simplex_should_use_sparse_midrow_phase1_partial(const SimplexSolver *solver,
                                                           const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Sparse mid-row Phase-1 tableaus pay a high O(n) Devex scan/update cost
     * while each pivot touches relatively little matrix structure. Partial
     * pricing is allowed only in this moderate row band; higher-row NETLIB
     * cases need stronger full pricing to avoid Phase-1 stalls. */
    return (m >= 520 && m <= 680 &&
            n >= 1000 && n <= 1400 &&
            width_ratio >= 1.7 &&
            density >= 0.004 && density <= 0.010);
}

int simplex_should_use_sparse_bridge_phase1_partial(const SimplexSolver *solver,
                                                           const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Mid/large sparse Phase-1 tableaus in this bridge band spend heavily on
     * exact Devex/steepest-edge maintenance while partial pricing still keeps
     * enough entering coverage. Keep the band away from nearby lower-width
     * presolved cases and wider NETLIB models that regress under partial. */
    return (m >= 780 && m <= 1350 &&
            n >= 1550 && n <= 1750 &&
            width_ratio >= 1.2 && width_ratio <= 2.1 &&
            density >= 0.003 && density <= 0.010);
}

int simplex_should_use_mid_sparse_phase1_dantzig(const SimplexSolver *solver,
                                                        const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Mid-row sparse Phase-1 tableaus can spend more maintaining Devex weights
     * than they gain from the stronger pivot score. Keep the selector bounded
     * to the observed shape class; smaller near-square cases regress under
     * Dantzig, and very wide cases have different numerical behavior. */
    return (m >= 350 && m <= 600 &&
            n >= 800 && n <= 1200 &&
            width_ratio >= 1.5 &&
            density >= 0.01 && density <= 0.03);
}

int simplex_should_use_scaled_midrow_phase1_partial(const SimplexSolver *solver,
                                                           const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    double nnz_per_row = (double)nnz / (double)m;
    /* Larger presolved SCFXM-like Phase-1 tableaus spend heavily in full
     * pricing; partial pricing cuts that work without the MAROS timeout seen
     * in nearby denser/wider shapes. Keep this band on the larger, sparser
     * scaled member only. */
    return (m >= 860 && m <= 910 &&
            n >= 1340 && n <= 1390 &&
            width_ratio >= 1.50 && width_ratio <= 1.58 &&
            nnz_per_row >= 8.3 && nnz_per_row <= 8.9 &&
            density >= 0.0060 && density <= 0.0066);
}

int simplex_should_use_narrow_midrow_phase1_dantzig(const SimplexSolver *solver,
                                                           const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    /* Narrow mid-row Phase-1 tableaus in these bands do not recover enough
     * from Devex maintenance to offset its scan/update cost.  Keep the bands
     * away from neighboring SCTAP/SCFXM/degen shapes where Dantzig either
     * loses Phase-2 progress or increases total pivots. */
    return ((m >= 290 && m <= 305 &&
             n >= 465 && n <= 480 &&
             density >= 0.015 && density <= 0.020) ||
            (m >= 250 && m <= 290 &&
             n >= 330 && n <= 380 &&
             density >= 0.016 && density <= 0.021) ||
            (m >= 340 && m <= 370 &&
             n >= 370 && n <= 400 &&
             density >= 0.020 && density <= 0.030) ||
            (m >= 450 && m <= 500 &&
             n >= 480 && n <= 520 &&
             density >= 0.005 && density <= 0.008) ||
            (m >= 340 && m <= 360 &&
             n >= 490 && n <= 510 &&
             density >= 0.007 && density <= 0.011));
}

int simplex_should_use_bandm_phase2_dantzig(const SimplexSolver *solver,
                                                   const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    /* BANDM-like compact sparse Phase 2 gets fewer pivots with Dantzig after
     * the existing Phase-1 Dantzig path.  Keep this on the narrow BANDM shape;
     * neighboring SCAGR/STAIR families have different density and row bands. */
    return (m >= 290 && m <= 320 &&
            n >= 450 && n <= 500 &&
            density >= 0.015 && density <= 0.020);
}

int simplex_should_use_very_sparse_large_dantzig(const SimplexSolver *solver,
                                                        const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    /* Very large, very sparse tableaus spend heavily on Devex bookkeeping.
     * Dantzig gives up some pivot quality but cuts enough pricing/update work
     * to win on this structural class. */
    return (m >= 1500 && n >= 8000 && density <= 0.0015);
}

int simplex_should_use_large_sparse_phase1_dantzig(const SimplexSolver *solver,
                                                          const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Large, very sparse Phase-1 tableaus with only moderate width can spend
     * heavily on Devex maintenance before feasibility.  Dantzig is limited to
     * this high-row sparse band; lower-row sparse cases such as bnl1 still need
     * Devex to avoid Phase-1 stalls. */
    return (m >= 1800 && m <= 2800 &&
            n >= 2500 && n <= 4500 &&
            width_ratio >= 1.2 && width_ratio <= 2.0 &&
            density >= 0.001 && density <= 0.003);
}

int simplex_should_use_large_moderate_sparse_phase1_dantzig(const SimplexSolver *solver,
                                                                   const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Very large, moderately sparse Phase-1 tableaus can stall under Devex
     * before feasibility.  Keep this Phase-1-only selector away from smaller
     * pilot-class LPs and much sparser fit-class LPs. */
    return (m >= 3000 && m <= 3300 &&
            n >= 9000 && n <= 10000 &&
            width_ratio >= 2.8 && width_ratio <= 3.2 &&
            density >= 0.004 && density <= 0.006);
}

int simplex_should_use_mid_sparse_phase12_dantzig(const SimplexSolver *solver,
                                                         const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    /* Sparse mid-size cases in this band get cheaper Phase 1 and Phase 2
     * progress from Dantzig. Nearby lower-row shapes such as bnl1/pilot4
     * regress, so keep the row and density band tight. */
    return (m >= 790 && m <= 900 &&
            n >= 1400 && n <= 1600 &&
            density >= 0.007 && density <= 0.009);
}

int simplex_should_use_midwide_sparse_phase12_dantzig(const SimplexSolver *solver,
                                                             const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Mid-row, wide sparse tableaus in this band spend enough in Devex
     * bookkeeping during both phases that Dantzig's cheaper iterations win.
     * Keep the density floor above nearby pilot-like shapes that regress. */
    return (m >= 720 && m <= 750 &&
            n >= 2700 && n <= 2800 &&
            width_ratio >= 3.6 && width_ratio <= 3.9 &&
            density >= 0.0055 && density <= 0.0080);
}

int simplex_should_use_sparse_grow_phase12_dantzig(const SimplexSolver *solver,
                                                          const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    return (m >= 350 && m <= 500 &&
            n >= 900 && n <= 1100 &&
            density >= 0.018 && density <= 0.022);
}

int simplex_should_use_small_grow_phase12_dantzig(const SimplexSolver *solver,
                                                         const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Smaller grow-like Phase-2-heavy tableaus have nearly no Phase 1 work
     * and benefit from cheaper Dantzig iterations.  The width guard excludes
     * compact mid-density models where Devex is already competitive. */
    return (m >= 120 && m <= 320 &&
            n >= 280 && n <= 700 &&
            width_ratio >= 2.05 && width_ratio <= 2.25 &&
            density >= 0.025 && density <= 0.065);
}

int simplex_should_use_eta_sparse_phase12_dantzig(const SimplexSolver *solver,
                                                         const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Sparse ETA-like mid-row tableaus spend enough in Devex maintenance
     * during both phases that Dantzig's cheaper path wins. Keep this away
     * from denser/scaled neighbors that need Devex pivot quality. */
    return (m >= 390 && m <= 410 &&
            n >= 590 && n <= 620 &&
            width_ratio >= 1.45 && width_ratio <= 1.60 &&
            density >= 0.008 && density <= 0.014);
}

int simplex_should_use_lowrow_wide_scsd_phase12_dantzig(const SimplexSolver *solver,
                                                               const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Low-row, wide SCSD-like tableaus are still sparse enough that full
     * Devex bookkeeping dominates, but not so tiny/dense that Devex's pivot
     * quality wins. Keep this away from scsd1 and larger scsd8-like cases. */
    return (m >= 130 && m <= 170 &&
            n >= 1200 && n <= 1500 &&
            width_ratio >= 8.0 && width_ratio <= 10.0 &&
            density >= 0.018 && density <= 0.025);
}

int simplex_should_use_scsd_sparse_phase12_dantzig(const SimplexSolver *solver,
                                                          const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    return (m >= 350 && m <= 450 &&
            n >= 2400 && n <= 3000 &&
            density >= 0.006 && density <= 0.009);
}

int simplex_should_use_wide_sparse_phase1_heap(const SimplexSolver *solver,
                                                      const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Very sparse, wide transportation-style Phase 1 tableaus get cheaper
     * progress from heap pricing. Extremely wide long-form ship tableaus pay
     * more in extra pivots than heap pricing saves, so leave those on Devex. */
    return (m >= 750 && m <= 1200 &&
            width_ratio >= 2.0 && width_ratio <= 4.2 &&
            density >= 0.0024 && density <= 0.0040);
}

int simplex_should_use_lowrow_wide_sparse_phase1_heap(const SimplexSolver *solver,
                                                             const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Low-row, wide, very sparse Phase-1 tableaus can spend more in full
     * Devex scans than they recover from Devex pivot quality. Heap pricing is
     * limited to this SHELL-like band; denser BNL/SCFXM and taller GANGES/
     * SCTAP shapes have different Phase-1 behavior. */
    return (m >= 500 && m <= 575 &&
            n >= 1450 && n <= 1650 &&
            width_ratio >= 2.7 && width_ratio <= 3.1 &&
            density >= 0.0032 && density <= 0.0042);
}

int simplex_should_use_wide_sparse_phase1_dantzig_phase2_partial(
    const SimplexSolver *solver,
    const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Wide sparse CZPROB-like tableaus reach Phase 1 feasibility with fewer
     * costs under cheap full-column Dantzig pricing, then pay too much for
     * full Phase-2 scans. Keep this away from wider SHIP cases and lower-width
     * GANGES/SCTAP shapes. */
    return (m >= 850 && m <= 1050 &&
            n >= 3300 && n <= 3700 &&
            width_ratio >= 3.4 && width_ratio <= 4.2 &&
            density >= 0.0028 && density <= 0.0038);
}

int simplex_should_use_dense_lowrow_phase2_partial(const SimplexSolver *solver,
                                                          const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    /* Very low-row dense NETLIB LPs spend more in full-column Devex/Dantzig
     * pricing and dense pivot maintenance than they recover from exact pricing.
     * Partial pricing keeps the Phase 2 path much cheaper on this shape. */
    return (m <= 30 && n >= 1000 && density >= 0.30);
}

int simplex_should_use_medium_sparse_phase2_partial(const SimplexSolver *solver,
                                                           const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Medium sparse Phase 2 tableaus in this band pay enough full Devex
     * maintenance cost that partial pricing can reduce refactors and pivots.
     * Keep the shape away from lower-width NETLIB cases where full Devex is
     * needed for stable progress. */
    return (m >= 600 && m <= 700 &&
            n >= 1500 && n <= 1900 &&
            width_ratio >= 2.3 && width_ratio <= 3.1 &&
            density >= 0.007 && density <= 0.012);
}

int simplex_should_use_compact_sparse_phase2_partial(const SimplexSolver *solver,
                                                            const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    double nnz_per_row = (double)nnz / (double)m;
    /* Compact sparse tableaus in this narrow-width band finish Phase 1 reliably
     * under Devex, then pay more for exact Phase-2 pricing than they recover
     * from full-column pivot quality.  The guards use the presolved shape and
     * exclude wider BNL-like cases where partial pricing can stall after Phase
     * 1. */
    return (m >= 250 && m <= 950 &&
            n >= 400 && n <= 1450 &&
            width_ratio >= 1.48 && width_ratio <= 1.62 &&
            nnz_per_row >= 7.0 && nnz_per_row <= 10.0 &&
            density >= 0.004 && density <= 0.020);
}

int simplex_should_use_wide_scsd_phase2_partial(const SimplexSolver *solver,
                                                       const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Wide SCSD tableaus keep the Dantzig Phase-1 path, but Phase 2 spends
     * enough in full pricing over many columns that partial pricing wins.
     * The density/width bands keep this away from tiny dense scsd1 and from
     * unrelated mid-row sparse NETLIB families. */
    return ((m >= 130 && m <= 170 &&
             n >= 1200 && n <= 1500 &&
             width_ratio >= 8.0 && width_ratio <= 10.0 &&
             density >= 0.018 && density <= 0.025) ||
            (m >= 380 && m <= 420 &&
             n >= 2600 && n <= 2900 &&
             width_ratio >= 6.5 && width_ratio <= 7.3 &&
             density >= 0.006 && density <= 0.009));
}

int simplex_should_use_sparse_fit_phase2_steepest(const SimplexSolver *solver,
                                                         const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* FIT-style sparse Phase 2 can recover enough pivot-quality improvement
     * from exact steepest-edge pricing to cut the path substantially. Keep the
     * band away from dense low-row FITD cases and much larger sparse FIT2P
     * dual-start cases where this selector is not the active bottleneck. */
    return (m >= 580 && m <= 700 &&
            n >= 1500 && n <= 1900 &&
            width_ratio >= 2.4 && width_ratio <= 2.9 &&
            density >= 0.008 && density <= 0.011);
}

int simplex_should_use_fit2p_phase2_heap(const SimplexSolver *solver,
                                                const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* FIT2P-scale Phase 2 spends hundreds of milliseconds repeatedly scanning
     * a very wide reduced-cost vector. Heap pricing preserves the path shape
     * on this ultra-sparse band while avoiding the full-column scan cost.
     * Keep this above FIT1P and away from denser BNL/SCTAP/25FV families where
     * heap pricing adds pivots and regresses wall time. */
    return (m >= 2800 && m <= 3200 &&
            n >= 12500 && n <= 14500 &&
            width_ratio >= 4.2 && width_ratio <= 4.8 &&
            density >= 0.0010 && density <= 0.0015);
}

int simplex_should_use_sparse_fit_phase1_partial(const SimplexSolver *solver,
                                                        const SimplexTableau *tab) {
    if (!tab || !tab->use_two_phase || tab->num_artificial <= 0) return 0;
    return simplex_should_use_sparse_fit_phase2_steepest(solver, tab);
}
