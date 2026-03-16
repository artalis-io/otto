/*
 * Benchmark supernodal LU on NETLIB instances.
 * Runs each problem with and without supernodal, comparing time and correctness.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include "ralph_test_mod_api.h"
#include "lp.h"

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

typedef struct {
    const char *name;
    const char *path;
} Problem;

static int find_mps_files(const char *dir, Problem *probs, int max_probs) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < max_probs) {
        const char *ext = strrchr(ent->d_name, '.');
        if (ext && strcmp(ext, ".mps") == 0) {
            char *path = malloc(strlen(dir) + strlen(ent->d_name) + 2);
            sprintf(path, "%s/%s", dir, ent->d_name);
            char *name = strdup(ent->d_name);
            char *dot = strrchr(name, '.');
            if (dot) *dot = '\0';
            probs[n].name = name;
            probs[n].path = path;
            n++;
        }
    }
    closedir(d);
    return n;
}

static int cmp_name(const void *a, const void *b) {
    return strcmp(((const Problem*)a)->name, ((const Problem*)b)->name);
}

int main(int argc, char **argv) {
    const char *netlib_dir = "benchmarks/netlib";
    int num_runs = 3;  /* Average over 3 runs */

    if (argc > 1) netlib_dir = argv[1];

    Problem probs[200];
    int nprobs = find_mps_files(netlib_dir, probs, 200);
    if (nprobs == 0) {
        fprintf(stderr, "No .mps files found in %s\n", netlib_dir);
        return 1;
    }
    qsort(probs, nprobs, sizeof(Problem), cmp_name);

    printf("%-14s %6s %6s  %8s %8s  %6s  %5s %5s  %-8s %-8s\n",
           "Problem", "Vars", "Cons", "Base(ms)", "SN(ms)", "Ratio",
           "SNCal", "SNOk", "BaseObj", "SN_Obj");
    printf("%-14s %6s %6s  %8s %8s  %6s  %5s %5s  %-8s %-8s\n",
           "-------", "----", "----", "--------", "------", "-----",
           "-----", "----", "-------", "------");

    for (int p = 0; p < nprobs; p++) {
        double base_time = 0, sn_time = 0;
        double base_obj = 0, sn_obj = 0;
        int base_status = -1, sn_status = -1;
        int sn_calls = 0, sn_successes = 0;
        int num_vars = 0, num_cons = 0;

        /* --- Baseline (no supernodal) --- */
        for (int run = 0; run < num_runs; run++) {
            RalphModel *model = ralph_test_create();
            if (ralph_test_read_mps(model, probs[p].path) != 0) {
                ralph_test_free(model);
                goto next;
            }
            if (run == 0) {
                num_vars = ralph_test_get_num_vars(model);
                num_cons = ralph_test_get_num_cons(model);
            }
            ralph_test_set_int_param(model, "verbose", 0);
            ralph_test_set_int_param(model, "presolve", 1);
            ralph_test_set_int_param(model, "verify", 1);
            ralph_test_set_dbl_param(model, "time_limit", 30.0);

            double t0 = get_time_ms();
            ralph_test_optimize(model);
            double t1 = get_time_ms();

            if (run == 0) {
                base_status = ralph_test_get_status(model);
                base_obj = ralph_test_get_objval(model);
            }
            base_time += (t1 - t0);
            ralph_test_free(model);
        }
        base_time /= num_runs;

        /* Skip if baseline didn't solve optimally */
        if (base_status != RALPH_STATUS_OPTIMAL &&
            base_status != RALPH_STATUS_IMPRECISE) {
            printf("%-14s %6d %6d  %8.1f %8s  %6s  %5s %5s  status=%d\n",
                   probs[p].name, num_vars, num_cons,
                   base_time, "skip", "-", "-", "-", base_status);
            goto next;
        }

        /* --- Supernodal --- */
        for (int run = 0; run < num_runs; run++) {
            RalphModel *model = ralph_test_create();
            ralph_test_read_mps(model, probs[p].path);
            ralph_test_set_int_param(model, "verbose", 0);
            ralph_test_set_int_param(model, "presolve", 1);
            ralph_test_set_int_param(model, "verify", 1);
            ralph_test_set_int_param(model, "lu_supernode", 1);
            ralph_test_set_dbl_param(model, "time_limit", 30.0);

            double t0 = get_time_ms();
            ralph_test_optimize(model);
            double t1 = get_time_ms();

            if (run == 0) {
                sn_status = ralph_test_get_status(model);
                sn_obj = ralph_test_get_objval(model);
                /* Read supernodal counters from the LU struct */
                /* Access internal: model->lp_solver->tableau->lu */
                struct RalphModel *m = (struct RalphModel *)model;
                if (m->lp_solver && m->lp_solver->tableau &&
                    m->lp_solver->tableau->lu) {
                    sn_calls = m->lp_solver->tableau->lu->sn_calls;
                    sn_successes = m->lp_solver->tableau->lu->sn_successes;
                }
            }
            sn_time += (t1 - t0);
            ralph_test_free(model);
        }
        sn_time /= num_runs;

        double ratio = (base_time > 0.01) ? sn_time / base_time : 0;
        double obj_err = (base_obj != 0) ? fabs((sn_obj - base_obj) / base_obj) : fabs(sn_obj - base_obj);

        printf("%-14s %6d %6d  %8.1f %8.1f  %5.2fx  %5d %5d",
               probs[p].name, num_vars, num_cons,
               base_time, sn_time, ratio, sn_calls, sn_successes);

        if (sn_status != base_status) {
            printf("  STATUS_MISMATCH(%d vs %d)", base_status, sn_status);
        } else if (obj_err > 1e-6) {
            printf("  OBJ_ERR=%.2e", obj_err);
        } else {
            printf("  OK");
        }
        printf("\n");

next:;
    }

    /* Cleanup */
    for (int i = 0; i < nprobs; i++) {
        free((void*)probs[i].name);
        free((void*)probs[i].path);
    }
    return 0;
}
