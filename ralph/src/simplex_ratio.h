/*
 * simplex_ratio.h - Ratio test (leaving variable selection) for the simplex method.
 *
 * Extracted from simplex.c - contains all ratio test variants.
 */

#ifndef SIMPLEX_RATIO_H
#define SIMPLEX_RATIO_H

#include "lp.h"

/* Bland's ratio test: smallest index among ties (anti-cycling) */
int ratio_test_bland(SimplexTableau *tab, int entering, int *leaving, double *theta);

/* Standard ratio test: strict minimum ratio, no Harris tolerance */
int ratio_test_standard(SimplexTableau *tab, int entering, int *leaving, double *theta);

/* Harris ratio test: allows small infeasibility for better pivot selection */
int ratio_test_harris(SimplexTableau *tab, int entering, int *leaving, double *theta);

/* Harris ratio test excluding a specific leaving position */
int ratio_test_harris_excluding_current(SimplexTableau *tab, int entering,
                                        int exclude_pos, int *leaving, double *theta);

/* Policy dispatch: selects ratio test variant based on solver settings */
int primal_ratio_test_with_policy(const SimplexSolver *solver,
                                  SimplexTableau *tab,
                                  int use_bland,
                                  int entering,
                                  int *leaving,
                                  double *theta);

#endif /* SIMPLEX_RATIO_H */
