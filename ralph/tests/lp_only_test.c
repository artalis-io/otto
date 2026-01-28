#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ralph.h"

int main(void) {
    printf("LP-only tests\n\n");

    /* Test 1: Simple LP */
    printf("Test 1: Simple 2-var LP\n");
    {
        RalphModel *model = ralph_create();
        ralph_set_obj_sense(model, RALPH_MINIMIZE);
        ralph_add_var(model, 0.0, 1e30, -1.0, RALPH_CONTINUOUS);
        ralph_add_var(model, 0.0, 1e30, -1.0, RALPH_CONTINUOUS);
        int idx[] = {0, 1};
        double val1[] = {1.0, 1.0};
        ralph_add_constraint(model, 2, idx, val1, RALPH_LESS_EQUAL, 4.0);
        double val2[] = {2.0, 1.0};
        ralph_add_constraint(model, 2, idx, val2, RALPH_LESS_EQUAL, 6.0);
        ralph_optimize(model);
        printf("  Status: %d, Obj: %.2f\n", ralph_get_status(model), ralph_get_objval(model));
        ralph_free(model);
    }

    /* Test 2: Equality constraint */
    printf("Test 2: Equality constraint\n");
    {
        RalphModel *model = ralph_create();
        ralph_set_obj_sense(model, RALPH_MINIMIZE);
        ralph_add_var(model, 1.0, 1e30, 1.0, RALPH_CONTINUOUS);
        ralph_add_var(model, 0.0, 1e30, 2.0, RALPH_CONTINUOUS);
        int idx[] = {0, 1};
        double val[] = {1.0, 1.0};
        ralph_add_constraint(model, 2, idx, val, RALPH_EQUAL, 3.0);
        ralph_optimize(model);
        printf("  Status: %d, Obj: %.2f\n", ralph_get_status(model), ralph_get_objval(model));
        ralph_free(model);
    }

    /* Test 3: >= constraint */
    printf("Test 3: >= constraint\n");
    {
        RalphModel *model = ralph_create();
        ralph_set_obj_sense(model, RALPH_MAXIMIZE);
        ralph_add_var(model, 0.0, 2.0, 2.0, RALPH_CONTINUOUS);
        ralph_add_var(model, 0.0, 2.0, 3.0, RALPH_CONTINUOUS);
        int idx[] = {0, 1};
        double val[] = {1.0, 1.0};
        ralph_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 1.0);
        ralph_optimize(model);
        printf("  Status: %d, Obj: %.2f\n", ralph_get_status(model), ralph_get_objval(model));
        ralph_free(model);
    }

    /* Test 4: Diet problem */
    printf("Test 4: Diet problem (3 vars, 3 >= constraints)\n");
    {
        RalphModel *model = ralph_create();
        ralph_set_obj_sense(model, RALPH_MINIMIZE);
        ralph_add_var(model, 0, 1e30, 2.0, RALPH_CONTINUOUS);
        ralph_add_var(model, 0, 1e30, 3.5, RALPH_CONTINUOUS);
        ralph_add_var(model, 0, 1e30, 8.0, RALPH_CONTINUOUS);
        int idx1[] = {0, 1, 2};
        double val1[] = {50, 42, 35};
        ralph_add_constraint(model, 3, idx1, val1, RALPH_GREATER_EQUAL, 300);
        double val2[] = {4, 8, 7};
        ralph_add_constraint(model, 3, idx1, val2, RALPH_GREATER_EQUAL, 10);
        double val3[] = {0, 3, 2};
        ralph_add_constraint(model, 3, idx1, val3, RALPH_GREATER_EQUAL, 8);
        ralph_optimize(model);
        printf("  Status: %d, Obj: %.2f\n", ralph_get_status(model), ralph_get_objval(model));
        ralph_free(model);
    }

    /* Test 5: Infeasible LP */
    printf("Test 5: Infeasible LP\n");
    {
        RalphModel *model = ralph_create();
        ralph_set_obj_sense(model, RALPH_MINIMIZE);
        ralph_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);
        int idx[] = {0};
        double val[] = {1.0};
        ralph_add_constraint(model, 1, idx, val, RALPH_LESS_EQUAL, 1.0);
        ralph_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 2.0);
        ralph_optimize(model);
        printf("  Status: %d (should be 2=infeasible)\n", ralph_get_status(model));
        ralph_free(model);
    }

    /* Test 6: Larger LP */
    printf("Test 6: Larger LP (20 vars, 10 constraints)\n");
    {
        RalphModel *model = ralph_create();
        ralph_set_obj_sense(model, RALPH_MINIMIZE);
        int n = 20, m = 10;
        for (int j = 0; j < n; j++) {
            ralph_add_var(model, 0, 1e30, (double)(j + 1), RALPH_CONTINUOUS);
        }
        int *indices = (int*)malloc(n * sizeof(int));
        double *values = (double*)malloc(n * sizeof(double));
        for (int i = 0; i < m; i++) {
            int nnz = 0;
            for (int j = 0; j < n; j++) {
                if ((i + j) % 3 == 0) {
                    indices[nnz] = j;
                    values[nnz] = 1.0 + (double)((i * j) % 5);
                    nnz++;
                }
            }
            if (nnz > 0) {
                ralph_add_constraint(model, nnz, indices, values, RALPH_LESS_EQUAL, 10.0 + i);
            }
        }
        free(indices);
        free(values);
        ralph_optimize(model);
        printf("  Status: %d, Obj: %.2f\n", ralph_get_status(model), ralph_get_objval(model));
        ralph_free(model);
    }

    printf("\nAll LP tests completed.\n");
    return 0;
}
