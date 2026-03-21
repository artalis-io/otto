/*
 * Ralph - Set Partitioning Helpers
 *
 * Internal exact-cover utilities used by SPP-specific heuristics and cuts.
 * This is intentionally separate from the generic MIP controller.
 */

#ifndef RALPH_SPP_H
#define RALPH_SPP_H

#include "detect.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SPPContext {
    RalphSetCoverType type;    /* Always PARTITIONING for a valid SPP context */
    int num_rows;              /* Number of exact-cover rows */
    int num_sets;              /* Number of binary set variables */
    int nnz;                   /* Total 0/1 incidences */
    int obj_sense;             /* Model objective sense */
    double obj_offset;         /* Model objective offset */

    /* Exact-cover row -> set incidence (CSR by row) */
    int *row_ptr;              /* Size num_rows + 1 */
    int *row_sets;             /* Size nnz */

    /* Set -> row incidence (CSC by set, owned copy from model) */
    int *set_ptr;              /* Size num_sets + 1 */
    int *set_rows;             /* Size nnz */

    /* Structural metadata */
    int *row_degree;           /* Size num_rows */
    int *set_size;             /* Size num_sets */
    int *row_order;            /* Sparse/forced rows first, stable */
    int *set_order;            /* Cheap/small sets first, stable */
    double *rhs;               /* Size num_rows, must be exactly 1.0 */
    double *costs;             /* Size num_sets */

    /* Conflict graph for exact-cover rows:
     * sets i and j conflict iff they appear in the same row. */
    int *conflict_ptr;         /* Size num_sets + 1 */
    int *conflict_adj;         /* Flattened adjacency */
    int num_conflict_edges;    /* Undirected edges */
} SPPContext;

typedef struct {
    int node_limit;            /* Max DFS nodes before heuristic stops */
    int nodes_visited;         /* DFS states visited */
    int forced_selections;     /* Propagation-selected sets */
    int forced_exclusions;     /* Propagation/exact-cover exclusions */
    int branch_failures;       /* Dead-end states */
    int incumbent_updates;     /* Feasible solutions improving the heuristic incumbent */
} SPPHeuristicStats;

typedef enum {
    SPP_CUT_CLIQUE = 0
} SPPCutKind;

typedef struct {
    int nnz;
    const int *indices;
    const double *values;
    char sense;
    double rhs;
    double violation;
    SPPCutKind kind;
} SPPCut;

typedef int (*SPPCutEmitFn)(void *user, const SPPCut *cut);

typedef struct {
    SPPCutEmitFn emit;
    void *user;
} SPPCutSink;

typedef struct {
    int clique_candidates;
    int clique_emitted;
    int duplicate_cliques;
    int unviolated_cliques;
} SPPCutStats;

/* Zero-initialize a context before first use. */
void spp_context_init(SPPContext *ctx);

/* Free memory owned by the context. Safe on zero-initialized contexts. */
void spp_context_free(SPPContext *ctx);

/* Build an exact-cover context from a set-cover signature.
 *
 * Contract:
 * - accepts only partitioning models with RHS exactly 1 for every row
 * - caller should initialize/free ctx before reuse
 * - returns 0 on success, -1 on unsupported structure or allocation failure
 */
int spp_context_build_from_signature(const LPModel *model,
                                     const SetCoverSignature *sig,
                                     SPPContext *ctx);

/* Detect and build an exact-cover context directly from a model. */
int spp_context_build(const LPModel *model, SPPContext *ctx);

/* Check whether two sets conflict in the exact-cover graph. */
int spp_context_sets_conflict(const SPPContext *ctx, int set_a, int set_b);

/* Verify that a binary solution satisfies exact cover. */
int spp_context_check_solution(const SPPContext *ctx, const double *x, double tol);

/* Compute model objective value for a candidate set vector. */
double spp_context_compute_objective(const SPPContext *ctx, const double *x);

/* Run a bounded exact-cover heuristic on the SPP context.
 *
 * Returns:
 *   0 on success with a feasible solution in sol_out and objective in obj_out
 *  -1 if no feasible solution was found within the node limit or on invalid input
 */
int spp_heuristic_run(const SPPContext *ctx,
                      const double *lp_x,
                      double *sol_out,
                      double *obj_out,
                      SPPHeuristicStats *stats);

/* Separate violated SPP-specific cuts from an LP relaxation vector.
 *
 * Contract:
 * - input is only the immutable exact-cover context plus LP values
 * - the sink receives one valid cut at a time
 * - returns the number of cuts accepted by the sink
 */
int spp_separate(const SPPContext *ctx,
                 const double *lp_x,
                 const SPPCutSink *sink,
                 SPPCutStats *stats);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_SPP_H */
