#include <stdio.h>
#include <stdlib.h>
#include "ralph_test_mod_api.h"

int main(void) {
    printf("Quick test: >= constraint\n");
    printf("Minimize 2*x subject to x >= 5\n");
    printf("Expected: x = 5, obj = 10\n\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, 1);  /* minimize */

    ralph_test_add_var(model, 0.0, 1e30, 2.0, 'C');  /* x with obj coef 2 */

    int idx[] = {0};
    double val[] = {1.0};
    ralph_test_add_constraint(model, 1, idx, val, 'G', 5.0);  /* x >= 5 */

    printf("Calling optimize...\n");
    int status = ralph_test_optimize(model);
    printf("Optimize returned %d\n", status);

    printf("Status: %d (1=optimal)\n", ralph_test_get_status(model));
    printf("Objective: %f (expected 10)\n", ralph_test_get_objval(model));

    double x[1];
    ralph_test_get_solution(model, x);
    printf("Solution: x=%f (expected 5)\n", x[0]);

    ralph_test_free(model);
    printf("Done\n");
    return 0;
}
