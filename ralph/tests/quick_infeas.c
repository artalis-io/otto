#include <stdio.h>
#include <stdlib.h>
#include "ralph_test_mod_api.h"

int main(void) {
    printf("Infeasible LP test\n");
    printf("min x s.t. x <= 1, x >= 2\n\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);

    int idx[] = {0};
    double val[] = {1.0};
    ralph_test_add_constraint(model, 1, idx, val, RALPH_LESS_EQUAL, 1.0);
    ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 2.0);

    printf("Calling optimize...\n");
    ralph_test_optimize(model);

    printf("Status: %d (2=infeasible)\n", ralph_test_get_status(model));
    printf("Objective: %f\n", ralph_test_get_objval(model));

    ralph_test_free(model);
    printf("Done\n");
    return 0;
}
