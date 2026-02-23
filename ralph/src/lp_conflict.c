/*
 * Ralph - Internal LP conflict/IIS refinement helpers
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lp_conflict.h"

#define LP_CONFLICT_FARKAS_SEED_TOL (10.0 * RALPH_FEAS_TOL)

static int lp_conflict_lb_finite(double lb) {
    return lb > -RALPH_INFINITY * 0.5;
}

static int lp_conflict_ub_finite(double ub) {
    return ub < RALPH_INFINITY * 0.5;
}

static int lp_conflict_cmp_member(const LPConflictMemberInternal *a,
                                  const LPConflictMemberInternal *b) {
    if (!a || !b) return 0;
    if (a->type != b->type) return (int)a->type - (int)b->type;
    return a->index - b->index;
}

static int lp_conflict_has_bound_conflict(const LPModel *model) {
    if (!model || !model->lb || !model->ub) return 0;
    for (int j = 0; j < model->num_vars; j++) {
        if (model->lb[j] > model->ub[j] + RALPH_FEAS_TOL) return 1;
    }
    return 0;
}

static LPModel* lp_conflict_build_probe_model(const LPModel *base,
                                              const LPConflictMemberInternal *members,
                                              const unsigned char *active,
                                              int num_members,
                                              int dropped_member_idx) {
    LPModel *probe = NULL;
    unsigned char *drop_rows = NULL;
    int m = 0;

    if (!base || !members || !active || num_members < 0) return NULL;
    probe = lp_model_copy(base);
    if (!probe) return NULL;

    m = base->num_cons;
    drop_rows = (unsigned char*)calloc((size_t)m, sizeof(unsigned char));
    if (!drop_rows) {
        lp_model_free(probe);
        return NULL;
    }

    for (int i = 0; i < num_members; i++) {
        int is_kept = active[i] ? 1 : 0;
        if (i == dropped_member_idx) is_kept = 0;
        if (is_kept) continue;

        if (members[i].type == LP_CONFLICT_MEMBER_ROW) {
            int row = members[i].index;
            if (row < 0 || row >= m) {
                free(drop_rows);
                lp_model_free(probe);
                return NULL;
            }
            drop_rows[row] = 1;
        } else if (members[i].type == LP_CONFLICT_MEMBER_VAR_LB) {
            int var = members[i].index;
            if (var < 0 || var >= probe->num_vars) {
                free(drop_rows);
                lp_model_free(probe);
                return NULL;
            }
            probe->lb[var] = -RALPH_INFINITY;
        } else if (members[i].type == LP_CONFLICT_MEMBER_VAR_UB) {
            int var = members[i].index;
            if (var < 0 || var >= probe->num_vars) {
                free(drop_rows);
                lp_model_free(probe);
                return NULL;
            }
            probe->ub[var] = RALPH_INFINITY;
        } else {
            free(drop_rows);
            lp_model_free(probe);
            return NULL;
        }
    }

    for (int row = m - 1; row >= 0; row--) {
        if (!drop_rows[row]) continue;
        if (lp_model_delete_constraint(probe, row) != 0) {
            free(drop_rows);
            lp_model_free(probe);
            return NULL;
        }
    }

    free(drop_rows);
    drop_rows = NULL;

    if (lp_model_finalize(probe) != 0) {
        lp_model_free(probe);
        return NULL;
    }

    return probe;
}

static int lp_conflict_probe_with_drop(const LPModel *base,
                                       const LPConflictMemberInternal *members,
                                       const unsigned char *active,
                                       int num_members,
                                       int dropped_member_idx,
                                       LPConflictProbeFn probe_fn,
                                       void *probe_ctx,
                                       RalphStatus *status_out,
                                       LPConflictReportInternal *report) {
    LPModel *probe = NULL;
    RalphStatus status = RALPH_STATUS_ERROR;

    if (!base || !members || !active || !probe_fn || !status_out || num_members < 0) return -1;

    probe = lp_conflict_build_probe_model(base, members, active, num_members, dropped_member_idx);
    if (!probe) return -1;

    if (lp_conflict_has_bound_conflict(probe)) {
        status = RALPH_STATUS_INFEASIBLE;
    } else {
        if (probe_fn(probe_ctx, probe, &status) != 0) {
            lp_model_free(probe);
            return -1;
        }
        if (report) report->probes++;
    }

    lp_model_free(probe);
    *status_out = status;
    return 0;
}

static int lp_conflict_seed_rows_with_farkas(const LPModel *base,
                                             const LPConflictOptionsInternal *opts,
                                             const LPConflictMemberInternal *members,
                                             unsigned char *active,
                                             int num_members,
                                             LPConflictProbeFn probe_fn,
                                             void *probe_ctx,
                                             LPConflictReportInternal *report) {
    int seeded = 0;
    int kept_rows = 0;
    RalphStatus status = RALPH_STATUS_ERROR;

    if (!base || !opts || !members || !active || !probe_fn || num_members < 0) return -1;
    if (!opts->use_farkas_seed || !opts->farkas_valid || !opts->farkas_ray) return 0;

    for (int i = 0; i < num_members; i++) {
        if (members[i].type != LP_CONFLICT_MEMBER_ROW) continue;
        if (!active[i]) continue;

        int row = members[i].index;
        if (row < 0 || row >= base->num_cons) return -1;

        if (fabs(opts->farkas_ray[row]) <= LP_CONFLICT_FARKAS_SEED_TOL) {
            active[i] = 0;
            seeded++;
        } else {
            kept_rows++;
        }
    }

    if (seeded == 0) {
        if (report) {
            report->seeded_rows = 0;
            report->used_farkas_seed = 0;
        }
        return 0;
    }

    if (kept_rows == 0) {
        for (int i = 0; i < num_members; i++) active[i] = 1;
        if (report) {
            report->seeded_rows = 0;
            report->used_farkas_seed = 0;
        }
        return 0;
    }

    if (lp_conflict_probe_with_drop(base, members, active, num_members, -1,
                                    probe_fn, probe_ctx, &status, report) != 0) {
        return -1;
    }

    if (status != RALPH_STATUS_INFEASIBLE) {
        for (int i = 0; i < num_members; i++) active[i] = 1;
        if (report) {
            report->seeded_rows = 0;
            report->used_farkas_seed = 0;
        }
        return 0;
    }

    if (report) {
        report->seeded_rows = seeded;
        report->used_farkas_seed = 1;
    }
    return 0;
}

int lp_conflict_compute(const LPModel *model,
                        const LPConflictOptionsInternal *opts,
                        LPConflictProbeFn probe_fn,
                        void *probe_ctx,
                        LPConflictMemberInternal **members_out,
                        int *count_out,
                        LPConflictReportInternal *report_out) {
    LPConflictOptionsInternal local_opts;
    LPConflictMemberInternal *candidates = NULL;
    LPConflictMemberInternal *result = NULL;
    unsigned char *active = NULL;
    int num_candidates = 0;
    int active_count = 0;

    if (count_out) *count_out = 0;
    if (report_out) memset(report_out, 0, sizeof(*report_out));
    if (!model || !probe_fn || !members_out || !count_out) return -1;

    local_opts.include_bounds = 1;
    local_opts.use_farkas_seed = 0;
    local_opts.farkas_ray = NULL;
    local_opts.farkas_valid = 0;
    if (opts) local_opts = *opts;

    if (model->num_cons < 0 || model->num_vars < 0) return -1;

    num_candidates = model->num_cons;
    if (local_opts.include_bounds) {
        for (int j = 0; j < model->num_vars; j++) {
            if (lp_conflict_lb_finite(model->lb[j])) num_candidates++;
            if (lp_conflict_ub_finite(model->ub[j])) num_candidates++;
        }
    }
    if (num_candidates <= 0) return -1;

    candidates = (LPConflictMemberInternal*)calloc((size_t)num_candidates, sizeof(*candidates));
    active = (unsigned char*)calloc((size_t)num_candidates, sizeof(unsigned char));
    if (!candidates || !active) {
        free(candidates);
        free(active);
        return -1;
    }

    int pos = 0;
    for (int row = 0; row < model->num_cons; row++) {
        candidates[pos].type = LP_CONFLICT_MEMBER_ROW;
        candidates[pos].index = row;
        active[pos] = 1;
        pos++;
    }
    if (local_opts.include_bounds) {
        for (int var = 0; var < model->num_vars; var++) {
            if (lp_conflict_lb_finite(model->lb[var])) {
                candidates[pos].type = LP_CONFLICT_MEMBER_VAR_LB;
                candidates[pos].index = var;
                active[pos] = 1;
                pos++;
            }
        }
        for (int var = 0; var < model->num_vars; var++) {
            if (lp_conflict_ub_finite(model->ub[var])) {
                candidates[pos].type = LP_CONFLICT_MEMBER_VAR_UB;
                candidates[pos].index = var;
                active[pos] = 1;
                pos++;
            }
        }
    }
    if (pos != num_candidates) {
        free(candidates);
        free(active);
        return -1;
    }

    if (report_out) {
        report_out->initial_size = num_candidates;
        report_out->final_size = 0;
        report_out->probes = 0;
        report_out->dropped = 0;
        report_out->used_farkas_seed = 0;
        report_out->seeded_rows = 0;
    }

    if (lp_conflict_seed_rows_with_farkas(model, &local_opts, candidates, active,
                                          num_candidates, probe_fn, probe_ctx,
                                          report_out) != 0) {
        free(candidates);
        free(active);
        return -1;
    }

    for (int i = 0; i < num_candidates; i++) {
        RalphStatus status = RALPH_STATUS_ERROR;
        if (!active[i]) continue;

        if (lp_conflict_probe_with_drop(model, candidates, active, num_candidates, i,
                                        probe_fn, probe_ctx, &status, report_out) != 0) {
            free(candidates);
            free(active);
            return -1;
        }

        if (status == RALPH_STATUS_INFEASIBLE) {
            active[i] = 0;
            if (report_out) report_out->dropped++;
            continue;
        }
    }

    for (int i = 0; i < num_candidates; i++) {
        if (active[i]) active_count++;
    }
    if (active_count <= 0) {
        free(candidates);
        free(active);
        return -1;
    }

    result = (LPConflictMemberInternal*)calloc((size_t)active_count, sizeof(*result));
    if (!result) {
        free(candidates);
        free(active);
        return -1;
    }

    pos = 0;
    for (int i = 0; i < num_candidates; i++) {
        if (!active[i]) continue;
        result[pos++] = candidates[i];
    }

    for (int i = 1; i < active_count; i++) {
        LPConflictMemberInternal key = result[i];
        int j = i - 1;
        while (j >= 0 && lp_conflict_cmp_member(&result[j], &key) > 0) {
            result[j + 1] = result[j];
            j--;
        }
        result[j + 1] = key;
    }

    *members_out = result;
    *count_out = active_count;
    if (report_out) report_out->final_size = active_count;

    free(candidates);
    free(active);
    return 0;
}

void lp_conflict_free_members(LPConflictMemberInternal *members) {
    free(members);
}
