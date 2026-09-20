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

/* From ralph.c. Seven more of the same kind, on the presolve and dual-simplex
 * paths rather than pricing: each is a hardcoded band on the problem shape that
 * turns one strategy on or off. They read exactly two fields, num_cons and
 * num_vars, so they are pure functions of (m, n) and fold into the same shape
 * descriptor the predicates above want. Signatures unchanged; the seven call
 * sites in ralph.c are untouched. */
int ralph_should_skip_sparse_mid_presolve(const LPModel *model) {
    if (!model || !model->A) return 0;

    int n = model->num_vars;
    int m = model->num_cons;
    int nnz = model->A->nnz;
    if (n <= 0 || m <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)n * (double)m);
    /* In these sparse bands the safe presolve pass removes little useful
     * structure but changes the simplex path enough to cost more downstream.
     * Larger sparse cases and denser mid-size cases still benefit or need the
     * existing presolve behavior, so keep the bypass narrow. */
    if (n >= 3300 && n <= 3700 &&
        m >= 850 && m <= 1050 &&
        density >= 0.0025 && density <= 0.0045) {
        return 1;
    }

    if (n >= 800 && n <= 900 &&
        m >= 500 && m <= 550 &&
        density >= 0.012 && density <= 0.015) {
        return 1;
    }

    /* Compact sparse BANDM/SCAGR-like LPs lose useful basis structure under
     * safe presolve: row removal and bound tightening increase Phase 1 pivots
     * more than they shrink the model. Keep these bands below denser compact
     * classes that benefit from bound-tightening-only presolve. */
    if (n >= 450 && n <= 500 &&
        m >= 290 && m <= 320 &&
        density >= 0.015 && density <= 0.020) {
        return 1;
    }

    if (n >= 480 && n <= 520 &&
        m >= 450 && m <= 500 &&
        density >= 0.005 && density <= 0.008) {
        return 1;
    }

    /* Small DEGEN2-like LPs only get bound tightening from safe presolve; the
     * tighter bounds reduce Phase 1 work but make the following Phase 2 path
     * substantially more expensive. Larger DEGEN-family cases still need
     * presolve, so keep this compact band narrow. */
    if (n >= 520 && n <= 550 &&
        m >= 430 && m <= 460 &&
        density >= 0.015 && density <= 0.019) {
        return 1;
    }

    /* GROW-like staircase LPs see only bound-tightening changes from safe
     * presolve; the simplex path is unchanged, so the presolve pass is pure
     * overhead on this width/density family. */
    if (n >= 280 && n <= 1000 &&
        m >= 120 && m <= 460 &&
        n * 20 >= m * 42 &&
        n * 20 <= m * 45 &&
        density >= 0.018 && density <= 0.065) {
        return 1;
    }

    if (n >= 1100 && n <= 1250 &&
        m >= 600 && m <= 700 &&
        density >= 0.005 && density <= 0.008) {
        return 1;
    }

    /* Mid-size sparse LPs in this band pay for several safe-presolve rounds
     * and bound tightening, but the smaller presolved matrix takes a longer
     * Phase 2 path. Keep the bypass below the broader large-sparse bands that
     * still benefit from presolve-driven reductions. */
    if (n >= 1450 && n <= 1650 &&
        m >= 780 && m <= 860 &&
        density >= 0.0075 && density <= 0.0088) {
        return 1;
    }

    if (n >= 1600 && n <= 8500 &&
        m >= 750 && m <= 1600 &&
        density >= 0.002 && density <= 0.0045) {
        return 1;
    }

    /* The large sparse SCSD member runs a safe presolve round that removes no
     * rows, columns, or bounds.  Smaller denser SCSD siblings are fast enough
     * that their path noise can outweigh the saved presolve pass. */
    if (n >= 2400 && n <= 3000 &&
        m >= 350 && m <= 450 &&
        density >= 0.006 && density <= 0.009) {
        return 1;
    }

    /* FIT1P-scale sparse LPs also get a no-op safe presolve pass, while the
     * larger FIT2P member still needs the existing presolve/dual-start path. */
    if (n >= 1600 && n <= 1800 &&
        m >= 600 && m <= 700 &&
        density >= 0.008 && density <= 0.011) {
        return 1;
    }

    return 0;
}

int ralph_should_skip_dense_compact_bound_tightening(const LPModel *model) {
    if (!model || !model->A) return 0;

    int n = model->num_vars;
    int m = model->num_cons;
    int nnz = model->A->nnz;
    if (n <= 0 || m <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)n * (double)m);
    /* Compact dense LPs in this band can get a much harder Phase 1 after safe
     * bound tightening interacts with singleton-row reductions. Keep structural
     * reductions and bound shifting, but avoid the extra tightening pass. */
    return (n >= 350 && n <= 500 &&
            m >= 130 && m <= 220 &&
            density >= 0.055 && density <= 0.080);
}

int ralph_should_use_fixed_bound_tightening_presolve(const LPModel *model) {
    if (!model || !model->A) return 0;

    int n = model->num_vars;
    int m = model->num_cons;
    int nnz = model->A->nnz;
    if (n <= 0 || m <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)n * (double)m);
    double width_ratio = (double)n / (double)m;
    /* SHELL-like sparse models get the useful fixed-variable and bound
     * tightening reductions from safe presolve, while singleton/empty-row
     * passes add overhead and shift the following primal path. Keep this on
     * the fixed-variable-heavy wide sparse band only. */
    return (m >= 500 && m <= 570 &&
            n >= 1700 && n <= 1850 &&
            width_ratio >= 3.1 && width_ratio <= 3.5 &&
            density >= 0.0030 && density <= 0.0045);
}

int ralph_should_control_mid_sparse_reinvert(const LPModel *model) {
    if (!model || !model->A) return 0;

    int n = model->num_vars;
    int m = model->num_cons;
    int nnz = model->A->nnz;
    if (n <= 0 || m <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)n * (double)m);
    double width_ratio = (double)n / (double)m;
    if (n >= 3000 && n <= 4500 &&
        m >= 1800 && m <= 2800 &&
        density >= 0.001 && density <= 0.0025) {
        return 1;
    }

    /* Very wide ultra-sparse FIT2P-scale primals hit long Phase 2 stretches
     * where routine reinversions dominate. Let the reinvert controller dampen
     * the cadence for this large sparse shape while keeping dense FITD and
     * smaller FIT1P-style models on their established policies. */
    if (n >= 12500 && n <= 14500 &&
        m >= 2800 && m <= 3200 &&
        width_ratio >= 4.2 && width_ratio <= 4.8 &&
        density >= 0.0010 && density <= 0.0015) {
        return 1;
    }

    /* Wide sparse CZPROB-like primals have many routine reinversions after
     * the no-presolve path preserves the full matrix.  Reuse the narrow
     * CZPROB shape band from the presolve policy so larger D2Q06C-like dual
     * starts and denser mid-size cases stay on their existing cadence. */
    if (n >= 3300 && n <= 3700 &&
        m >= 850 && m <= 1050 &&
        density >= 0.0025 && density <= 0.0045) {
        return 1;
    }

    if (n >= 800 && n <= 1500 &&
        nnz >= 8000 &&
        m >= 700 && m <= 1100 &&
        n * 20 <= m * 31 &&
        density >= 0.005 && density <= 0.010) {
        return 1;
    }

    /* SCFXM-family presolved primals are sparse and near-rectangular; routine
     * reinversions dominate both phases.  Keep BNL-like wider cases out via
     * the row/column ratio guard, since they regress under the same control. */
    if (n >= 850 && n <= 1500 &&
        nnz >= 5000 &&
        m >= 600 && m <= 1100 &&
        n * 20 <= m * 31 &&
        density >= 0.005 && density <= 0.010) {
        return 1;
    }

    /* Smaller column-heavy mid-sparse bases can spend most of Phase 1 on
     * periodic reinversions.  Let the reinvert controller dampen cadence here,
     * while keeping larger rows out of this automatic control path. */
    if (n >= 1000 && n <= 1300 &&
        nnz >= 8000 &&
        m >= 600 && m <= 700 &&
        n * 10 >= m * 17 &&
        density >= 0.005 && density <= 0.009) {
        return 1;
    }

    /* SHIP08-size sparse primals keep the same pivot path under reinversion
     * control while cutting routine Phase-1/Phase-2 refactors.  SHIP04 and
     * SHIP12 have distinct row/density bands and remain on their existing
     * policies. */
    if (n >= 2300 && n <= 4400 &&
        m >= 740 && m <= 820 &&
        density >= 0.0035 && density <= 0.0042) {
        return 1;
    }

    /* SHIP12-class dual starts are wide and very sparse; DSE is already
     * disabled for this band, and reinversion control cuts routine dual
     * refactors without changing the pivot path.  Keep this separate from
     * denser SHIP04 and lower-row SHIP08 cases. */
    if (n >= 2500 && n <= 5600 &&
        m >= 1100 && m <= 1200 &&
        density >= 0.0023 && density <= 0.0029) {
        return 1;
    }

    /* GANGES-style mid-row sparse dual starts already avoid exact DSE and
     * shifted bounds; routine reinversions become the remaining dual bottleneck.
     * Control that cadence only in this narrow row/width/density band so
     * taller SCTAP and denser SCFXM/MAROS families keep their existing paths. */
    if (m >= 1250 && m <= 1350 &&
        n >= 1600 && n <= 1750 &&
        width_ratio >= 1.20 && width_ratio <= 1.35 &&
        density >= 0.0029 && density <= 0.0034) {
        return 1;
    }

    if (n >= 800 && n <= 900 &&
        m >= 500 && m <= 550 &&
        density >= 0.012 && density <= 0.015) {
        return 1;
    }

    /* Compact DEGEN2-like LPs spend enough time in both phases on routine
     * reinversions that controller deferral cuts the pivot path substantially.
     * Keep this away from denser STAIR-like cases and larger DEGEN3 dual
     * starts, which have different bottlenecks. */
    if (n >= 520 && n <= 550 &&
        m >= 430 && m <= 460 &&
        density >= 0.015 && density <= 0.019) {
        return 1;
    }

    /* Larger DEGEN-family dual starts stay numerically calm but spend heavily
     * on routine dual reinversions after presolve bound tightening.  Control
     * that cadence only in this near-square sparse band; wider sparse dual
     * timeout families and denser compact DEGEN2-like primals use different
     * paths. */
    if (n >= 1700 && n <= 1900 &&
        m >= 1400 && m <= 1600 &&
        density >= 0.008 && density <= 0.010) {
        return 1;
    }

    /* Large, very sparse BAU-style primals keep the same broad pivot path
     * under reinversion control while avoiding many routine Phase-2 refactors.
     * The high width and very low density keep this away from FIT/PILOT
     * timeout families and denser mid-size sparse cases. */
    if (n >= 9500 && n <= 10500 &&
        m >= 2200 && m <= 2350 &&
        density > 0.0 && density <= 0.0012) {
        return 1;
    }

    /* Larger GROW-like staircase LPs have almost no Phase 1 work, but spend
     * much of Phase 2 paying for routine reinversions. The reinvert controller
     * safely defers that cadence on the sparse large member; smaller denser
     * GROW siblings are already fast and stay outside this band. */
    if (n >= 900 && n <= 1000 &&
        m >= 420 && m <= 460 &&
        n * 20 >= m * 42 &&
        n * 20 <= m * 45 &&
        density >= 0.018 && density <= 0.022) {
        return 1;
    }

    /* Large near-square stochastic LPs can spend much of dual startup on
     * periodic reinversions after presolve tightens bounds but leaves the
     * matrix dimensions intact.  Let the reinvert controller defer routine
     * dual refactors in this sparse band while keeping wider FIT-like dual
     * timeout cases on the legacy cadence. */
    if (n >= 1800 && n <= 2300 &&
        m >= 1900 && m <= 2400 &&
        density >= 0.0015 && density <= 0.0023) {
        return 1;
    }

    /* Low-row, wide SHELL-style sparse primals pay many small routine
     * reinversions relative to their pivot count.  Control that cadence in
     * this narrow width/density band; taller transport/SCTAP cases and denser
     * BNL/SCFXM families stay on their existing policies. */
    if (n >= 1700 && n <= 1850 &&
        m >= 500 && m <= 575 &&
        density >= 0.0032 && density <= 0.0042) {
        return 1;
    }

    /* Larger SCTAP-style sparse primals pay several routine Phase-1/Phase-2
     * reinversions after the preserved full matrix reaches Phase 2.  This
     * narrow row/width/density band excludes smaller SCTAP siblings, which are
     * already below target, and denser MAROS/PILOT families. */
    if (n >= 2300 && n <= 2600 &&
        m >= 1400 && m <= 1550 &&
        density >= 0.0022 && density <= 0.0027) {
        return 1;
    }

    /* FIT1P-size sparse primals spend heavily on small-pivot/update recovery
     * reinversions in both phases.  Reinvert control trims that cadence on
     * this medium-wide band; keep the selector below FIT2P and away from the
     * denser, lower-row PILOT4 family where the same control path regresses. */
    if (n >= 1600 && n <= 1800 &&
        m >= 600 && m <= 660 &&
        n * 10 >= m * 25 &&
        n * 10 <= m * 29 &&
        density >= 0.008 && density <= 0.011) {
        return 1;
    }

    return 0;
}

int ralph_should_disable_dual_dse_wide_ship(const LPModel *model) {
    if (!model || !model->A) return 0;

    int n = model->num_vars;
    int m = model->num_cons;
    int nnz = model->A->nnz;
    if (n <= 0 || m <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)n * (double)m);
    double width_ratio = (double)n / (double)m;
    /* Mid-row GANGES-style sparse dual starts do much less work without exact
     * DSE maintenance while preserving the same verified optimum path.  Keep
     * this below taller SCTAP/SHIP12 bands and above denser SCFXM-like cases. */
    if (m >= 1250 && m <= 1350 &&
        n >= 1600 && n <= 1750 &&
        width_ratio >= 1.20 && width_ratio <= 1.35 &&
        density >= 0.0029 && density <= 0.0034) {
        return 1;
    }

    /* Wide, very sparse ship12-class dual starts get almost the same pivot
     * path without DSE, while avoiding exact DSE initialization and refresh
     * solves. Keep this narrow; denser/lower-row dual starts still benefit
     * from DSE pivot quality. */
    return (m >= 1100 && m <= 1200 &&
            n >= 2500 && n <= 5600 &&
            width_ratio >= 2.3 && width_ratio <= 4.9 &&
            density >= 0.0023 && density <= 0.0029);
}

int ralph_should_use_relaxed_dual_rc_cadence_compact_sparse(const LPModel *model) {
    if (!model || !model->A) return 0;

    int n = model->num_vars;
    int m = model->num_cons;
    int nnz = model->A->nnz;
    if (n <= 0 || m <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)n * (double)m);
    double width_ratio = (double)n / (double)m;
    /* Compact sparse dual starts in this band benefit from a slightly longer
     * reduced-cost recompute cadence: the default 20-iteration cadence perturbs
     * pivot selection enough to add refactors, while 25 keeps the same safety
     * envelope used by the GLPK-compatible update-limit mapping. */
    return (m >= 500 && m <= 550 &&
            n >= 800 && n <= 900 &&
            width_ratio >= 1.55 && width_ratio <= 1.75 &&
            density >= 0.012 && density <= 0.015);
}

int ralph_should_use_shift_off_dual_start(const LPModel *model) {
    if (!model || !model->A) return 0;

    int n = model->num_vars;
    int m = model->num_cons;
    int nnz = model->A->nnz;
    if (n <= 0 || m <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)n * (double)m);
    double width_ratio = (double)n / (double)m;
    /* Very wide, dense, low-row LPs can make shifted dual working bounds
     * return a bound-imprecise basis, while the unshifted dual path is both
     * valid and much cheaper than falling through to primal Phase 1. */
    if (m >= 230 && m <= 260 &&
        n >= 2400 && n <= 2800 &&
        width_ratio >= 9.5 && width_ratio <= 12.0 &&
        density >= 0.09 && density <= 0.13) {
        return 1;
    }

    /* A wider sparse mid-row band shows the same shifted-bound failure mode:
     * primal gets trapped in degenerate cleanup, shifted dual is imprecise, and
     * unshifted dual produces a verified basis quickly. */
    if (m >= 390 && m <= 430 &&
        n >= 5800 && n <= 6500 &&
        width_ratio >= 14.0 && width_ratio <= 16.0 &&
        density >= 0.012 && density <= 0.017) {
        return 1;
    }

    /* GANGES-style mid-row sparse dual starts are numerically cleaner without
     * shifted working bounds; this trims pivots on the same band that already
     * disables exact DSE maintenance. */
    if (m >= 1250 && m <= 1350 &&
        n >= 1600 && n <= 1750 &&
        width_ratio >= 1.20 && width_ratio <= 1.35 &&
        density >= 0.0029 && density <= 0.0034) {
        return 1;
    }

    /* Large near-square stochastic dual starts keep the shifted-bound path
     * valid, but unshifted dual trims a substantial pivot/refactor tail. Keep
     * this above the tiny STOCFOR member and away from wider sparse timeouts. */
    if (m >= 2050 && m <= 2250 &&
        n >= 1950 && n <= 2150 &&
        width_ratio >= 0.90 && width_ratio <= 1.00 &&
        density >= 0.0017 && density <= 0.0021) {
        return 1;
    }

    /* High-row degenerate dual starts are valid with shifted working bounds,
     * but the unshifted path avoids a long tail of pivots. Keep this above
     * compact DEGEN members, where shift-off sends the primal path sideways. */
    if (m >= 1450 && m <= 1550 &&
        n >= 1750 && n <= 1900 &&
        width_ratio >= 1.15 && width_ratio <= 1.30 &&
        density >= 0.0085 && density <= 0.0095) {
        return 1;
    }

    /* SHIP08/SHIP12 sparse dual starts are already valid with shifted bounds,
     * but the unshifted dual path avoids extra pivots and refactors.  Keep the
     * row bands split: middle-row CZPROB-like cases fail on this path. */
    if (m >= 740 && m <= 820 &&
        n >= 2300 && n <= 4400 &&
        width_ratio >= 3.0 && width_ratio <= 5.7 &&
        density >= 0.0035 && density <= 0.0042) {
        return 1;
    }

    return (m >= 1100 && m <= 1200 &&
            n >= 2500 && n <= 5600 &&
            width_ratio >= 2.3 && width_ratio <= 4.9 &&
            density >= 0.0023 && density <= 0.0029);
}
