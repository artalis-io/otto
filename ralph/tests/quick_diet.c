#include <stdio.h>
#include <stdlib.h>
#include "ralph_test_mod_api.h"

int main(void) {
    printf("Diet Problem test\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Foods: Bread, Milk, Cheese (cost: 2, 3.5, 8) */
    ralph_test_add_var(model, 0, 1e30, 2.0, RALPH_CONTINUOUS);    /* Bread */
    ralph_test_add_var(model, 0, 1e30, 3.5, RALPH_CONTINUOUS);    /* Milk */
    ralph_test_add_var(model, 0, 1e30, 8.0, RALPH_CONTINUOUS);    /* Cheese */

    /* Calories >= 300: 50*bread + 42*milk + 35*cheese >= 300 */
    int idx1[] = {0, 1, 2};
    double val1[] = {50, 42, 35};
    ralph_test_add_constraint(model, 3, idx1, val1, RALPH_GREATER_EQUAL, 300);

    /* Protein >= 10: 4*bread + 8*milk + 7*cheese >= 10 */
    double val2[] = {4, 8, 7};
    ralph_test_add_constraint(model, 3, idx1, val2, RALPH_GREATER_EQUAL, 10);

    /* Calcium >= 8: 0*bread + 3*milk + 2*cheese >= 8 */
    double val3[] = {0, 3, 2};
    ralph_test_add_constraint(model, 3, idx1, val3, RALPH_GREATER_EQUAL, 8);

    printf("Calling optimize...\n");
    ralph_test_optimize(model);

    printf("Status: %d\n", ralph_test_get_status(model));
    printf("Diet cost: %.2f\n", ralph_test_get_objval(model));

    double x[3];
    ralph_test_get_solution(model, x);
    printf("Solution: bread=%.2f, milk=%.2f, cheese=%.2f\n", x[0], x[1], x[2]);

    ralph_test_free(model);
    printf("Done\n");
    return 0;
}
