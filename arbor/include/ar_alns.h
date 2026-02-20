#ifndef ARBOR_AR_ALNS_H
#define ARBOR_AR_ALNS_H

#include <stdint.h>

#include "ar_types.h"

typedef struct ARALNSContext ARALNSContext;

typedef ARStatus (*ARDestroyOp)(void *op_ctx, void *solution, int count,
                                uint32_t *removed_ids, int *removed_count);
typedef ARStatus (*ARRepairOp)(void *op_ctx, void *solution,
                               const uint32_t *removed_ids, int removed_count);

typedef void *(*ARSolutionCopyFn)(const void *solution, void *user_ctx);
typedef void (*ARSolutionFreeFn)(void *solution, void *user_ctx);
typedef double (*ARSolutionCostFn)(const void *solution, void *user_ctx);
typedef int (*ARSolutionSizeFn)(const void *solution, void *user_ctx);
typedef int (*ARSolutionValidateFn)(const void *solution, void *user_ctx);

typedef struct {
    ARSolutionCopyFn copy;
    ARSolutionFreeFn free;
    ARSolutionCostFn cost;
    ARSolutionSizeFn size;
    ARSolutionValidateFn validate;
    void *user_ctx;
} ARSolutionOps;

void ar_alns_params_default(ARALNSParams *params);

ARALNSContext *ar_alns_create(const ARALNSParams *params,
                              const ARSolutionOps *ops,
                              void *solver_ctx);
void ar_alns_free(ARALNSContext *ctx);

ARStatus ar_alns_add_destroy(ARALNSContext *ctx, const char *name,
                             ARDestroyOp op, void *op_ctx,
                             double initial_weight);
ARStatus ar_alns_add_repair(ARALNSContext *ctx, const char *name,
                            ARRepairOp op, void *op_ctx,
                            double initial_weight);

ARStatus ar_alns_set_seed(ARALNSContext *ctx, uint64_t seed);

ARStatus ar_alns_solve(ARALNSContext *ctx, const void *initial_solution,
                       void **best_solution_out);
ARStatus ar_alns_get_best_copy(const ARALNSContext *ctx, void **best_solution_out);

void ar_alns_get_stats(const ARALNSContext *ctx, ARALNSStats *out_stats);
int ar_alns_destroy_count(const ARALNSContext *ctx);
int ar_alns_repair_count(const ARALNSContext *ctx);

ARStatus ar_alns_get_destroy_stats(const ARALNSContext *ctx, int index,
                                   ARALNSOperatorStats *out_stats);
ARStatus ar_alns_get_repair_stats(const ARALNSContext *ctx, int index,
                                  ARALNSOperatorStats *out_stats);

void ar_alns_calibrate_sa(ARALNSParams *params, double initial_cost, int max_iterations);

#endif /* ARBOR_AR_ALNS_H */
