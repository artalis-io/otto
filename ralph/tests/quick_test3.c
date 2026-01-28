#include <stdio.h>
#include <stdlib.h>
#include "ralph.h"

int main(void) {
    printf("Quick test: Equality constraint\n");
    printf("Maximize x + 2y subject to:\n");
    printf("  x + y = 3\n");
    printf("  x, y >= 0\n");
    printf("Expected: x=0, y=3, obj=6\n\n");

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, -1);  /* maximize */

    ralph_add_var(model, 0.0, 1e30, 1.0, 'C');  /* x */
    ralph_add_var(model, 0.0, 1e30, 2.0, 'C');  /* y */

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_add_constraint(model, 2, idx, val, 'E', 3.0);  /* x + y = 3 */

    printf("Calling optimize...\n");
    int status = ralph_optimize(model);
    printf("Optimize returned %d\n", status);

    printf("Status: %d (1=optimal)\n", ralph_get_status(model));
    printf("Objective: %f (expected 6)\n", ralph_get_objval(model));

    double x[2];
    ralph_get_solution(model, x);
    printf("Solution: x=%f (expected 0), y=%f (expected 3)\n", x[0], x[1]);

    ralph_free(model);
    printf("Done\n");
    return 0;
}
