/*
 * Ralph - Internal LP conflict/IIS refinement helpers
 */

#ifndef RALPH_LP_CONFLICT_H
#define RALPH_LP_CONFLICT_H

#include "lp.h"

typedef enum {
    LP_CONFLICT_MEMBER_ROW = 0,
    LP_CONFLICT_MEMBER_VAR_LB = 1,
    LP_CONFLICT_MEMBER_VAR_UB = 2
} LPConflictMemberTypeInternal;

typedef struct {
    LPConflictMemberTypeInternal type;
    int index;
} LPConflictMemberInternal;

typedef struct {
    int include_bounds;      /* 1 = include finite var bounds in candidate set */
    int use_farkas_seed;     /* 1 = prune row candidates by Farkas support first */
    const double *farkas_ray;/* Size num_cons when farkas_valid=1 */
    int farkas_valid;        /* 1 if farkas_ray is valid for the current infeasible solve */
} LPConflictOptionsInternal;

typedef struct {
    int probes;              /* Number of LP probes executed */
    int dropped;             /* Number of candidates removed by deletion filter */
    int initial_size;        /* Candidates before optional seeding */
    int final_size;          /* Final irreducible conflict size */
    int used_farkas_seed;    /* 1 if seed pruning was accepted */
    int seeded_rows;         /* Row candidates removed by accepted seeding */
} LPConflictReportInternal;

typedef int (*LPConflictProbeFn)(void *ctx, LPModel *probe_model, RalphStatus *status_out);

int lp_conflict_compute(const LPModel *model,
                        const LPConflictOptionsInternal *opts,
                        LPConflictProbeFn probe_fn,
                        void *probe_ctx,
                        LPConflictMemberInternal **members_out,
                        int *count_out,
                        LPConflictReportInternal *report_out);

void lp_conflict_free_members(LPConflictMemberInternal *members);

#endif /* RALPH_LP_CONFLICT_H */
