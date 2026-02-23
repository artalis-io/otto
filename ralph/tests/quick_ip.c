#include <stdio.h>
#include <stdlib.h>
#include "ralph_test_mod_api.h"

int main(void) {
    printf("Integer programming test\n");
    printf("min x + y s.t. x + y >= 3.5, x,y integer >= 0\n\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_INTEGER);
    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_INTEGER);

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 3.5);

    printf("Calling optimize...\n");
    ralph_test_set_int_param(model, "verbose", 1);
    ralph_test_optimize(model);

    printf("Status: %d (1=optimal)\n", ralph_test_get_status(model));
    printf("Objective: %f (expected 4)\n", ralph_test_get_objval(model));

    double x[2];
    ralph_test_get_solution(model, x);
    printf("Solution: x=%.0f, y=%.0f\n", x[0], x[1]);

    ralph_test_free(model);
    printf("Done\n");
    return 0;
}
