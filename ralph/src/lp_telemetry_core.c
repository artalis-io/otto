/*
 * Ralph - LP/LU Telemetry Core Helpers
 *
 * Shared wall-clock timing and reason classification utilities.
 */

#include "lp.h"

double lp_telemetry_now_ms(void) {
    return sh_perf_now_ms();
}

double lp_telemetry_timer_start(void) {
    return sh_perf_now_ms();
}

double lp_telemetry_timer_elapsed_ms(double start_ms) {
    return sh_perf_now_ms() - start_ms;
}

int lp_telemetry_refactor_reason_is_safety_forced(int reason) {
    switch ((RalphRefactorReason)reason) {
        case RALPH_REFACTOR_REASON_RATIO_RECOVERY:
        case RALPH_REFACTOR_REASON_PIVOT_RECOVERY:
        case RALPH_REFACTOR_REASON_FORCED_SMALL_PIVOT:
        case RALPH_REFACTOR_REASON_UPDATE_RECOVERY:
        case RALPH_REFACTOR_REASON_DIRECTION_STABILIZE:
        case RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP:
            return 1;
        case RALPH_REFACTOR_REASON_OTHER:
        case RALPH_REFACTOR_REASON_SETUP:
        case RALPH_REFACTOR_REASON_PHASE_TRANSITION:
        case RALPH_REFACTOR_REASON_PERIODIC:
        default:
            return 0;
    }
}
