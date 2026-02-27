#include "sg_time_budget.h"

#include <limits.h>
#include <math.h>

void sg_time_budget_init(SGTimeBudget *tb, double now, double total_seconds) {
    tb->solve_start = now;
    if (total_seconds <= 0.0) {
        tb->deadline = -1.0;  /* sentinel: unlimited */
    } else {
        tb->deadline = now + total_seconds;
    }
}

int sg_time_budget_expired(const SGTimeBudget *tb, double now) {
    if (tb->deadline < 0.0) return 0;  /* unlimited */
    return now >= tb->deadline;
}

double sg_time_budget_remaining(const SGTimeBudget *tb, double now) {
    double rem;
    if (tb->deadline < 0.0) return DBL_MAX;  /* unlimited */
    rem = tb->deadline - now;
    return rem > 0.0 ? rem : 0.0;
}

double sg_time_budget_phase(const SGTimeBudget *tb, double now,
                            double fraction, double min_seconds) {
    double rem, alloc;
    if (tb->deadline < 0.0) return DBL_MAX;  /* unlimited */
    rem = tb->deadline - now;
    if (rem <= 0.0) return 0.0;
    alloc = rem * fraction;
    if (alloc < min_seconds) alloc = min_seconds;
    if (alloc > rem) alloc = rem;
    return alloc;
}

int sg_time_budget_remaining_int(const SGTimeBudget *tb, double now) {
    double rem;
    int secs;
    if (tb->deadline < 0.0) return 0;  /* unlimited: return 0 so caller uses its own default */
    rem = tb->deadline - now;
    if (rem <= 0.0) return 0;
    secs = (int)ceil(rem);
    if (secs < 1) secs = 1;
    return secs;
}

double sg_time_budget_elapsed(const SGTimeBudget *tb, double now) {
    return now - tb->solve_start;
}
