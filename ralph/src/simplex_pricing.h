/*
 * simplex_pricing.h - Pricing (entering variable selection) for the simplex method.
 *
 * Extracted from simplex.c - contains all pricing strategies and heap infrastructure.
 */

#ifndef SIMPLEX_PRICING_H
#define SIMPLEX_PRICING_H

#include "lp.h"

/* ============================================================================
 * Heap Pricing Infrastructure
 * ============================================================================ */

double heap_score(const SimplexTableau *tab, int j);
void heap_swap(SimplexTableau *tab, int a, int b);
void heap_sift_up(SimplexTableau *tab, int pos);
void heap_sift_down(SimplexTableau *tab, int pos);
void heap_build(SimplexTableau *tab);
void heap_remove(SimplexTableau *tab, int var_j);
void heap_insert(SimplexTableau *tab, int var_j);
void heap_update(SimplexTableau *tab, int var_j);

/* ============================================================================
 * Pricing Strategies
 * ============================================================================ */

int pricing_heap(SimplexTableau *tab, int *entering);
int pricing_dantzig(SimplexTableau *tab, int *entering);
int pricing_bland(SimplexTableau *tab, int *entering);
int pricing_bland_excluding(SimplexTableau *tab, int excluded_var, int *entering);
int pricing_bland_excluding_two(SimplexTableau *tab,
                                int excluded_a,
                                int excluded_b,
                                int *entering);
int pricing_bland_excluding_set(SimplexTableau *tab,
                                const int *excluded_vars,
                                int excluded_count,
                                int *entering);
int pricing_steepest_edge(SimplexTableau *tab, int *entering);
int pricing_devex(SimplexTableau *tab, int *entering);
int pricing_partial(SimplexTableau *tab, int *entering);
int pricing_devex_partial(SimplexTableau *tab, int *entering);

/* ============================================================================
 * Pricing Helpers
 * ============================================================================ */

/* Partial pricing constants */
#define PARTIAL_PRICE_BLOCK 100
#define PARTIAL_PRICE_THRESHOLD 1e-6
#define PARTIAL_HOT_ACCEPT 1e-4
#define DEVEX_PARTIAL_BLOCK 240
#define DEVEX_PARTIAL_ENABLE_M 400
#define DEVEX_PARTIAL_ENABLE_N 1200
#define DEVEX_PARTIAL_DEGEN_TRIGGER 20
#define DEVEX_PARTIAL_ITER_TRIGGER 4000
#define DEVEX_PARTIAL_FULL_RESCAN_MASK 1

int is_entering_eligible(SimplexTableau *tab, int j, double *rc_out);
void add_to_hot_set(SimplexTableau *tab, int var);
int devex_entering_eligible(SimplexTableau *tab, int j, double *score_out);

/* Adaptive devex partial pricing trigger */
int phase2_use_adaptive_devex_partial(const SimplexTableau *tab,
                                      int iter,
                                      int degenerate_count,
                                      int use_bland,
                                      int pricing_strategy);

/* Pricing dispatch: unified pricing switch used by both Phase 1 and Phase 2.
 * strategy: 0=Dantzig, 1=SE, 2=Devex, 3=Partial, 4=Heap
 * use_bland: if non-zero, override with Bland's rule
 * adaptive_devex_partial: if non-zero AND strategy==2, may use devex partial
 * iter: current iteration (used for devex partial rescan mask) */
int pricing_dispatch(SimplexTableau *tab, int strategy, int use_bland,
                     int adaptive_devex_partial, int iter, int *entering);

#endif /* SIMPLEX_PRICING_H */
