/*
 * simplex_perturb.h - Primal perturbation (anti-cycling) for the simplex method.
 *
 * Extracted from simplex.c - contains bound perturbation and artificial variable helpers.
 */

#ifndef SIMPLEX_PERTURB_H
#define SIMPLEX_PERTURB_H

#include "lp.h"

/* Perturbation constants */
#define PRIMAL_PERTURB_BASE 1e-6
#define PRIMAL_PERTURB_MULT 7

/* Check if smcp_shift setting allows perturbation */
int simplex_smcp_shift_allows_perturb_for_test(int smcp_shift);
int simplex_smcp_shift_allows_perturb(const SimplexTableau *tab);

/* Artificial variable detection */
int is_artificial_var(const SimplexTableau *tab, int var_idx);
int artificial_var_row(const SimplexTableau *tab, int var_idx);

/* Mark basic artificial rows as redundant for LU */
int mark_basic_artificial_rows_redundant(SimplexTableau *tab, int only_infeasible);

/* Apply/remove primal bound perturbation (anti-cycling) */
void primal_apply_perturbation(SimplexTableau *tab);
void primal_apply_perturbation_scaled(SimplexTableau *tab, double scale);
void primal_remove_perturbation(SimplexTableau *tab);

#endif /* SIMPLEX_PERTURB_H */
