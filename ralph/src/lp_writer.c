/*
 * Ralph - CPLEX LP File Writer
 *
 * Writes LP/MIP problems in CPLEX LP format.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "lp.h"
#include "ralph.h"

#define LP_LINE_WIDTH 80
#define LP_MAX_NAME 256

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/* Generate a default variable name if none set */
static void get_var_name(const LPModel *model, int var, char *buf, size_t buf_size) {
    const char *name = NULL;
    if (model->var_names && var < model->num_vars) {
        name = model->var_names[var];
    }

    if (name) {
        snprintf(buf, buf_size, "%s", name);
    } else {
        snprintf(buf, buf_size, "x%d", var + 1);  /* 1-indexed for LP format */
    }
}

/* Generate a default constraint name if none set */
static void get_con_name(const LPModel *model, int con, char *buf, size_t buf_size) {
    const char *name = NULL;
    if (model->con_names && con < model->num_cons) {
        name = model->con_names[con];
    }

    if (name) {
        snprintf(buf, buf_size, "%s", name);
    } else {
        snprintf(buf, buf_size, "c%d", con + 1);  /* 1-indexed for LP format */
    }
}

/* Format a coefficient for output */
static int format_coef(double val, int is_first, char *buf, size_t buf_size) {
    if (fabs(val) < 1e-15) return 0;  /* Skip zeros */

    char sign = val >= 0 ? '+' : '-';
    double abs_val = fabs(val);

    if (is_first) {
        if (fabs(abs_val - 1.0) < 1e-10) {
            if (val < 0) {
                snprintf(buf, buf_size, "-");
            } else {
                buf[0] = '\0';
            }
        } else {
            snprintf(buf, buf_size, "%.15g ", val);
        }
    } else {
        if (fabs(abs_val - 1.0) < 1e-10) {
            snprintf(buf, buf_size, " %c ", sign);
        } else {
            snprintf(buf, buf_size, " %c %.15g ", sign, abs_val);
        }
    }

    return 1;
}

/* ============================================================================
 * Section Writers
 * ============================================================================ */

static int write_objective(FILE *f, const LPModel *model) {
    /* Write section header */
    if (model->obj_sense == 1) {
        fprintf(f, "MINIMIZE\n");
    } else {
        fprintf(f, "MAXIMIZE\n");
    }

    /* Write objective name and start expression */
    fprintf(f, " obj:");

    int col = 6;  /* Current column position */
    int first = 1;
    char name_buf[LP_MAX_NAME];
    char coef_buf[64];

    for (int j = 0; j < model->num_vars; j++) {
        double c = model->c[j];
        if (fabs(c) < 1e-15) continue;  /* Skip zeros */

        get_var_name(model, j, name_buf, sizeof(name_buf));

        if (!format_coef(c, first, coef_buf, sizeof(coef_buf))) continue;

        /* Check line length */
        size_t term_len = strlen(coef_buf) + strlen(name_buf);
        if (col + term_len > LP_LINE_WIDTH && !first) {
            fprintf(f, "\n      ");
            col = 6;
        }

        fprintf(f, "%s%s", coef_buf, name_buf);
        col += (int)term_len;
        first = 0;
    }

    if (first) {
        /* No non-zero terms */
        fprintf(f, " 0");
    }

    fprintf(f, "\n\n");
    return 0;
}

static int write_constraints(FILE *f, const LPModel *model) {
    if (model->num_cons == 0) return 0;

    fprintf(f, "SUBJECT TO\n");

    char name_buf[LP_MAX_NAME];
    char var_buf[LP_MAX_NAME];
    char coef_buf[64];

    /* For CSC matrix, we need to access by row - build row pointers */
    int *row_start = (int*)calloc(model->num_cons + 1, sizeof(int));
    int *row_idx = (int*)calloc(model->A->nnz, sizeof(int));
    double *row_val = (double*)calloc(model->A->nnz, sizeof(double));

    if (!row_start || !row_idx || !row_val) {
        free(row_start);
        free(row_idx);
        free(row_val);
        return -1;
    }

    /* Count non-zeros per row */
    for (int p = 0; p < model->A->nnz; p++) {
        row_start[model->A->rowidx[p] + 1]++;
    }
    for (int i = 1; i <= model->num_cons; i++) {
        row_start[i] += row_start[i - 1];
    }

    /* Fill row data */
    int *row_pos = (int*)calloc(model->num_cons, sizeof(int));
    if (!row_pos) {
        free(row_start);
        free(row_idx);
        free(row_val);
        return -1;
    }

    for (int j = 0; j < model->num_vars; j++) {
        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            int i = model->A->rowidx[p];
            int pos = row_start[i] + row_pos[i];
            row_idx[pos] = j;
            row_val[pos] = model->A->values[p];
            row_pos[i]++;
        }
    }
    free(row_pos);

    /* Write each constraint */
    for (int i = 0; i < model->num_cons; i++) {
        get_con_name(model, i, name_buf, sizeof(name_buf));
        fprintf(f, " %s:", name_buf);

        int col = 2 + (int)strlen(name_buf);
        int first = 1;

        for (int p = row_start[i]; p < row_start[i + 1]; p++) {
            int j = row_idx[p];
            double val = row_val[p];

            if (fabs(val) < 1e-15) continue;

            get_var_name(model, j, var_buf, sizeof(var_buf));

            if (!format_coef(val, first, coef_buf, sizeof(coef_buf))) continue;

            /* Check line length */
            size_t term_len = strlen(coef_buf) + strlen(var_buf);
            if (col + term_len > LP_LINE_WIDTH && !first) {
                fprintf(f, "\n      ");
                col = 6;
            }

            fprintf(f, "%s%s", coef_buf, var_buf);
            col += (int)term_len;
            first = 0;
        }

        if (first) {
            fprintf(f, " 0");
        }

        /* Sense and RHS */
        char sense_str[4];
        if (model->sense[i] == 'L') snprintf(sense_str, 4, "<=");
        else if (model->sense[i] == 'G') snprintf(sense_str, 4, ">=");
        else snprintf(sense_str, 4, "=");

        fprintf(f, " %s %.15g\n", sense_str, model->b[i]);
    }

    free(row_start);
    free(row_idx);
    free(row_val);

    fprintf(f, "\n");
    return 0;
}

static int write_bounds(FILE *f, const LPModel *model) {
    if (model->num_vars == 0) return 0;

    /* Check if any non-default bounds */
    int has_nondefault = 0;
    for (int j = 0; j < model->num_vars; j++) {
        if (model->lb[j] != 0.0 || model->ub[j] != RALPH_INFINITY) {
            has_nondefault = 1;
            break;
        }
    }

    if (!has_nondefault) return 0;

    fprintf(f, "BOUNDS\n");

    char name_buf[LP_MAX_NAME];

    for (int j = 0; j < model->num_vars; j++) {
        double lb = model->lb[j];
        double ub = model->ub[j];

        /* Skip default bounds (0 <= x <= inf) */
        if (lb == 0.0 && ub == RALPH_INFINITY) continue;

        get_var_name(model, j, name_buf, sizeof(name_buf));

        /* Free variable */
        if (lb <= -RALPH_INFINITY && ub >= RALPH_INFINITY) {
            fprintf(f, " %s free\n", name_buf);
        }
        /* Fixed variable */
        else if (fabs(lb - ub) < 1e-15) {
            fprintf(f, " %s = %.15g\n", name_buf, lb);
        }
        /* Both finite bounds */
        else if (lb > -RALPH_INFINITY && ub < RALPH_INFINITY) {
            fprintf(f, " %.15g <= %s <= %.15g\n", lb, name_buf, ub);
        }
        /* Only lower bound */
        else if (lb > -RALPH_INFINITY) {
            if (lb == 0.0) {
                /* Default, but upper is not infinity */
                fprintf(f, " %s <= %.15g\n", name_buf, ub);
            } else {
                fprintf(f, " %s >= %.15g\n", name_buf, lb);
            }
        }
        /* Only upper bound (lb is -inf) */
        else {
            fprintf(f, " -infinity <= %s <= %.15g\n", name_buf, ub);
        }
    }

    fprintf(f, "\n");
    return 0;
}

static int write_generals(FILE *f, const LPModel *model) {
    /* Count general integers (not binary) */
    int count = 0;
    for (int j = 0; j < model->num_vars; j++) {
        if (model->var_type[j] == 'I') count++;
    }

    if (count == 0) return 0;

    fprintf(f, "GENERAL\n");

    char name_buf[LP_MAX_NAME];
    int col = 1;

    for (int j = 0; j < model->num_vars; j++) {
        if (model->var_type[j] != 'I') continue;

        get_var_name(model, j, name_buf, sizeof(name_buf));

        size_t name_len = strlen(name_buf);
        if (col + name_len + 1 > LP_LINE_WIDTH) {
            fprintf(f, "\n");
            col = 1;
        }

        fprintf(f, " %s", name_buf);
        col += (int)name_len + 1;
    }

    fprintf(f, "\n\n");
    return 0;
}

static int write_binaries(FILE *f, const LPModel *model) {
    if (model->num_binary == 0) return 0;

    fprintf(f, "BINARY\n");

    char name_buf[LP_MAX_NAME];
    int col = 1;

    for (int j = 0; j < model->num_vars; j++) {
        if (model->var_type[j] != 'B') continue;

        get_var_name(model, j, name_buf, sizeof(name_buf));

        size_t name_len = strlen(name_buf);
        if (col + name_len + 1 > LP_LINE_WIDTH) {
            fprintf(f, "\n");
            col = 1;
        }

        fprintf(f, " %s", name_buf);
        col += (int)name_len + 1;
    }

    fprintf(f, "\n\n");
    return 0;
}

/* ============================================================================
 * Public Interface
 * ============================================================================ */

int ralph_write_lp(const RalphModel *model, const char *filename) {
    if (!model || !filename) return -1;

    /* Access internal LPModel */
    /* We need to add a way to get the LPModel from RalphModel */
    /* For now, we'll work with a workaround using the public API */

    /* Get the internal LPModel through a helper function */
    extern LPModel* ralph_get_lp_model(const RalphModel *model);
    LPModel *lp = ralph_get_lp_model(model);
    if (!lp) return -1;

    /* Finalize if needed */
    if (!lp->A) {
        if (lp_model_finalize(lp) != 0) {
            return -1;
        }
    }

    FILE *f = fopen(filename, "w");
    if (!f) return -1;

    /* Write header comment */
    fprintf(f, "\\ Generated by Ralph %s\n", ralph_version());
    if (lp->name) {
        fprintf(f, "\\ Problem: %s\n", lp->name);
    }
    fprintf(f, "\n");

    /* Write sections */
    if (write_objective(f, lp) < 0) goto error;
    if (write_constraints(f, lp) < 0) goto error;
    if (write_bounds(f, lp) < 0) goto error;
    if (write_generals(f, lp) < 0) goto error;
    if (write_binaries(f, lp) < 0) goto error;

    fprintf(f, "END\n");

    fclose(f);
    return 0;

error:
    fclose(f);
    return -1;
}
