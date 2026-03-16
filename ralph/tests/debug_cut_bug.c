/*
 * Debug: reproduce the cut_normalize bug on 10-var binary knapsack
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ralph_test_mod_api.h"

int main(void) {
    double obj_c[]  = {-16, -22, -12, -8, -11, -19, -7, -14, -9, -13};
    double a1[]     = { 5,   7,   4,  3,   6,   2,  8,   4,  3,   6};
    double a2[]     = { 3,   2,   6,  5,   1,   7,  2,   5,  4,   3};
    double a3[]     = { 4,   5,   3,  6,   4,   3,  5,   7,  2,   4};
    int n = 10;
    int idx[10];
    for (int j = 0; j < n; j++) idx[j] = j;

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "verbose", 2);
    ralph_test_set_int_param(model, "max_cut_rounds", 1);
    ralph_test_set_int_param(model, "max_nodes", 10000);

    for (int j = 0; j < n; j++)
        ralph_test_add_var(model, 0.0, 1.0, obj_c[j], RALPH_BINARY);
    ralph_test_add_constraint(model, n, idx, a1, RALPH_LESS_EQUAL, 15.0);
    ralph_test_add_constraint(model, n, idx, a2, RALPH_LESS_EQUAL, 12.0);
    ralph_test_add_constraint(model, n, idx, a3, RALPH_LESS_EQUAL, 18.0);

    ralph_test_optimize(model);

    printf("\n=== RESULT ===\n");
    printf("Status: %d\n", ralph_test_get_status(model));
    printf("Objective: %.6f\n", ralph_test_get_objval(model));
    printf("Nodes: %d\n", ralph_test_get_node_count(model));

    ralph_test_free(model);
    return 0;
}
