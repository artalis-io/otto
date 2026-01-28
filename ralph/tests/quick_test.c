#include <stdio.h>
#include <stdlib.h>
#include "ralph.h"

int main(void) {
    printf("Quick test: Simple 2-var LP\n");

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, -1);  /* maximize */

    ralph_add_var(model, 0.0, 1e30, 1.0, 'C');  /* x1 */
    ralph_add_var(model, 0.0, 1e30, 1.0, 'C');  /* x2 */

    int idx[] = {0, 1};
    double val1[] = {1.0, 1.0};
    ralph_add_constraint(model, 2, idx, val1, 'L', 4.0);  /* x1 + x2 <= 4 */
    double val2[] = {2.0, 1.0};
    ralph_add_constraint(model, 2, idx, val2, 'L', 6.0);  /* 2x1 + x2 <= 6 */

    printf("Calling optimize...\n");
    int status = ralph_optimize(model);
    printf("Optimize returned %d\n", status);

    printf("Status: %d\n", ralph_get_status(model));
    printf("Objective: %f\n", ralph_get_objval(model));

    double x[2];
    ralph_get_solution(model, x);
    printf("Solution: x[0]=%f, x[1]=%f\n", x[0], x[1]);

    ralph_free(model);
    printf("Done\n");
    return 0;
}
