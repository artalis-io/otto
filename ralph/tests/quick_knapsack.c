#include <stdio.h>
#include <stdlib.h>
#include "ralph_test_mod_api.h"

int main(void) {
    printf("Knapsack test\n");
    printf("max 5x1 + 4x2 + 3x3 s.t. 2x1 + 3x2 + x3 <= 5, x binary\n\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);

    ralph_test_add_var(model, 0, 1, 5.0, RALPH_BINARY);
    ralph_test_add_var(model, 0, 1, 4.0, RALPH_BINARY);
    ralph_test_add_var(model, 0, 1, 3.0, RALPH_BINARY);

    int idx[] = {0, 1, 2};
    double val[] = {2.0, 3.0, 1.0};
    ralph_test_add_constraint(model, 3, idx, val, RALPH_LESS_EQUAL, 5.0);

    printf("Calling optimize...\n");
    ralph_test_set_int_param(model, "verbose", 1);
    ralph_test_optimize(model);

    printf("Status: %d (1=optimal)\n", ralph_test_get_status(model));
    printf("Objective: %f\n", ralph_test_get_objval(model));

    double x[3];
    ralph_test_get_solution(model, x);
    printf("Solution: x1=%.0f, x2=%.0f, x3=%.0f\n", x[0], x[1], x[2]);

    ralph_test_free(model);
    printf("Done\n");
    return 0;
}
