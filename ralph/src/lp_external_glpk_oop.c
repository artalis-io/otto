#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "lp_external_adapter.h"

#define GLPK_OOP_DEFAULT_BIN "glpsol"

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

static int glpk_oop_is_neg_inf(double x) {
    return x <= -RALPH_INFINITY / 2.0;
}

static int glpk_oop_is_pos_inf(double x) {
    return x >= RALPH_INFINITY / 2.0;
}

static int glpk_oop_write_mps(const LPModel *lp, const char *filename) {
    FILE *f;
    int in_integer = 0;
    int marker_count = 0;

    if (!lp || !lp->A || !filename) return -1;
    f = fopen(filename, "w");
    if (!f) return -1;

    fprintf(f, "NAME          RALPH\n");
    fprintf(f, "OBJSENSE\n");
    fprintf(f, " %s\n", lp->obj_sense == -1 ? "MAX" : "MIN");

    fprintf(f, "ROWS\n");
    fprintf(f, "N OBJ\n");
    for (int i = 0; i < lp->num_cons; i++) {
        char sense = 'E';
        if (lp->sense) {
            if (lp->sense[i] == 'L' || lp->sense[i] == 'G' || lp->sense[i] == 'E') {
                sense = lp->sense[i];
            }
        }
        fprintf(f, "%c R%d\n", sense, i + 1);
    }

    fprintf(f, "COLUMNS\n");
    for (int j = 0; j < lp->num_vars; j++) {
        char var_type = lp->var_type ? lp->var_type[j] : 'C';
        int is_integer = (var_type == 'I' || var_type == 'B');

        if (is_integer && !in_integer) {
            fprintf(f, "MARK%04d 'MARKER' 'INTORG'\n", marker_count++);
            in_integer = 1;
        } else if (!is_integer && in_integer) {
            fprintf(f, "MARK%04d 'MARKER' 'INTEND'\n", marker_count++);
            in_integer = 0;
        }

        if (lp->c && lp->c[j] != 0.0) {
            fprintf(f, "X%d OBJ %.17g\n", j + 1, lp->c[j]);
        }
        for (int p = lp->A->colptr[j]; p < lp->A->colptr[j + 1]; p++) {
            int i = lp->A->rowidx[p];
            double val = lp->A->values[p];
            if (val == 0.0) continue;
            fprintf(f, "X%d R%d %.17g\n", j + 1, i + 1, val);
        }
    }
    if (in_integer) {
        fprintf(f, "MARK%04d 'MARKER' 'INTEND'\n", marker_count++);
    }

    fprintf(f, "RHS\n");
    if (lp->obj_offset != 0.0) {
        fprintf(f, "RHS1 OBJ %.17g\n", lp->obj_offset);
    }
    if (lp->b) {
        for (int i = 0; i < lp->num_cons; i++) {
            if (lp->b[i] == 0.0) continue;
            fprintf(f, "RHS1 R%d %.17g\n", i + 1, lp->b[i]);
        }
    }

    fprintf(f, "BOUNDS\n");
    for (int j = 0; j < lp->num_vars; j++) {
        char var_type = lp->var_type ? lp->var_type[j] : 'C';
        double lb = lp->lb ? lp->lb[j] : 0.0;
        double ub = lp->ub ? lp->ub[j] : RALPH_INFINITY;

        if (var_type == 'B') {
            fprintf(f, "BV BND1 X%d\n", j + 1);
            continue;
        }
        if (glpk_oop_is_neg_inf(lb) && glpk_oop_is_pos_inf(ub)) {
            fprintf(f, "FR BND1 X%d\n", j + 1);
            continue;
        }
        if (!glpk_oop_is_neg_inf(lb) && !glpk_oop_is_pos_inf(ub) && lb == ub) {
            fprintf(f, "FX BND1 X%d %.17g\n", j + 1, lb);
            continue;
        }
        if (glpk_oop_is_neg_inf(lb)) {
            fprintf(f, "MI BND1 X%d\n", j + 1);
        } else if (lb != 0.0) {
            fprintf(f, "LO BND1 X%d %.17g\n", j + 1, lb);
        }
        if (!glpk_oop_is_pos_inf(ub)) {
            fprintf(f, "UP BND1 X%d %.17g\n", j + 1, ub);
        }
    }

    fprintf(f, "ENDATA\n");
    fclose(f);
    return 0;
}

static int glpk_oop_make_temp(char *path, size_t path_size, const char *prefix) {
    int fd;

    if (!path || path_size < 32 || !prefix) return -1;
    snprintf(path, path_size, "/tmp/%sXXXXXX", prefix);
    fd = mkstemp(path);
    if (fd < 0) return -1;
    close(fd);
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

    iters = atoi(buf);
    if (iters < 0) return 0;
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

    iters = atoi(buf);
    if (iters < 0) return 0;
    *iters_out = iters;
    return 1;
}

static GLPKOOPStatus glpk_oop_status_from_line(const char *line) {
    if (!line) return GLPK_OOP_STATUS_UNKNOWN;
    if (strstr(line, "OPTIMAL")) return GLPK_OOP_STATUS_OPTIMAL;
    if (strstr(line, "INFEASIBLE") || strstr(line, "NO FEASIBLE")) {
        return GLPK_OOP_STATUS_INFEASIBLE;
    }
    if (strstr(line, "UNBOUNDED")) return GLPK_OOP_STATUS_UNBOUNDED;
    if (strstr(line, "TIME LIMIT")) return GLPK_OOP_STATUS_TIME_LIMIT;
    if (strstr(line, "ITERATION LIMIT")) return GLPK_OOP_STATUS_ITERATION_LIMIT;
    if (strstr(line, "UNDEFINED")) return GLPK_OOP_STATUS_NUMERICAL;
    return GLPK_OOP_STATUS_UNKNOWN;
}

static int glpk_oop_parse_solution_file(const char *sol_file,
                                        int num_vars,
                                        double *x,
                                        double *obj_out,
                                        GLPKOOPStatus *status_out) {
    FILE *f;
    char line[1024];
    int in_columns = 0;
    int parsed_cols = 0;
    GLPKOOPStatus status = GLPK_OOP_STATUS_UNKNOWN;

    if (!sol_file || !obj_out || !status_out) return -1;
    *obj_out = 0.0;
    *status_out = GLPK_OOP_STATUS_UNKNOWN;

    f = fopen(sol_file, "r");
    if (!f) return -1;

    while (fgets(line, sizeof(line), f)) {
        GLPKOOPStatus parsed_status;

        if (strstr(line, "Status:")) {
            parsed_status = glpk_oop_status_from_line(line);
            if (parsed_status != GLPK_OOP_STATUS_UNKNOWN) status = parsed_status;
        }

        if (strstr(line, "Objective:")) {
            char *eq = strchr(line, '=');
            if (eq) {
                *obj_out = atof(eq + 1);
            }
        }

        if (strstr(line, "Column name") && strstr(line, "Activity")) {
            if (fgets(line, sizeof(line), f)) {
                /* Skip dashed separator line. */
            }
            in_columns = 1;
            continue;
        }

        if (in_columns && (line[0] == '\n' || line[0] == '\r' || line[0] == '\0')) {
            break;
        }
        if (in_columns && strstr(line, "Karush-Kuhn-Tucker")) {
            break;
        }

        if (in_columns && x && num_vars > 0) {
            int col_num = 0;
            char col_name[256];
            char col_status[8];
            double activity = 0.0;
            int n = sscanf(line, " %d %255s %7s %lf",
                           &col_num, col_name, col_status, &activity);
            if (n >= 3 && col_num >= 1 && col_num <= num_vars) {
                if (n < 4) activity = 0.0;
                x[col_num - 1] = activity;
                parsed_cols++;
            }
        }
    }

    fclose(f);
    *status_out = status;

    if (num_vars > 0 && status == GLPK_OOP_STATUS_OPTIMAL && parsed_cols <= 0) {
        return -1;
    }
    if (status == GLPK_OOP_STATUS_UNKNOWN) {
        return -1;
    }
    return 0;
}

static int glpk_oop_run_glpsol(const LPExternalGLPKOOPState *state,
                               LPExternalBackendKind backend,
                               const char *mps_file,
                               const char *sol_file,
                               double time_limit_sec,
                               int *iterations_out,
                               GLPKOOPStatus *hint_status_out) {
    int pipefd[2] = {-1, -1};
    pid_t pid;
    FILE *stream;
    char line[1024];
    int iterations = 0;
    GLPKOOPStatus hint_status = GLPK_OOP_STATUS_UNKNOWN;
    int wstatus = 0;
    pid_t wait_rc;
    char tmlim_buf[32];
    char *argv[12];
    int argc = 0;
    int tmlim_int = 0;

    if (!state || !state->glpsol_path || !mps_file || !sol_file) return -1;
    if (!iterations_out || !hint_status_out) return -1;
    *iterations_out = 0;
    *hint_status_out = GLPK_OOP_STATUS_UNKNOWN;

    if (pipe(pipefd) != 0) return -1;

    argv[argc++] = state->glpsol_path;
    if (backend == LP_EXTERNAL_BACKEND_DUAL_SIMPLEX) {
        argv[argc++] = "--dual";
    }
    argv[argc++] = "--mps";
    argv[argc++] = (char*)mps_file;
    if (time_limit_sec > 0.0 && time_limit_sec < RALPH_INFINITY / 2.0) {
        tmlim_int = (int)(time_limit_sec + 0.5);
        if (tmlim_int < 1) tmlim_int = 1;
        snprintf(tmlim_buf, sizeof(tmlim_buf), "%d", tmlim_int);
        argv[argc++] = "--tmlim";
        argv[argc++] = tmlim_buf;
    }
    argv[argc++] = "-o";
    argv[argc++] = (char*)sol_file;
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(state->glpsol_path, argv);
        _exit(127);
    }

    close(pipefd[1]);
    stream = fdopen(pipefd[0], "r");
    if (stream) {
        while (fgets(line, sizeof(line), stream)) {
            int parsed_iters = 0;
            GLPKOOPStatus parsed_status;

            if (glpk_oop_parse_iterations_line(line, &parsed_iters)) {
                iterations = parsed_iters;
            }
            if (glpk_oop_parse_progress_iteration(line, &parsed_iters) &&
                parsed_iters > iterations) {
                iterations = parsed_iters;
            }

            parsed_status = glpk_oop_status_from_line(line);
            if (parsed_status != GLPK_OOP_STATUS_UNKNOWN) {
                hint_status = parsed_status;
            }
        }
        fclose(stream);
    } else {
        close(pipefd[0]);
    }

    do {
        wait_rc = waitpid(pid, &wstatus, 0);
    } while (wait_rc < 0 && errno == EINTR);
    if (wait_rc < 0) return -1;

    *iterations_out = iterations;
    *hint_status_out = hint_status;

    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) return 0;
    return -1;
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
    char sol_file[256];
    int run_rc = -1;
    int parse_rc = -1;
    int iterations = 0;
    double objective = 0.0;
    GLPKOOPStatus parsed_status = GLPK_OOP_STATUS_UNKNOWN;
    GLPKOOPStatus hint_status = GLPK_OOP_STATUS_UNKNOWN;
    double *x = NULL;
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

    if (glpk_oop_make_temp(model_file, sizeof(model_file), "ralph_glpk_model_") != 0) {
        return RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
    }
    if (glpk_oop_make_temp(sol_file, sizeof(sol_file), "ralph_glpk_sol_") != 0) {
        unlink(model_file);
        return RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
    }

    if (glpk_oop_write_mps(model, model_file) != 0) {
        unlink(model_file);
        unlink(sol_file);
        return RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
    }

    x = (double*)calloc((size_t)model->num_vars, sizeof(double));
    if (model->num_vars > 0 && !x) {
        unlink(model_file);
        unlink(sol_file);
        return RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
    }

    run_rc = glpk_oop_run_glpsol(state,
                                 backend,
                                 model_file,
                                 sol_file,
                                 solver->time_limit,
                                 &iterations,
                                 &hint_status);
    parse_rc = glpk_oop_parse_solution_file(sol_file,
                                            model->num_vars,
                                            x,
                                            &objective,
                                            &parsed_status);
    unlink(model_file);
    unlink(sol_file);

    if (parse_rc != 0) {
        if (hint_status == GLPK_OOP_STATUS_TIME_LIMIT) {
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_TIME_LIMIT;
        } else if (hint_status == GLPK_OOP_STATUS_ITERATION_LIMIT) {
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_ITERATION_LIMIT;
        } else {
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
        }
        goto done;
    }

    if (run_rc != 0 && parsed_status == GLPK_OOP_STATUS_UNKNOWN) {
        ret = RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
        goto done;
    }

    solver->iterations = iterations;
    solver->obj_value = objective;
    glpk_oop_solver_clear_solution(solver);

    switch (parsed_status) {
        case GLPK_OOP_STATUS_OPTIMAL:
            solver->status = RALPH_STATUS_OPTIMAL;
            if (model->num_vars > 0) {
                solver->solution = (double*)calloc((size_t)model->num_vars, sizeof(double));
                if (!solver->solution) {
                    ret = RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
                    goto done;
                }
                memcpy(solver->solution, x, (size_t)model->num_vars * sizeof(double));
            }
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_OK;
            break;
        case GLPK_OOP_STATUS_INFEASIBLE:
            solver->status = RALPH_STATUS_INFEASIBLE;
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_OK;
            break;
        case GLPK_OOP_STATUS_UNBOUNDED:
            solver->status = RALPH_STATUS_UNBOUNDED;
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_OK;
            break;
        case GLPK_OOP_STATUS_TIME_LIMIT:
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_TIME_LIMIT;
            break;
        case GLPK_OOP_STATUS_ITERATION_LIMIT:
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_ITERATION_LIMIT;
            break;
        case GLPK_OOP_STATUS_NUMERICAL:
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_NUMERICAL_FAILURE;
            break;
        case GLPK_OOP_STATUS_UNKNOWN:
        default:
            ret = RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR;
            break;
    }

done:
    free(x);
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
