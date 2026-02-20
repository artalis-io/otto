#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "surge.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    tests_run++; \
    printf("  %-50s", #fn); \
    fn(); \
    tests_passed++; \
    printf("OK\n"); \
} while (0)

/* ---------- helpers ---------- */

static SGContext *make_config(int max_iter, uint64_t seed) {
    SGContext *ctx = sg_create();
    SGConfig cfg;
    assert(ctx != NULL);
    sg_config_default(&cfg);
    cfg.max_iterations = max_iter;
    cfg.seed = seed;
    cfg.deterministic = true;
    cfg.require_bound_requests_at_solve = true;
    assert(sg_set_config(ctx, &cfg) == SG_STATUS_OK);
    return ctx;
}

static void add_depot_with_location(SGContext *ctx, uint32_t *id_out,
                                    double x, double y) {
    uint32_t id = sg_add_depot(ctx);
    assert(id != UINT32_MAX);
    assert(sg_depot_set_location(ctx, id, x, y) == SG_STATUS_OK);
    *id_out = id;
}

static void add_delivery_request(SGContext *ctx, double x, double y,
                                 int32_t tw_early, int32_t tw_late,
                                 int32_t service_seconds, double demand) {
    uint32_t req = sg_add_request(ctx);
    uint32_t task = sg_add_task(ctx, SG_TASK_DELIVERY);
    assert(sg_task_set_location(ctx, task, x, y) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, task, tw_early, tw_late) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, task, service_seconds) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, task, &demand, 1) == SG_STATUS_OK);
    assert(sg_request_bind_delivery_task(ctx, req, task) == SG_STATUS_OK);
}

static void add_pd_request(SGContext *ctx,
                           double px, double py, int32_t p_early, int32_t p_late, int32_t p_svc,
                           double dx, double dy, int32_t d_early, int32_t d_late, int32_t d_svc,
                           double demand) {
    double neg_demand = -demand;
    uint32_t req = sg_add_request(ctx);
    uint32_t p_task = sg_add_task(ctx, SG_TASK_PICKUP);
    uint32_t d_task = sg_add_task(ctx, SG_TASK_DELIVERY);
    assert(sg_task_set_location(ctx, p_task, px, py) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, p_task, p_early, p_late) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, p_task, p_svc) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, p_task, &demand, 1) == SG_STATUS_OK);
    assert(sg_task_set_location(ctx, d_task, dx, dy) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, d_task, d_early, d_late) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, d_task, d_svc) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, d_task, &neg_demand, 1) == SG_STATUS_OK);
    assert(sg_request_bind_pickup_delivery_tasks(ctx, req, p_task, d_task) == SG_STATUS_OK);
}

static void add_vehicle_with_depot(SGContext *ctx, uint32_t depot,
                                   int32_t shift_early, int32_t shift_late,
                                   double capacity) {
    uint32_t v = sg_add_vehicle(ctx);
    assert(v != UINT32_MAX);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, shift_early, shift_late) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &capacity, 1) == SG_STATUS_OK);
}

/* ===== Context & Model Tests ===== */

static void test_create_free_idempotent(void) {
    SGContext *ctx = sg_create();
    assert(ctx != NULL);
    sg_free(ctx);
    sg_free(NULL);
}

static void test_version(void) {
    const char *v = sg_version();
    assert(v != NULL);
    assert(strlen(v) > 0);
}

static void test_dimension_locking(void) {
    SGContext *ctx = sg_create();
    assert(ctx != NULL);
    assert(sg_get_dimension_count(ctx) == 1);
    assert(sg_set_dimension_count(ctx, 3) == SG_STATUS_OK);
    assert(sg_get_dimension_count(ctx) == 3);

    (void)sg_add_vehicle(ctx);
    assert(sg_set_dimension_count(ctx, 2) == SG_STATUS_INVALID_ARG);
    assert(sg_get_dimension_count(ctx) == 3);

    assert(sg_set_dimension_count(ctx, 0) == SG_STATUS_INVALID_ARG);
    sg_free(ctx);
}

static void test_config_validation(void) {
    SGContext *ctx = sg_create();
    SGConfig cfg;
    assert(ctx != NULL);

    sg_config_default(&cfg);
    cfg.priority_removal_policy = (SGPriorityRemovalPolicy)99;
    assert(sg_set_config(ctx, &cfg) == SG_STATUS_INVALID_ARG);

    sg_config_default(&cfg);
    assert(sg_set_config(ctx, &cfg) == SG_STATUS_OK);

    sg_free(ctx);
}

static void test_zone_matrix_validation(void) {
    SGContext *ctx = sg_create();
    const double valid[9] = {0, 1, 3, 1, 0, 2, 3, 2, 0};
    const double asym[4] = {0, 1, 2, 0};
    assert(ctx != NULL);

    assert(sg_set_zone_distance_matrix(ctx, 3, valid) == SG_STATUS_OK);
    assert(sg_clear_zone_distance_matrix(ctx) == SG_STATUS_OK);
    assert(sg_set_zone_distance_matrix(ctx, 2, asym) == SG_STATUS_INVALID_ARG);

    sg_free(ctx);
}

static void test_demand_conventions(void) {
    SGContext *ctx = sg_create();
    assert(ctx != NULL);

    assert(sg_get_demand_sign_convention(ctx) ==
           SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE);
    assert(sg_set_demand_sign_convention(ctx,
           SG_DEMAND_PICKUP_NEGATIVE_DELIVERY_POSITIVE) == SG_STATUS_OK);
    assert(sg_get_demand_sign_convention(ctx) ==
           SG_DEMAND_PICKUP_NEGATIVE_DELIVERY_POSITIVE);
    assert(sg_set_demand_sign_convention(ctx, (SGDemandSignConvention)77) ==
           SG_STATUS_INVALID_ARG);

    sg_free(ctx);
}

static void test_validate_model_edge_cases(void) {
    SGContext *ctx = make_config(100, 1);
    uint32_t depot;
    uint32_t task;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);

    task = sg_add_task(ctx, SG_TASK_DELIVERY);
    assert(sg_task_set_location(ctx, task, 1.0, 1.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, task, 0, 3600) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, task, (double[]){-10.0}, 1) == SG_STATUS_OK);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);

    sg_free(ctx);
}

/* ===== Solve Integration Tests ===== */

static void test_trivial_1v_1r(void) {
    SGContext *ctx = make_config(50, 42);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);
    add_delivery_request(ctx, 1.0, 0.0, 0, 86400, 60, -10.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_get_used_vehicle_count(ctx) == 1);
    assert(sg_get_total_distance(ctx) > 0.0);

    sg_free(ctx);
}

static void test_infeasible_no_vehicles(void) {
    SGContext *ctx = make_config(50, 42);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_delivery_request(ctx, 1.0, 0.0, 0, 86400, 60, -10.0);

    assert(sg_solve(ctx) == SG_STATUS_INFEASIBLE);

    sg_free(ctx);
}

static void test_zero_requests(void) {
    SGContext *ctx = make_config(50, 42);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_get_used_vehicle_count(ctx) == 0);

    sg_free(ctx);
}

static void test_single_pd_request(void) {
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);
    add_pd_request(ctx,
                   1.0, 0.0, 0, 43200, 60,
                   2.0, 0.0, 0, 86400, 60,
                   10.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_get_used_vehicle_count(ctx) == 1);

    sg_free(ctx);
}

static void test_all_assigned_ample_capacity(void) {
    SGContext *ctx = make_config(200, 77);
    uint32_t depot;
    int i;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 10000.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 10000.0);

    for (i = 0; i < 5; i++) {
        add_delivery_request(ctx,
                             (double)(i + 1), (double)(i + 1),
                             0, 86400, 60, -10.0);
    }

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    sg_free(ctx);
}

static void test_capacity_limited(void) {
    SGContext *ctx = make_config(500, 88);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 40.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 40.0);

    add_delivery_request(ctx, 1.0, 0.0, 0, 86400, 60, -15.0);
    add_delivery_request(ctx, 2.0, 0.0, 0, 86400, 60, -15.0);
    add_delivery_request(ctx, 3.0, 0.0, 0, 86400, 60, -15.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_get_used_vehicle_count(ctx) >= 1);

    sg_free(ctx);
}

static void test_time_limited(void) {
    SGContext *ctx = make_config(500, 99);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 3600, 1000.0);
    add_vehicle_with_depot(ctx, depot, 3600, 7200, 1000.0);

    add_delivery_request(ctx, 1.0, 0.0, 0, 1800, 60, -1.0);
    add_delivery_request(ctx, 2.0, 0.0, 3600, 5400, 60, -1.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_get_used_vehicle_count(ctx) == 2);

    sg_free(ctx);
}

/* ===== Feasibility Tests (via solve outcomes) ===== */

static void test_tw_violation_causes_unassigned(void) {
    SGContext *ctx = make_config(200, 55);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 3600, 1000.0);

    add_delivery_request(ctx, 1.0, 0.0, 7200, 10800, 60, -1.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 1);

    sg_free(ctx);
}

static void test_capacity_violation_causes_unassigned(void) {
    SGContext *ctx = make_config(200, 66);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 5.0);

    add_delivery_request(ctx, 1.0, 0.0, 0, 86400, 60, -10.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 1);

    sg_free(ctx);
}

static void test_multiple_pd_requests(void) {
    SGContext *ctx = make_config(500, 111);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);

    add_pd_request(ctx,
                   1.0, 0.0, 0, 43200, 60,
                   3.0, 0.0, 0, 86400, 60,
                   5.0);
    add_pd_request(ctx,
                   2.0, 0.0, 0, 43200, 60,
                   4.0, 0.0, 0, 86400, 60,
                   5.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    sg_free(ctx);
}

/* ===== Stats and Output Tests ===== */

static void test_stats_populated(void) {
    SGContext *ctx = make_config(100, 42);
    SGStats stats;
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);
    add_delivery_request(ctx, 10.0, 0.0, 0, 86400, 60, -10.0);

    assert(sg_solve(ctx) == SG_STATUS_OK);

    sg_get_stats(ctx, &stats);
    assert(stats.iterations > 0);
    assert(stats.total_distance > 0.0);
    assert(stats.total_cost > 0.0);
    assert(stats.unassigned == 0);
    assert(stats.vehicles_used == 1);

    assert(fabs(sg_get_total_cost(ctx) - stats.total_cost) < 1e-9);
    assert(fabs(sg_get_total_distance(ctx) - stats.total_distance) < 1e-9);
    assert(sg_get_unassigned(ctx) == stats.unassigned);
    assert(sg_get_used_vehicle_count(ctx) == stats.vehicles_used);

    sg_free(ctx);
}

static void test_deterministic_solve(void) {
    double cost1;
    double cost2;
    double dist1;
    double dist2;
    uint32_t unassigned1;
    uint32_t unassigned2;
    int run;

    for (run = 0; run < 2; run++) {
        SGContext *ctx = make_config(200, 0xCAFE);
        uint32_t depot;
        int i;

        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_vehicle_with_depot(ctx, depot, 0, 86400, 500.0);
        add_vehicle_with_depot(ctx, depot, 0, 86400, 500.0);

        for (i = 0; i < 8; i++) {
            add_delivery_request(ctx,
                                 (double)(i * 3 + 1), (double)(i * 2),
                                 0, 86400, 120, -10.0);
        }

        assert(sg_solve(ctx) == SG_STATUS_OK);

        if (run == 0) {
            cost1 = sg_get_total_cost(ctx);
            dist1 = sg_get_total_distance(ctx);
            unassigned1 = sg_get_unassigned(ctx);
        } else {
            cost2 = sg_get_total_cost(ctx);
            dist2 = sg_get_total_distance(ctx);
            unassigned2 = sg_get_unassigned(ctx);
        }

        sg_free(ctx);
    }

    assert(fabs(cost1 - cost2) < 1e-9);
    assert(fabs(dist1 - dist2) < 1e-9);
    assert(unassigned1 == unassigned2);
}

/* ===== NULL Safety Tests ===== */

static void test_null_context_safety(void) {
    SGStats stats;

    sg_free(NULL);
    assert(sg_get_total_cost(NULL) == 0.0);
    assert(sg_get_total_distance(NULL) == 0.0);
    assert(sg_get_unassigned(NULL) == 0);
    assert(sg_get_used_vehicle_count(NULL) == 0);
    assert(sg_get_request_count(NULL) == 0);
    assert(sg_get_dimension_count(NULL) == 0);
    assert(sg_solve(NULL) == SG_STATUS_INVALID_ARG);
    assert(sg_set_config(NULL, NULL) == SG_STATUS_INVALID_ARG);

    memset(&stats, 0xFF, sizeof(stats));
    sg_get_stats(NULL, &stats);

    assert(sg_get_require_bound_requests_at_solve(NULL) == true);
    assert(sg_get_demand_sign_convention(NULL) ==
           SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE);
}

/* ===== Domain Model Test (original) ===== */

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
    status = sg_solve(ctx);
    assert(status == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

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

/* ===== Loader Smoke Tests (original) ===== */

static void test_solomon_loader_smoke(void) {
    SGContext *ctx = sg_create();
    SGConfig cfg;
    SGStatus status;

    assert(ctx != NULL);

    sg_config_default(&cfg);
    cfg.max_iterations = 200;
    cfg.seed = 101;
    cfg.deterministic = true;
    status = sg_set_config(ctx, &cfg);
    assert(status == SG_STATUS_OK);

    status = sg_load_solomon_vrptw(ctx, "benchmarks/solomon/C101.txt");
    assert(status == SG_STATUS_OK);
    assert(sg_get_request_count(ctx) == 100);

    status = sg_validate_model(ctx);
    assert(status == SG_STATUS_OK);

    status = sg_solve(ctx);
    assert(status == SG_STATUS_OK);

    sg_free(ctx);
}

static void test_li_lim_loader_smoke(void) {
    SGContext *ctx = sg_create();
    SGConfig cfg;
    SGStatus status;

    assert(ctx != NULL);

    sg_config_default(&cfg);
    cfg.max_iterations = 250;
    cfg.seed = 202;
    cfg.deterministic = true;
    status = sg_set_config(ctx, &cfg);
    assert(status == SG_STATUS_OK);

    status = sg_load_li_lim_pdptw(ctx, "benchmarks/li_lim/LC101-mini.txt");
    assert(status == SG_STATUS_OK);
    assert(sg_get_request_count(ctx) == 2);

    status = sg_validate_model(ctx);
    assert(status == SG_STATUS_OK);

    status = sg_solve(ctx);
    assert(status == SG_STATUS_OK || status == SG_STATUS_LIMIT);
    assert(sg_get_unassigned(ctx) == 0);

    sg_free(ctx);
}

static void test_solomon_deterministic(void) {
    double cost1;
    double cost2;
    uint32_t unassigned1;
    uint32_t unassigned2;
    uint32_t vehicles1;
    uint32_t vehicles2;
    int run;

    for (run = 0; run < 2; run++) {
        SGContext *ctx = sg_create();
        SGConfig cfg;
        assert(ctx != NULL);
        sg_config_default(&cfg);
        cfg.max_iterations = 200;
        cfg.seed = 303;
        cfg.deterministic = true;
        assert(sg_set_config(ctx, &cfg) == SG_STATUS_OK);
        assert(sg_load_solomon_vrptw(ctx, "benchmarks/solomon/R101.txt") == SG_STATUS_OK);
        assert(sg_solve(ctx) == SG_STATUS_OK);

        if (run == 0) {
            cost1 = sg_get_total_cost(ctx);
            unassigned1 = sg_get_unassigned(ctx);
            vehicles1 = sg_get_used_vehicle_count(ctx);
        } else {
            cost2 = sg_get_total_cost(ctx);
            unassigned2 = sg_get_unassigned(ctx);
            vehicles2 = sg_get_used_vehicle_count(ctx);
        }
        sg_free(ctx);
    }

    assert(fabs(cost1 - cost2) < 1e-9);
    assert(unassigned1 == unassigned2);
    assert(vehicles1 == vehicles2);
}

static void test_li_lim_deterministic(void) {
    double cost1;
    double cost2;
    uint32_t unassigned1;
    uint32_t unassigned2;
    int run;

    for (run = 0; run < 2; run++) {
        SGContext *ctx = sg_create();
        SGConfig cfg;
        assert(ctx != NULL);
        sg_config_default(&cfg);
        cfg.max_iterations = 250;
        cfg.seed = 404;
        cfg.deterministic = true;
        assert(sg_set_config(ctx, &cfg) == SG_STATUS_OK);
        assert(sg_load_li_lim_pdptw(ctx, "benchmarks/li_lim/LC101-mini.txt") == SG_STATUS_OK);
        assert(sg_solve(ctx) == SG_STATUS_OK || sg_solve(ctx) == SG_STATUS_LIMIT);

        if (run == 0) {
            cost1 = sg_get_total_cost(ctx);
            unassigned1 = sg_get_unassigned(ctx);
        } else {
            cost2 = sg_get_total_cost(ctx);
            unassigned2 = sg_get_unassigned(ctx);
        }
        sg_free(ctx);
    }

    assert(fabs(cost1 - cost2) < 1e-9);
    assert(unassigned1 == unassigned2);
}

/* ===== Bootstrap Test (original inline test) ===== */

static void test_bootstrap_with_hints(void) {
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
    assert(sg_get_total_cost(ctx) >= 0.0);

    sg_get_stats(ctx, &stats);
    assert(stats.iterations >= 0);

    sg_free(ctx);
}

/* ===== Multi-Vehicle Routing Tests ===== */

static void test_multi_vehicle_delivery(void) {
    SGContext *ctx = make_config(500, 222);
    uint32_t depot;
    int i;

    add_depot_with_location(ctx, &depot, 50.0, 50.0);
    for (i = 0; i < 3; i++) {
        add_vehicle_with_depot(ctx, depot, 0, 86400, 50.0);
    }

    for (i = 0; i < 10; i++) {
        add_delivery_request(ctx,
                             50.0 + (double)(i % 5) * 10.0,
                             50.0 + (double)(i / 5) * 10.0,
                             0, 86400, 120, -10.0);
    }

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_get_used_vehicle_count(ctx) >= 2);
    assert(sg_get_total_distance(ctx) > 0.0);

    sg_free(ctx);
}

static void test_mixed_pd_and_delivery(void) {
    SGContext *ctx = make_config(500, 333);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 200.0);

    add_delivery_request(ctx, 5.0, 0.0, 0, 86400, 60, -20.0);
    add_delivery_request(ctx, 10.0, 0.0, 0, 86400, 60, -20.0);

    add_pd_request(ctx,
                   2.0, 0.0, 0, 43200, 60,
                   8.0, 0.0, 0, 86400, 60,
                   15.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    sg_free(ctx);
}

static void test_depot_time_window(void) {
    SGContext *ctx = make_config(200, 444);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    assert(sg_depot_set_time_window(ctx, depot, 0, 7200) == SG_STATUS_OK);
    add_vehicle_with_depot(ctx, depot, 0, 7200, 100.0);
    add_delivery_request(ctx, 1.0, 0.0, 0, 3600, 60, -10.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    sg_free(ctx);
}

static void test_service_time_impact(void) {
    SGContext *ctx = make_config(200, 555);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 3600, 100.0);

    add_delivery_request(ctx, 0.001, 0.0, 0, 1800, 1800, -10.0);
    add_delivery_request(ctx, 0.002, 0.0, 0, 3600, 1800, -10.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);

    sg_free(ctx);
}

static void test_multi_dimension_capacity(void) {
    SGContext *ctx = sg_create();
    SGConfig cfg;
    uint32_t depot;
    uint32_t v;
    uint32_t req;
    uint32_t task;
    const double cap[2] = {100.0, 5.0};
    const double demand[2] = {-50.0, -3.0};

    assert(ctx != NULL);
    sg_config_default(&cfg);
    cfg.max_iterations = 200;
    cfg.seed = 666;
    cfg.deterministic = true;
    assert(sg_set_config(ctx, &cfg) == SG_STATUS_OK);
    assert(sg_set_dimension_count(ctx, 2) == SG_STATUS_OK);

    depot = sg_add_depot(ctx);
    assert(sg_depot_set_location(ctx, depot, 0.0, 0.0) == SG_STATUS_OK);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 86400) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, cap, 2) == SG_STATUS_OK);

    req = sg_add_request(ctx);
    task = sg_add_task(ctx, SG_TASK_DELIVERY);
    assert(sg_task_set_location(ctx, task, 1.0, 1.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, task, 0, 86400) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, task, demand, 2) == SG_STATUS_OK);
    assert(sg_request_bind_delivery_task(ctx, req, task) == SG_STATUS_OK);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    sg_free(ctx);
}

/* ===== main ===== */

int main(void) {
    printf("surge test suite\n");
    printf("================\n");

    RUN_TEST(test_create_free_idempotent);
    RUN_TEST(test_version);
    RUN_TEST(test_dimension_locking);
    RUN_TEST(test_config_validation);
    RUN_TEST(test_zone_matrix_validation);
    RUN_TEST(test_demand_conventions);
    RUN_TEST(test_validate_model_edge_cases);
    RUN_TEST(test_null_context_safety);
    RUN_TEST(test_bootstrap_with_hints);
    RUN_TEST(test_domain_model);
    RUN_TEST(test_bound_request_gate);
    RUN_TEST(test_trivial_1v_1r);
    RUN_TEST(test_infeasible_no_vehicles);
    RUN_TEST(test_zero_requests);
    RUN_TEST(test_single_pd_request);
    RUN_TEST(test_all_assigned_ample_capacity);
    RUN_TEST(test_capacity_limited);
    RUN_TEST(test_time_limited);
    RUN_TEST(test_tw_violation_causes_unassigned);
    RUN_TEST(test_capacity_violation_causes_unassigned);
    RUN_TEST(test_multiple_pd_requests);
    RUN_TEST(test_stats_populated);
    RUN_TEST(test_deterministic_solve);
    RUN_TEST(test_multi_vehicle_delivery);
    RUN_TEST(test_mixed_pd_and_delivery);
    RUN_TEST(test_depot_time_window);
    RUN_TEST(test_service_time_impact);
    RUN_TEST(test_multi_dimension_capacity);
    RUN_TEST(test_solomon_loader_smoke);
    RUN_TEST(test_li_lim_loader_smoke);
    RUN_TEST(test_solomon_deterministic);
    RUN_TEST(test_li_lim_deterministic);

    printf("================\n");
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
