#include <math.h>
#include <limits.h>

#include "spp.h"

static int spp_bound_fixed_zero(double ub) {
    return ub <= RALPH_INT_TOL;
}

static int spp_bound_fixed_one(double lb) {
    return lb >= 1.0 - RALPH_INT_TOL;
}

static int spp_bound_branchable(double lb, double ub) {
    return ub - lb > RALPH_INT_TOL;
}

static double spp_clamp_to_bounds(double value, double lb, double ub) {
    if (value < lb) return lb;
    if (value > ub) return ub;
    return value;
}

int spp_propagate_bounds(const SPPContext *ctx,
                         double *lb,
                         double *ub,
                         int *fixings_out) {
    int total_fixings = 0;
    int changed = 1;

    if (fixings_out) *fixings_out = 0;
    if (!ctx || !lb || !ub) return -1;

    while (changed) {
        changed = 0;

        for (int ord = 0; ord < ctx->num_rows; ord++) {
            int row = ctx->row_order ? ctx->row_order[ord] : ord;
            int selected = 0;
            int selected_set = -1;
            int available = 0;
            int only_set = -1;

            for (int p = ctx->row_ptr[row]; p < ctx->row_ptr[row + 1]; p++) {
                int set = ctx->row_sets[p];
                if (ub[set] < lb[set] - RALPH_INT_TOL) return -1;
                if (spp_bound_fixed_zero(ub[set])) continue;

                available++;
                only_set = set;
                if (spp_bound_fixed_one(lb[set])) {
                    selected++;
                    selected_set = set;
                }
            }

            if (selected > 1) return -1;
            if (available == 0) return -1;

            if (selected == 1) {
                if (selected_set < 0 || ub[selected_set] < 1.0 - RALPH_INT_TOL) return -1;
                if (lb[selected_set] < 1.0 - RALPH_INT_TOL) {
                    lb[selected_set] = 1.0;
                    total_fixings++;
                    changed = 1;
                }
                if (ub[selected_set] > 1.0 + RALPH_INT_TOL) {
                    ub[selected_set] = 1.0;
                    total_fixings++;
                    changed = 1;
                }

                for (int p = ctx->row_ptr[row]; p < ctx->row_ptr[row + 1]; p++) {
                    int set = ctx->row_sets[p];
                    if (set == selected_set || spp_bound_fixed_zero(ub[set])) continue;
                    if (lb[set] > RALPH_INT_TOL) return -1;
                    ub[set] = 0.0;
                    total_fixings++;
                    changed = 1;
                }
            } else if (available == 1) {
                if (only_set < 0 || ub[only_set] < 1.0 - RALPH_INT_TOL) return -1;
                if (lb[only_set] < 1.0 - RALPH_INT_TOL) {
                    lb[only_set] = 1.0;
                    total_fixings++;
                    changed = 1;
                }
                if (ub[only_set] > 1.0 + RALPH_INT_TOL) {
                    ub[only_set] = 1.0;
                    total_fixings++;
                    changed = 1;
                }
            }
        }
    }

    if (fixings_out) *fixings_out = total_fixings;
    return 0;
}

int spp_select_branch_set(const SPPContext *ctx,
                          const double *lb,
                          const double *ub,
                          const double *lp_x,
                          int *row_out,
                          int *set_out) {
    int best_row = -1;
    int best_set = -1;
    int best_choices = INT_MAX;
    int best_available = INT_MAX;
    double best_lp = -1.0;

    if (row_out) *row_out = -1;
    if (set_out) *set_out = -1;
    if (!ctx || !lb || !ub || !set_out) return -1;

    for (int ord = 0; ord < ctx->num_rows; ord++) {
        int row = ctx->row_order ? ctx->row_order[ord] : ord;
        int selected = 0;
        int available = 0;
        int branchable = 0;
        int row_best_set = -1;
        double row_best_lp = -1.0;

        for (int p = ctx->row_ptr[row]; p < ctx->row_ptr[row + 1]; p++) {
            int set = ctx->row_sets[p];
            double value;

            if (ub[set] < lb[set] - RALPH_INT_TOL) return -1;
            if (spp_bound_fixed_zero(ub[set])) continue;

            available++;
            if (spp_bound_fixed_one(lb[set])) {
                selected++;
                continue;
            }
            if (!spp_bound_branchable(lb[set], ub[set])) continue;

            branchable++;
            value = lp_x ? spp_clamp_to_bounds(lp_x[set], lb[set], ub[set])
                         : 0.5 * (lb[set] + ub[set]);

            if (row_best_set < 0 ||
                value > row_best_lp + RALPH_ZERO_TOL ||
                (fabs(value - row_best_lp) <= RALPH_ZERO_TOL &&
                 ctx->costs[set] < ctx->costs[row_best_set] - RALPH_ZERO_TOL) ||
                (fabs(value - row_best_lp) <= RALPH_ZERO_TOL &&
                 fabs(ctx->costs[set] - ctx->costs[row_best_set]) <= RALPH_ZERO_TOL &&
                 set < row_best_set)) {
                row_best_set = set;
                row_best_lp = value;
            }
        }

        if (selected > 0 || branchable <= 0) continue;

        if (best_row < 0 ||
            branchable < best_choices ||
            (branchable == best_choices && available < best_available) ||
            (branchable == best_choices && available == best_available &&
             row_best_lp > best_lp + RALPH_ZERO_TOL) ||
            (branchable == best_choices && available == best_available &&
             fabs(row_best_lp - best_lp) <= RALPH_ZERO_TOL &&
             row < best_row)) {
            best_row = row;
            best_set = row_best_set;
            best_choices = branchable;
            best_available = available;
            best_lp = row_best_lp;
        }
    }

    if (best_set < 0) return -1;
    if (row_out) *row_out = best_row;
    *set_out = best_set;
    return 0;
}
