/* Isolated packing/cache measurements, NOT counters for the HGS search. */
#include "sg_internal.h"
#include "sh_pal.h"
#include <stdio.h>

static int probe_failed(int line) {
    fprintf(stderr, "loading probe failed at line %d\n", line);
    return 1;
}
static int stop_after_polls(void *opaque) {
    unsigned *polls = opaque;
    return ++*polls >= 100;
}
int main(void) {
    SGLoadingVehicle vehicle = {0};
    vehicle.slot_count = 4; vehicle.max_layers = 2; vehicle.clearance_mm = 2200;
    vehicle.class_count = 1; vehicle.allowed_top[0] = 1;
    vehicle.max_stack_weight_g = 1000; vehicle.payload_g = 10000;
    for (unsigned i = 0; i < 4; ++i) vehicle.depth[i] = i;
    SGContext *ctx = sg_create();
    if (!ctx) return probe_failed(__LINE__);
    uint32_t depot = sg_add_depot(ctx), v = sg_add_vehicle(ctx);
    double cap = 8, demand = -1;
    if (sg_depot_set_location(ctx, depot, 0, 0) != SG_STATUS_OK ||
        sg_vehicle_set_depots(ctx, v, depot, depot) != SG_STATUS_OK ||
        sg_vehicle_set_capacity(ctx, v, &cap, 1) != SG_STATUS_OK ||
        sg_vehicle_set_loading(ctx, v, &vehicle) != SG_STATUS_OK) return probe_failed(__LINE__);
    SGRouteStop stops[8] = {{0}};
    for (unsigned i = 0; i < 8; ++i) {
        uint32_t task = sg_add_task(ctx, SG_TASK_DELIVERY), request = sg_add_request(ctx);
        SGLoadingUnit unit = {i, 1000, 500, 500, 0, 1};
        if (sg_task_set_location(ctx, task, i + 1, 0) != SG_STATUS_OK ||
            sg_task_set_time_window(ctx, task, 0, 100000) != SG_STATUS_OK ||
            sg_task_set_service_seconds(ctx, task, 0) != SG_STATUS_OK ||
            sg_task_set_demand(ctx, task, &demand, 1) != SG_STATUS_OK ||
            sg_request_bind_delivery_task(ctx, request, task) != SG_STATUS_OK ||
            sg_request_set_loading(ctx, request, &unit, 1) != SG_STATUS_OK) return probe_failed(__LINE__);
        stops[i].request_id = request; stops[i].task_id = task;
    }
    if (sg_set_hard_loading(ctx, 1, 1000) != SG_STATUS_OK ||
        sg_validate_model(ctx) != SG_STATUS_OK) {
        fprintf(stderr, "%s\n", sg_get_last_error(ctx));
        return probe_failed(__LINE__);
    }
    SGLoadingResult result;
    const unsigned repeats = 20000;
    uint64_t durations[2];
    for (unsigned cold = 0; cold < 2; ++cold) {
        uint64_t begin = sh_monotonic_ns();
        for (unsigned i = 0; i < repeats; ++i) {
            if (cold) sg_loading_invalidate(ctx);
            if (sg_loading_round(ctx, v, stops, 8, 0, &result, NULL, NULL) != SG_STATUS_OK ||
                result.outcome != SG_LOADING_FEASIBLE) return probe_failed(__LINE__);
        }
        durations[cold] = sh_monotonic_ns() - begin;
    }
    sg_free(ctx);
    /* All 16 units individually fit and pass aggregate slot/height bounds,
     * but none may support another: deliberately combinatorial rejection. */
    vehicle.slot_count = 8; vehicle.allowed_top[0] = 0;
    for (unsigned i = 0; i < 8; ++i) vehicle.depth[i] = i;
    SGLoadingUnit units[16] = {{0}};
    for (unsigned i = 0; i < 16; ++i)
        units[i] = (SGLoadingUnit){i, 1000, 500, 500, 0, 1};
    uint64_t begin = sh_monotonic_ns();
    unsigned unknown = 0;
    for (unsigned i = 0; i < 100; ++i) {
        if (sg_loading_check(&vehicle, units, 16, 1000, &result) != SG_STATUS_OK ||
            result.outcome != SG_LOADING_UNKNOWN || result.reason != SG_LOADING_BUDGET ||
            result.nodes != 1000) return probe_failed(__LINE__);
        ++unknown;
    }
    uint64_t stress = sh_monotonic_ns() - begin;
    unsigned polls = 0;
    begin = sh_monotonic_ns();
    if (sg_loading_check_controlled(&vehicle, units, 16, 100000, stop_after_polls, &polls,
            &result) != SG_STATUS_OK || result.reason != SG_LOADING_INTERRUPTED ||
            result.outcome != SG_LOADING_UNKNOWN) return probe_failed(__LINE__);
    uint64_t cancel = sh_monotonic_ns() - begin;
    printf("{\"repeats\":%u,\"warm_ns_per_query\":%.3f,\"cold_with_invalidation_ns_per_query\":%.3f,"
           "\"stress_queries\":100,\"stress_unknown\":%u,\"stress_ns_per_query\":%.3f,"
           "\"cancel_after_polls\":%u,\"cancel_ns\":%llu}\n", repeats,
           (double)durations[0] / repeats, (double)durations[1] / repeats, unknown,
           (double)stress / 100, polls, (unsigned long long)cancel);
    return 0;
}
