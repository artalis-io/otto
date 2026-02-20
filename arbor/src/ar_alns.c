#include "ar_alns.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "sh_dist.h"

#define AR_MIN_WEIGHT 0.01
#define AR_DEFAULT_OP_CAPACITY 8

typedef struct {
    char name[32];
    ARDestroyOp op;
    void *op_ctx;
    double weight;
    double segment_score;
    int segment_uses;
    ARALNSOperatorStats stats;
} ARDestroyEntry;

typedef struct {
    char name[32];
    ARRepairOp op;
    void *op_ctx;
    double weight;
    double segment_score;
    int segment_uses;
    ARALNSOperatorStats stats;
} ARRepairEntry;

struct ARALNSContext {
    ARALNSParams params;
    ARSolutionOps ops;
    void *solver_ctx;

    SHRng *rng;

    ARDestroyEntry *destroy_ops;
    int num_destroy;
    int cap_destroy;

    ARRepairEntry *repair_ops;
    int num_repair;
    int cap_repair;

    ARALNSStats stats;
    void *best_solution;
};

static double ar_now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + ((double)tv.tv_usec / 1000000.0);
}

static void ar_copy_name(char dst[32], const char *src) {
    if (!dst || !src) {
        return;
    }
    strncpy(dst, src, 31);
    dst[31] = '\0';
}

static double ar_rng_double(ARALNSContext *ctx) {
    return sh_rng_uniform(ctx->rng);
}

static int ar_rng_int(ARALNSContext *ctx, int min_val, int max_val) {
    return sh_rng_int_range(ctx->rng, min_val, max_val);
}

static int ar_roulette_destroy(ARALNSContext *ctx) {
    double sum = 0.0;
    int i;

    for (i = 0; i < ctx->num_destroy; i++) {
        if (ctx->destroy_ops[i].weight > 0.0) {
            sum += ctx->destroy_ops[i].weight;
        }
    }

    if (sum <= 0.0) {
        return ar_rng_int(ctx, 0, ctx->num_destroy - 1);
    }

    {
        double draw = ar_rng_double(ctx) * sum;
        double cumulative = 0.0;
        for (i = 0; i < ctx->num_destroy; i++) {
            if (ctx->destroy_ops[i].weight > 0.0) {
                cumulative += ctx->destroy_ops[i].weight;
            }
            if (draw <= cumulative) {
                return i;
            }
        }
    }

    return ctx->num_destroy - 1;
}

static int ar_roulette_repair(ARALNSContext *ctx) {
    double sum = 0.0;
    int i;

    for (i = 0; i < ctx->num_repair; i++) {
        if (ctx->repair_ops[i].weight > 0.0) {
            sum += ctx->repair_ops[i].weight;
        }
    }

    if (sum <= 0.0) {
        return ar_rng_int(ctx, 0, ctx->num_repair - 1);
    }

    {
        double draw = ar_rng_double(ctx) * sum;
        double cumulative = 0.0;
        for (i = 0; i < ctx->num_repair; i++) {
            if (ctx->repair_ops[i].weight > 0.0) {
                cumulative += ctx->repair_ops[i].weight;
            }
            if (draw <= cumulative) {
                return i;
            }
        }
    }

    return ctx->num_repair - 1;
}

static void ar_update_weights(ARALNSContext *ctx) {
    int i;
    double reaction = ctx->params.reaction_factor;

    if (reaction < 0.0) {
        reaction = 0.0;
    }
    if (reaction > 1.0) {
        reaction = 1.0;
    }

    for (i = 0; i < ctx->num_destroy; i++) {
        ARDestroyEntry *entry = &ctx->destroy_ops[i];
        if (entry->segment_uses > 0) {
            double avg_reward = entry->segment_score / (double)entry->segment_uses;
            entry->weight = (1.0 - reaction) * entry->weight + reaction * avg_reward;
            if (entry->weight < AR_MIN_WEIGHT) {
                entry->weight = AR_MIN_WEIGHT;
            }
        }
        entry->segment_score = 0.0;
        entry->segment_uses = 0;
        entry->stats.weight = entry->weight;
    }

    for (i = 0; i < ctx->num_repair; i++) {
        ARRepairEntry *entry = &ctx->repair_ops[i];
        if (entry->segment_uses > 0) {
            double avg_reward = entry->segment_score / (double)entry->segment_uses;
            entry->weight = (1.0 - reaction) * entry->weight + reaction * avg_reward;
            if (entry->weight < AR_MIN_WEIGHT) {
                entry->weight = AR_MIN_WEIGHT;
            }
        }
        entry->segment_score = 0.0;
        entry->segment_uses = 0;
        entry->stats.weight = entry->weight;
    }
}

static int ar_accept_candidate(ARALNSContext *ctx, double delta,
                               double candidate_cost, double best_cost,
                               double *temperature, double *water_level) {
    if (delta < 0.0) {
        return 1;
    }

    switch (ctx->params.accept_type) {
        case AR_ACCEPT_SA:
            if (*temperature <= 1e-12) {
                return 0;
            }
            return ar_rng_double(ctx) < exp(-delta / (*temperature));

        case AR_ACCEPT_RRT:
            return candidate_cost <= (best_cost + ctx->params.threshold);

        case AR_ACCEPT_GD:
            return candidate_cost <= *water_level;

        case AR_ACCEPT_IMPROVING:
        default:
            return 0;
    }
}

static int ar_validate_solution_ops(const ARSolutionOps *ops) {
    return ops && ops->copy && ops->free && ops->cost;
}

static int ar_validate_params(const ARALNSParams *params) {
    if (!params) {
        return 0;
    }

    if (params->max_iterations <= 0) {
        return 0;
    }
    if (params->max_time_seconds < 0) {
        return 0;
    }
    if (params->max_stagnation_iterations < 0) {
        return 0;
    }
    if (params->segment_size <= 0) {
        return 0;
    }
    if (params->q_min < 0 || params->q_max < 0 || params->q_max < params->q_min) {
        return 0;
    }
    if (params->reaction_factor < 0.0 || params->reaction_factor > 1.0) {
        return 0;
    }
    if (params->restart_threshold < 0) {
        return 0;
    }
    if (params->restart_temp_ratio < 0.0 || params->restart_temp_ratio > 1.0) {
        return 0;
    }

    switch (params->accept_type) {
        case AR_ACCEPT_SA:
            if (params->initial_temp <= 0.0) {
                return 0;
            }
            if (params->cooling_rate <= 0.0 || params->cooling_rate > 1.0) {
                return 0;
            }
            break;

        case AR_ACCEPT_RRT:
            if (params->threshold < 0.0) {
                return 0;
            }
            break;

        case AR_ACCEPT_GD:
            if (params->gd_decay <= 0.0 || params->gd_decay > 1.0) {
                return 0;
            }
            break;

        case AR_ACCEPT_IMPROVING:
            break;

        default:
            return 0;
    }

    return 1;
}

static ARStatus ar_grow_destroy_ops(ARALNSContext *ctx) {
    int new_capacity = ctx->cap_destroy > 0
                       ? ctx->cap_destroy * 2
                       : AR_DEFAULT_OP_CAPACITY;
    ARDestroyEntry *new_ops = (ARDestroyEntry *)realloc(
        ctx->destroy_ops, (size_t)new_capacity * sizeof(ARDestroyEntry));

    if (!new_ops) {
        return AR_STATUS_OUT_OF_MEMORY;
    }

    ctx->destroy_ops = new_ops;
    ctx->cap_destroy = new_capacity;
    return AR_STATUS_OK;
}

static ARStatus ar_grow_repair_ops(ARALNSContext *ctx) {
    int new_capacity = ctx->cap_repair > 0
                       ? ctx->cap_repair * 2
                       : AR_DEFAULT_OP_CAPACITY;
    ARRepairEntry *new_ops = (ARRepairEntry *)realloc(
        ctx->repair_ops, (size_t)new_capacity * sizeof(ARRepairEntry));

    if (!new_ops) {
        return AR_STATUS_OUT_OF_MEMORY;
    }

    ctx->repair_ops = new_ops;
    ctx->cap_repair = new_capacity;
    return AR_STATUS_OK;
}

void ar_alns_params_default(ARALNSParams *params) {
    if (!params) {
        return;
    }

    memset(params, 0, sizeof(*params));
    params->max_iterations = 1000;
    params->max_time_seconds = 0;
    params->max_stagnation_iterations = 0;
    params->segment_size = 100;
    params->q_min = 4;
    params->q_max = 20;
    params->accept_type = AR_ACCEPT_SA;
    params->initial_temp = 100.0;
    params->cooling_rate = 0.995;
    params->threshold = 0.0;
    params->target_cost = -INFINITY;
    params->water_level = 0.0;
    params->gd_decay = 0.999;
    params->reaction_factor = 0.1;
    params->reward_best = 8.0;
    params->reward_better = 4.0;
    params->reward_accepted = 2.0;
    params->reward_rejected = 0.5;
    params->restart_threshold = 0;
    params->restart_temp_ratio = 0.5;
}

ARALNSContext *ar_alns_create(const ARALNSParams *params,
                              const ARSolutionOps *ops,
                              void *solver_ctx) {
    ARALNSContext *ctx;

    if (!ar_validate_params(params) || !ar_validate_solution_ops(ops)) {
        return NULL;
    }

    ctx = (ARALNSContext *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->params = *params;
    ctx->ops = *ops;
    ctx->solver_ctx = solver_ctx;
    ctx->rng = sh_rng_create_default();
    if (!ctx->rng) {
        free(ctx);
        return NULL;
    }
    sh_rng_seed(ctx->rng, 0x243F6A8885A308D3ULL);

    return ctx;
}

void ar_alns_free(ARALNSContext *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->best_solution) {
        ctx->ops.free(ctx->best_solution, ctx->ops.user_ctx);
        ctx->best_solution = NULL;
    }

    sh_rng_free(ctx->rng);
    ctx->rng = NULL;

    free(ctx->destroy_ops);
    free(ctx->repair_ops);
    free(ctx);
}

ARStatus ar_alns_add_destroy(ARALNSContext *ctx, const char *name,
                             ARDestroyOp op, void *op_ctx,
                             double initial_weight) {
    ARDestroyEntry *entry;

    if (!ctx || !name || !op) {
        return AR_STATUS_INVALID_ARG;
    }

    if (ctx->num_destroy == ctx->cap_destroy) {
        ARStatus status = ar_grow_destroy_ops(ctx);
        if (status != AR_STATUS_OK) {
            return status;
        }
    }

    entry = &ctx->destroy_ops[ctx->num_destroy++];
    memset(entry, 0, sizeof(*entry));
    ar_copy_name(entry->name, name);
    ar_copy_name(entry->stats.name, name);
    entry->op = op;
    entry->op_ctx = op_ctx;
    entry->weight = initial_weight > 0.0 ? initial_weight : 1.0;
    entry->stats.weight = entry->weight;
    return AR_STATUS_OK;
}

ARStatus ar_alns_add_repair(ARALNSContext *ctx, const char *name,
                            ARRepairOp op, void *op_ctx,
                            double initial_weight) {
    ARRepairEntry *entry;

    if (!ctx || !name || !op) {
        return AR_STATUS_INVALID_ARG;
    }

    if (ctx->num_repair == ctx->cap_repair) {
        ARStatus status = ar_grow_repair_ops(ctx);
        if (status != AR_STATUS_OK) {
            return status;
        }
    }

    entry = &ctx->repair_ops[ctx->num_repair++];
    memset(entry, 0, sizeof(*entry));
    ar_copy_name(entry->name, name);
    ar_copy_name(entry->stats.name, name);
    entry->op = op;
    entry->op_ctx = op_ctx;
    entry->weight = initial_weight > 0.0 ? initial_weight : 1.0;
    entry->stats.weight = entry->weight;
    return AR_STATUS_OK;
}

ARStatus ar_alns_set_seed(ARALNSContext *ctx, uint64_t seed) {
    if (!ctx || !ctx->rng) {
        return AR_STATUS_INVALID_ARG;
    }
    sh_rng_seed(ctx->rng, seed == 0 ? 0xA4093822299F31D0ULL : seed);
    return AR_STATUS_OK;
}

ARStatus ar_alns_solve(ARALNSContext *ctx, const void *initial_solution,
                       void **best_solution_out) {
    ARStatus status = AR_STATUS_OK;
    double start_time;
    double temperature;
    double water_level;
    void *current;
    void *best;
    double current_cost;
    double best_cost;
    int iteration;
    int iterations_done = 0;
    int stagnation_iterations = 0;
    int segment_size;
    int fatal_error = 0;
    ARStopReason stop_reason = AR_STOP_MAX_ITERATIONS;

    if (!ctx || !initial_solution) {
        return AR_STATUS_INVALID_ARG;
    }
    if (ctx->num_destroy == 0 || ctx->num_repair == 0) {
        return AR_STATUS_NO_OPERATORS;
    }

    if (best_solution_out) {
        *best_solution_out = NULL;
    }

    memset(&ctx->stats, 0, sizeof(ctx->stats));

    current = ctx->ops.copy(initial_solution, ctx->ops.user_ctx);
    best = ctx->ops.copy(initial_solution, ctx->ops.user_ctx);
    if (!current || !best) {
        if (current) {
            ctx->ops.free(current, ctx->ops.user_ctx);
        }
        if (best) {
            ctx->ops.free(best, ctx->ops.user_ctx);
        }
        return AR_STATUS_OUT_OF_MEMORY;
    }

    current_cost = ctx->ops.cost(current, ctx->ops.user_ctx);
    best_cost = current_cost;
    if (!isfinite(current_cost)) {
        ctx->ops.free(current, ctx->ops.user_ctx);
        ctx->ops.free(best, ctx->ops.user_ctx);
        return AR_STATUS_INVALID_ARG;
    }

    temperature = ctx->params.initial_temp > 0.0 ? ctx->params.initial_temp : 1.0;
    water_level = ctx->params.water_level > 0.0 ? ctx->params.water_level : current_cost;
    segment_size = ctx->params.segment_size > 0 ? ctx->params.segment_size : 1;

    start_time = ar_now_seconds();

    for (iteration = 0; iteration < ctx->params.max_iterations; iteration++) {
        int d_idx;
        int r_idx;
        void *candidate;
        int solution_size = 0;
        int q = 0;
        uint32_t *removed_ids = NULL;
        int removed_count = 0;
        ARStatus op_status;
        double candidate_cost = current_cost;
        double delta = 0.0;
        int accepted = 0;
        double reward = ctx->params.reward_rejected;

        if (ctx->params.max_time_seconds > 0) {
            double elapsed = ar_now_seconds() - start_time;
            if (elapsed >= (double)ctx->params.max_time_seconds) {
                status = AR_STATUS_LIMIT;
                stop_reason = AR_STOP_TIME_LIMIT;
                break;
            }
        }

        if (ctx->params.restart_threshold > 0 &&
            stagnation_iterations >= ctx->params.restart_threshold) {
            /* Restart from best solution with reheated temperature */
            void *restart_copy = ctx->ops.copy(best, ctx->ops.user_ctx);
            if (restart_copy) {
                ctx->ops.free(current, ctx->ops.user_ctx);
                current = restart_copy;
                current_cost = best_cost;
                if (ctx->params.accept_type == AR_ACCEPT_SA &&
                    ctx->params.restart_temp_ratio > 0.0) {
                    temperature = ctx->params.initial_temp *
                                  ctx->params.restart_temp_ratio;
                }
                stagnation_iterations = 0;
                ctx->stats.restarts++;
            }
        }

        if (ctx->params.max_stagnation_iterations > 0 &&
            stagnation_iterations >= ctx->params.max_stagnation_iterations) {
            status = AR_STATUS_LIMIT;
            stop_reason = AR_STOP_STAGNATION;
            break;
        }
        if (best_cost <= ctx->params.target_cost) {
            stop_reason = AR_STOP_TARGET_COST;
            break;
        }

        d_idx = ar_roulette_destroy(ctx);
        r_idx = ar_roulette_repair(ctx);

        ctx->destroy_ops[d_idx].stats.selected++;
        ctx->repair_ops[r_idx].stats.selected++;

        candidate = ctx->ops.copy(current, ctx->ops.user_ctx);
        if (!candidate) {
            status = AR_STATUS_OUT_OF_MEMORY;
            fatal_error = 1;
            stop_reason = AR_STOP_ERROR;
            break;
        }

        if (ctx->ops.size) {
            solution_size = ctx->ops.size(candidate, ctx->ops.user_ctx);
            if (solution_size < 0) {
                solution_size = 0;
            }
        }

        if (solution_size > 0) {
            int q_min = ctx->params.q_min;
            int q_max = ctx->params.q_max;
            if (q_min < 0) {
                q_min = 0;
            }
            if (q_max < q_min) {
                q_max = q_min;
            }
            if (q_max > solution_size) {
                q_max = solution_size;
            }
            if (q_min > q_max) {
                q_min = q_max;
            }
            q = ar_rng_int(ctx, q_min, q_max);
        }

        if (q > 0) {
            removed_ids = (uint32_t *)calloc((size_t)q, sizeof(uint32_t));
            if (!removed_ids) {
                ctx->ops.free(candidate, ctx->ops.user_ctx);
                status = AR_STATUS_OUT_OF_MEMORY;
                fatal_error = 1;
                stop_reason = AR_STOP_ERROR;
                break;
            }
        }

        op_status = ctx->destroy_ops[d_idx].op(
            ctx->destroy_ops[d_idx].op_ctx, candidate, q, removed_ids, &removed_count);
        if (op_status == AR_STATUS_OK) {
            if (removed_count < 0) {
                removed_count = 0;
            }
            if (removed_count > q) {
                removed_count = q;
            }
            op_status = ctx->repair_ops[r_idx].op(
                ctx->repair_ops[r_idx].op_ctx, candidate, removed_ids, removed_count);
        }

        free(removed_ids);
        removed_ids = NULL;

        if (op_status == AR_STATUS_OK &&
            (!ctx->ops.validate || ctx->ops.validate(candidate, ctx->ops.user_ctx))) {
            candidate_cost = ctx->ops.cost(candidate, ctx->ops.user_ctx);
            if (isfinite(candidate_cost)) {
                delta = candidate_cost - current_cost;
                accepted = ar_accept_candidate(ctx, delta, candidate_cost, best_cost,
                                               &temperature, &water_level);
            } else {
                ctx->stats.invalid_candidates++;
                accepted = 0;
            }
        } else {
            ctx->stats.invalid_candidates++;
            accepted = 0;
        }

        if (accepted) {
            if (delta < 0.0) {
                reward = ctx->params.reward_better;
            } else {
                reward = ctx->params.reward_accepted;
            }

            ctx->destroy_ops[d_idx].stats.accepted++;
            ctx->repair_ops[r_idx].stats.accepted++;
            ctx->stats.accepted++;

            ctx->ops.free(current, ctx->ops.user_ctx);
            current = candidate;
            candidate = NULL;
            current_cost = candidate_cost;

            if (current_cost < best_cost) {
                void *best_copy = ctx->ops.copy(current, ctx->ops.user_ctx);
                if (!best_copy) {
                    status = AR_STATUS_OUT_OF_MEMORY;
                    fatal_error = 1;
                    stop_reason = AR_STOP_ERROR;
                    break;
                }
                ctx->ops.free(best, ctx->ops.user_ctx);
                best = best_copy;
                best_cost = current_cost;
                reward = ctx->params.reward_best;
                ctx->stats.improvements++;
                ctx->destroy_ops[d_idx].stats.improvements++;
                ctx->repair_ops[r_idx].stats.improvements++;
                stagnation_iterations = 0;
            } else {
                stagnation_iterations++;
            }
        } else {
            ctx->stats.rejected++;
            stagnation_iterations++;
        }

        if (candidate) {
            ctx->ops.free(candidate, ctx->ops.user_ctx);
            candidate = NULL;
        }

        ctx->destroy_ops[d_idx].segment_score += reward;
        ctx->destroy_ops[d_idx].segment_uses++;
        ctx->repair_ops[r_idx].segment_score += reward;
        ctx->repair_ops[r_idx].segment_uses++;

        if (ctx->params.accept_type == AR_ACCEPT_SA) {
            temperature *= ctx->params.cooling_rate > 0.0
                           ? ctx->params.cooling_rate
                           : 0.995;
            if (temperature < 1e-12) {
                temperature = 1e-12;
            }
        } else if (ctx->params.accept_type == AR_ACCEPT_GD) {
            water_level *= ctx->params.gd_decay > 0.0 ? ctx->params.gd_decay : 0.999;
        }

        if (((iteration + 1) % segment_size) == 0) {
            ar_update_weights(ctx);
        }
        iterations_done++;
    }

    if (!fatal_error) {
        if ((iteration % segment_size) != 0) {
            ar_update_weights(ctx);
        }

        ctx->stats.iterations = iterations_done;
        ctx->stats.best_cost = best_cost;
        ctx->stats.elapsed_seconds = ar_now_seconds() - start_time;
        ctx->stats.stop_reason = stop_reason;

        if (ctx->best_solution) {
            ctx->ops.free(ctx->best_solution, ctx->ops.user_ctx);
            ctx->best_solution = NULL;
        }
        ctx->best_solution = best;

        if (best_solution_out) {
            *best_solution_out = ctx->ops.copy(ctx->best_solution, ctx->ops.user_ctx);
            if (!*best_solution_out) {
                status = AR_STATUS_OUT_OF_MEMORY;
            }
        }
    } else {
        ctx->stats.stop_reason = AR_STOP_ERROR;
        ctx->ops.free(best, ctx->ops.user_ctx);
    }

    if (current) {
        ctx->ops.free(current, ctx->ops.user_ctx);
    }

    return status;
}

ARStatus ar_alns_get_best_copy(const ARALNSContext *ctx, void **best_solution_out) {
    if (!ctx || !best_solution_out) {
        return AR_STATUS_INVALID_ARG;
    }
    if (!ctx->best_solution) {
        *best_solution_out = NULL;
        return AR_STATUS_OK;
    }

    *best_solution_out = ctx->ops.copy(ctx->best_solution, ctx->ops.user_ctx);
    if (!*best_solution_out) {
        return AR_STATUS_OUT_OF_MEMORY;
    }
    return AR_STATUS_OK;
}

void ar_alns_get_stats(const ARALNSContext *ctx, ARALNSStats *out_stats) {
    if (!ctx || !out_stats) {
        return;
    }
    *out_stats = ctx->stats;
}

int ar_alns_destroy_count(const ARALNSContext *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->num_destroy;
}

int ar_alns_repair_count(const ARALNSContext *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->num_repair;
}

ARStatus ar_alns_get_destroy_stats(const ARALNSContext *ctx, int index,
                                   ARALNSOperatorStats *out_stats) {
    if (!ctx || !out_stats || index < 0 || index >= ctx->num_destroy) {
        return AR_STATUS_INVALID_ARG;
    }
    *out_stats = ctx->destroy_ops[index].stats;
    return AR_STATUS_OK;
}

ARStatus ar_alns_get_repair_stats(const ARALNSContext *ctx, int index,
                                  ARALNSOperatorStats *out_stats) {
    if (!ctx || !out_stats || index < 0 || index >= ctx->num_repair) {
        return AR_STATUS_INVALID_ARG;
    }
    *out_stats = ctx->repair_ops[index].stats;
    return AR_STATUS_OK;
}

void ar_alns_calibrate_sa(ARALNSParams *params, double initial_cost,
                           int max_iterations) {
    double abs_cost;
    double t0;

    if (!params || max_iterations <= 0) {
        return;
    }

    abs_cost = fabs(initial_cost);
    if (abs_cost < 1e-12) {
        abs_cost = 1.0;
    }

    /* T0 set so a 5%-worse solution is accepted with ~50% probability.
       Derivation: P(accept) = exp(-delta/T) = 0.5
       => T = delta / ln(2) = 0.05 * cost / ln(2) */
    t0 = 0.05 * abs_cost / 0.693147180559945;

    /* cooling_rate chosen so temperature drops to 0.1% of T0 after
       max_iterations: T0 * cr^N = 0.001 * T0
       => cr = exp(ln(0.001) / N) = exp(-6.9078 / N) */
    params->accept_type = AR_ACCEPT_SA;
    params->initial_temp = t0;
    params->cooling_rate = exp(-6.907755278982137 / (double)max_iterations);
}
