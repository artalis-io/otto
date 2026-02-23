/*
 * Ralph - CPLEX LP File Writer
 *
 * Writes LP/MIP problems in CPLEX LP format.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include "lp.h"
#include "ralph_mip.h"

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
        if (model->lb[j] != 0.0 || model->ub[j] != RALPH_LP_INFINITY) {
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
        if (lb == 0.0 && ub == RALPH_LP_INFINITY) continue;

        get_var_name(model, j, name_buf, sizeof(name_buf));

        /* Free variable */
        if (lb <= -RALPH_LP_INFINITY && ub >= RALPH_LP_INFINITY) {
            fprintf(f, " %s free\n", name_buf);
        }
        /* Fixed variable */
        else if (fabs(lb - ub) < 1e-15) {
            fprintf(f, " %s = %.15g\n", name_buf, lb);
        }
        /* Both finite bounds */
        else if (lb > -RALPH_LP_INFINITY && ub < RALPH_LP_INFINITY) {
            fprintf(f, " %.15g <= %s <= %.15g\n", lb, name_buf, ub);
        }
        /* Only lower bound */
        else if (lb > -RALPH_LP_INFINITY) {
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

/* Sanitize names for token-based MPS output (free format). */
static void mps_make_name(const char *src, const char *prefix, int idx,
                          char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;

    if (!src || src[0] == '\0') {
        snprintf(buf, buf_size, "%s%d", prefix, idx + 1);
        return;
    }

    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 1 < buf_size; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '_' || c == '.' || c == '$') {
            buf[out++] = (char)c;
        } else {
            buf[out++] = '_';
        }
    }
    buf[out] = '\0';

    if (buf[0] == '\0') {
        snprintf(buf, buf_size, "%s%d", prefix, idx + 1);
    }
}

static void mps_get_var_name(const LPModel *model, int var, char *buf, size_t buf_size) {
    const char *name = NULL;
    if (model->var_names && var < model->num_vars) {
        name = model->var_names[var];
    }
    mps_make_name(name, "X", var, buf, buf_size);
}

static void mps_get_con_name(const LPModel *model, int con, char *buf, size_t buf_size) {
    const char *name = NULL;
    if (model->con_names && con < model->num_cons) {
        name = model->con_names[con];
    }
    mps_make_name(name, "R", con, buf, buf_size);
}

static int mps_is_neg_inf(double x) {
    return x <= -RALPH_LP_INFINITY / 2.0;
}

static int mps_is_pos_inf(double x) {
    return x >= RALPH_LP_INFINITY / 2.0;
}

/* ============================================================================
 * Public Interface
 * ============================================================================ */

int ralph_write_lp(const RalphLPModel *model, const char *filename) {
    if (!model || !filename) return -1;

    /* Access internal LPModel */
    /* We need to add a way to get the LPModel from RalphModel */
    /* For now, we'll work with a workaround using the public API */

    /* Get the internal LPModel through a helper function */
    extern LPModel* ralph_get_lp_model(const RalphLPModel *model);
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
    fprintf(f, "\\ Generated by Ralph %s\n", ralph_lp_version());
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

int ralph_write_mps(const RalphLPModel *model, const char *filename) {
    if (!model || !filename) return -1;

    extern LPModel* ralph_get_lp_model(const RalphLPModel *model);
    LPModel *lp = ralph_get_lp_model(model);
    if (!lp) return -1;

    if (!lp->A) {
        if (lp_model_finalize(lp) != 0) {
            return -1;
        }
    }

    FILE *f = fopen(filename, "w");
    if (!f) return -1;

    const char *obj_row = "OBJ";
    const char *rhs_name = "RHS1";
    const char *bnd_name = "BND1";
    char prob_name[LP_MAX_NAME];
    mps_make_name(lp->name, "PROB", 0, prob_name, sizeof(prob_name));

    fprintf(f, "NAME          %s\n", prob_name);
    fprintf(f, "OBJSENSE\n");
    fprintf(f, " %s\n", lp->obj_sense == -1 ? "MAX" : "MIN");

    fprintf(f, "ROWS\n");
    fprintf(f, "N %s\n", obj_row);
    for (int i = 0; i < lp->num_cons; i++) {
        char row_name[LP_MAX_NAME];
        char sense = 'E';
        if (lp->sense && (lp->sense[i] == 'L' || lp->sense[i] == 'G' || lp->sense[i] == 'E')) {
            sense = lp->sense[i];
        }
        mps_get_con_name(lp, i, row_name, sizeof(row_name));
        fprintf(f, "%c %s\n", sense, row_name);
    }

    fprintf(f, "COLUMNS\n");
    int in_integer = 0;
    int marker_count = 0;
    for (int j = 0; j < lp->num_vars; j++) {
        char col_name[LP_MAX_NAME];
        char var_type = lp->var_type ? lp->var_type[j] : 'C';
        int is_integer = (var_type == 'I' || var_type == 'B');

        if (is_integer && !in_integer) {
            fprintf(f, "MARK%04d 'MARKER' 'INTORG'\n", marker_count++);
            in_integer = 1;
        } else if (!is_integer && in_integer) {
            fprintf(f, "MARK%04d 'MARKER' 'INTEND'\n", marker_count++);
            in_integer = 0;
        }

        mps_get_var_name(lp, j, col_name, sizeof(col_name));

        if (lp->c && fabs(lp->c[j]) > 1e-15) {
            fprintf(f, "%s %s %.17g\n", col_name, obj_row, lp->c[j]);
        }

        for (int p = lp->A->colptr[j]; p < lp->A->colptr[j + 1]; p++) {
            int i = lp->A->rowidx[p];
            double val = lp->A->values[p];
            if (fabs(val) <= 1e-15) continue;

            char row_name[LP_MAX_NAME];
            mps_get_con_name(lp, i, row_name, sizeof(row_name));
            fprintf(f, "%s %s %.17g\n", col_name, row_name, val);
        }
    }
    if (in_integer) {
        fprintf(f, "MARK%04d 'MARKER' 'INTEND'\n", marker_count++);
    }

    fprintf(f, "RHS\n");
    if (fabs(lp->obj_offset) > 1e-15) {
        fprintf(f, "%s %s %.17g\n", rhs_name, obj_row, lp->obj_offset);
    }
    for (int i = 0; i < lp->num_cons; i++) {
        if (!lp->b || fabs(lp->b[i]) <= 1e-15) continue;
        char row_name[LP_MAX_NAME];
        mps_get_con_name(lp, i, row_name, sizeof(row_name));
        fprintf(f, "%s %s %.17g\n", rhs_name, row_name, lp->b[i]);
    }

    int write_bounds = 0;
    for (int j = 0; j < lp->num_vars; j++) {
        char var_type = lp->var_type ? lp->var_type[j] : 'C';
        double lb = lp->lb ? lp->lb[j] : 0.0;
        double ub = lp->ub ? lp->ub[j] : RALPH_LP_INFINITY;
        if (var_type == 'B' || fabs(lb) > 1e-15 || !mps_is_pos_inf(ub) || mps_is_neg_inf(lb)) {
            write_bounds = 1;
            break;
        }
    }

    if (write_bounds) {
        fprintf(f, "BOUNDS\n");
        for (int j = 0; j < lp->num_vars; j++) {
            char col_name[LP_MAX_NAME];
            char var_type = lp->var_type ? lp->var_type[j] : 'C';
            double lb = lp->lb ? lp->lb[j] : 0.0;
            double ub = lp->ub ? lp->ub[j] : RALPH_LP_INFINITY;

            mps_get_var_name(lp, j, col_name, sizeof(col_name));

            if (var_type == 'B') {
                fprintf(f, "BV %s %s\n", bnd_name, col_name);
                continue;
            }

            if (mps_is_neg_inf(lb) && mps_is_pos_inf(ub)) {
                fprintf(f, "FR %s %s\n", bnd_name, col_name);
                continue;
            }

            if (!mps_is_neg_inf(lb) && !mps_is_pos_inf(ub) && fabs(lb - ub) <= 1e-15) {
                fprintf(f, "FX %s %s %.17g\n", bnd_name, col_name, lb);
                continue;
            }

            if (mps_is_neg_inf(lb)) {
                fprintf(f, "MI %s %s\n", bnd_name, col_name);
            } else if (fabs(lb) > 1e-15) {
                fprintf(f, "LO %s %s %.17g\n", bnd_name, col_name, lb);
            }

            if (!mps_is_pos_inf(ub)) {
                fprintf(f, "UP %s %s %.17g\n", bnd_name, col_name, ub);
            }
        }
    }

    fprintf(f, "ENDATA\n");
    fclose(f);
    return 0;
}

/* ============================================================================
 * Solution Format Writer
 * ============================================================================ */

int ralph_write_solution_buf(const RalphLPModel *model, char *buf, size_t buf_size) {
    if (!model || !buf || buf_size == 0) return -1;

    extern LPModel* ralph_get_lp_model(const RalphLPModel *model);
    LPModel *lp = ralph_get_lp_model(model);
    if (!lp) return -1;

    RalphLPStatus status = ralph_lp_get_status(model);
    size_t pos = 0;
    int n;

    /* Header comment */
    n = snprintf(buf + pos, buf_size - pos,
                 "\\ Solution generated by Ralph %s\n", ralph_lp_version());
    if (n > 0) pos += n;

    n = snprintf(buf + pos, buf_size - pos,
                 "\\ Iterations: %d\n", ralph_lp_get_iterations(model));
    if (n > 0) pos += n;

    if (ralph_lp_get_num_integer_vars(model) > 0) {
        n = snprintf(buf + pos, buf_size - pos,
                     "\\ Nodes: %d\n", ralph_mip_get_node_count((const RalphMIPModel *)model));
        if (n > 0) pos += n;
    }

    n = snprintf(buf + pos, buf_size - pos, "\n");
    if (n > 0) pos += n;

    /* Status line */
    n = snprintf(buf + pos, buf_size - pos,
                 "solution status: %s\n", ralph_lp_status_string(status));
    if (n > 0) pos += n;

    /* Solution values if available */
    if (status == RALPH_LP_STATUS_OPTIMAL ||
        status == RALPH_LP_STATUS_TIME_LIMIT ||
        status == RALPH_LP_STATUS_ITERATION_LIMIT) {

        double objval = ralph_lp_get_objval(model);
        n = snprintf(buf + pos, buf_size - pos,
                     "objective value: %.10g\n\n", objval);
        if (n > 0) pos += n;

        int num_vars = ralph_lp_get_num_vars(model);
        double *x = (double *)malloc(num_vars * sizeof(double));
        if (x && ralph_lp_get_solution(model, x) == 0) {
            for (int j = 0; j < num_vars; j++) {
                const char *name = ralph_lp_get_var_name(model, j);
                if (name && name[0]) {
                    n = snprintf(buf + pos, buf_size - pos, "%s %.10g\n", name, x[j]);
                } else {
                    n = snprintf(buf + pos, buf_size - pos, "x%d %.10g\n", j, x[j]);
                }
                if (n > 0) pos += n;
            }
        }
        free(x);
    } else if (status == RALPH_LP_STATUS_INFEASIBLE) {
        n = snprintf(buf + pos, buf_size - pos, "\\ Problem is infeasible\n");
        if (n > 0) pos += n;
    } else if (status == RALPH_LP_STATUS_UNBOUNDED) {
        n = snprintf(buf + pos, buf_size - pos, "\\ Problem is unbounded\n");
        if (n > 0) pos += n;
    }

    /* Null terminate */
    if (pos < buf_size) {
        buf[pos] = '\0';
    } else {
        buf[buf_size - 1] = '\0';
    }

    return (int)pos;
}

int ralph_write_solution(const RalphLPModel *model, const char *filename) {
    if (!model || !filename) return -1;

    /* Estimate buffer size */
    int num_vars = ralph_lp_get_num_vars(model);
    size_t buf_size = 4096 + num_vars * 64;  /* Generous estimate */
    char *buf = (char *)malloc(buf_size);
    if (!buf) return -1;

    int len = ralph_write_solution_buf(model, buf, buf_size);
    if (len < 0) {
        free(buf);
        return -1;
    }

    FILE *f = fopen(filename, "w");
    if (!f) {
        free(buf);
        return -1;
    }

    fprintf(f, "%s", buf);
    fclose(f);
    free(buf);
    return 0;
}
