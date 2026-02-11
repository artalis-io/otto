#include <assert.h>
#include <stdio.h>

#include "surge.h"

static void test_domain_model(void) {
    SGContext *ctx = sg_create();
    SGStatus status;
    uint32_t depot_a;
    uint32_t depot_b;
    uint32_t vehicle_id;
    uint32_t req_pd;
    uint32_t req_delivery;
    uint32_t task_pickup;
    uint32_t task_dropoff;
    uint32_t task_delivery;
    const double vehicle_capacity[2] = {1200.0, 14.0};
    const double pickup_demand[2] = {80.0, 1.0};
    const double dropoff_demand[2] = {-80.0, -1.0};
    const double delivery_only_demand[2] = {-25.0, -0.4};
    const double invalid_dropoff_demand[2] = {-70.0, -1.0};
    const double pickup_demand_inverse[2] = {-80.0, -1.0};
    const double dropoff_demand_inverse[2] = {80.0, 1.0};
    const double delivery_only_demand_inverse[2] = {25.0, 0.4};

    assert(ctx != NULL);
    status = sg_set_dimension_count(ctx, 2);
    assert(status == SG_STATUS_OK);
    assert(sg_get_dimension_count(ctx) == 2);

    depot_a = sg_add_depot(ctx);
    depot_b = sg_add_depot(ctx);
    assert(depot_a == 0);
    assert(depot_b == 1);
    status = sg_depot_set_location(ctx, depot_a, 12.10, 55.68);
    assert(status == SG_STATUS_OK);
    status = sg_depot_set_location(ctx, depot_b, 12.40, 55.72);
    assert(status == SG_STATUS_OK);
    status = sg_depot_set_time_window(ctx, depot_a, 6 * 3600, 22 * 3600);
    assert(status == SG_STATUS_OK);

    vehicle_id = sg_add_vehicle(ctx);
    assert(vehicle_id == 0);
    status = sg_vehicle_set_depots(ctx, vehicle_id, depot_a, depot_b);
    assert(status == SG_STATUS_OK);
    status = sg_vehicle_set_shift_time_window(ctx, vehicle_id, 7 * 3600, 19 * 3600);
    assert(status == SG_STATUS_OK);
    status = sg_vehicle_set_capacity(ctx, vehicle_id, vehicle_capacity, 2);
    assert(status == SG_STATUS_OK);
    status = sg_set_dimension_count(ctx, 3);
    assert(status == SG_STATUS_INVALID_ARG);

    req_pd = sg_add_request(ctx);
    req_delivery = sg_add_request(ctx);
    assert(req_pd == 0);
    assert(req_delivery == 1);

    task_pickup = sg_add_task(ctx, SG_TASK_PICKUP);
    task_dropoff = sg_add_task(ctx, SG_TASK_DELIVERY);
    task_delivery = sg_add_task(ctx, SG_TASK_DELIVERY);
    assert(task_pickup == 0);
    assert(task_dropoff == 1);
    assert(task_delivery == 2);

    status = sg_task_set_location(ctx, task_pickup, 12.11, 55.67);
    assert(status == SG_STATUS_OK);
    status = sg_task_set_time_window(ctx, task_pickup, 8 * 3600, 9 * 3600);
    assert(status == SG_STATUS_OK);
    status = sg_task_set_service_seconds(ctx, task_pickup, 300);
    assert(status == SG_STATUS_OK);
    status = sg_task_set_demand(ctx, task_pickup, pickup_demand, 2);
    assert(status == SG_STATUS_OK);

    status = sg_task_set_location(ctx, task_dropoff, 12.22, 55.70);
    assert(status == SG_STATUS_OK);
    status = sg_task_set_time_window(ctx, task_dropoff, 9 * 3600, 11 * 3600);
    assert(status == SG_STATUS_OK);
    status = sg_task_set_service_seconds(ctx, task_dropoff, 180);
    assert(status == SG_STATUS_OK);
    status = sg_task_set_demand(ctx, task_dropoff, dropoff_demand, 2);
    assert(status == SG_STATUS_OK);

    status = sg_task_set_location(ctx, task_delivery, 12.18, 55.71);
    assert(status == SG_STATUS_OK);
    status = sg_task_set_time_window(ctx, task_delivery, 10 * 3600, 12 * 3600);
    assert(status == SG_STATUS_OK);
    status = sg_task_set_service_seconds(ctx, task_delivery, 120);
    assert(status == SG_STATUS_OK);
    status = sg_task_set_demand(ctx, task_delivery, delivery_only_demand, 2);
    assert(status == SG_STATUS_OK);

    status = sg_request_bind_pickup_delivery_tasks(ctx, req_pd, task_pickup, task_dropoff);
    assert(status == SG_STATUS_OK);
    status = sg_request_bind_delivery_task(ctx, req_delivery, task_delivery);
    assert(status == SG_STATUS_OK);

    status = sg_validate_model(ctx);
    assert(status == SG_STATUS_OK);

    status = sg_task_set_demand(ctx, task_dropoff, invalid_dropoff_demand, 2);
    assert(status == SG_STATUS_OK);
    status = sg_validate_model(ctx);
    assert(status == SG_STATUS_INFEASIBLE);

    status = sg_set_demand_sign_convention(ctx, SG_DEMAND_PICKUP_NEGATIVE_DELIVERY_POSITIVE);
    assert(status == SG_STATUS_OK);
    status = sg_set_demand_sign_convention(ctx, (SGDemandSignConvention)77);
    assert(status == SG_STATUS_INVALID_ARG);
    status = sg_task_set_demand(ctx, task_pickup, pickup_demand_inverse, 2);
    assert(status == SG_STATUS_OK);
    status = sg_task_set_demand(ctx, task_dropoff, dropoff_demand_inverse, 2);
    assert(status == SG_STATUS_OK);
    status = sg_task_set_demand(ctx, task_delivery, delivery_only_demand_inverse, 2);
    assert(status == SG_STATUS_OK);
    status = sg_validate_model(ctx);
    assert(status == SG_STATUS_OK);
    assert(sg_get_demand_sign_convention(ctx) ==
           SG_DEMAND_PICKUP_NEGATIVE_DELIVERY_POSITIVE);

    sg_free(ctx);
}

static void test_bound_request_gate(void) {
    SGContext *ctx = sg_create();
    SGStatus status;
    SGConfig cfg;

    assert(ctx != NULL);
    sg_config_default(&cfg);
    cfg.max_iterations = 50;
    cfg.seed = 7;
    cfg.deterministic = true;
    status = sg_set_config(ctx, &cfg);
    assert(status == SG_STATUS_OK);
    assert(sg_get_require_bound_requests_at_solve(ctx) == true);

    (void)sg_add_vehicle(ctx);
    assert(sg_add_request(ctx) == 0);

    status = sg_solve(ctx);
    assert(status == SG_STATUS_INFEASIBLE);

    status = sg_set_require_bound_requests_at_solve(ctx, false);
    assert(status == SG_STATUS_OK);
    assert(sg_get_require_bound_requests_at_solve(ctx) == false);

    status = sg_solve(ctx);
    assert(status == SG_STATUS_OK);

    sg_free(ctx);
}

int main(void) {
    SGContext *ctx = sg_create();
    SGConfig cfg;
    SGStatus status;
    SGStats stats;
    const double zone_distances[9] = {
        0.0, 1.0, 3.0,
        1.0, 0.0, 2.0,
        3.0, 2.0, 0.0
    };
    const double invalid_zone_distances[4] = {
        0.0, 1.0,
        2.0, 0.0
    };

    assert(ctx != NULL);
    sg_config_default(&cfg);
    cfg.priority_removal_policy = (SGPriorityRemovalPolicy)99;
    status = sg_set_config(ctx, &cfg);
    assert(status == SG_STATUS_INVALID_ARG);

    sg_config_default(&cfg);
    cfg.max_iterations = 200;
    cfg.seed = 42;
    cfg.deterministic = true;
    cfg.require_bound_requests_at_solve = false;

    status = sg_set_config(ctx, &cfg);
    assert(status == SG_STATUS_OK);

    (void)sg_add_vehicle(ctx);
    assert(sg_add_request(ctx) == 0);
    assert(sg_add_request(ctx) == 1);
    assert(sg_add_request(ctx) == 2);

    status = sg_request_set_priority_hint(ctx, 0, 90);
    assert(status == SG_STATUS_OK);
    status = sg_request_set_priority_hint(ctx, 1, 20);
    assert(status == SG_STATUS_OK);
    status = sg_request_set_time_window_hint(ctx, 0, 8 * 3600, 10 * 3600);
    assert(status == SG_STATUS_OK);
    status = sg_request_set_time_window_hint(ctx, 1, 9 * 3600, 14 * 3600);
    assert(status == SG_STATUS_OK);
    status = sg_request_set_zone_hint(ctx, 0, 7);
    assert(status == SG_STATUS_OK);
    status = sg_request_set_zone_hint(ctx, 1, 7);
    assert(status == SG_STATUS_OK);
    status = sg_request_set_zone_hint(ctx, 2, 9);
    assert(status == SG_STATUS_OK);

    status = sg_set_priority_removal_policy(ctx, SG_PRIORITY_REMOVE_HIGHER_FIRST);
    assert(status == SG_STATUS_OK);
    status = sg_set_priority_removal_policy(ctx, (SGPriorityRemovalPolicy)7);
    assert(status == SG_STATUS_INVALID_ARG);

    status = sg_set_zone_distance_matrix(ctx, 2, invalid_zone_distances);
    assert(status == SG_STATUS_INVALID_ARG);
    status = sg_set_zone_distance_matrix(ctx, 3, zone_distances);
    assert(status == SG_STATUS_OK);
    status = sg_clear_zone_distance_matrix(ctx);
    assert(status == SG_STATUS_OK);
    status = sg_set_zone_distance_matrix(ctx, 3, zone_distances);
    assert(status == SG_STATUS_OK);

    status = sg_request_set_time_window_hint(ctx, 2, 100, 50);
    assert(status == SG_STATUS_INVALID_ARG);
    status = sg_request_set_priority_hint(ctx, 99, 10);
    assert(status == SG_STATUS_INVALID_ARG);

    status = sg_solve(ctx);
    assert(status == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_get_total_cost(ctx) == 0.0);

    sg_get_stats(ctx, &stats);
    assert(stats.iterations > 0);

    sg_free(ctx);
    test_domain_model();
    test_bound_request_gate();
    printf("surge bootstrap test passed\n");
    return 0;
}
