/*
 * simplex_pricing.c - Pricing (entering variable selection) for the simplex method.
 *
 * Extracted from simplex.c - contains all pricing strategies, heap infrastructure,
 * and the unified pricing dispatch function.
 */

#include <stdlib.h>
#include <math.h>
#include "lp.h"
#include "simplex_internal.h"
#include "simplex_pricing.h"

/* ============================================================================
 * Heap Pricing Infrastructure (T2.2)
 *
 * Binary max-heap of non-basic variable indices, keyed by improvement score.
 * Score = effective Dantzig improvement considering variable status:
 *   NONBASIC_LOWER with rc < 0: score = -rc
 *   NONBASIC_UPPER with rc > 0: score = +rc
 *   NONBASIC_FREE:              score = |rc|
 *   Otherwise (wrong sign):     score = 0 (sinks to bottom)
 * This eliminates stale entries: ineligible variables have score 0.
 * Maintained incrementally during RC updates in simplex_pivot().
 * ============================================================================ */

double heap_score(const SimplexTableau *tab, int j) {
    if (simplex_smcp_excl_skip_var(tab, j)) return 0.0;
    double rc = tab->rc[j];
    VarStatus st = tab->var_status[j];
    if (st == RALPH_NONBASIC_LOWER && rc < 0) return -rc;
    if (st == RALPH_NONBASIC_UPPER && rc > 0) return rc;
    if (st == RALPH_NONBASIC_FREE) return fabs(rc);
    return 0.0;
}

void heap_swap(SimplexTableau *tab, int a, int b) {
    int va = tab->heap[a], vb = tab->heap[b];
    tab->heap[a] = vb;
    tab->heap[b] = va;
    tab->heap_pos[va] = b;
    tab->heap_pos[vb] = a;
}

void heap_sift_up(SimplexTableau *tab, int pos) {
    const int *heap = tab->heap;
    while (pos > 0) {
        int parent = (pos - 1) >> 1;
        if (heap_score(tab, heap[pos]) > heap_score(tab, heap[parent])) {
            heap_swap(tab, pos, parent);
            pos = parent;
        } else {
            break;
        }
    }
}

void heap_sift_down(SimplexTableau *tab, int pos) {
    const int *heap = tab->heap;
    int size = tab->heap_size;
    for (;;) {
        int best = pos;
        int left = 2 * pos + 1;
        int right = left + 1;
        if (left < size && heap_score(tab, heap[left]) > heap_score(tab, heap[best]))
            best = left;
        if (right < size && heap_score(tab, heap[right]) > heap_score(tab, heap[best]))
            best = right;
        if (best == pos) break;
        heap_swap(tab, pos, best);
        pos = best;
    }
}

void heap_build(SimplexTableau *tab) {
    tab->heap_size = 0;
    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) {
            tab->heap_pos[j] = -1;
        } else {
            tab->heap_pos[j] = tab->heap_size;
            tab->heap[tab->heap_size++] = j;
        }
    }
    /* Bottom-up heapify in O(n) */
    for (int i = (tab->heap_size >> 1) - 1; i >= 0; i--) {
        heap_sift_down(tab, i);
    }
}

void heap_remove(SimplexTableau *tab, int var_j) {
    int pos = tab->heap_pos[var_j];
    if (pos < 0) return;  /* not in heap */
    tab->heap_pos[var_j] = -1;
    int last = --tab->heap_size;
    if (pos == last) return;  /* was last element */
    int moved = tab->heap[last];
    tab->heap[pos] = moved;
    tab->heap_pos[moved] = pos;
    /* Sift in the correct direction */
    if (pos > 0 && heap_score(tab, moved) > heap_score(tab, tab->heap[(pos - 1) >> 1])) {
        heap_sift_up(tab, pos);
    } else {
        heap_sift_down(tab, pos);
    }
}

void heap_insert(SimplexTableau *tab, int var_j) {
    int pos = tab->heap_size++;
    tab->heap[pos] = var_j;
    tab->heap_pos[var_j] = pos;
    heap_sift_up(tab, pos);
}

void heap_update(SimplexTableau *tab, int var_j) {
    int pos = tab->heap_pos[var_j];
    if (pos < 0) return;  /* basic var, not in heap */
    /* Sift up or down based on new score */
    if (pos > 0 && heap_score(tab, var_j) > heap_score(tab, tab->heap[(pos - 1) >> 1])) {
        heap_sift_up(tab, pos);
    } else {
        heap_sift_down(tab, pos);
    }
}

/* Heap-based Dantzig pricing: O(1) extraction of max-improvement variable.
 * Ineligible variables have score 0 and naturally sit at the bottom. */
int pricing_heap(SimplexTableau *tab, int *entering) {
    /* Lazy rebuild: heap was invalidated by full RC recomputation */
    if (tab->heap_size == 0 && tab->rc_all_valid) heap_build(tab);
    /* Pop ineligible entries (safety net for status changes missed by
     * incremental maintenance, e.g. bound flips in ratio test). */
    while (tab->heap_size > 0) {
        int j = tab->heap[0];
        double sc = heap_score(tab, j);
        if (sc >= RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        }
        /* Ineligible root — remove and try next */
        heap_remove(tab, j);
    }
    return 1;  /* optimal */
}

/* ============================================================================
 * Pricing (Entering Variable Selection)
 * ============================================================================ */

int pricing_dantzig(SimplexTableau *tab, int *entering) {
    /* Standard Dantzig pricing: most negative reduced cost */
    double best_rc = -RALPH_OPT_TOL;
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        double rc = tab->rc[j];

        /* Check if this variable can improve */
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < best_rc) {
            best_rc = rc;
            *entering = j;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && -rc < best_rc) {
            best_rc = -rc;
            *entering = j;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > -best_rc) {
            best_rc = -fabs(rc);
            *entering = j;
        }
    }

    return (*entering >= 0) ? 0 : 1;  /* 1 = optimal */
}

/* Bland's rule pricing: choose smallest index among eligible variables.
 * Used as fallback when cycling is detected. */
int pricing_bland(SimplexTableau *tab, int *entering) {
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        double rc = tab->rc[j];

        /* Check if this variable can improve */
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            *entering = j;
            return 0;  /* Return first eligible */
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        }
    }

    return 1;  /* 1 = optimal */
}

/* Bland-style pricing with one excluded variable index. */
int pricing_bland_excluding(SimplexTableau *tab, int excluded_var, int *entering) {
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (j == excluded_var) continue;
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        double rc = tab->rc[j];

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        }
    }

    return 1;
}

int pricing_bland_excluding_two(SimplexTableau *tab,
                                int excluded_a,
                                int excluded_b,
                                int *entering) {
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (j == excluded_a || j == excluded_b) continue;
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        double rc = tab->rc[j];

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        }
    }

    return 1;
}

int pricing_steepest_edge(SimplexTableau *tab, int *entering) {
    /* Steepest edge pricing: max |rc_j| / sqrt(gamma_j)
     * Uses exact weights updated with the formula:
     *   gamma_j = ||B^{-1} * a_j||^2
     *
     * Optimization: Compare rc²/weight instead of |rc|/sqrt(weight)
     * to eliminate expensive sqrt() calls. Mathematically equivalent:
     *   |rc|/sqrt(w) > t  ⟺  rc²/w > t²
     */
    double best_ratio_sq = RALPH_OPT_TOL * RALPH_OPT_TOL;  /* Squared threshold */
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        double rc = tab->rc[j];
        double weight = tab->se_weights[j];
        if (weight < 1e-10) weight = 1.0;

        double ratio_sq = 0.0;

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            ratio_sq = (rc * rc) / weight;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            ratio_sq = (rc * rc) / weight;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            ratio_sq = (rc * rc) / weight;
        }

        if (ratio_sq > best_ratio_sq) {
            best_ratio_sq = ratio_sq;
            *entering = j;
        }
    }

    return (*entering >= 0) ? 0 : 1;
}

int pricing_devex(SimplexTableau *tab, int *entering) {
    /* Devex pricing: max |rc_j|² / gamma_j
     *
     * Uses approximate steepest edge weights with periodic reset.
     * Reference: Harris, "Pivot Selection Methods of the Devex LP Code", 1973
     */
    double best_ratio = RALPH_OPT_TOL * RALPH_OPT_TOL;  /* Squared tolerance */
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        double rc = tab->rc[j];
        double weight = tab->se_weights[j];
        if (weight < 1.0) weight = 1.0;  /* Devex weights are always >= 1 */

        double ratio = 0.0;

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            ratio = (rc * rc) / weight;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            ratio = (rc * rc) / weight;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            ratio = (rc * rc) / weight;
        }

        if (ratio > best_ratio) {
            best_ratio = ratio;
            *entering = j;
        }
    }

    return (*entering >= 0) ? 0 : 1;
}

/* ============================================================================
 * Partial Pricing
 * ============================================================================ */

/* Check if variable j is eligible for entering */
int is_entering_eligible(SimplexTableau *tab, int j, double *rc_out) {
    if (j < 0 || j >= tab->n) return 0;  /* Bounds check */
    if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) return 0;

    /* Use lazy RC computation - computes on demand if not already cached */
    double rc = tableau_get_rc(tab, j);
    *rc_out = rc;

    if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -PARTIAL_PRICE_THRESHOLD) {
        return 1;
    } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > PARTIAL_PRICE_THRESHOLD) {
        return 1;
    } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > PARTIAL_PRICE_THRESHOLD) {
        return 1;
    }
    return 0;
}

/* Add variable to hot set if not already present and not full */
void add_to_hot_set(SimplexTableau *tab, int var) {
    /* Check if already in hot set */
    for (int i = 0; i < tab->partial_cand_count; i++) {
        if (tab->partial_candidates[i] == var) return;
    }
    /* Add if space available */
    if (tab->partial_cand_count < tab->partial_cand_capacity) {
        tab->partial_candidates[tab->partial_cand_count++] = var;
    }
}

int pricing_partial(SimplexTableau *tab, int *entering) {
    *entering = -1;

    /* Ensure duals are valid for lazy RC computation */
    if (!tab->duals_valid) {
        tableau_compute_duals(tab);
    }

    int n = tab->n;
    double best_rc_val = 0.0;
    int best_var = -1;

    /* Phase 1: Scan hot set first (fast path) */
    int write_idx = 0;
    for (int i = 0; i < tab->partial_cand_count; i++) {
        int j = tab->partial_candidates[i];

        /* Skip and remove basic variables from hot set */
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        /* Keep this variable in the compacted hot set */
        tab->partial_candidates[write_idx++] = j;

        double rc;
        if (is_entering_eligible(tab, j, &rc)) {
            double rc_abs = fabs(rc);

            /* Accept immediately if reduced cost is very attractive */
            if (rc_abs > PARTIAL_HOT_ACCEPT) {
                *entering = j;
                tab->partial_cand_count = write_idx;
                return 0;
            }

            /* Track best candidate seen */
            if (rc_abs > fabs(best_rc_val)) {
                best_rc_val = rc;
                best_var = j;
            }
        }
    }
    /* Update hot set count after compaction */
    tab->partial_cand_count = write_idx;

    /* Phase 2: Partial scan from current position */
    int start = tab->partial_price_pos;
    int scanned = 0;

    for (int i = 0; i < n && scanned < PARTIAL_PRICE_BLOCK; i++) {
        int j = (start + i) % n;
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        scanned++;
        double rc;
        if (is_entering_eligible(tab, j, &rc)) {
            double rc_abs = fabs(rc);
            if (rc_abs > fabs(best_rc_val)) {
                best_rc_val = rc;
                best_var = j;
            }
        }
    }

    /* Update scan position for next call (round-robin) */
    tab->partial_price_pos = (start + PARTIAL_PRICE_BLOCK) % n;

    /* Use best variable found (if any) */
    if (best_var >= 0) {
        *entering = best_var;
        add_to_hot_set(tab, best_var);
        return 0;
    }

    /* Phase 3: Full scan if partial scan found nothing (rare) */
    for (int i = 0; i < n; i++) {
        double rc;
        if (is_entering_eligible(tab, i, &rc)) {
            double rc_abs = fabs(rc);
            if (rc_abs > fabs(best_rc_val)) {
                best_rc_val = rc;
                best_var = i;
            }
        }
    }

    if (best_var >= 0) {
        *entering = best_var;
        add_to_hot_set(tab, best_var);
        return 0;
    }

    return 1;  /* Optimal - no eligible variable found */
}

/* Devex-scored partial pricing.
 * Uses the same hot-set/round-robin idea as pricing_partial(), but keeps
 * Devex's rc^2/weight scoring to preserve pivot quality characteristics. */
int devex_entering_eligible(SimplexTableau *tab, int j, double *score_out) {
    if (j < 0 || j >= tab->n) return 0;
    if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) return 0;

    double rc = tableau_get_rc(tab, j);
    double score = 0.0;

    if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
        score = rc * rc;
    } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
        score = rc * rc;
    } else if (tab->var_status[j] == RALPH_NONBASIC_FREE &&
               (rc > RALPH_OPT_TOL || rc < -RALPH_OPT_TOL)) {
        score = rc * rc;
    } else {
        return 0;
    }

    double weight = tab->se_weights[j];
    if (weight < 1.0) weight = 1.0;
    *score_out = score / weight;
    return *score_out > 0.0;
}

int pricing_devex_partial(SimplexTableau *tab, int *entering) {
    *entering = -1;

    if (!tab->duals_valid) {
        tableau_compute_duals(tab);
    }

    double best_score = RALPH_OPT_TOL * RALPH_OPT_TOL;
    int best_var = -1;
    int n = tab->n;

    /* Phase 1: scan and compact the hot set. */
    int write_idx = 0;
    for (int i = 0; i < tab->partial_cand_count; i++) {
        int j = tab->partial_candidates[i];
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;
        tab->partial_candidates[write_idx++] = j;

        double score;
        if (devex_entering_eligible(tab, j, &score) && score > best_score) {
            best_score = score;
            best_var = j;
        }
    }
    tab->partial_cand_count = write_idx;

    /* Phase 2: bounded round-robin scan through the full variable space. */
    int start = tab->partial_price_pos;
    int scanned = 0;
    for (int i = 0; i < n && scanned < DEVEX_PARTIAL_BLOCK; i++) {
        int j = (start + i) % n;
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        scanned++;
        double score;
        if (devex_entering_eligible(tab, j, &score) && score > best_score) {
            best_score = score;
            best_var = j;
        }
    }
    tab->partial_price_pos = (start + DEVEX_PARTIAL_BLOCK) % n;

    if (best_var >= 0) {
        *entering = best_var;
        add_to_hot_set(tab, best_var);
        return 0;
    }

    /* Fallback for safety: if bounded scan missed a candidate, run full Devex. */
    if (pricing_devex(tab, entering) == 0) {
        add_to_hot_set(tab, *entering);
        return 0;
    }

    return 1;
}

/* ============================================================================
 * Adaptive Devex Partial Trigger
 * ============================================================================ */

int phase2_use_adaptive_devex_partial(const SimplexTableau *tab,
                                      int iter,
                                      int degenerate_count,
                                      int use_bland,
                                      int pricing_strategy) {
    if (!tab) return 0;
    if (use_bland) return 0;
    if (pricing_strategy != 2) return 0;
    if (tab->m < DEVEX_PARTIAL_ENABLE_M || tab->n < DEVEX_PARTIAL_ENABLE_N) return 0;
    if (degenerate_count >= DEVEX_PARTIAL_DEGEN_TRIGGER) return 1;
    return iter >= DEVEX_PARTIAL_ITER_TRIGGER;
}

/* ============================================================================
 * Unified Pricing Dispatch
 * ============================================================================ */

int pricing_dispatch(SimplexTableau *tab, int strategy, int use_bland,
                     int adaptive_devex_partial, int iter, int *entering) {
    if (use_bland) {
        return pricing_bland(tab, entering);
    } else if (strategy == 0) {
        return pricing_dantzig(tab, entering);
    } else if (strategy == 1) {
        return pricing_steepest_edge(tab, entering);
    } else if (strategy == 3) {
        return pricing_partial(tab, entering);
    } else if (strategy == 4) {
        return pricing_heap(tab, entering);
    } else {
        /* strategy == 2 (Devex) — default */
        if (adaptive_devex_partial &&
            (iter & DEVEX_PARTIAL_FULL_RESCAN_MASK) != 0) {
            return pricing_devex_partial(tab, entering);
        } else {
            return pricing_devex(tab, entering);
        }
    }
}
