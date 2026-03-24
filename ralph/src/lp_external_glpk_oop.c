#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lp_external_adapter.h"
#include "lp_external_oop.h"

#define GLPK_OOP_DEFAULT_BIN "glpsol"
#define GLPK_OOP_MAX_TOKENS 16

typedef enum {
    GLPK_OOP_STATUS_UNKNOWN = 0,
    GLPK_OOP_STATUS_OPTIMAL,
    GLPK_OOP_STATUS_INFEASIBLE,
    GLPK_OOP_STATUS_UNBOUNDED,
    GLPK_OOP_STATUS_TIME_LIMIT,
    GLPK_OOP_STATUS_ITERATION_LIMIT,
    GLPK_OOP_STATUS_NUMERICAL
} GLPKOOPStatus;

typedef struct {
    char *glpsol_path;
} LPExternalGLPKOOPState;

typedef struct {
    int iterations;
    GLPKOOPStatus hint_status;
} GLPKOOPRunHints;

static int glpk_oop_is_neg_inf(double x) {
    return x <= -RALPH_INFINITY / 2.0;
}

static int glpk_oop_is_pos_inf(double x) {
    return x >= RALPH_INFINITY / 2.0;
}

static int glpk_oop_format_lp_coef(double val, int first, char *buf, size_t buf_size) {
    char sign = val >= 0.0 ? '+' : '-';
    double abs_val = val >= 0.0 ? val : -val;

    if (buf_size == 0) return 0;
    if (val == 0.0) {
        buf[0] = '\0';
        return 0;
    }

    if (first) {
        if (abs_val == 1.0) {
            if (val < 0.0) {
                snprintf(buf, buf_size, "-");
            } else {
                buf[0] = '\0';
            }
        } else {
            snprintf(buf, buf_size, "%.17g ", val);
        }
    } else {
        if (abs_val == 1.0) {
            snprintf(buf, buf_size, " %c ", sign);
        } else {
            snprintf(buf, buf_size, " %c %.17g ", sign, abs_val);
        }
    }

    return 1;
}

static int glpk_oop_build_rowwise(const LPModel *lp,
                                  int **row_start_out,
                                  int **row_idx_out,
                                  double **row_val_out) {
    int *row_start = NULL;
    int *row_pos = NULL;
    int *row_idx = NULL;
    double *row_val = NULL;
    int nnz;

    if (!lp || !lp->A || !row_start_out || !row_idx_out || !row_val_out) return -1;
    nnz = lp->A->nnz;

    row_start = (int*)calloc((size_t)lp->num_cons + 1, sizeof(int));
    row_pos = (int*)calloc((size_t)lp->num_cons, sizeof(int));
    row_idx = (int*)calloc((size_t)nnz, sizeof(int));
    row_val = (double*)calloc((size_t)nnz, sizeof(double));
    if (!row_start || !row_pos || !row_idx || !row_val) {
        free(row_start);
        free(row_pos);
        free(row_idx);
        free(row_val);
        return -1;
    }

    for (int p = 0; p < nnz; p++) {
        int row = lp->A->rowidx[p];
        if (row >= 0 && row < lp->num_cons) row_start[row + 1]++;
    }
    for (int i = 1; i <= lp->num_cons; i++) row_start[i] += row_start[i - 1];

    for (int j = 0; j < lp->num_vars; j++) {
        for (int p = lp->A->colptr[j]; p < lp->A->colptr[j + 1]; p++) {
            int row = lp->A->rowidx[p];
            int pos;
            if (row < 0 || row >= lp->num_cons) continue;
            pos = row_start[row] + row_pos[row];
            row_idx[pos] = j;
            row_val[pos] = lp->A->values[p];
            row_pos[row]++;
        }
    }

    free(row_pos);
    *row_start_out = row_start;
    *row_idx_out = row_idx;
    *row_val_out = row_val;
    return 0;
}

static int glpk_oop_write_lp(const LPModel *lp, const char *filename) {
    FILE *f = NULL;
    int *row_start = NULL;
    int *row_idx = NULL;
    double *row_val = NULL;
    char coef_buf[64];

    if (!lp || !lp->A || !filename) return -1;
    if (glpk_oop_build_rowwise(lp, &row_start, &row_idx, &row_val) != 0) return -1;

    f = fopen(filename, "w");
    if (!f) {
        free(row_start);
        free(row_idx);
        free(row_val);
        return -1;
    }

    fprintf(f, "%s\n", lp->obj_sense == -1 ? "Maximize" : "Minimize");
    fprintf(f, " obj:");
    {
        int first = 1;
        for (int j = 0; j < lp->num_vars; j++) {
            double c = lp->c ? lp->c[j] : 0.0;
            if (c == 0.0) continue;
            if (!glpk_oop_format_lp_coef(c, first, coef_buf, sizeof(coef_buf))) continue;
            fprintf(f, "%sx%d", coef_buf, j + 1);
            first = 0;
        }
        if (first) {
            if (lp->num_vars > 0) {
                fprintf(f, " 0 x1");
            } else {
                fprintf(f, " 0");
            }
        }
    }
    fprintf(f, "\n");

    fprintf(f, "Subject To\n");
    for (int i = 0; i < lp->num_cons; i++) {
        int first = 1;
        fprintf(f, " c%d:", i + 1);
        for (int p = row_start[i]; p < row_start[i + 1]; p++) {
            int j = row_idx[p];
            double v = row_val[p];
            if (v == 0.0) continue;
            if (!glpk_oop_format_lp_coef(v, first, coef_buf, sizeof(coef_buf))) continue;
            fprintf(f, "%sx%d", coef_buf, j + 1);
            first = 0;
        }
        if (first) {
            if (lp->num_vars > 0) {
                fprintf(f, " 0 x1");
            } else {
                fprintf(f, " 0");
            }
        }

        if (lp->sense && lp->sense[i] == 'G') {
            fprintf(f, " >= %.17g", lp->b ? lp->b[i] : 0.0);
        } else if (lp->sense && lp->sense[i] == 'E') {
            fprintf(f, " = %.17g", lp->b ? lp->b[i] : 0.0);
        } else {
            fprintf(f, " <= %.17g", lp->b ? lp->b[i] : 0.0);
        }
        fprintf(f, "\n");
    }

    fprintf(f, "Bounds\n");
    for (int j = 0; j < lp->num_vars; j++) {
        double lb = lp->lb ? lp->lb[j] : 0.0;
        double ub = lp->ub ? lp->ub[j] : RALPH_INFINITY;
        char vt = lp->var_type ? lp->var_type[j] : 'C';

        if (vt == 'B') {
            fprintf(f, " 0 <= x%d <= 1\n", j + 1);
            continue;
        }

        if (glpk_oop_is_neg_inf(lb) && glpk_oop_is_pos_inf(ub)) {
            fprintf(f, " x%d free\n", j + 1);
        } else if (!glpk_oop_is_neg_inf(lb) && !glpk_oop_is_pos_inf(ub) && lb == ub) {
            fprintf(f, " x%d = %.17g\n", j + 1, lb);
        } else if (!glpk_oop_is_neg_inf(lb) && !glpk_oop_is_pos_inf(ub)) {
            fprintf(f, " %.17g <= x%d <= %.17g\n", lb, j + 1, ub);
        } else if (!glpk_oop_is_neg_inf(lb)) {
            fprintf(f, " x%d >= %.17g\n", j + 1, lb);
        } else {
            fprintf(f, " x%d <= %.17g\n", j + 1, ub);
        }
    }

    {
        int has_general = 0;
        int has_binary = 0;

        for (int j = 0; j < lp->num_vars; j++) {
            char vt = lp->var_type ? lp->var_type[j] : 'C';
            if (vt == 'I') has_general = 1;
            else if (vt == 'B') has_binary = 1;
        }

        if (has_general) {
            fprintf(f, "Generals\n");
            for (int j = 0; j < lp->num_vars; j++) {
                char vt = lp->var_type ? lp->var_type[j] : 'C';
                if (vt == 'I') fprintf(f, " x%d\n", j + 1);
            }
        }

        if (has_binary) {
            fprintf(f, "Binary\n");
            for (int j = 0; j < lp->num_vars; j++) {
                char vt = lp->var_type ? lp->var_type[j] : 'C';
                if (vt == 'B') fprintf(f, " x%d\n", j + 1);
            }
        }
    }

    fprintf(f, "End\n");
    fclose(f);
    free(row_start);
    free(row_idx);
    free(row_val);
    return 0;
}

static int glpk_oop_parse_iterations_line(const char *line, int *iters_out) {
    const char *marker;
    const char *end;
    const char *start;
    char buf[32];
    size_t len;
    int iters;

    if (!line || !iters_out) return 0;
    marker = strstr(line, "simplex iterations");
    if (!marker) return 0;

    end = marker;
    while (end > line && isspace((unsigned char)end[-1])) end--;

    start = end;
    while (start > line && isdigit((unsigned char)start[-1])) start--;
    if (start == end) return 0;

    len = (size_t)(end - start);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';

    {
        char *end;
        long val = strtol(buf, &end, 10);
        if (end == buf || *end != '\0') return 0;
        if (val < 0 || val > INT_MAX) return 0;
        iters = (int)val;
    }
    *iters_out = iters;
    return 1;
}

static int glpk_oop_parse_progress_iteration(const char *line, int *iters_out) {
    const char *marker;
    const char *end;
    const char *start;
    char buf[32];
    size_t len;
    int iters;

    if (!line || !iters_out) return 0;
    marker = strstr(line, ": obj");
    if (!marker) return 0;

    end = marker;
    while (end > line && isspace((unsigned char)end[-1])) end--;

    start = end;
    while (start > line && isdigit((unsigned char)start[-1])) start--;
    if (start == end) return 0;

    len = (size_t)(end - start);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';

    {
        char *end;
        long val = strtol(buf, &end, 10);
        if (end == buf || *end != '\0') return 0;
        if (val < 0 || val > INT_MAX) return 0;
        iters = (int)val;
    }
    *iters_out = iters;
    return 1;
}

static GLPKOOPStatus glpk_oop_status_from_line(const char *line) {
    if (!line) return GLPK_OOP_STATUS_UNKNOWN;
    if (strstr(line, "OPTIMAL SOLUTION")) return GLPK_OOP_STATUS_OPTIMAL;
    if (strstr(line, "Status:") && strstr(line, "OPTIMAL")) return GLPK_OOP_STATUS_OPTIMAL;
    if (strstr(line, "PROBLEM HAS NO PRIMAL FEASIBLE SOLUTION")) {
        return GLPK_OOP_STATUS_INFEASIBLE;
    }
    if (strstr(line, "PROBLEM HAS NO DUAL FEASIBLE SOLUTION")) {
        return GLPK_OOP_STATUS_UNBOUNDED;
    }
    if (strstr(line, "Status:") &&
        (strstr(line, "INFEASIBLE") || strstr(line, "NO FEASIBLE"))) {
        return GLPK_OOP_STATUS_INFEASIBLE;
    }
    if (strstr(line, "Status:") && strstr(line, "UNBOUNDED")) {
        return GLPK_OOP_STATUS_UNBOUNDED;
    }
    if (strstr(line, "TIME LIMIT")) return GLPK_OOP_STATUS_TIME_LIMIT;
    if (strstr(line, "ITERATION LIMIT")) return GLPK_OOP_STATUS_ITERATION_LIMIT;
    if (strstr(line, "UNDEFINED")) return GLPK_OOP_STATUS_NUMERICAL;
    return GLPK_OOP_STATUS_UNKNOWN;
}

static int glpk_oop_on_line(const char *line, void *user_data) {
    GLPKOOPRunHints *hints = (GLPKOOPRunHints*)user_data;
    int parsed_iters = 0;
    GLPKOOPStatus st;

    if (!hints || !line) return 0;

    if (glpk_oop_parse_iterations_line(line, &parsed_iters)) {
        hints->iterations = parsed_iters;
    }
    if (glpk_oop_parse_progress_iteration(line, &parsed_iters) &&
        parsed_iters > hints->iterations) {
        hints->iterations = parsed_iters;
    }

    st = glpk_oop_status_from_line(line);
    if (st != GLPK_OOP_STATUS_UNKNOWN) hints->hint_status = st;
    return 0;
}

static int glpk_oop_tokenize(char *line, char **tokens, int capacity) {
    int count = 0;
    char *save = NULL;
    char *tok;

    if (!line || !tokens || capacity <= 0) return 0;
    tok = strtok_r(line, " \t\r\n", &save);
    while (tok && count < capacity) {
        tokens[count++] = tok;
        tok = strtok_r(NULL, " \t\r\n", &save);
    }
    return count;
}

static int glpk_oop_parse_double(const char *s, double *out) {
    char *end = NULL;
    double v;

    if (!s || !out) return -1;
    v = strtod(s, &end);
    if (end == s || !end || *end != '\0') return -1;
    *out = v;
    return 0;
}

static GLPKOOPStatus glpk_oop_status_from_write_text(const char *line) {
    if (!line) return GLPK_OOP_STATUS_UNKNOWN;
    if (strstr(line, "OPTIMAL")) return GLPK_OOP_STATUS_OPTIMAL;
    if (strstr(line, "INFEASIBLE") || strstr(line, "NO FEASIBLE")) {
        return GLPK_OOP_STATUS_INFEASIBLE;
    }
    if (strstr(line, "UNBOUNDED")) return GLPK_OOP_STATUS_UNBOUNDED;
    if (strstr(line, "TIME LIMIT")) return GLPK_OOP_STATUS_TIME_LIMIT;
    if (strstr(line, "ITERATION LIMIT")) return GLPK_OOP_STATUS_ITERATION_LIMIT;
    if (strstr(line, "UNDEFINED")) return GLPK_OOP_STATUS_UNKNOWN;
    return GLPK_OOP_STATUS_UNKNOWN;
}

static GLPKOOPStatus glpk_oop_status_from_basis_flags(char p_stat, char d_stat) {
    if (p_stat == 'f' && d_stat == 'f') return GLPK_OOP_STATUS_OPTIMAL;
    if (p_stat == 'i' || p_stat == 'n') return GLPK_OOP_STATUS_INFEASIBLE;
    if (p_stat == 'f' && (d_stat == 'i' || d_stat == 'n')) return GLPK_OOP_STATUS_UNBOUNDED;
    return GLPK_OOP_STATUS_UNKNOWN;
}

static GLPKOOPStatus glpk_oop_merge_status(GLPKOOPStatus write_status,
                                           GLPKOOPStatus hint_status) {
    if (write_status == GLPK_OOP_STATUS_OPTIMAL) return write_status;
    if (write_status == GLPK_OOP_STATUS_TIME_LIMIT) return write_status;
    if (write_status == GLPK_OOP_STATUS_ITERATION_LIMIT) return write_status;
    if (write_status == GLPK_OOP_STATUS_INFEASIBLE) return write_status;
    if (write_status == GLPK_OOP_STATUS_UNBOUNDED) return write_status;
    if (hint_status != GLPK_OOP_STATUS_UNKNOWN) return hint_status;
    return write_status;
}

static int glpk_oop_parse_write_file(const char *write_file,
                                     int num_rows,
                                     int num_cols,
                                     double *x,
                                     double *y,
                                     double *rc,
                                     double *obj_out,
                                     GLPKOOPStatus *status_out) {
    FILE *f;
    char raw[1024];
    char line[1024];
    char *tok[GLPK_OOP_MAX_TOKENS];
    int cols_seen = 0;
    GLPKOOPStatus status = GLPK_OOP_STATUS_UNKNOWN;

    if (!write_file || !obj_out || !status_out) return -1;
    *obj_out = 0.0;
    *status_out = GLPK_OOP_STATUS_UNKNOWN;
    if (x && num_cols > 0) memset(x, 0, (size_t)num_cols * sizeof(double));
    if (y && num_rows > 0) memset(y, 0, (size_t)num_rows * sizeof(double));
    if (rc && num_cols > 0) memset(rc, 0, (size_t)num_cols * sizeof(double));

    f = fopen(write_file, "r");
    if (!f) return -1;

    while (fgets(raw, sizeof(raw), f)) {
        int ntok;

        if (raw[0] == 'c') {
            if (strstr(raw, "Status:")) {
                GLPKOOPStatus st = glpk_oop_status_from_write_text(raw);
                if (st != GLPK_OOP_STATUS_UNKNOWN) status = st;
            } else if (strstr(raw, "Objective:")) {
                char *eq = strchr(raw, '=');
                if (eq) {
                    double v;
                    char tok_buf[64];
                    char *p = eq + 1;
                    int k = 0;
                    while (*p && isspace((unsigned char)*p)) p++;
                    while (p[k] != '\0' && !isspace((unsigned char)p[k]) &&
                           k < (int)sizeof(tok_buf) - 1) {
                        tok_buf[k] = p[k];
                        k++;
                    }
                    tok_buf[k] = '\0';
                    if (k > 0 && glpk_oop_parse_double(tok_buf, &v) == 0) {
                        *obj_out = v;
                    }
                }
            }
            continue;
        }

        strncpy(line, raw, sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        ntok = glpk_oop_tokenize(line, tok, GLPK_OOP_MAX_TOKENS);
        if (ntok <= 0) continue;

        if (tok[0][0] == 's' && ntok >= 7) {
            if (strcmp(tok[1], "bas") == 0) {
                char p_stat = tok[4][0];
                char d_stat = tok[5][0];
                double v;
                GLPKOOPStatus st = glpk_oop_status_from_basis_flags(p_stat, d_stat);
                if (st != GLPK_OOP_STATUS_UNKNOWN) status = st;
                if (glpk_oop_parse_double(tok[6], &v) == 0) *obj_out = v;
            }
            continue;
        }

        if (tok[0][0] == 'i' && ntok >= 5) {
            char *endp;
            long lval = strtol(tok[1], &endp, 10);
            if (endp == tok[1] || lval < 0 || lval > INT_MAX) continue;
            int idx = (int)lval;
            double dual = 0.0;
            if (idx >= 1 && idx <= num_rows && y) {
                if (glpk_oop_parse_double(tok[4], &dual) == 0) {
                    y[idx - 1] = dual;
                }
            }
            continue;
        }

        if (tok[0][0] == 'j' && ntok >= 5) {
            char *endp;
            long lval = strtol(tok[1], &endp, 10);
            if (endp == tok[1] || lval < 0 || lval > INT_MAX) continue;
            int idx = (int)lval;
            double prim = 0.0;
            double dual = 0.0;
            if (idx >= 1 && idx <= num_cols) {
                if (glpk_oop_parse_double(tok[3], &prim) == 0) {
                    if (x) x[idx - 1] = prim;
                }
                if (glpk_oop_parse_double(tok[4], &dual) == 0) {
                    if (rc) rc[idx - 1] = dual;
                }
                cols_seen++;
            }
            continue;
        }
    }

    fclose(f);
    *status_out = status;
    if (num_cols > 0 && cols_seen == 0 && status == GLPK_OOP_STATUS_OPTIMAL) return -1;
    return 0;
}

static void glpk_oop_solver_clear_solution(SimplexSolver *solver) {
    if (!solver) return;
    free(solver->solution);
    free(solver->dual_solution);
    free(solver->reduced_costs);
    solver->solution = NULL;
    solver->dual_solution = NULL;
    solver->reduced_costs = NULL;
}

static int glpk_oop_solver_set_solution(SimplexSolver *solver,
                                        int m,
                                        int n,
                                        const double *x,
                                        const double *y,
                                        const double *rc) {
    if (!solver) return -1;
    glpk_oop_solver_clear_solution(solver);

    if (n > 0) {
        solver->solution = (double*)calloc((size_t)n, sizeof(double));
        solver->reduced_costs = (double*)calloc((size_t)n, sizeof(double));
        if (!solver->solution || !solver->reduced_costs) return -1;
        if (x) memcpy(solver->solution, x, (size_t)n * sizeof(double));
        if (rc) memcpy(solver->reduced_costs, rc, (size_t)n * sizeof(double));
    }

    if (m > 0) {
        solver->dual_solution = (double*)calloc((size_t)m, sizeof(double));
        if (!solver->dual_solution) return -1;
        if (y) memcpy(solver->dual_solution, y, (size_t)m * sizeof(double));
    }

    return 0;
}

static int glpk_oop_get_capabilities(LPExternalCapabilities *caps, void *user_data) {
    LPExternalGLPKOOPState *state = (LPExternalGLPKOOPState*)user_data;
    if (!caps || !state) return -1;

    memset(caps, 0, sizeof(*caps));
    caps->supports_simplex = 1;
    caps->supports_dual_simplex = 1;
    caps->supports_barrier = 0;
    caps->supports_crossover = 0;
    return 0;
}

static int glpk_oop_solve(LPExternalBackendKind backend,
                          SimplexSolver *solver,
                          void *user_data) {
    LPExternalGLPKOOPState *state = (LPExternalGLPKOOPState*)user_data;
    LPModel *model;
    char model_file[256];
    char write_file[256];
    int m;
    int n;
    double *x = NULL;
    double *y = NULL;
    double *rc = NULL;
    double objective = 0.0;
    GLPKOOPStatus write_status = GLPK_OOP_STATUS_UNKNOWN;
    GLPKOOPStatus merged_status = GLPK_OOP_STATUS_UNKNOWN;
    GLPKOOPRunHints hints;
    LPExternalOOPRunRequest req;
    LPExternalOOPRunResult run_result;
    RalphLPCancelCallback cancel_cb;
    int has_cancel_cb = 0;
    int parse_rc;
    int ret = RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;

    if (!state || !solver || !solver->model || !state->glpsol_path) {
        return RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
    }
    if (backend != LP_EXTERNAL_BACKEND_SIMPLEX &&
        backend != LP_EXTERNAL_BACKEND_DUAL_SIMPLEX) {
        return RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
    }

    model = solver->model;
    if (!model->A) return RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
    m = model->num_cons;
    n = model->num_vars;

    if (lp_external_oop_make_tempfile("ralph_glpk_lp_", model_file, sizeof(model_file)) != 0) {
        return RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
    }
    if (lp_external_oop_make_tempfile("ralph_glpk_write_", write_file, sizeof(write_file)) != 0) {
        lp_external_oop_cleanup_file(model_file);
        return RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
    }

    if (glpk_oop_write_lp(model, model_file) != 0) {
        lp_external_oop_cleanup_file(model_file);
        lp_external_oop_cleanup_file(write_file);
        return RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
    }

    if (n > 0) {
        x = (double*)calloc((size_t)n, sizeof(double));
        rc = (double*)calloc((size_t)n, sizeof(double));
        if (!x || !rc) goto done;
    }
    if (m > 0) {
        y = (double*)calloc((size_t)m, sizeof(double));
        if (!y) goto done;
    }

    memset(&hints, 0, sizeof(hints));
    hints.iterations = 0;
    hints.hint_status = GLPK_OOP_STATUS_UNKNOWN;

    if (solver->has_lp_cancel_callback && solver->lp_cancel_callback.should_cancel) {
        cancel_cb = solver->lp_cancel_callback;
        has_cancel_cb = 1;
    }

    {
        char tmlim_buf[32];
        char *argv[12];
        int argc = 0;

        argv[argc++] = state->glpsol_path;
        if (backend == LP_EXTERNAL_BACKEND_DUAL_SIMPLEX) {
            argv[argc++] = "--dual";
        }
        argv[argc++] = "--lp";
        argv[argc++] = model_file;
        if (solver->time_limit > 0.0 && solver->time_limit < RALPH_INFINITY / 2.0) {
            int tlim = (int)(solver->time_limit + 0.5);
            if (tlim < 1) tlim = 1;
            snprintf(tmlim_buf, sizeof(tmlim_buf), "%d", tlim);
            argv[argc++] = "--tmlim";
            argv[argc++] = tmlim_buf;
        }
        argv[argc++] = "--write";
        argv[argc++] = write_file;
        argv[argc] = NULL;

        memset(&req, 0, sizeof(req));
        req.program = state->glpsol_path;
        req.argv = argv;
        req.wall_time_limit_sec = solver->time_limit;
        req.cancel_cb = has_cancel_cb ? &cancel_cb : NULL;
        req.poll_interval_ms = 10;
        req.on_line = glpk_oop_on_line;
        req.line_user_data = &hints;

        if (lp_external_oop_run(&req, &run_result) != 0) goto done;
    }

    parse_rc = glpk_oop_parse_write_file(write_file,
                                         m,
                                         n,
                                         x,
                                         y,
                                         rc,
                                         &objective,
                                         &write_status);
    if (parse_rc != 0) {
        if (run_result.timed_out || hints.hint_status == GLPK_OOP_STATUS_TIME_LIMIT) {
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_TIME_LIMIT;
        } else if (hints.hint_status == GLPK_OOP_STATUS_ITERATION_LIMIT) {
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_ITERATION_LIMIT;
        } else if (hints.hint_status == GLPK_OOP_STATUS_NUMERICAL) {
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_NUMERICAL_FAILURE;
        } else {
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
        }
        goto done;
    }

    merged_status = glpk_oop_merge_status(write_status, hints.hint_status);
    if (run_result.timed_out || merged_status == GLPK_OOP_STATUS_TIME_LIMIT) {
        ret = RALPH_LP_EXTERNAL_ADAPTER_RC_TIME_LIMIT;
        goto done;
    }
    if (run_result.cancelled) {
        ret = RALPH_LP_EXTERNAL_ADAPTER_RC_TIME_LIMIT;
        goto done;
    }
    if (merged_status == GLPK_OOP_STATUS_ITERATION_LIMIT) {
        ret = RALPH_LP_EXTERNAL_ADAPTER_RC_ITERATION_LIMIT;
        goto done;
    }
    if (merged_status == GLPK_OOP_STATUS_NUMERICAL) {
        ret = RALPH_LP_EXTERNAL_ADAPTER_RC_NUMERICAL_FAILURE;
        goto done;
    }

    solver->iterations = hints.iterations;
    /* Keep objective semantics aligned with internal simplex/dual paths:
     * user-space objective includes the model constant term. */
    solver->obj_value = objective + (model ? model->obj_offset : 0.0);

    switch (merged_status) {
        case GLPK_OOP_STATUS_OPTIMAL:
            solver->status = RALPH_STATUS_OPTIMAL;
            if (glpk_oop_solver_set_solution(solver, m, n, x, y, rc) != 0) {
                ret = RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
                goto done;
            }
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_OK;
            break;
        case GLPK_OOP_STATUS_INFEASIBLE:
            solver->status = RALPH_STATUS_INFEASIBLE;
            glpk_oop_solver_clear_solution(solver);
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_OK;
            break;
        case GLPK_OOP_STATUS_UNBOUNDED:
            solver->status = RALPH_STATUS_UNBOUNDED;
            glpk_oop_solver_clear_solution(solver);
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_OK;
            break;
        case GLPK_OOP_STATUS_UNKNOWN:
        default:
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
            break;
    }

done:
    free(x);
    free(y);
    free(rc);
    lp_external_oop_cleanup_file(model_file);
    lp_external_oop_cleanup_file(write_file);
    return ret;
}

static void glpk_oop_destroy_user_data(void *user_data) {
    LPExternalGLPKOOPState *state = (LPExternalGLPKOOPState*)user_data;
    if (!state) return;
    free(state->glpsol_path);
    free(state);
}

int lp_external_glpk_oop_register(const char *glpsol_path) {
    LPExternalGLPKOOPState *state;
    LPExternalAdapter adapter;
    const char *bin = glpsol_path ? glpsol_path : GLPK_OOP_DEFAULT_BIN;

    if (bin[0] == '\0') return -1;

    state = (LPExternalGLPKOOPState*)calloc(1, sizeof(*state));
    if (!state) return -1;

    state->glpsol_path = strdup(bin);
    if (!state->glpsol_path) {
        free(state);
        return -1;
    }

    memset(&adapter, 0, sizeof(adapter));
    adapter.abi_version = LP_EXTERNAL_ADAPTER_ABI_VERSION;
    adapter.provider = LP_EXTERNAL_PROVIDER_GLPK;
    adapter.provider_name = "GLPK (out-of-process)";
    adapter.get_capabilities = glpk_oop_get_capabilities;
    adapter.solve = glpk_oop_solve;
    adapter.user_data = state;
    adapter.destroy_user_data = glpk_oop_destroy_user_data;

    if (lp_external_adapter_register(&adapter) != 0) {
        glpk_oop_destroy_user_data(state);
        return -1;
    }
    return 0;
}

int lp_external_glpk_oop_unregister(void) {
    return lp_external_adapter_unregister(LP_EXTERNAL_PROVIDER_GLPK);
}
