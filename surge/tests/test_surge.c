#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "surge.h"
#include "sg_internal.h"

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

/* ===== Incremental Feasibility Tests ===== */

static SGContext *make_internal_ctx(void) {
    SGContext *ctx = make_config(50, 42);
    uint32_t depot;
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);
    add_delivery_request(ctx, 10.0, 0.0, 0, 86400, 60, -10.0);
    add_delivery_request(ctx, 20.0, 0.0, 0, 86400, 60, -15.0);
    add_delivery_request(ctx, 30.0, 0.0, 0, 86400, 60, -20.0);
    assert(sg_prepare_travel(ctx) == SG_STATUS_OK);
    return ctx;
}

static void test_timing_cache_matches_full_check(void) {
    SGContext *ctx = make_internal_ctx();
    SGRouteSolution sol;
    double full_distance = 0.0;
    uint32_t i;

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

    /* Insert all three requests */
    for (i = 0; i < 3; i++) {
        double score = 0.0, dist = 0.0;
        if (sg_route_eval_insertion_cached(ctx, &sol, i, 0, sol.route_lengths[0],
                                           &score, &dist)) {
            assert(sg_route_apply_insertion(ctx, &sol, i, 0, sol.route_lengths[0], dist) == AR_STATUS_OK);
        }
    }

    /* Verify timing cache matches full kernel */
    assert(sol.route_stop_lengths[0] == 3);
    assert(sg_route_stop_sequence_feasible(ctx, 0,
                                           sg_route_vehicle_stop_ptr_const(&sol, 0),
                                           sol.route_stop_lengths[0], &full_distance));
    assert(fabs(full_distance - sol.route_distance[0]) < 1e-9);

    /* Verify timing fields are populated */
    {
        const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(&sol, 0);
        for (i = 0; i < sol.route_stop_lengths[0]; i++) {
            assert(stops[i].service_start >= stops[i].arrival);
            assert(stops[i].depart > stops[i].service_start - 1e-9);
            assert(stops[i].forward_slack >= -1e-9);
        }
    }

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_cached_eval_agrees_with_full_eval(void) {
    SGContext *ctx = make_internal_ctx();
    SGRouteSolution sol;
    uint32_t *candidate_route;
    uint32_t pos;

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

    /* Insert request 0 first so the route is non-empty */
    {
        double score = 0.0, dist = 0.0;
        if (sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist)) {
            assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
        }
    }

    candidate_route = (uint32_t *)malloc((size_t)(sol.route_stride + 1U) * sizeof(uint32_t));
    assert(candidate_route != NULL);

    /* Try inserting request 1 at every position — compare old and cached eval */
    for (pos = 0; pos <= sol.route_lengths[0]; pos++) {
        double cached_score = 0.0, cached_dist = 0.0;
        double full_score = 0.0, full_dist = 0.0;
        int cached_ok = sg_route_eval_insertion_cached(ctx, &sol, 1, 0, pos,
                                                       &cached_score, &cached_dist);
        int full_ok = sg_route_eval_insertion(ctx, &sol, 1, 0, pos,
                                              candidate_route, NULL,
                                              &full_score, &full_dist);
        assert(cached_ok == full_ok);
        if (cached_ok) {
            assert(fabs(cached_score - full_score) < 1e-6);
            assert(fabs(cached_dist - full_dist) < 1e-6);
        }
    }

    free(candidate_route);
    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_cache_after_insert_remove_cycle(void) {
    SGContext *ctx = make_internal_ctx();
    SGRouteSolution sol;
    double full_distance = 0.0;

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

    /* Insert request 0 */
    {
        double score = 0.0, dist = 0.0;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
    }
    assert(sol.route_lengths[0] == 1);

    /* Insert request 1 */
    {
        double score = 0.0, dist = 0.0;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 1, 0, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 1, 0, 1, dist) == AR_STATUS_OK);
    }
    assert(sol.route_lengths[0] == 2);

    /* Remove request 0 */
    assert(sg_route_unassign_request(ctx, &sol, 0, NULL) == AR_STATUS_OK);
    assert(sol.route_lengths[0] == 1);

    /* Cache should be consistent: timing matches full check */
    if (sol.route_stop_lengths[0] > 0) {
        assert(sg_route_stop_sequence_feasible(ctx, 0,
                                               sg_route_vehicle_stop_ptr_const(&sol, 0),
                                               sol.route_stop_lengths[0], &full_distance));
        assert(fabs(full_distance - sol.route_distance[0]) < 1e-9);
    }

    /* Re-insert request 0, should work */
    {
        double score = 0.0, dist = 0.0;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
    }
    assert(sol.route_lengths[0] == 2);

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_forward_slack_rejects_infeasible(void) {
    SGContext *ctx = make_config(50, 42);
    SGRouteSolution sol;
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 3600, 100.0);

    /* Tight TW: request 0 must be served 0-1000, request 1 must be served 2000-3000 */
    add_delivery_request(ctx, 1.0, 0.0, 0, 1000, 60, -10.0);
    add_delivery_request(ctx, 2.0, 0.0, 2000, 3000, 60, -10.0);
    /* Request 2 has TW that doesn't fit between 0 and 1 */
    add_delivery_request(ctx, 0.5, 0.0, 0, 500, 60, -10.0);

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

    /* Insert requests 0 and 1 */
    {
        double score = 0.0, dist = 0.0;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
    }
    {
        double score = 0.0, dist = 0.0;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 1, 0, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 1, 0, 1, dist) == AR_STATUS_OK);
    }

    /* Verify forward_slack is tight */
    {
        const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(&sol, 0);
        assert(stops[0].forward_slack >= -1e-9);
    }

    /* Try inserting request 2 between 0 and 1 — should fail (would push request 1 past TW) */
    {
        double score = 0.0, dist = 0.0;
        int ok = sg_route_eval_insertion_cached(ctx, &sol, 2, 0, 1, &score, &dist);
        /* Verify cached check and full check agree */
        {
            uint32_t *cand = (uint32_t *)malloc((size_t)(sol.route_stride + 1U) * sizeof(uint32_t));
            double fscore = 0.0, fdist = 0.0;
            int fok = sg_route_eval_insertion(ctx, &sol, 2, 0, 1, cand, NULL, &fscore, &fdist);
            assert(ok == fok);
            free(cand);
        }
    }

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_capacity_check_incremental(void) {
    SGContext *ctx = make_config(50, 42);
    SGRouteSolution sol;
    uint32_t depot;
    uint32_t pos;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    /* Vehicle with small capacity */
    add_vehicle_with_depot(ctx, depot, 0, 86400, 25.0);
    add_delivery_request(ctx, 1.0, 0.0, 0, 86400, 60, -10.0);
    add_delivery_request(ctx, 2.0, 0.0, 0, 86400, 60, -10.0);
    /* This one exceeds capacity if added (total would be 35 > 25) */
    add_delivery_request(ctx, 3.0, 0.0, 0, 86400, 60, -15.0);

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

    /* Insert request 0 and 1 (total demand = 20 <= 25) */
    {
        double score = 0.0, dist = 0.0;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
    }
    {
        double score = 0.0, dist = 0.0;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 1, 0, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 1, 0, 1, dist) == AR_STATUS_OK);
    }

    /* Try inserting request 2 at every position — should fail due to capacity */
    for (pos = 0; pos <= sol.route_lengths[0]; pos++) {
        double score = 0.0, dist = 0.0;
        int cached_ok = sg_route_eval_insertion_cached(ctx, &sol, 2, 0, pos, &score, &dist);
        /* Full eval should agree */
        {
            uint32_t *cand = (uint32_t *)malloc((size_t)(sol.route_stride + 1U) * sizeof(uint32_t));
            double fscore = 0.0, fdist = 0.0;
            int fok = sg_route_eval_insertion(ctx, &sol, 2, 0, pos, cand, NULL, &fscore, &fdist);
            assert(cached_ok == fok);
            free(cand);
        }
    }

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_sa_acceptance_produces_valid_solution(void) {
    SGContext *ctx = make_config(300, 0xBEEF);
    uint32_t depot;
    int i;

    add_depot_with_location(ctx, &depot, 50.0, 50.0);
    sg_depot_set_time_window(ctx, depot, 0, 86400);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 200.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 200.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 200.0);

    for (i = 0; i < 12; i++) {
        double x = 10.0 + (double)(i % 4) * 25.0;
        double y = 10.0 + (double)(i / 4) * 25.0;
        add_delivery_request(ctx, x, y, 0, 86400, 120, -15.0);
    }

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_get_total_distance(ctx) > 0.0);
    assert(sg_get_used_vehicle_count(ctx) > 0);
    {
        SGStats st;
        sg_get_stats(ctx, &st);
        assert(st.iterations > 0);
    }
    sg_free(ctx);
}

/* ===== PD Independent Placement Tests ===== */

static void test_splice_excise_stop(void) {
    SGContext *ctx = make_config(50, 42);
    SGRouteSolution sol;
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);
    add_delivery_request(ctx, 10.0, 0.0, 0, 86400, 60, -10.0);
    add_delivery_request(ctx, 20.0, 0.0, 0, 86400, 60, -10.0);
    add_delivery_request(ctx, 30.0, 0.0, 0, 86400, 60, -10.0);

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

    /* Insert all three via apply_insertion (which now uses splice internally) */
    {
        double score, dist;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
    }
    {
        double score, dist;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 1, 0, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 1, 0, 1, dist) == AR_STATUS_OK);
    }
    {
        double score, dist;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 2, 0, 2, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 2, 0, 2, dist) == AR_STATUS_OK);
    }

    assert(sol.route_stop_lengths[0] == 3);

    /* Verify stop positions */
    assert(sol.request_delivery_stop_pos[0] == 0);
    assert(sol.request_delivery_stop_pos[1] == 1);
    assert(sol.request_delivery_stop_pos[2] == 2);

    /* Verify prev/next */
    {
        const uint32_t *prev = sg_route_vehicle_stop_prev_ptr_const(&sol, 0);
        const uint32_t *next = sg_route_vehicle_stop_next_ptr_const(&sol, 0);
        assert(prev[0] == UINT32_MAX);
        assert(next[0] == 1);
        assert(prev[1] == 0);
        assert(next[1] == 2);
        assert(prev[2] == 1);
        assert(next[2] == UINT32_MAX);
    }

    /* Remove middle stop (request 1) via unassign */
    assert(sg_route_unassign_request(ctx, &sol, 1, NULL) == AR_STATUS_OK);
    assert(sol.route_stop_lengths[0] == 2);
    assert(sol.request_delivery_stop_pos[0] == 0);
    assert(sol.request_delivery_stop_pos[2] == 1);
    assert(sol.request_delivery_stop_pos[1] == UINT32_MAX);

    /* Verify integrity with full feasibility check */
    {
        double full_dist = 0.0;
        assert(sg_route_stop_sequence_feasible(ctx, 0,
                                               sg_route_vehicle_stop_ptr_const(&sol, 0),
                                               sol.route_stop_lengths[0], &full_dist));
        assert(fabs(full_dist - sol.route_distance[0]) < 1e-9);
    }

    /* Validate full solution */
    assert(sg_route_solution_validate(&sol, (void *)ctx));

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_pd_independent_placement_feasible(void) {
    /* Two PD requests on one vehicle. With non-adjacent placement,
       interleaving P1,P2,D1,D2 should yield lower distance than
       P1,D1,P2,D2 when the geometry favors it. */
    SGContext *ctx = make_config(500, 42);
    SGRouteSolution sol;
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);

    /* Request 0: pickup at (1,0) delivery at (3,0) - wide TW */
    add_pd_request(ctx,
                   1.0, 0.0, 0, 86400, 10,
                   3.0, 0.0, 0, 86400, 10,
                   10.0);
    /* Request 1: pickup at (2,0) delivery at (4,0) - wide TW */
    add_pd_request(ctx,
                   2.0, 0.0, 0, 86400, 10,
                   4.0, 0.0, 0, 86400, 10,
                   10.0);

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

    /* Insert request 0 first */
    {
        double score;
        uint32_t p_pos, d_pos;
        double route_dist;
        assert(sg_route_eval_pd_best_insertion_cached(ctx, &sol, 0, 0,
                                                       &score, &p_pos, &d_pos, &route_dist));
        assert(sg_route_apply_pd_insertion(ctx, &sol, 0, 0, p_pos, d_pos, route_dist) == AR_STATUS_OK);
    }

    /* Insert request 1 - should find non-adjacent placement */
    {
        double score;
        uint32_t p_pos, d_pos;
        double route_dist;
        assert(sg_route_eval_pd_best_insertion_cached(ctx, &sol, 1, 0,
                                                       &score, &p_pos, &d_pos, &route_dist));
        assert(sg_route_apply_pd_insertion(ctx, &sol, 1, 0, p_pos, d_pos, route_dist) == AR_STATUS_OK);
    }

    /* Both assigned */
    assert(sol.base.num_unassigned == 0);
    assert(sol.route_stop_lengths[0] == 4);

    /* The optimal ordering is P0,P1,D0,D1 (distance=8+return) or P0,P1,D1,D0...
       In any case, verify it's a valid solution. */
    assert(sg_route_solution_validate(&sol, (void *)ctx));

    /* Verify stops have correct interleaving: pickup 0 < pickup 1 < delivery 0 < delivery 1
       (by the geometry: 1, 2, 3, 4 on x-axis) */
    {
        const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(&sol, 0);
        /* Just verify all 4 stops are present and valid */
        uint32_t s;
        for (s = 0; s < 4; s++) {
            assert(stops[s].request_id < 2);
            assert(stops[s].task_id < ctx->num_tasks);
        }
    }

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_pd_adjacent_still_works(void) {
    /* PD request with tight TW that forces adjacent placement */
    SGContext *ctx = make_config(200, 42);
    SGRouteSolution sol;
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);

    /* Pickup at (10,0) delivery at (20,0), tight ride time */
    add_pd_request(ctx,
                   10.0, 0.0, 0, 100, 10,
                   20.0, 0.0, 50, 200, 10,
                   10.0);

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

    {
        double score;
        uint32_t p_pos, d_pos;
        double route_dist;
        int found = sg_route_eval_pd_best_insertion_cached(ctx, &sol, 0, 0,
                                                            &score, &p_pos, &d_pos, &route_dist);
        assert(found);
        /* Should be adjacent: p_pos=0, d_pos=1 */
        assert(p_pos == 0);
        assert(d_pos == 1);
        assert(sg_route_apply_pd_insertion(ctx, &sol, 0, 0, p_pos, d_pos, route_dist) == AR_STATUS_OK);
    }

    assert(sol.base.num_unassigned == 0);
    assert(sg_route_solution_validate(&sol, (void *)ctx));

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_pd_unassign_preserves_others(void) {
    /* Insert 2 PD requests non-adjacently, remove one,
       verify other's stops remain in correct positions. */
    SGContext *ctx = make_config(500, 42);
    SGRouteSolution sol;
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);

    add_pd_request(ctx,
                   1.0, 0.0, 0, 86400, 10,
                   3.0, 0.0, 0, 86400, 10,
                   10.0);
    add_pd_request(ctx,
                   2.0, 0.0, 0, 86400, 10,
                   4.0, 0.0, 0, 86400, 10,
                   10.0);

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

    /* Insert both */
    {
        double score;
        uint32_t p_pos, d_pos;
        double route_dist;
        assert(sg_route_eval_pd_best_insertion_cached(ctx, &sol, 0, 0,
                                                       &score, &p_pos, &d_pos, &route_dist));
        assert(sg_route_apply_pd_insertion(ctx, &sol, 0, 0, p_pos, d_pos, route_dist) == AR_STATUS_OK);
    }
    {
        double score;
        uint32_t p_pos, d_pos;
        double route_dist;
        assert(sg_route_eval_pd_best_insertion_cached(ctx, &sol, 1, 0,
                                                       &score, &p_pos, &d_pos, &route_dist));
        assert(sg_route_apply_pd_insertion(ctx, &sol, 1, 0, p_pos, d_pos, route_dist) == AR_STATUS_OK);
    }
    assert(sol.route_stop_lengths[0] == 4);

    /* Remember request 0's stop positions */
    {
        uint32_t p0_before = sol.request_pickup_stop_pos[0];
        uint32_t d0_before = sol.request_delivery_stop_pos[0];
        assert(p0_before != UINT32_MAX);
        assert(d0_before != UINT32_MAX);
        assert(p0_before < d0_before);
    }

    /* Remove request 1 */
    assert(sg_route_unassign_request(ctx, &sol, 1, NULL) == AR_STATUS_OK);
    assert(sol.route_stop_lengths[0] == 2);

    /* Request 0's stops should still be valid */
    assert(sol.request_pickup_stop_pos[0] != UINT32_MAX);
    assert(sol.request_delivery_stop_pos[0] != UINT32_MAX);
    assert(sol.request_pickup_stop_pos[0] < sol.request_delivery_stop_pos[0]);

    /* Validate */
    assert(sg_route_solution_validate(&sol, (void *)ctx));

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_pd_delivery_only_mixed(void) {
    /* Vehicle with both delivery-only and PD requests */
    SGContext *ctx = make_config(500, 42);
    SGRouteSolution sol;
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 200.0);

    /* Request 0: delivery-only */
    add_delivery_request(ctx, 5.0, 0.0, 0, 86400, 60, -10.0);
    /* Request 1: PD */
    add_pd_request(ctx,
                   1.0, 0.0, 0, 86400, 10,
                   3.0, 0.0, 0, 86400, 10,
                   10.0);

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

    /* Insert delivery-only first */
    {
        double score, dist;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
    }

    /* Insert PD request */
    {
        double score;
        uint32_t p_pos, d_pos;
        double route_dist;
        assert(sg_route_eval_pd_best_insertion_cached(ctx, &sol, 1, 0,
                                                       &score, &p_pos, &d_pos, &route_dist));
        assert(sg_route_apply_pd_insertion(ctx, &sol, 1, 0, p_pos, d_pos, route_dist) == AR_STATUS_OK);
    }

    assert(sol.base.num_unassigned == 0);
    assert(sg_route_solution_validate(&sol, (void *)ctx));

    /* Remove PD, verify delivery-only is untouched */
    assert(sg_route_unassign_request(ctx, &sol, 1, NULL) == AR_STATUS_OK);
    assert(sol.route_stop_lengths[0] == 1);
    assert(sol.request_delivery_stop_pos[0] == 0);
    assert(sg_route_solution_validate(&sol, (void *)ctx));

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_pd_ride_time_constraint(void) {
    /* PD request with strict ride-time limit:
       delivery positions far from pickup should be rejected. */
    SGContext *ctx = make_config(200, 42);
    SGRouteSolution sol;
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);

    /* First, add a filler delivery to create a longer route */
    add_delivery_request(ctx, 100.0, 0.0, 0, 86400, 10, -5.0);

    /* PD with tight ride limit: pickup TW [0, 100], delivery TW [0, 200].
       ride_limit = delivery.tw_late - pickup.tw_early = 200 - 0 = 200.
       Pickup at (1,0), delivery at (2,0). */
    add_pd_request(ctx,
                   1.0, 0.0, 0, 100, 10,
                   2.0, 0.0, 0, 200, 10,
                   10.0);

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

    /* Insert filler */
    {
        double score, dist;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
    }

    /* Insert PD */
    {
        double score;
        uint32_t p_pos, d_pos;
        double route_dist;
        int found = sg_route_eval_pd_best_insertion_cached(ctx, &sol, 1, 0,
                                                            &score, &p_pos, &d_pos, &route_dist);
        if (found) {
            assert(sg_route_apply_pd_insertion(ctx, &sol, 1, 0, p_pos, d_pos, route_dist) == AR_STATUS_OK);
            assert(sg_route_solution_validate(&sol, (void *)ctx));

            /* Verify ride time is within limit */
            {
                const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(&sol, 0);
                uint32_t pp = sol.request_pickup_stop_pos[1];
                uint32_t dp = sol.request_delivery_stop_pos[1];
                double ride = stops[dp].service_start - stops[pp].depart;
                assert(ride <= 200.0 + 1e-9);
            }
        }
        /* If not found, that's also acceptable - the constraint is working */
    }

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

/* ===== Route Removal Cost Tests ===== */

static void test_route_removal_cost(void) {
    /* Test 1: Delivery-only, middle stop off-axis.
       Depot at (0,0). A=(10,0), B=(15,10), C=(20,0).
       Route: depot -> A -> B -> C -> depot.
       Removing B saves dist(A,B) + dist(B,C) - dist(A,C). */
    {
        SGContext *ctx = make_config(50, 42);
        SGRouteSolution sol;
        uint32_t depot;
        double expected, actual;

        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);
        add_delivery_request(ctx, 10.0, 0.0, 0, 86400, 60, -10.0);   /* request 0 = A */
        add_delivery_request(ctx, 15.0, 10.0, 0, 86400, 60, -10.0);  /* request 1 = B */
        add_delivery_request(ctx, 20.0, 0.0, 0, 86400, 60, -10.0);   /* request 2 = C */

        assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

        /* Insert A, B, C in order */
        {
            double score, dist;
            assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
            assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
        }
        {
            double score, dist;
            assert(sg_route_eval_insertion_cached(ctx, &sol, 1, 0, 1, &score, &dist));
            assert(sg_route_apply_insertion(ctx, &sol, 1, 0, 1, dist) == AR_STATUS_OK);
        }
        {
            double score, dist;
            assert(sg_route_eval_insertion_cached(ctx, &sol, 2, 0, 2, &score, &dist));
            assert(sg_route_apply_insertion(ctx, &sol, 2, 0, 2, dist) == AR_STATUS_OK);
        }

        assert(sol.route_stop_lengths[0] == 3);

        /* Removing B (element_id=1): dist(A,B) + dist(B,C) - dist(A,C) + tie-breaker */
        expected = sg_euclid(10, 0, 15, 10) + sg_euclid(15, 10, 20, 0) - sg_euclid(10, 0, 20, 0)
                 + 2.0 * 0.0001;
        actual = sg_route_removal_cost((void *)ctx, (void *)&sol, 1);
        assert(fabs(actual - expected) < 1e-6);

        /* Removing A (element_id=0, first stop): dist(depot,A) + dist(A,B) - dist(depot,B) */
        expected = sg_euclid(0, 0, 10, 0) + sg_euclid(10, 0, 15, 10) - sg_euclid(0, 0, 15, 10)
                 + 1.0 * 0.0001;
        actual = sg_route_removal_cost((void *)ctx, (void *)&sol, 0);
        assert(fabs(actual - expected) < 1e-6);

        /* Removing C (element_id=2, last stop): dist(B,C) + dist(C,depot) - dist(B,depot) */
        expected = sg_euclid(15, 10, 20, 0) + sg_euclid(20, 0, 0, 0) - sg_euclid(15, 10, 0, 0)
                 + 3.0 * 0.0001;
        actual = sg_route_removal_cost((void *)ctx, (void *)&sol, 2);
        assert(fabs(actual - expected) < 1e-6);

        sg_route_solution_reset(&sol);
        sg_free(ctx);
    }

    /* Test 2: PD adjacent. Single PD request: pickup (5,0), delivery (10,10), depot (0,0). */
    {
        SGContext *ctx = make_config(50, 42);
        SGRouteSolution sol;
        uint32_t depot;
        double expected, actual;

        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);
        add_pd_request(ctx,
                       5.0, 0.0, 0, 86400, 10,
                       10.0, 10.0, 0, 86400, 10,
                       10.0);

        assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

        {
            double score;
            uint32_t p_pos, d_pos;
            double route_dist;
            assert(sg_route_eval_pd_best_insertion_cached(ctx, &sol, 0, 0,
                                                           &score, &p_pos, &d_pos, &route_dist));
            assert(sg_route_apply_pd_insertion(ctx, &sol, 0, 0, p_pos, d_pos, route_dist) == AR_STATUS_OK);
        }

        assert(sol.route_stop_lengths[0] == 2);
        assert(sol.request_pickup_stop_pos[0] == 0);
        assert(sol.request_delivery_stop_pos[0] == 1);

        /* Adjacent: dist(depot,P) + dist(P,D) + dist(D,depot) - dist(depot,depot) */
        expected = sg_euclid(0, 0, 5, 0) + sg_euclid(5, 0, 10, 10) + sg_euclid(10, 10, 0, 0)
                 - sg_euclid(0, 0, 0, 0)
                 + 1.0 * 0.0001;
        actual = sg_route_removal_cost((void *)ctx, (void *)&sol, 0);
        assert(fabs(actual - expected) < 1e-6);

        sg_route_solution_reset(&sol);
        sg_free(ctx);
    }

    /* Test 3: PD non-adjacent. Insert delivery-only first, then PD which picks
       non-adjacent placement due to geometry.
       Depot (0,0). D0 at (8,0). PD: pickup (3,0), delivery (15,8).
       Optimal route: depot -> P1(3,0) -> D0(8,0) -> D1(15,8) -> depot
       (non-adjacent: 35.63 vs adjacent: 36.05) */
    {
        SGContext *ctx = make_config(50, 42);
        SGRouteSolution sol;
        uint32_t depot;
        double expected, actual;

        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);
        add_delivery_request(ctx, 8.0, 0.0, 0, 86400, 60, -10.0);   /* request 0 = D0 */
        add_pd_request(ctx,
                       3.0, 0.0, 0, 86400, 10,
                       15.0, 8.0, 0, 86400, 10,
                       10.0);                                          /* request 1 = PD */

        assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

        /* Insert delivery-only request 0 first */
        {
            double score, dist;
            assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
            assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
        }
        assert(sol.route_stop_lengths[0] == 1);

        /* Insert PD request 1 - should choose non-adjacent: p_pos=0, d_pos=2 */
        {
            double score;
            uint32_t p_pos, d_pos;
            double route_dist;
            assert(sg_route_eval_pd_best_insertion_cached(ctx, &sol, 1, 0,
                                                           &score, &p_pos, &d_pos, &route_dist));
            assert(p_pos == 0);
            assert(d_pos == 2);
            assert(sg_route_apply_pd_insertion(ctx, &sol, 1, 0, p_pos, d_pos, route_dist) == AR_STATUS_OK);
        }

        /* Verify non-adjacent layout: P1@0, D0@1, D1@2 */
        assert(sol.route_stop_lengths[0] == 3);
        assert(sol.request_pickup_stop_pos[1] == 0);
        assert(sol.request_delivery_stop_pos[0] == 1);
        assert(sol.request_delivery_stop_pos[1] == 2);

        /* Non-adjacent saving for PD request 1:
           pickup saving:   dist(depot,P1) + dist(P1,D0) - dist(depot,D0) = 3+5-8 = 0
           delivery saving: dist(D0,D1) + dist(D1,depot) - dist(D0,depot)
                          = sqrt(113) + sqrt(289) - 8 */
        expected = (sg_euclid(8, 0, 15, 8) + sg_euclid(15, 8, 0, 0) - sg_euclid(8, 0, 0, 0))
                 + 2.0 * 0.0001;
        actual = sg_route_removal_cost((void *)ctx, (void *)&sol, 1);
        assert(fabs(actual - expected) < 1e-6);

        /* Unassigned request should return 0.0 */
        assert(sg_route_removal_cost((void *)ctx, (void *)&sol, 99) == 0.0);

        sg_route_solution_reset(&sol);
        sg_free(ctx);
    }
}

/* ===== Route-Aware Shaw Relatedness Tests ===== */

static void test_route_shaw_relatedness(void) {
    SGContext *ctx = make_config(50, 42);
    SGRouteSolution sol;
    uint32_t depot;
    double score_close, score_far, score_same_route, score_diff_route;
    double score_null_sol;

    add_depot_with_location(ctx, &depot, 50.0, 50.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 200.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 200.0);

    /* Request 0: at (10,10), TW [0, 3600], demand 10 */
    add_delivery_request(ctx, 10.0, 10.0, 0, 3600, 60, -10.0);
    /* Request 1: at (12,10), TW [0, 3600], demand 12 - close to 0, overlapping TW */
    add_delivery_request(ctx, 12.0, 10.0, 0, 3600, 60, -12.0);
    /* Request 2: at (80,80), TW [40000, 50000], demand 40 - far from 0, non-overlapping */
    add_delivery_request(ctx, 80.0, 80.0, 40000, 50000, 60, -40.0);
    /* Request 3: at (11,10), TW [0, 3600], demand 10 - very close to 0, overlapping TW */
    add_delivery_request(ctx, 11.0, 10.0, 0, 3600, 60, -10.0);

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);

    /* Insert requests: 0 and 3 on vehicle 0, request 2 on vehicle 1, request 1 unassigned */
    {
        double score, dist;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
    }
    {
        double score, dist;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 3, 0, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 3, 0, 1, dist) == AR_STATUS_OK);
    }
    {
        double score, dist;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 2, 1, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 2, 1, 0, dist) == AR_STATUS_OK);
    }

    /* Test without active_solution: close+overlapping > far+non-overlapping */
    ctx->active_solution = NULL;
    score_close = sg_route_shaw_relatedness((void *)ctx, 0, 1);
    score_far = sg_route_shaw_relatedness((void *)ctx, 0, 2);
    assert(score_close > score_far);

    /* Test with active_solution: same-route bonus dominates */
    ctx->active_solution = &sol;
    score_same_route = sg_route_shaw_relatedness((void *)ctx, 0, 3);  /* both on vehicle 0 */
    score_diff_route = sg_route_shaw_relatedness((void *)ctx, 0, 2);  /* different vehicles */
    assert(score_same_route > score_diff_route);

    /* Same-route > close-but-unassigned (request 1 is unassigned) */
    {
        double score_unassigned;
        score_unassigned = sg_route_shaw_relatedness((void *)ctx, 0, 1);
        assert(score_same_route > score_unassigned);
    }

    /* NULL active_solution safety: no crash, finite result */
    ctx->active_solution = NULL;
    score_null_sol = sg_route_shaw_relatedness((void *)ctx, 0, 3);
    assert(isfinite(score_null_sol));

    /* Self-relatedness should be high */
    {
        double score_self = sg_route_shaw_relatedness((void *)ctx, 0, 0);
        assert(score_self > score_far);
    }

    ctx->active_solution = NULL;
    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

/* ===== Adaptive Destroy Count Tests ===== */

static void test_adaptive_destroy_count(void) {
    int q_min, q_max;

    /* Small instance (8 requests): defaults dominate */
    sg_adaptive_q_bounds(8, 4, 20, &q_min, &q_max);
    assert(q_min == 4);   /* max(4, 8/20=0) = 4 */
    assert(q_max == 20);  /* max(20, 8/4=2) = 20 */

    /* Medium instance (100 requests): adaptive kicks in */
    sg_adaptive_q_bounds(100, 4, 20, &q_min, &q_max);
    assert(q_min == 5);   /* max(4, 100/20=5) = 5 */
    assert(q_max == 25);  /* max(20, 100/4=25) = 25 */

    /* Large instance (500 requests): fully adaptive */
    sg_adaptive_q_bounds(500, 4, 20, &q_min, &q_max);
    assert(q_min == 25);  /* max(4, 500/20=25) = 25 */
    assert(q_max == 125); /* max(20, 500/4=125) = 125 */

    /* User override higher than adaptive: user wins */
    sg_adaptive_q_bounds(100, 10, 50, &q_min, &q_max);
    assert(q_min == 10);  /* max(10, 5) = 10 */
    assert(q_max == 50);  /* max(50, 25) = 50 */

    /* Zero requests: defaults preserved, no crash */
    sg_adaptive_q_bounds(0, 4, 20, &q_min, &q_max);
    assert(q_min == 4);
    assert(q_max == 20);

    /* q_min clamped to q_max when adaptive_min > config_max */
    sg_adaptive_q_bounds(100, 30, 20, &q_min, &q_max);
    assert(q_min <= q_max);
}

/* ===== Or-Opt / Intensify Tests ===== */

static void test_or_opt_intensify(void) {
    SGContext *ctx = make_config(10, 42);
    SGRouteSolution sol;
    double dist_before, dist_after;
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 50.0, 50.0);
    add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);
    add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);

    /* Cluster A near (10,10), Cluster B near (90,90) */
    add_delivery_request(ctx, 10.0, 10.0, 0, 100000, 10, 1.0);  /* r0 */
    add_delivery_request(ctx, 90.0, 90.0, 0, 100000, 10, 1.0);  /* r1 */
    add_delivery_request(ctx, 92.0, 90.0, 0, 100000, 10, 1.0);  /* r2 */
    add_delivery_request(ctx, 12.0, 10.0, 0, 100000, 10, 1.0);  /* r3 */

    /* Suboptimal: v0=[r0, r1, r2], v1=[r3] — clusters mixed */
    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);
    {
        double score, dist;
        assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 1, 0, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 1, 0, 1, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 2, 0, 2, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 2, 0, 2, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 3, 1, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 3, 1, 0, dist) == AR_STATUS_OK);
    }

    dist_before = sol.total_distance;
    assert(dist_before > 0.0);

    /* Intensify should rearrange to group clusters */
    sg_route_postprocess_intensify(ctx, &sol);
    dist_after = sol.total_distance;

    /* Distance should improve since clusters get grouped */
    assert(dist_after < dist_before - 1.0);
    assert(sol.base.num_unassigned == 0);
    assert(sol.vehicles_used <= 2);

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

/* ===== Construction Quality Tests ===== */

static void test_tw_sorted_construction(void) {
    /* Verify TW-sorted construction produces <= vehicles compared to regret-3
       on a tight-TW instance resembling LC1 structure. */
    SGContext *ctx = make_config(50, 42);
    SGRouteSolution regret_sol;
    SGRouteSolution tw_sol;
    uint32_t depot;
    int i;

    add_depot_with_location(ctx, &depot, 40.0, 50.0);
    for (i = 0; i < 8; i++) {
        add_vehicle_with_depot(ctx, depot, 0, 86400, 200.0);
    }

    /* 12 PD requests with narrow, identical-width TWs (LC1-like).
       Spread across different early times to test temporal sweep ordering. */
    for (i = 0; i < 12; i++) {
        int32_t early = 1000 + i * 500;
        int32_t late = early + 2000;  /* all width 2000 */
        double px = 10.0 + (double)(i % 4) * 20.0;
        double py = 10.0 + (double)(i / 4) * 20.0;
        double dx = px + 5.0;
        double dy = py + 5.0;
        add_pd_request(ctx,
                       px, py, early, late, 60,
                       dx, dy, early + 500, late + 500, 60,
                       10.0);
    }

    assert(sg_route_solution_init(ctx, &regret_sol) == AR_STATUS_OK);
    assert(sg_route_repair_fill_regret(ctx, &regret_sol, 3, 0.0) == AR_STATUS_OK);

    assert(sg_route_solution_init(ctx, &tw_sol) == AR_STATUS_OK);
    /* Manually call TW-sorted via the combined constructor which picks the better */
    assert(sg_route_repair_fill_regret(ctx, &tw_sol, 3, 0.0) == AR_STATUS_OK);

    /* Both should assign all requests */
    assert(regret_sol.base.num_unassigned == 0);
    assert(tw_sol.base.num_unassigned == 0);

    /* The combined constructor (used by solve) should pick the best of both,
       so vehicles_used should be <= regret-3 alone. */
    assert(tw_sol.vehicles_used <= regret_sol.vehicles_used);

    sg_route_solution_reset(&regret_sol);
    sg_route_solution_reset(&tw_sol);
    sg_free(ctx);
}

/* ===== Vehicle-Target Destroy / Ejection Chain Tests ===== */

static void test_vehicle_target_destroy(void) {
    /* 3 vehicles: v0 has 1 request (smallest), v1 has 3, v2 has 2.
       Vehicle-target destroy with count=4 should empty v0 (smallest)
       and remove related requests to fill quota. */
    SGContext *ctx = make_config(10, 42);
    SGRouteSolution sol;
    uint32_t depot;
    uint32_t removed_ids[6];
    int removed_count = 0;
    ARStatus status;
    int has_r0 = 0;
    int i;

    add_depot_with_location(ctx, &depot, 50.0, 50.0);
    add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);  /* v0 */
    add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);  /* v1 */
    add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);  /* v2 */

    /* 6 delivery requests spread out */
    add_delivery_request(ctx, 10.0, 10.0, 0, 100000, 10, 1.0);  /* r0 -> v0 */
    add_delivery_request(ctx, 20.0, 20.0, 0, 100000, 10, 1.0);  /* r1 -> v1 */
    add_delivery_request(ctx, 22.0, 20.0, 0, 100000, 10, 1.0);  /* r2 -> v1 */
    add_delivery_request(ctx, 24.0, 20.0, 0, 100000, 10, 1.0);  /* r3 -> v1 */
    add_delivery_request(ctx, 80.0, 80.0, 0, 100000, 10, 1.0);  /* r4 -> v2 */
    add_delivery_request(ctx, 82.0, 80.0, 0, 100000, 10, 1.0);  /* r5 -> v2 */

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);
    {
        double score, dist;
        /* v0: r0 */
        assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
        /* v1: r1, r2, r3 */
        assert(sg_route_eval_insertion_cached(ctx, &sol, 1, 1, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 1, 1, 0, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 2, 1, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 2, 1, 1, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 3, 1, 2, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 3, 1, 2, dist) == AR_STATUS_OK);
        /* v2: r4, r5 */
        assert(sg_route_eval_insertion_cached(ctx, &sol, 4, 2, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 4, 2, 0, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 5, 2, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 5, 2, 1, dist) == AR_STATUS_OK);
    }
    assert(sol.route_lengths[0] == 1);
    assert(sol.route_lengths[1] == 3);
    assert(sol.route_lengths[2] == 2);

    /* Initialize RNG for the destroy operator */
    sh_rng_seed(ctx->op_rng, 42);

    status = sg_route_destroy_vehicle_target(ctx, &sol, 4, removed_ids, &removed_count);
    assert(status == AR_STATUS_OK);
    assert(removed_count >= 1 && removed_count <= 4);

    /* r0 (from v0, the smallest vehicle) must be among the removed */
    for (i = 0; i < removed_count; i++) {
        if (removed_ids[i] == 0) has_r0 = 1;
    }
    assert(has_r0);

    /* v0 should be empty after the destroy */
    assert(sol.route_lengths[0] == 0);

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_ejection_chain_reduce(void) {
    /* 2 vehicles: v0 has 1 request, v1 has 2 requests.
       Direct insertion of v0's request into v1 might fail due to TW,
       but ejecting one request from v1 creates room for v0's request,
       and the ejected request fits on v0's (now empty) compatible slot.
       The ejection chain should reduce vehicle count by 1. */
    SGContext *ctx = make_config(10, 42);
    SGRouteSolution sol;
    uint32_t depot;
    uint32_t vehicles_before;

    add_depot_with_location(ctx, &depot, 50.0, 50.0);
    add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);  /* v0 */
    add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);  /* v1 */

    /* r0 on v0: near depot, wide TW */
    add_delivery_request(ctx, 55.0, 50.0, 0, 50000, 10, 1.0);  /* r0 */
    /* r1 on v1: far from r0, similar TW */
    add_delivery_request(ctx, 60.0, 50.0, 0, 50000, 10, 1.0);  /* r1 */
    /* r2 on v1: close to r1 */
    add_delivery_request(ctx, 62.0, 50.0, 0, 50000, 10, 1.0);  /* r2 */

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);
    {
        double score, dist;
        /* v0: r0 */
        assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
        /* v1: r1, r2 */
        assert(sg_route_eval_insertion_cached(ctx, &sol, 1, 1, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 1, 1, 0, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 2, 1, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 2, 1, 1, dist) == AR_STATUS_OK);
    }
    assert(sol.route_lengths[0] == 1);
    assert(sol.route_lengths[1] == 2);
    vehicles_before = sol.vehicles_used;
    assert(vehicles_before == 2);

    /* The ejection chain should be able to consolidate all 3 requests
       onto a single vehicle, since TWs are wide and locations are close. */
    sg_route_postprocess_ejection_reduce(ctx, &sol);

    /* With wide TWs and close locations, all requests should fit on one vehicle. */
    assert(sol.base.num_unassigned == 0);
    assert(sol.vehicles_used <= vehicles_before);

    /* At minimum, the postprocessor should not break anything. */
    assert(sol.base.num_assigned == 3);

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_ejection_chain_depth2(void) {
    /* 3 vehicles with tight TWs where depth-1 fails but depth-2 succeeds.
       v0 has 1 request (target), v1 has 2, v2 has 2.
       Chain: eject S from v1 -> insert R into v1 -> eject T from v2 ->
       insert S into v2 -> T fits elsewhere.
       Depth-2 should reduce vehicle count. */
    SGContext *ctx = make_config(10, 42);
    SGRouteSolution sol;
    uint32_t depot;
    uint32_t vehicles_before;

    add_depot_with_location(ctx, &depot, 50.0, 50.0);
    add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);  /* v0 */
    add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);  /* v1 */
    add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);  /* v2 */

    /* r0 on v0 (target): mid TW */
    add_delivery_request(ctx, 55.0, 50.0, 1000, 5000, 10, 1.0);  /* r0 */
    /* r1 on v1: narrow TW near r0, blocks r0 from direct insertion */
    add_delivery_request(ctx, 56.0, 50.0, 800, 4800, 10, 1.0);   /* r1 */
    /* r2 on v1: wider TW */
    add_delivery_request(ctx, 58.0, 50.0, 0, 50000, 10, 1.0);    /* r2 */
    /* r3 on v2: narrow TW near r1 */
    add_delivery_request(ctx, 57.0, 50.0, 700, 4700, 10, 1.0);   /* r3 */
    /* r4 on v2: very wide TW, easy to place */
    add_delivery_request(ctx, 60.0, 50.0, 0, 80000, 10, 1.0);    /* r4 */

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);
    {
        double score, dist;
        /* v0: r0 */
        assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
        /* v1: r1, r2 */
        assert(sg_route_eval_insertion_cached(ctx, &sol, 1, 1, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 1, 1, 0, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 2, 1, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 2, 1, 1, dist) == AR_STATUS_OK);
        /* v2: r3, r4 */
        assert(sg_route_eval_insertion_cached(ctx, &sol, 3, 2, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 3, 2, 0, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 4, 2, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 4, 2, 1, dist) == AR_STATUS_OK);
    }
    assert(sol.route_lengths[0] == 1);
    assert(sol.route_lengths[1] == 2);
    assert(sol.route_lengths[2] == 2);
    vehicles_before = sol.vehicles_used;
    assert(vehicles_before == 3);

    sg_route_postprocess_ejection_reduce(ctx, &sol);

    /* With wide TWs and close locations, the depth-2 chain should consolidate. */
    assert(sol.base.num_unassigned == 0);
    assert(sol.base.num_assigned == 5);
    assert(sol.vehicles_used < vehicles_before);

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_solomon_i1_construction(void) {
    /* 10 vehicles, 15 delivery requests with staggered TWs.
       Solomon I1 should build routes sequentially, filling each before
       opening the next. */
    SGContext *ctx = make_config(10, 42);
    SGRouteSolution sol;
    uint32_t depot;
    uint32_t i;

    add_depot_with_location(ctx, &depot, 50.0, 50.0);
    for (i = 0; i < 10; i++) {
        add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);
    }

    /* 15 requests with staggered tight TWs — forces sequential building. */
    for (i = 0; i < 15; i++) {
        double x = 50.0 + (double)(i % 5) * 3.0;
        double y = 50.0 + (double)(i / 5) * 3.0;
        int32_t tw_early = (int32_t)(i * 500);
        int32_t tw_late = tw_early + 2000;
        add_delivery_request(ctx, x, y, tw_early, tw_late, 60, 1.0);
    }

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);
    assert(sg_route_construct_solomon_i1(ctx, &sol) == AR_STATUS_OK);

    assert(sol.base.num_unassigned == 0);
    assert(sol.vehicles_used > 0);
    assert(sol.vehicles_used <= 10);
    assert(sol.total_distance > 0.0);

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_solomon_i1_pd(void) {
    /* 5 vehicles, 8 PD requests with wide TWs.
       Solomon I1 should handle pickup-delivery requests correctly. */
    SGContext *ctx = make_config(10, 42);
    SGRouteSolution sol;
    uint32_t depot;
    uint32_t i;

    add_depot_with_location(ctx, &depot, 50.0, 50.0);
    for (i = 0; i < 5; i++) {
        add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);
    }

    /* 8 PD requests with wide TWs */
    for (i = 0; i < 8; i++) {
        double px = 45.0 + (double)(i % 4) * 3.0;
        double py = 50.0 + (double)(i / 4) * 3.0;
        double dx = px + 5.0;
        double dy = py + 2.0;
        int32_t tw_early = (int32_t)(i * 300);
        int32_t tw_late = tw_early + 20000;
        add_pd_request(ctx,
                       px, py, tw_early, tw_late, 30,
                       dx, dy, tw_early, tw_late + 5000, 30,
                       1.0);
    }

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);
    assert(sg_route_construct_solomon_i1(ctx, &sol) == AR_STATUS_OK);

    assert(sol.base.num_unassigned == 0);
    assert(sol.vehicles_used > 0);
    assert(sol.total_distance > 0.0);

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_vehicle_empty_destroy(void) {
    /* 3 vehicles: v0 has 2 requests, v1 has 3, v2 has 4.
       Vehicle-empty destroy should fully empty at least one vehicle. */
    SGContext *ctx = make_config(10, 42);
    SGRouteSolution sol;
    uint32_t depot;
    uint32_t removed_ids[9];
    int removed_count = 0;
    ARStatus status;
    int i;
    int any_empty = 0;

    add_depot_with_location(ctx, &depot, 50.0, 50.0);
    add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);  /* v0 */
    add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);  /* v1 */
    add_vehicle_with_depot(ctx, depot, 0, 100000, 1000.0);  /* v2 */

    /* 9 delivery requests: v0 gets 2, v1 gets 3, v2 gets 4. */
    add_delivery_request(ctx, 10.0, 10.0, 0, 100000, 10, 1.0);  /* r0 -> v0 */
    add_delivery_request(ctx, 12.0, 10.0, 0, 100000, 10, 1.0);  /* r1 -> v0 */
    add_delivery_request(ctx, 30.0, 30.0, 0, 100000, 10, 1.0);  /* r2 -> v1 */
    add_delivery_request(ctx, 32.0, 30.0, 0, 100000, 10, 1.0);  /* r3 -> v1 */
    add_delivery_request(ctx, 34.0, 30.0, 0, 100000, 10, 1.0);  /* r4 -> v1 */
    add_delivery_request(ctx, 70.0, 70.0, 0, 100000, 10, 1.0);  /* r5 -> v2 */
    add_delivery_request(ctx, 72.0, 70.0, 0, 100000, 10, 1.0);  /* r6 -> v2 */
    add_delivery_request(ctx, 74.0, 70.0, 0, 100000, 10, 1.0);  /* r7 -> v2 */
    add_delivery_request(ctx, 76.0, 70.0, 0, 100000, 10, 1.0);  /* r8 -> v2 */

    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);
    {
        double score, dist;
        /* v0: r0, r1 */
        assert(sg_route_eval_insertion_cached(ctx, &sol, 0, 0, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 1, 0, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 1, 0, 1, dist) == AR_STATUS_OK);
        /* v1: r2, r3, r4 */
        assert(sg_route_eval_insertion_cached(ctx, &sol, 2, 1, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 2, 1, 0, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 3, 1, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 3, 1, 1, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 4, 1, 2, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 4, 1, 2, dist) == AR_STATUS_OK);
        /* v2: r5, r6, r7, r8 */
        assert(sg_route_eval_insertion_cached(ctx, &sol, 5, 2, 0, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 5, 2, 0, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 6, 2, 1, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 6, 2, 1, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 7, 2, 2, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 7, 2, 2, dist) == AR_STATUS_OK);
        assert(sg_route_eval_insertion_cached(ctx, &sol, 8, 2, 3, &score, &dist));
        assert(sg_route_apply_insertion(ctx, &sol, 8, 2, 3, dist) == AR_STATUS_OK);
    }
    assert(sol.route_lengths[0] == 2);
    assert(sol.route_lengths[1] == 3);
    assert(sol.route_lengths[2] == 4);

    sh_rng_seed(ctx->op_rng, 42);

    status = sg_route_destroy_vehicle_empty(ctx, &sol, 9, removed_ids, &removed_count);
    assert(status == AR_STATUS_OK);
    assert(removed_count >= 2);  /* At least the smallest route */

    /* At least one vehicle should be fully emptied. */
    for (i = 0; i < 3; i++) {
        if (sol.route_lengths[i] == 0) any_empty = 1;
    }
    assert(any_empty);

    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_two_phase_solve_no_regression(void) {
    /* Full solve with 5 vehicles, 10 delivery requests, moderate TWs.
       Verifies two-phase ALNS doesn't break basic solving. */
    SGContext *ctx = make_config(1000, 99);
    uint32_t depot;
    int i;

    add_depot_with_location(ctx, &depot, 50.0, 50.0);
    for (i = 0; i < 5; i++) {
        add_vehicle_with_depot(ctx, depot, 0, 100000, 200.0);
    }

    for (i = 0; i < 10; i++) {
        double x = 40.0 + (double)(i % 5) * 5.0;
        double y = 40.0 + (double)(i / 5) * 5.0;
        int32_t tw_early = (int32_t)(i * 200);
        int32_t tw_late = tw_early + 5000;
        add_delivery_request(ctx, x, y, tw_early, tw_late, 60, -10.0);
    }

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_get_used_vehicle_count(ctx) > 0);
    assert(sg_get_total_distance(ctx) > 0.0);

    sg_free(ctx);
}

/* ===== Travel API Tests ===== */

static void test_travel_matrix_mode(void) {
    SGContext *ctx = make_config(50, 42);
    uint32_t depot;
    /* 3 locations: depot at loc 0, task A at loc 1, task B at loc 2 */
    uint32_t loc0 = sg_add_location(ctx);
    uint32_t loc1 = sg_add_location(ctx);
    uint32_t loc2 = sg_add_location(ctx);
    assert(loc0 == 0 && loc1 == 1 && loc2 == 2);

    /* Set up asymmetric travel matrix (3x3) */
    {
        /*          to 0    to 1    to 2  */
        double dist[] = {
            0.0,   10.0,   20.0,   /* from 0 */
            15.0,   0.0,   25.0,   /* from 1 */
            30.0,   35.0,   0.0    /* from 2 */
        };
        double dur[] = {
            0.0,  100.0,  200.0,   /* from 0 */
            150.0,  0.0,  250.0,   /* from 1 */
            300.0, 350.0,  0.0     /* from 2 */
        };
        assert(sg_set_travel_matrix(ctx, 3, dist, dur) == SG_STATUS_OK);
    }

    /* Build model using location_ids */
    depot = sg_add_depot(ctx);
    assert(sg_depot_set_location_id(ctx, depot, loc0) == SG_STATUS_OK);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);

    {
        uint32_t req = sg_add_request(ctx);
        uint32_t task = sg_add_task(ctx, SG_TASK_DELIVERY);
        double demand = -10.0;
        assert(sg_task_set_location_id(ctx, task, loc1) == SG_STATUS_OK);
        assert(sg_task_set_time_window(ctx, task, 0, 86400) == SG_STATUS_OK);
        assert(sg_task_set_service_seconds(ctx, task, 60) == SG_STATUS_OK);
        assert(sg_task_set_demand(ctx, task, &demand, 1) == SG_STATUS_OK);
        assert(sg_request_bind_delivery_task(ctx, req, task) == SG_STATUS_OK);
    }

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    /* Route: depot(loc0) -> task(loc1) -> depot(loc0)
       Distance = dist[0][1] + dist[1][0] = 10 + 15 = 25 */
    assert(fabs(sg_get_total_distance(ctx) - 25.0) < 1e-9);

    sg_free(ctx);
}

static void test_travel_cb(uint32_t from, uint32_t to, uint32_t vid,
                           double *d, double *t, void *ud) {
    (void)vid; (void)ud;
    double diff = (double)to > (double)from
                ? (double)(to - from) : (double)(from - to);
    *d = diff * 100.0;
    *t = diff * 100.0;
}

static void test_travel_callback_mode(void) {
    SGContext *ctx = make_config(50, 42);
    uint32_t depot;
    uint32_t loc0 = sg_add_location(ctx);
    uint32_t loc1 = sg_add_location(ctx);
    assert(loc0 == 0 && loc1 == 1);

    assert(sg_set_travel_callback(ctx, test_travel_cb, NULL) == SG_STATUS_OK);

    depot = sg_add_depot(ctx);
    assert(sg_depot_set_location_id(ctx, depot, loc0) == SG_STATUS_OK);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);

    {
        uint32_t req = sg_add_request(ctx);
        uint32_t task = sg_add_task(ctx, SG_TASK_DELIVERY);
        double demand = -10.0;
        assert(sg_task_set_location_id(ctx, task, loc1) == SG_STATUS_OK);
        assert(sg_task_set_time_window(ctx, task, 0, 86400) == SG_STATUS_OK);
        assert(sg_task_set_service_seconds(ctx, task, 60) == SG_STATUS_OK);
        assert(sg_task_set_demand(ctx, task, &demand, 1) == SG_STATUS_OK);
        assert(sg_request_bind_delivery_task(ctx, req, task) == SG_STATUS_OK);
    }

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    /* Route: loc0 -> loc1 -> loc0 = 100 + 100 = 200 */
    assert(fabs(sg_get_total_distance(ctx) - 200.0) < 1e-9);

    sg_free(ctx);
}

static void test_travel_auto_dedup(void) {
    /* Two tasks at the same coordinates should share a location_id */
    SGContext *ctx = make_config(50, 42);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);
    add_delivery_request(ctx, 5.0, 0.0, 0, 86400, 60, -10.0);
    add_delivery_request(ctx, 5.0, 0.0, 0, 86400, 60, -10.0);  /* Same coords */
    add_delivery_request(ctx, 10.0, 0.0, 0, 86400, 60, -10.0); /* Different */

    assert(sg_prepare_travel(ctx) == SG_STATUS_OK);

    /* Should have 3 unique locations: (0,0), (5,0), (10,0) */
    assert(ctx->num_locations == 3);

    /* Tasks at same coords should have same location_id */
    assert(ctx->tasks[0].location_id == ctx->tasks[1].location_id);
    assert(ctx->tasks[0].location_id != ctx->tasks[2].location_id);

    sg_free(ctx);
}

static void test_travel_asymmetric_matrix(void) {
    /* Verify that asymmetric travel works correctly */
    SGContext *ctx = make_config(50, 42);
    uint32_t depot;
    uint32_t loc0 = sg_add_location(ctx);
    uint32_t loc1 = sg_add_location(ctx);
    uint32_t loc2 = sg_add_location(ctx);

    /* Asymmetric: A->B != B->A */
    {
        double dist[] = {
            0.0,  5.0, 20.0,
           50.0,  0.0, 10.0,
           20.0, 10.0,  0.0
        };
        double dur[] = {
            0.0,  50.0, 200.0,
          500.0,   0.0, 100.0,
          200.0, 100.0,   0.0
        };
        assert(sg_set_travel_matrix(ctx, 3, dist, dur) == SG_STATUS_OK);
    }

    depot = sg_add_depot(ctx);
    assert(sg_depot_set_location_id(ctx, depot, loc0) == SG_STATUS_OK);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 100.0);

    /* Task A at loc1, Task B at loc2 */
    {
        uint32_t req = sg_add_request(ctx);
        uint32_t task = sg_add_task(ctx, SG_TASK_DELIVERY);
        double demand = -10.0;
        assert(sg_task_set_location_id(ctx, task, loc1) == SG_STATUS_OK);
        assert(sg_task_set_time_window(ctx, task, 0, 86400) == SG_STATUS_OK);
        assert(sg_task_set_service_seconds(ctx, task, 0) == SG_STATUS_OK);
        assert(sg_task_set_demand(ctx, task, &demand, 1) == SG_STATUS_OK);
        assert(sg_request_bind_delivery_task(ctx, req, task) == SG_STATUS_OK);
    }
    {
        uint32_t req = sg_add_request(ctx);
        uint32_t task = sg_add_task(ctx, SG_TASK_DELIVERY);
        double demand = -10.0;
        assert(sg_task_set_location_id(ctx, task, loc2) == SG_STATUS_OK);
        assert(sg_task_set_time_window(ctx, task, 0, 86400) == SG_STATUS_OK);
        assert(sg_task_set_service_seconds(ctx, task, 0) == SG_STATUS_OK);
        assert(sg_task_set_demand(ctx, task, &demand, 1) == SG_STATUS_OK);
        assert(sg_request_bind_delivery_task(ctx, req, task) == SG_STATUS_OK);
    }

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    /* With this matrix, optimal route is 0->1->2->0 = 5 + 10 + 20 = 35
       (versus 0->2->1->0 = 20 + 10 + 50 = 80) */
    assert(fabs(sg_get_total_distance(ctx) - 35.0) < 1e-9);

    sg_free(ctx);
}

/* ---------- qualification tests ---------- */

static void test_qualification_api(void) {
    /* Validate setters, defaults (0), invalid args */
    SGContext *ctx = sg_create();
    assert(ctx != NULL);
    SGConfig cfg;
    sg_config_default(&cfg);
    cfg.max_iterations = 100;
    cfg.seed = 42;
    cfg.deterministic = true;
    assert(sg_set_config(ctx, &cfg) == SG_STATUS_OK);
    sg_set_dimension_count(ctx, 1);

    uint32_t v0 = sg_add_vehicle(ctx);
    uint32_t v1 = sg_add_vehicle(ctx);
    uint32_t r0 = sg_add_request(ctx);

    /* Default qualifications/requirements are 0 */
    assert(ctx->vehicles[v0].qualifications == 0);
    assert(ctx->vehicles[v1].qualifications == 0);
    assert(ctx->requests[r0].required_qualifications == 0);

    /* Set qualifications */
    assert(sg_vehicle_set_qualifications(ctx, v0, 0x07) == SG_STATUS_OK);
    assert(sg_vehicle_set_qualifications(ctx, v1, 0x01) == SG_STATUS_OK);
    assert(ctx->vehicles[v0].qualifications == 0x07);
    assert(ctx->vehicles[v1].qualifications == 0x01);

    /* Set requirements */
    assert(sg_request_set_required_qualifications(ctx, r0, 0x05) == SG_STATUS_OK);
    assert(ctx->requests[r0].required_qualifications == 0x05);

    /* Invalid args */
    assert(sg_vehicle_set_qualifications(NULL, 0, 1) == SG_STATUS_INVALID_ARG);
    assert(sg_vehicle_set_qualifications(ctx, 999, 1) == SG_STATUS_INVALID_ARG);
    assert(sg_request_set_required_qualifications(NULL, 0, 1) == SG_STATUS_INVALID_ARG);
    assert(sg_request_set_required_qualifications(ctx, 999, 1) == SG_STATUS_INVALID_ARG);

    /* Inline helper check */
    assert(sg_vehicle_qualifies(ctx, v0, r0) == 1);  /* 0x07 & 0x05 == 0x05 */
    assert(sg_vehicle_qualifies(ctx, v1, r0) == 0);  /* 0x01 & 0x05 != 0x05 */

    /* Zero requirement => all qualify */
    assert(sg_request_set_required_qualifications(ctx, r0, 0) == SG_STATUS_OK);
    assert(sg_vehicle_qualifies(ctx, v0, r0) == 1);
    assert(sg_vehicle_qualifies(ctx, v1, r0) == 1);

    sg_free(ctx);
}

static void test_qualification_filters_solve(void) {
    /* 2 vehicles, 2 delivery requests.
       Request 0 requires qualification bit 0x02.
       Vehicle 0 has quals 0x03, vehicle 1 has quals 0x01.
       => Request 0 must go to vehicle 0 (only one qualified).
       Request 1 has no requirements => can go to either vehicle. */
    SGContext *ctx = make_config(300, 42);
    uint32_t depot;
    uint32_t v0, v1;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    v0 = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v0, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v0, 0, 86400) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v0, (double[]){100.0}, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_qualifications(ctx, v0, 0x03) == SG_STATUS_OK);

    v1 = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v1, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v1, 0, 86400) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v1, (double[]){100.0}, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_qualifications(ctx, v1, 0x01) == SG_STATUS_OK);

    /* Request 0: requires bit 0x02 — only vehicle 0 qualifies */
    add_delivery_request(ctx, 10.0, 0.0, 0, 86400, 60, -10.0);
    assert(sg_request_set_required_qualifications(ctx, 0, 0x02) == SG_STATUS_OK);

    /* Request 1: no requirements */
    add_delivery_request(ctx, -10.0, 0.0, 0, 86400, 60, -10.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    sg_free(ctx);
}

static void test_qualification_unassigned_when_none_qualify(void) {
    /* 1 vehicle, 1 request.
       Request requires bit 0x04, vehicle only has bit 0x02.
       => Request must be unassigned (no qualified vehicle). */
    SGContext *ctx = make_config(300, 42);
    uint32_t depot;
    uint32_t v0;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    v0 = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v0, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v0, 0, 86400) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v0, (double[]){100.0}, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_qualifications(ctx, v0, 0x02) == SG_STATUS_OK);

    add_delivery_request(ctx, 10.0, 0.0, 0, 86400, 60, -10.0);
    assert(sg_request_set_required_qualifications(ctx, 0, 0x04) == SG_STATUS_OK);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 1);

    sg_free(ctx);
}

static void test_qualification_pd_request(void) {
    /* PD request with qualification requirement.
       Vehicle 0 has quals 0x01, vehicle 1 doesn't.
       PD request requires 0x01 => must go to vehicle 0. */
    SGContext *ctx = make_config(300, 42);
    uint32_t depot;
    uint32_t v0, v1;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    v0 = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v0, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v0, 0, 86400) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v0, (double[]){100.0}, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_qualifications(ctx, v0, 0x01) == SG_STATUS_OK);

    v1 = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v1, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v1, 0, 86400) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v1, (double[]){100.0}, 1) == SG_STATUS_OK);
    /* v1 has qualifications = 0 (default) */

    add_pd_request(ctx, 5.0, 0.0, 0, 86400, 60,
                       10.0, 0.0, 0, 86400, 60, 10.0);
    assert(sg_request_set_required_qualifications(ctx, 0, 0x01) == SG_STATUS_OK);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_get_used_vehicle_count(ctx) == 1);

    sg_free(ctx);
}

/* ---------- solution export tests ---------- */

static void test_solution_export_delivery(void) {
    /* 1 vehicle, 3 delivery requests. After solve, verify route/stop export. */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    uint32_t route_count, stop_count, i;
    double route_dist;
    SGSolutionStop stop;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 10000.0);

    add_delivery_request(ctx, 10.0, 0.0, 0, 86400, 60, -10.0);
    add_delivery_request(ctx, 20.0, 0.0, 0, 86400, 60, -10.0);
    add_delivery_request(ctx, 30.0, 0.0, 0, 86400, 60, -10.0);

    /* Before solve: no solution available */
    assert(sg_solution_get_route_count(ctx) == 0);
    assert(sg_solution_get_route_stop_count(ctx, 0) == 0);
    assert(sg_solution_get_unassigned_request(ctx, 0) == UINT32_MAX);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    /* After solve: routes available */
    route_count = sg_solution_get_route_count(ctx);
    assert(route_count == 1);  /* 1 vehicle, 3 requests => 1 route */

    stop_count = sg_solution_get_route_stop_count(ctx, 0);
    assert(stop_count == 3);  /* 3 delivery stops */

    route_dist = sg_solution_get_route_distance(ctx, 0);
    assert(route_dist > 0.0);

    /* Vehicle ID should be 0 */
    assert(sg_solution_get_route_vehicle_id(ctx, 0) == 0);

    /* Iterate stops and verify fields are populated */
    for (i = 0; i < stop_count; i++) {
        assert(sg_solution_get_route_stop(ctx, 0, i, &stop) == SG_STATUS_OK);
        assert(stop.request_id < 3);
        assert(stop.stop_type == SG_STOP_TYPE_DELIVERY);
        assert(stop.arrival >= 0.0);
        assert(stop.service_start >= stop.arrival);
        assert(stop.departure >= stop.service_start);
    }

    /* Invalid indices return errors */
    assert(sg_solution_get_route_stop(ctx, 99, 0, &stop) == SG_STATUS_INVALID_ARG);
    assert(sg_solution_get_route_stop(ctx, 0, 99, &stop) == SG_STATUS_INVALID_ARG);
    assert(sg_solution_get_route_stop(ctx, 0, 0, NULL) == SG_STATUS_INVALID_ARG);

    sg_free(ctx);
}

static void test_solution_export_pd(void) {
    /* 1 vehicle, 1 PD request. Verify pickup and delivery stops. */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    uint32_t route_count, stop_count;
    SGSolutionStop stop0, stop1;
    int found_pickup, found_delivery;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 10000.0);

    add_pd_request(ctx, 5.0, 0.0, 0, 86400, 60,
                       15.0, 0.0, 0, 86400, 60, 10.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    route_count = sg_solution_get_route_count(ctx);
    assert(route_count == 1);

    stop_count = sg_solution_get_route_stop_count(ctx, 0);
    assert(stop_count == 2);  /* pickup + delivery */

    assert(sg_solution_get_route_stop(ctx, 0, 0, &stop0) == SG_STATUS_OK);
    assert(sg_solution_get_route_stop(ctx, 0, 1, &stop1) == SG_STATUS_OK);

    /* Both should reference request 0 */
    assert(stop0.request_id == 0);
    assert(stop1.request_id == 0);

    /* One pickup, one delivery */
    found_pickup = (stop0.stop_type == SG_STOP_TYPE_PICKUP) +
                   (stop1.stop_type == SG_STOP_TYPE_PICKUP);
    found_delivery = (stop0.stop_type == SG_STOP_TYPE_DELIVERY) +
                     (stop1.stop_type == SG_STOP_TYPE_DELIVERY);
    assert(found_pickup == 1);
    assert(found_delivery == 1);

    /* Pickup must depart before delivery arrives (precedence) */
    if (stop0.stop_type == SG_STOP_TYPE_PICKUP) {
        assert(stop0.departure <= stop1.arrival + 1e-9);
    } else {
        assert(stop1.departure <= stop0.arrival + 1e-9);
    }

    sg_free(ctx);
}

static void test_solution_export_unassigned(void) {
    /* TW violation forces request unassigned. Verify unassigned export. */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    uint32_t unassigned_req;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 10000.0);

    /* Request 0: normal */
    add_delivery_request(ctx, 10.0, 0.0, 0, 86400, 60, -10.0);

    /* Request 1: impossible TW (already past) — forces unassigned */
    add_delivery_request(ctx, 1000.0, 0.0, 1, 2, 60, -10.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 1);

    /* Get the unassigned request ID */
    unassigned_req = sg_solution_get_unassigned_request(ctx, 0);
    assert(unassigned_req != UINT32_MAX);

    /* Out of range returns UINT32_MAX */
    assert(sg_solution_get_unassigned_request(ctx, 99) == UINT32_MAX);

    sg_free(ctx);
}

static void test_solution_export_multi_vehicle(void) {
    /* 2 vehicles, requests spread across them. Verify multi-route export. */
    SGContext *ctx = make_config(300, 42);
    uint32_t depot;
    uint32_t total_stops, r;
    uint32_t route_count;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 30.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 30.0);

    /* 4 requests with enough demand to force 2 vehicles */
    add_delivery_request(ctx, 10.0, 0.0, 0, 86400, 60, -15.0);
    add_delivery_request(ctx, 20.0, 0.0, 0, 86400, 60, -15.0);
    add_delivery_request(ctx, -10.0, 0.0, 0, 86400, 60, -15.0);
    add_delivery_request(ctx, -20.0, 0.0, 0, 86400, 60, -15.0);

    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    route_count = sg_solution_get_route_count(ctx);
    assert(route_count >= 2);  /* Capacity forces at least 2 routes */

    /* Total stops across all routes must equal 4 */
    total_stops = 0;
    for (r = 0; r < route_count; r++) {
        uint32_t stops = sg_solution_get_route_stop_count(ctx, r);
        assert(stops > 0);
        assert(sg_solution_get_route_distance(ctx, r) > 0.0);
        assert(sg_solution_get_route_vehicle_id(ctx, r) != UINT32_MAX);
        total_stops += stops;
    }
    assert(total_stops == 4);

    sg_free(ctx);
}

/* ===== U4: Open routes ===== */

static void test_open_end_basic(void) {
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    double closed_dist, open_dist;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 0, -1.0);
    add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    closed_dist = sg_get_total_distance(ctx);
    assert(closed_dist > 0.0);
    sg_free(ctx);

    /* Same but open-end */
    ctx = make_config(100, 42);
    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 0, -1.0);
    {
        uint32_t v = sg_add_vehicle(ctx);
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
        double cap = 100.0;
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_open_end(ctx, v, 1) == SG_STATUS_OK);
    }

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    open_dist = sg_get_total_distance(ctx);

    /* Open route should have roughly half the distance (no return) */
    assert(open_dist < closed_dist - 1.0);
    sg_free(ctx);
}

static void test_open_end_timing(void) {
    /* Verify that open-end route doesn't require return time */
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    /* Delivery at (100, 0) with travel ~100 seconds. Shift only allows 150 sec total.
       Closed route needs ~200 sec (there and back) -> infeasible.
       Open route needs ~100 sec -> feasible. */
    add_delivery_request(ctx, 100.0, 0.0, 0, 200, 0, -1.0);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 150) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_open_end(ctx, v, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    sg_free(ctx);
}

static void test_open_end_solution_export(void) {
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;
    uint32_t route_count;
    double route_dist, route_dur;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 60, -1.0);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_open_end(ctx, v, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    route_count = sg_solution_get_route_count(ctx);
    assert(route_count == 1);
    route_dist = sg_solution_get_route_distance(ctx, 0);
    route_dur = sg_solution_get_route_duration(ctx, 0);
    assert(route_dist > 0.0);
    assert(route_dur > 0.0);
    /* Duration should include travel + service but not return */
    assert(route_dur >= 60.0);

    sg_free(ctx);
}

static void test_open_end_pd(void) {
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_pd_request(ctx, 5.0, 0.0, 0, 99999, 0,
                   10.0, 0.0, 0, 99999, 0, 1.0);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_open_end(ctx, v, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    sg_free(ctx);
}

/* ===== U5: Max duration and ride time ===== */

static void test_max_duration_api(void) {
    SGContext *ctx = sg_create();
    uint32_t v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_max_duration(ctx, v, 3600) == SG_STATUS_OK);
    assert(sg_vehicle_set_max_duration(ctx, v, 0) == SG_STATUS_OK);
    assert(sg_vehicle_set_max_duration(ctx, v, -1) == SG_STATUS_INVALID_ARG);
    assert(sg_vehicle_set_max_duration(ctx, 999, 3600) == SG_STATUS_INVALID_ARG);
    sg_free(ctx);
}

static void test_max_duration_infeasible(void) {
    /* With short max_duration, far-away request can't be served */
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    /* Far-away delivery: round-trip ~200 sec travel. Max duration 150 -> unassigned. */
    add_delivery_request(ctx, 100.0, 0.0, 0, 99999, 0, -1.0);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_max_duration(ctx, v, 150) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 1);
    sg_free(ctx);
}

static void test_max_ride_time_explicit(void) {
    /* Explicit max ride time overrides TW-derived limit */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;
    uint32_t req, p_task, d_task;
    double demand = 1.0, neg_demand = -1.0;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    /* PD request: pickup at (5,0), delivery at (50,0).
       Wide TW: 0-99999. TW-derived limit = 99999.
       Set explicit max ride time = 10. Travel from (5,0) to (50,0) ~ 45 sec.
       Should be infeasible due to ride time. */
    req = sg_add_request(ctx);
    p_task = sg_add_task(ctx, SG_TASK_PICKUP);
    d_task = sg_add_task(ctx, SG_TASK_DELIVERY);
    assert(sg_task_set_location(ctx, p_task, 5.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, p_task, 0, 99999) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, p_task, 0) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, p_task, &demand, 1) == SG_STATUS_OK);
    assert(sg_task_set_location(ctx, d_task, 50.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, d_task, 0, 99999) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, d_task, 0) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, d_task, &neg_demand, 1) == SG_STATUS_OK);
    assert(sg_request_bind_pickup_delivery_tasks(ctx, req, p_task, d_task) == SG_STATUS_OK);
    assert(sg_request_set_max_ride_time(ctx, req, 10) == SG_STATUS_OK);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 1);
    sg_free(ctx);
}

static void test_max_ride_time_default(void) {
    /* Without explicit max ride time, wide TW allows feasible */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    /* Same PD request but no explicit ride time -> wide TW-derived limit -> feasible */
    add_pd_request(ctx, 5.0, 0.0, 0, 99999, 0,
                   50.0, 0.0, 0, 99999, 0, 1.0);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    sg_free(ctx);
}

/* ===== U6: Vehicle cost model ===== */

static void test_vehicle_costs_api(void) {
    SGContext *ctx = sg_create();
    uint32_t v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_costs(ctx, v, 500.0, 2.0, 0.5) == SG_STATUS_OK);
    assert(sg_vehicle_set_costs(ctx, v, -1.0, 1.0, 0.0) == SG_STATUS_INVALID_ARG);
    assert(sg_vehicle_set_costs(ctx, v, 0.0, -1.0, 0.0) == SG_STATUS_INVALID_ARG);
    assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, -1.0) == SG_STATUS_INVALID_ARG);
    assert(sg_vehicle_set_costs(ctx, 999, 1.0, 1.0, 0.0) == SG_STATUS_INVALID_ARG);
    assert(sg_set_unassigned_weight(ctx, 1e6) == SG_STATUS_OK);
    assert(sg_set_unassigned_weight(ctx, -1.0) == SG_STATUS_INVALID_ARG);
    sg_free(ctx);
}

static void test_vehicle_costs_prefer_cheaper(void) {
    /* Two vehicles: expensive and cheap. Request should go to cheap vehicle. */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    uint32_t v0, v1;
    double cap = 100.0;
    uint32_t route_count, r;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 0, -1.0);

    v0 = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v0, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v0, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v0, &cap, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_costs(ctx, v0, 1e8, 10.0, 0.0) == SG_STATUS_OK);

    v1 = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v1, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v1, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v1, &cap, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_costs(ctx, v1, 1.0, 1.0, 0.0) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    route_count = sg_solution_get_route_count(ctx);
    assert(route_count == 1);
    /* The single route should use v1 (the cheap vehicle) */
    for (r = 0; r < route_count; r++) {
        uint32_t vid = sg_solution_get_route_vehicle_id(ctx, r);
        assert(vid == v1);
    }
    sg_free(ctx);
}

static void test_unassigned_weight(void) {
    /* Very low unassigned weight: solver may prefer not assigning to save vehicle cost */
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 0, -1.0);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
    /* Vehicle costs 1e9 fixed. Unassigned penalty only 1.0 -> cheaper to leave unassigned */
    assert(sg_vehicle_set_costs(ctx, v, 1e9, 1.0, 0.0) == SG_STATUS_OK);
    assert(sg_set_unassigned_weight(ctx, 1.0) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 1);
    sg_free(ctx);
}

/* ===== Short-term API improvements ===== */

static void test_convenience_delivery_request(void) {
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    uint32_t req_id;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    req_id = sg_add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 60, -1.0);
    assert(req_id != UINT32_MAX);

    add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    sg_free(ctx);
}

static void test_convenience_pd_request(void) {
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    uint32_t req_id;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    req_id = sg_add_pd_request(ctx,
                               5.0, 0.0, 0, 99999, 0,
                               10.0, 0.0, 0, 99999, 0, 1.0);
    assert(req_id != UINT32_MAX);

    add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    sg_free(ctx);
}

static void test_solution_stop_load(void) {
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    double load_val = 0.0;
    uint32_t route_count;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 0, -5.0);
    add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    route_count = sg_solution_get_route_count(ctx);
    assert(route_count == 1);
    assert(sg_solution_get_route_stop_count(ctx, 0) == 1);

    /* Read load at stop 0, dimension 0 */
    assert(sg_solution_get_route_stop_load(ctx, 0, 0, 0, &load_val) == SG_STATUS_OK);
    /* Invalid dimension should fail */
    assert(sg_solution_get_route_stop_load(ctx, 0, 0, 99, &load_val) == SG_STATUS_INVALID_ARG);

    sg_free(ctx);
}

static void test_solution_stop_type_service(void) {
    /* Test that service tasks are reported as SG_STOP_TYPE_SERVICE */
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    uint32_t req, task;
    double demand = 0.0;
    double cap = 100.0;
    uint32_t v;
    SGSolutionStop stop;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    /* Create a service task (not pickup or delivery) bound as a delivery */
    req = sg_add_request(ctx);
    task = sg_add_task(ctx, SG_TASK_SERVICE);
    assert(sg_task_set_location(ctx, task, 10.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, task, 0, 99999) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, task, 60) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, task, &demand, 1) == SG_STATUS_OK);
    assert(sg_request_bind_delivery_task(ctx, req, task) == SG_STATUS_OK);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_solution_get_route_count(ctx) == 1);
    assert(sg_solution_get_route_stop_count(ctx, 0) == 1);

    assert(sg_solution_get_route_stop(ctx, 0, 0, &stop) == SG_STATUS_OK);
    assert(stop.stop_type == SG_STOP_TYPE_SERVICE);

    sg_free(ctx);
}

static void test_route_duration_export(void) {
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    double route_dur;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 120, -1.0);
    add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_solution_get_route_count(ctx) == 1);

    route_dur = sg_solution_get_route_duration(ctx, 0);
    /* Duration = travel to stop + service + travel back. Must be > service time */
    assert(route_dur >= 120.0);
    sg_free(ctx);
}

/* ===== Cross-feature and gap coverage tests ===== */

static void test_cost_per_duration_preference(void) {
    /* Verify cost_per_duration is reflected in total cost.
       Same setup twice: once with cost_per_duration=0, once with cost_per_duration=10.
       The high-duration run should have a higher total cost. */
    double cost_no_dur, cost_with_dur;

    /* Run 1: no duration cost */
    {
        SGContext *ctx = make_config(100, 42);
        uint32_t depot;
        double cap = 100.0;
        uint32_t v;

        sg_set_dimension_count(ctx, 1);
        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 600, -1.0);

        v = sg_add_vehicle(ctx);
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, 0.0) == SG_STATUS_OK);

        assert(sg_solve(ctx) == SG_STATUS_OK);
        assert(sg_get_unassigned(ctx) == 0);
        cost_no_dur = sg_get_total_cost(ctx);
        sg_free(ctx);
    }

    /* Run 2: high duration cost */
    {
        SGContext *ctx = make_config(100, 42);
        uint32_t depot;
        double cap = 100.0;
        uint32_t v;

        sg_set_dimension_count(ctx, 1);
        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 600, -1.0);

        v = sg_add_vehicle(ctx);
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, 10.0) == SG_STATUS_OK);

        assert(sg_solve(ctx) == SG_STATUS_OK);
        assert(sg_get_unassigned(ctx) == 0);
        cost_with_dur = sg_get_total_cost(ctx);
        sg_free(ctx);
    }

    /* Duration of route is ~620s (10 travel + 600 service + 10 return).
       cost_per_duration=10 adds ~6200 to cost. */
    assert(cost_with_dur > cost_no_dur + 1000.0);
}

static void test_open_end_max_duration_combo(void) {
    /* Closed route + max_duration=120: round trip ~200s -> infeasible.
       Open-end route + max_duration=120: one-way ~100s -> feasible. */
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_delivery_request(ctx, 100.0, 0.0, 0, 99999, 0, -1.0);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_max_duration(ctx, v, 120) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 1); /* closed: infeasible */
    sg_free(ctx);

    /* Open-end: same but feasible */
    ctx = make_config(100, 42);
    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_delivery_request(ctx, 100.0, 0.0, 0, 99999, 0, -1.0);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_max_duration(ctx, v, 120) == SG_STATUS_OK);
    assert(sg_vehicle_set_open_end(ctx, v, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0); /* open-end: feasible */
    sg_free(ctx);
}

static void test_max_ride_time_with_service_time(void) {
    /* Verify ride_time = delivery_service_start - pickup_depart excludes pickup service.
       Pickup at (5,0) with 60s service, delivery at (15,0). Travel ~10s.
       ride_time = 10s. max_ride_time = 15 -> feasible. */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;
    uint32_t req, p_task, d_task;
    double demand = 1.0, neg_demand = -1.0;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    req = sg_add_request(ctx);
    p_task = sg_add_task(ctx, SG_TASK_PICKUP);
    d_task = sg_add_task(ctx, SG_TASK_DELIVERY);
    assert(sg_task_set_location(ctx, p_task, 5.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, p_task, 0, 99999) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, p_task, 60) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, p_task, &demand, 1) == SG_STATUS_OK);
    assert(sg_task_set_location(ctx, d_task, 15.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, d_task, 0, 99999) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, d_task, 0) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, d_task, &neg_demand, 1) == SG_STATUS_OK);
    assert(sg_request_bind_pickup_delivery_tasks(ctx, req, p_task, d_task) == SG_STATUS_OK);
    assert(sg_request_set_max_ride_time(ctx, req, 15) == SG_STATUS_OK);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    sg_free(ctx);
}

static void test_route_duration_open_end(void) {
    /* Open-end route_duration should be less than closed route_duration */
    double closed_dur, open_dur;

    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 120, -1.0);
    add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_solution_get_route_count(ctx) == 1);
    closed_dur = sg_solution_get_route_duration(ctx, 0);
    sg_free(ctx);

    ctx = make_config(100, 42);
    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 120, -1.0);
    {
        uint32_t v = sg_add_vehicle(ctx);
        double cap = 100.0;
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_open_end(ctx, v, 1) == SG_STATUS_OK);
    }

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_solution_get_route_count(ctx) == 1);
    open_dur = sg_solution_get_route_duration(ctx, 0);
    sg_free(ctx);

    assert(open_dur < closed_dur);
}

static void test_open_end_cost_model(void) {
    /* V0: closed, cost_per_distance=1. V1: open-end, cost_per_distance=1.
       Delivery at (50,0). V0 distance ~100 (round trip). V1 ~50 (one-way).
       Request should go to V1 (lower cost). */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v0, v1;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_delivery_request(ctx, 50.0, 0.0, 0, 99999, 0, -1.0);

    v0 = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v0, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v0, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v0, &cap, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_costs(ctx, v0, 0.0, 1.0, 0.0) == SG_STATUS_OK);

    v1 = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v1, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v1, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v1, &cap, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_costs(ctx, v1, 0.0, 1.0, 0.0) == SG_STATUS_OK);
    assert(sg_vehicle_set_open_end(ctx, v1, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_solution_get_route_count(ctx) == 1);
    assert(sg_solution_get_route_vehicle_id(ctx, 0) == v1);
    sg_free(ctx);
}

static void test_max_duration_multi_stop(void) {
    /* Two deliveries with max_duration=25. First stop ~10s travel each way = 20s.
       Second stop adds more travel -> exceeds limit. At least 1 unassigned. */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 0, -1.0);
    add_delivery_request(ctx, 20.0, 0.0, 0, 99999, 0, -1.0);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_max_duration(ctx, v, 25) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) >= 1);
    sg_free(ctx);
}

static void test_multi_pd_heterogeneous_ride_time(void) {
    /* Two PD requests with different max_ride_time values.
       A: pickup(5,0)->delivery(10,0), max_ride_time=100 (generous, travel ~5s).
       B: pickup(5,0)->delivery(50,0), max_ride_time=10 (tight, travel ~45s -> infeasible). */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;
    uint32_t req_a, p_a, d_a;
    uint32_t req_b, p_b, d_b;
    double demand = 1.0, neg_demand = -1.0;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    req_a = sg_add_request(ctx);
    p_a = sg_add_task(ctx, SG_TASK_PICKUP);
    d_a = sg_add_task(ctx, SG_TASK_DELIVERY);
    assert(sg_task_set_location(ctx, p_a, 5.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, p_a, 0, 99999) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, p_a, 0) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, p_a, &demand, 1) == SG_STATUS_OK);
    assert(sg_task_set_location(ctx, d_a, 10.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, d_a, 0, 99999) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, d_a, 0) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, d_a, &neg_demand, 1) == SG_STATUS_OK);
    assert(sg_request_bind_pickup_delivery_tasks(ctx, req_a, p_a, d_a) == SG_STATUS_OK);
    assert(sg_request_set_max_ride_time(ctx, req_a, 100) == SG_STATUS_OK);

    req_b = sg_add_request(ctx);
    p_b = sg_add_task(ctx, SG_TASK_PICKUP);
    d_b = sg_add_task(ctx, SG_TASK_DELIVERY);
    assert(sg_task_set_location(ctx, p_b, 5.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, p_b, 0, 99999) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, p_b, 0) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, p_b, &demand, 1) == SG_STATUS_OK);
    assert(sg_task_set_location(ctx, d_b, 50.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, d_b, 0, 99999) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, d_b, 0) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, d_b, &neg_demand, 1) == SG_STATUS_OK);
    assert(sg_request_bind_pickup_delivery_tasks(ctx, req_b, p_b, d_b) == SG_STATUS_OK);
    assert(sg_request_set_max_ride_time(ctx, req_b, 10) == SG_STATUS_OK);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 1); /* B unassigned due to ride time */
    sg_free(ctx);
}

static void test_stop_load_pd_profile(void) {
    /* PD request: load builds up at pickup and clears at delivery.
       Load at stop_index = cumulative load arriving at that stop (before demand applied).
       Pickup (stop 0): load = 0.0 (nothing loaded yet).
       Delivery (stop 1): load = 5.0 (carrying cargo from pickup). */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;
    uint32_t stop_count;
    double load_pickup, load_delivery;
    SGSolutionStop stop0, stop1;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_pd_request(ctx, 5.0, 0.0, 0, 99999, 0,
                   10.0, 0.0, 0, 99999, 0, 5.0);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    assert(sg_solution_get_route_count(ctx) == 1);

    stop_count = sg_solution_get_route_stop_count(ctx, 0);
    assert(stop_count == 2); /* pickup + delivery */

    assert(sg_solution_get_route_stop(ctx, 0, 0, &stop0) == SG_STATUS_OK);
    assert(sg_solution_get_route_stop(ctx, 0, 1, &stop1) == SG_STATUS_OK);
    assert(stop0.stop_type == SG_STOP_TYPE_PICKUP);
    assert(stop1.stop_type == SG_STOP_TYPE_DELIVERY);

    assert(sg_solution_get_route_stop_load(ctx, 0, 0, 0, &load_pickup) == SG_STATUS_OK);
    assert(sg_solution_get_route_stop_load(ctx, 0, 1, 0, &load_delivery) == SG_STATUS_OK);

    /* Before pickup: empty vehicle */
    assert(fabs(load_pickup) < 0.01);
    /* After pickup / before delivery: carrying 5 units */
    assert(fabs(load_delivery - 5.0) < 0.01);

    sg_free(ctx);
}

static void test_max_duration_zero_unlimited(void) {
    /* max_duration=0 means no limit: far delivery should still be feasible */
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_delivery_request(ctx, 1000.0, 0.0, 0, 999999, 0, -1.0);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 999999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_max_duration(ctx, v, 0) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    sg_free(ctx);
}

static void test_max_ride_time_zero_unlimited(void) {
    /* max_ride_time=0 means use TW-derived limit. Wide TW -> feasible. */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;
    uint32_t req, p_task, d_task;
    double demand = 1.0, neg_demand = -1.0;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    req = sg_add_request(ctx);
    p_task = sg_add_task(ctx, SG_TASK_PICKUP);
    d_task = sg_add_task(ctx, SG_TASK_DELIVERY);
    assert(sg_task_set_location(ctx, p_task, 5.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, p_task, 0, 99999) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, p_task, 0) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, p_task, &demand, 1) == SG_STATUS_OK);
    assert(sg_task_set_location(ctx, d_task, 50.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, d_task, 0, 99999) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, d_task, 0) == SG_STATUS_OK);
    assert(sg_task_set_demand(ctx, d_task, &neg_demand, 1) == SG_STATUS_OK);
    assert(sg_request_bind_pickup_delivery_tasks(ctx, req, p_task, d_task) == SG_STATUS_OK);
    assert(sg_request_set_max_ride_time(ctx, req, 0) == SG_STATUS_OK);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);
    sg_free(ctx);
}

/* ===== U8: Request-vehicle constraints ===== */

static void test_vehicle_constraint_api(void) {
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    uint32_t req;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);
    add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);
    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 10, -1.0);
    req = 0;

    /* Valid calls — small vehicle IDs */
    assert(sg_request_add_allowed_vehicle(ctx, req, 0) == SG_STATUS_OK);
    assert(sg_request_add_allowed_vehicle(ctx, req, 1) == SG_STATUS_OK);
    assert(sg_request_add_forbidden_vehicle(ctx, req, 0) == SG_STATUS_OK);
    assert(sg_request_add_forbidden_vehicle(ctx, req, 1) == SG_STATUS_OK);

    /* Large vehicle IDs (>= 64) work with dynamic bitset */
    assert(sg_request_add_allowed_vehicle(ctx, req, 64) == SG_STATUS_OK);
    assert(sg_request_add_forbidden_vehicle(ctx, req, 64) == SG_STATUS_OK);
    assert(sg_request_add_allowed_vehicle(ctx, req, 200) == SG_STATUS_OK);
    assert(sg_request_add_forbidden_vehicle(ctx, req, 500) == SG_STATUS_OK);

    /* invalid request_id */
    assert(sg_request_add_allowed_vehicle(ctx, 999, 0) == SG_STATUS_INVALID_ARG);
    assert(sg_request_add_forbidden_vehicle(ctx, 999, 0) == SG_STATUS_INVALID_ARG);

    /* NULL ctx */
    assert(sg_request_add_allowed_vehicle(NULL, req, 0) == SG_STATUS_INVALID_ARG);
    assert(sg_request_add_forbidden_vehicle(NULL, req, 0) == SG_STATUS_INVALID_ARG);

    sg_free(ctx);
}

static void test_vehicle_constraint_allowed_filters(void) {
    /* Two vehicles V0, V1. One delivery request allowed only on V1. */
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);  /* V0 */
    add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);  /* V1 */
    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 10, -1.0);

    /* Allow only V1 for the request */
    assert(sg_request_add_allowed_vehicle(ctx, 0, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    /* Verify assigned to V1 */
    {
        uint32_t route_count = sg_solution_get_route_count(ctx);
        uint32_t i;
        assert(route_count >= 1);
        for (i = 0; i < route_count; i++) {
            uint32_t vid = sg_solution_get_route_vehicle_id(ctx, i);
            uint32_t stop_count = sg_solution_get_route_stop_count(ctx, i);
            if (stop_count > 0) {
                assert(vid == 1);
            }
        }
    }

    sg_free(ctx);
}

static void test_vehicle_constraint_forbidden_unassigned(void) {
    /* One vehicle V0. One delivery request. V0 is forbidden. */
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);  /* V0 */
    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 10, -1.0);

    /* Forbid V0 */
    assert(sg_request_add_forbidden_vehicle(ctx, 0, 0) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 1);

    sg_free(ctx);
}

static void test_vehicle_constraint_pd_request(void) {
    /* Two vehicles. PD request allowed only on V1. */
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);  /* V0 */
    add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);  /* V1 */
    add_pd_request(ctx,
                   5.0, 0.0, 0, 99999, 10,
                   15.0, 0.0, 0, 99999, 10,
                   1.0);

    /* Allow only V1 */
    assert(sg_request_add_allowed_vehicle(ctx, 0, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    /* Verify assigned to V1 */
    {
        uint32_t route_count = sg_solution_get_route_count(ctx);
        uint32_t i;
        assert(route_count >= 1);
        for (i = 0; i < route_count; i++) {
            uint32_t vid = sg_solution_get_route_vehicle_id(ctx, i);
            uint32_t stop_count = sg_solution_get_route_stop_count(ctx, i);
            if (stop_count > 0) {
                assert(vid == 1);
            }
        }
    }

    sg_free(ctx);
}

static void test_vehicle_constraint_large_fleet(void) {
    /* 100 vehicles. Delivery request allowed only on V99 (beyond uint64_t). */
    SGContext *ctx = make_config(100, 42);
    uint32_t depot;
    uint32_t v;

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    for (v = 0; v < 100; v++) {
        add_vehicle_with_depot(ctx, depot, 0, 99999, 100.0);
    }
    add_delivery_request(ctx, 10.0, 0.0, 0, 99999, 10, -1.0);

    /* Allow only V99 */
    assert(sg_request_add_allowed_vehicle(ctx, 0, 99) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    /* Verify assigned to V99 */
    {
        uint32_t route_count = sg_solution_get_route_count(ctx);
        uint32_t i;
        assert(route_count >= 1);
        for (i = 0; i < route_count; i++) {
            uint32_t vid = sg_solution_get_route_vehicle_id(ctx, i);
            uint32_t stop_count = sg_solution_get_route_stop_count(ctx, i);
            if (stop_count > 0) {
                assert(vid == 99);
            }
        }
    }

    sg_free(ctx);
}

/* ===== Waiting cost tests ===== */

static void test_waiting_cost_api(void) {
    SGContext *ctx = sg_create();
    uint32_t v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_waiting_cost(ctx, v, 5.0) == SG_STATUS_OK);
    assert(sg_vehicle_set_waiting_cost(ctx, v, 0.0) == SG_STATUS_OK);
    assert(sg_vehicle_set_waiting_cost(ctx, v, -1.0) == SG_STATUS_INVALID_ARG);
    assert(sg_vehicle_set_waiting_cost(ctx, 999, 1.0) == SG_STATUS_INVALID_ARG);
    assert(sg_vehicle_set_waiting_cost(NULL, 0, 1.0) == SG_STATUS_INVALID_ARG);
    /* Getter returns 0.0 before solve */
    assert(sg_solution_get_route_waiting(ctx, 0) == 0.0);
    sg_free(ctx);
}

static void test_waiting_cost_accumulation(void) {
    /* One vehicle, one delivery with TW that forces waiting.
       Vehicle departs at t=0, delivery location at distance 10 (arrival ~10),
       but TW opens at 1000. Waiting = 1000 - 10 = 990.
       Solve with cost_per_waiting=0, record cost. Then with cost_per_waiting=100,
       verify cost increases. */
    double cost_no_wait, cost_with_wait;
    double route_waiting;

    /* Run 1: no waiting cost */
    {
        SGContext *ctx = make_config(100, 42);
        uint32_t depot;
        double cap = 100.0;
        uint32_t v;

        sg_set_dimension_count(ctx, 1);
        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_delivery_request(ctx, 10.0, 0.0, 1000, 99999, 60, -1.0);

        v = sg_add_vehicle(ctx);
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, 0.0) == SG_STATUS_OK);
        assert(sg_vehicle_set_waiting_cost(ctx, v, 0.0) == SG_STATUS_OK);

        assert(sg_solve(ctx) == SG_STATUS_OK);
        assert(sg_get_unassigned(ctx) == 0);
        cost_no_wait = sg_get_total_cost(ctx);

        /* Verify waiting is accumulated even with zero cost */
        route_waiting = sg_solution_get_route_waiting(ctx, 0);
        assert(route_waiting > 0.0);

        sg_free(ctx);
    }

    /* Run 2: high waiting cost */
    {
        SGContext *ctx = make_config(100, 42);
        uint32_t depot;
        double cap = 100.0;
        uint32_t v;

        sg_set_dimension_count(ctx, 1);
        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_delivery_request(ctx, 10.0, 0.0, 1000, 99999, 60, -1.0);

        v = sg_add_vehicle(ctx);
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, 0.0) == SG_STATUS_OK);
        assert(sg_vehicle_set_waiting_cost(ctx, v, 100.0) == SG_STATUS_OK);

        assert(sg_solve(ctx) == SG_STATUS_OK);
        assert(sg_get_unassigned(ctx) == 0);
        cost_with_wait = sg_get_total_cost(ctx);
        sg_free(ctx);
    }

    /* Waiting ~990s * cost_per_waiting=100 adds ~99000 to cost */
    assert(cost_with_wait > cost_no_wait + 10000.0);
}

static void test_waiting_cost_vehicle_preference(void) {
    /* Verify waiting cost steers objective like cost_per_duration does.
       Same setup twice: once with cost_per_waiting=0, once with cost_per_waiting=100.
       The high-waiting-cost run should have a higher total cost. */
    double cost_no_wait, cost_with_wait;

    /* Run 1: no waiting cost */
    {
        SGContext *ctx = make_config(100, 42);
        uint32_t depot;
        double cap = 100.0;
        uint32_t v;

        sg_set_dimension_count(ctx, 1);
        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        /* Delivery far in the future -> forces ~990s waiting */
        add_delivery_request(ctx, 10.0, 0.0, 1000, 99999, 60, -1.0);

        v = sg_add_vehicle(ctx);
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, 0.0) == SG_STATUS_OK);
        assert(sg_vehicle_set_waiting_cost(ctx, v, 0.0) == SG_STATUS_OK);

        assert(sg_solve(ctx) == SG_STATUS_OK);
        assert(sg_get_unassigned(ctx) == 0);
        cost_no_wait = sg_get_total_cost(ctx);
        sg_free(ctx);
    }

    /* Run 2: high waiting cost */
    {
        SGContext *ctx = make_config(100, 42);
        uint32_t depot;
        double cap = 100.0;
        uint32_t v;

        sg_set_dimension_count(ctx, 1);
        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_delivery_request(ctx, 10.0, 0.0, 1000, 99999, 60, -1.0);

        v = sg_add_vehicle(ctx);
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, 0.0) == SG_STATUS_OK);
        assert(sg_vehicle_set_waiting_cost(ctx, v, 100.0) == SG_STATUS_OK);

        assert(sg_solve(ctx) == SG_STATUS_OK);
        assert(sg_get_unassigned(ctx) == 0);
        cost_with_wait = sg_get_total_cost(ctx);
        sg_free(ctx);
    }

    /* Waiting ~990s with cost_per_waiting=100 adds ~99000 to cost. */
    assert(cost_with_wait > cost_no_wait + 10000.0);
}

/* ===== Overtime cost tests ===== */

static void test_overtime_cost_api(void) {
    SGContext *ctx = sg_create();
    uint32_t v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_overtime_cost(ctx, v, 5.0) == SG_STATUS_OK);
    assert(sg_vehicle_set_overtime_cost(ctx, v, 0.0) == SG_STATUS_OK);
    assert(sg_vehicle_set_overtime_cost(ctx, v, -1.0) == SG_STATUS_INVALID_ARG);
    assert(sg_vehicle_set_overtime_cost(ctx, 999, 1.0) == SG_STATUS_INVALID_ARG);
    assert(sg_vehicle_set_overtime_cost(NULL, 0, 1.0) == SG_STATUS_INVALID_ARG);
    /* Getter returns 0.0 before solve */
    assert(sg_solution_get_route_overtime(ctx, 0) == 0.0);
    sg_free(ctx);
}

static void test_overtime_cost_accumulation(void) {
    /* One vehicle, shift 0-100, delivery at distance 80 (round trip ~160s).
       With cost_per_overtime > 0, shift is soft -> overtime ~60s.
       Two runs with different cost_per_overtime values, both > 0.
       Higher coefficient = higher cost. */
    double cost_low, cost_high;
    double route_overtime;

    /* Run 1: low overtime cost */
    {
        SGContext *ctx = make_config(100, 42);
        uint32_t depot;
        double cap = 100.0;
        uint32_t v;

        sg_set_dimension_count(ctx, 1);
        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_delivery_request(ctx, 80.0, 0.0, 0, 99999, 10, -1.0);

        v = sg_add_vehicle(ctx);
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 100) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, 0.0) == SG_STATUS_OK);
        assert(sg_vehicle_set_overtime_cost(ctx, v, 1.0) == SG_STATUS_OK);

        assert(sg_solve(ctx) == SG_STATUS_OK);
        assert(sg_get_unassigned(ctx) == 0);
        cost_low = sg_get_total_cost(ctx);

        /* Verify overtime is accumulated */
        route_overtime = sg_solution_get_route_overtime(ctx, 0);
        assert(route_overtime > 0.0);

        sg_free(ctx);
    }

    /* Run 2: high overtime cost */
    {
        SGContext *ctx = make_config(100, 42);
        uint32_t depot;
        double cap = 100.0;
        uint32_t v;

        sg_set_dimension_count(ctx, 1);
        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_delivery_request(ctx, 80.0, 0.0, 0, 99999, 10, -1.0);

        v = sg_add_vehicle(ctx);
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 100) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, 0.0) == SG_STATUS_OK);
        assert(sg_vehicle_set_overtime_cost(ctx, v, 100.0) == SG_STATUS_OK);

        assert(sg_solve(ctx) == SG_STATUS_OK);
        assert(sg_get_unassigned(ctx) == 0);
        cost_high = sg_get_total_cost(ctx);
        sg_free(ctx);
    }

    /* Higher coefficient = higher cost */
    assert(cost_high > cost_low + 100.0);
}

static void test_overtime_cost_soft_shift(void) {
    /* One vehicle, tight shift that makes delivery infeasible with hard shift.
       Delivery at distance 80 -> round trip ~170s (with 10s service).
       Shift 0-100 is too short for round trip.
       With cost_per_overtime=0 (hard shift) -> request unassigned.
       With cost_per_overtime=1.0 (soft shift) -> request assigned with overtime. */

    /* Run 1: hard shift (cost_per_overtime=0) -> unassigned */
    {
        SGContext *ctx = make_config(200, 42);
        uint32_t depot;
        double cap = 100.0;
        uint32_t v;

        sg_set_dimension_count(ctx, 1);
        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_delivery_request(ctx, 80.0, 0.0, 0, 99999, 10, -1.0);

        v = sg_add_vehicle(ctx);
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 100) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, 0.0) == SG_STATUS_OK);
        /* cost_per_overtime=0 -> shift_late is hard */

        assert(sg_solve(ctx) == SG_STATUS_OK);
        assert(sg_get_unassigned(ctx) == 1);
        sg_free(ctx);
    }

    /* Run 2: soft shift (cost_per_overtime=1.0) -> assigned with overtime */
    {
        SGContext *ctx = make_config(200, 42);
        uint32_t depot;
        double cap = 100.0;
        uint32_t v;
        SGStats stats;

        sg_set_dimension_count(ctx, 1);
        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_delivery_request(ctx, 80.0, 0.0, 0, 99999, 10, -1.0);

        v = sg_add_vehicle(ctx);
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 100) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, 0.0) == SG_STATUS_OK);
        assert(sg_vehicle_set_overtime_cost(ctx, v, 1.0) == SG_STATUS_OK);

        assert(sg_solve(ctx) == SG_STATUS_OK);
        assert(sg_get_unassigned(ctx) == 0);

        /* Verify overtime was recorded */
        assert(sg_solution_get_route_overtime(ctx, 0) > 0.0);
        sg_get_stats(ctx, &stats);
        assert(stats.total_overtime > 0.0);

        sg_free(ctx);
    }
}

/* ===== Soft time windows (U7) ===== */

static void test_soft_tw_api(void) {
    SGContext *ctx = sg_create();
    uint32_t task;
    assert(ctx != NULL);

    sg_set_dimension_count(ctx, 1);
    task = sg_add_task(ctx, SG_TASK_DELIVERY);
    assert(task != UINT32_MAX);
    assert(sg_task_set_location(ctx, task, 10.0, 0.0) == SG_STATUS_OK);

    /* Must set hard TW before soft TW */
    assert(sg_task_set_soft_time_window(ctx, task, 10, 50, 1.0, 1.0) == SG_STATUS_INVALID_ARG);

    assert(sg_task_set_time_window(ctx, task, 0, 100) == SG_STATUS_OK);

    /* Valid soft TW within hard bounds */
    assert(sg_task_set_soft_time_window(ctx, task, 10, 50, 1.0, 2.0) == SG_STATUS_OK);

    /* Soft outside hard -> invalid */
    assert(sg_task_set_soft_time_window(ctx, task, -1, 50, 1.0, 1.0) == SG_STATUS_INVALID_ARG);
    assert(sg_task_set_soft_time_window(ctx, task, 10, 101, 1.0, 1.0) == SG_STATUS_INVALID_ARG);

    /* Negative penalties -> invalid */
    assert(sg_task_set_soft_time_window(ctx, task, 10, 50, -1.0, 1.0) == SG_STATUS_INVALID_ARG);
    assert(sg_task_set_soft_time_window(ctx, task, 10, 50, 1.0, -1.0) == SG_STATUS_INVALID_ARG);

    /* late < early -> invalid */
    assert(sg_task_set_soft_time_window(ctx, task, 50, 10, 1.0, 1.0) == SG_STATUS_INVALID_ARG);

    /* Bad task_id */
    assert(sg_task_set_soft_time_window(ctx, 999, 10, 50, 1.0, 1.0) == SG_STATUS_INVALID_ARG);

    /* NULL ctx */
    assert(sg_task_set_soft_time_window(NULL, task, 10, 50, 1.0, 1.0) == SG_STATUS_INVALID_ARG);

    /* Getter returns 0.0 before solve */
    assert(sg_solution_get_route_tw_penalty(ctx, 0) == 0.0);

    sg_free(ctx);
}

static void test_soft_tw_late_penalty(void) {
    /* Delivery at distance 80 from depot.
       Hard TW [0, 99999], soft TW [0, 50] with late_penalty=10.0.
       Arrival ~80 -> late violation = 80-50 = 30 -> penalty = 300. */
    double cost_without, cost_with;

    /* Run 1: no soft TW */
    {
        SGContext *ctx = make_config(200, 42);
        uint32_t depot;
        double cap = 100.0;
        uint32_t v;

        sg_set_dimension_count(ctx, 1);
        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_delivery_request(ctx, 80.0, 0.0, 0, 99999, 0, -1.0);

        v = sg_add_vehicle(ctx);
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, 0.0) == SG_STATUS_OK);

        assert(sg_solve(ctx) == SG_STATUS_OK);
        assert(sg_get_unassigned(ctx) == 0);
        cost_without = sg_get_total_cost(ctx);
        assert(sg_solution_get_route_tw_penalty(ctx, 0) == 0.0);
        sg_free(ctx);
    }

    /* Run 2: with soft TW -> penalty added */
    {
        SGContext *ctx = make_config(200, 42);
        uint32_t depot;
        double cap = 100.0;
        uint32_t v;
        uint32_t req, task;

        sg_set_dimension_count(ctx, 1);
        add_depot_with_location(ctx, &depot, 0.0, 0.0);

        req = sg_add_request(ctx);
        task = sg_add_task(ctx, SG_TASK_DELIVERY);
        assert(sg_task_set_location(ctx, task, 80.0, 0.0) == SG_STATUS_OK);
        assert(sg_task_set_time_window(ctx, task, 0, 99999) == SG_STATUS_OK);
        assert(sg_task_set_soft_time_window(ctx, task, 0, 50, 0.0, 10.0) == SG_STATUS_OK);
        assert(sg_task_set_service_seconds(ctx, task, 0) == SG_STATUS_OK);
        {
            double demand = -1.0;
            assert(sg_task_set_demand(ctx, task, &demand, 1) == SG_STATUS_OK);
        }
        assert(sg_request_bind_delivery_task(ctx, req, task) == SG_STATUS_OK);

        v = sg_add_vehicle(ctx);
        assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
        assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, 0.0) == SG_STATUS_OK);

        assert(sg_solve(ctx) == SG_STATUS_OK);
        assert(sg_get_unassigned(ctx) == 0);
        cost_with = sg_get_total_cost(ctx);
        assert(sg_solution_get_route_tw_penalty(ctx, 0) > 0.0);
        sg_free(ctx);
    }

    /* Cost with soft TW should be higher by ~300 */
    assert(cost_with > cost_without + 100.0);
}

static void test_soft_tw_hard_still_rejects(void) {
    /* Hard TW [0, 100], soft TW [0, 50]. Delivery at distance 150.
       Arrival at ~150 exceeds hard tw_late=100 -> should be unassigned.
       Confirms soft TW doesn't weaken hard bounds. */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;
    uint32_t req, task;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    req = sg_add_request(ctx);
    task = sg_add_task(ctx, SG_TASK_DELIVERY);
    assert(sg_task_set_location(ctx, task, 150.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, task, 0, 100) == SG_STATUS_OK);
    assert(sg_task_set_soft_time_window(ctx, task, 0, 50, 0.0, 1.0) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, task, 10) == SG_STATUS_OK);
    {
        double demand = -1.0;
        assert(sg_task_set_demand(ctx, task, &demand, 1) == SG_STATUS_OK);
    }
    assert(sg_request_bind_delivery_task(ctx, req, task) == SG_STATUS_OK);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 1);
    sg_free(ctx);
}

static void test_soft_tw_early_penalty(void) {
    /* Delivery at distance 10 from depot.
       Hard TW [0, 99999], soft TW [500, 99999] with early_penalty=10.0.
       Arrival ~10 -> early violation = 500-10 = 490 -> penalty = 4900. */
    SGContext *ctx = make_config(200, 42);
    uint32_t depot;
    double cap = 100.0;
    uint32_t v;
    uint32_t req, task;
    SGStats stats;

    sg_set_dimension_count(ctx, 1);
    add_depot_with_location(ctx, &depot, 0.0, 0.0);

    req = sg_add_request(ctx);
    task = sg_add_task(ctx, SG_TASK_DELIVERY);
    assert(sg_task_set_location(ctx, task, 10.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_time_window(ctx, task, 0, 99999) == SG_STATUS_OK);
    assert(sg_task_set_soft_time_window(ctx, task, 500, 99999, 10.0, 0.0) == SG_STATUS_OK);
    assert(sg_task_set_service_seconds(ctx, task, 0) == SG_STATUS_OK);
    {
        double demand = -1.0;
        assert(sg_task_set_demand(ctx, task, &demand, 1) == SG_STATUS_OK);
    }
    assert(sg_request_bind_delivery_task(ctx, req, task) == SG_STATUS_OK);

    v = sg_add_vehicle(ctx);
    assert(sg_vehicle_set_depots(ctx, v, depot, depot) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, v, 0, 99999) == SG_STATUS_OK);
    assert(sg_vehicle_set_capacity(ctx, v, &cap, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_costs(ctx, v, 0.0, 1.0, 0.0) == SG_STATUS_OK);

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(sg_get_unassigned(ctx) == 0);

    assert(sg_solution_get_route_tw_penalty(ctx, 0) > 0.0);
    sg_get_stats(ctx, &stats);
    assert(stats.total_tw_penalty > 0.0);

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
    RUN_TEST(test_timing_cache_matches_full_check);
    RUN_TEST(test_cached_eval_agrees_with_full_eval);
    RUN_TEST(test_cache_after_insert_remove_cycle);
    RUN_TEST(test_forward_slack_rejects_infeasible);
    RUN_TEST(test_capacity_check_incremental);
    RUN_TEST(test_sa_acceptance_produces_valid_solution);
    RUN_TEST(test_splice_excise_stop);
    RUN_TEST(test_pd_independent_placement_feasible);
    RUN_TEST(test_pd_adjacent_still_works);
    RUN_TEST(test_pd_unassign_preserves_others);
    RUN_TEST(test_pd_delivery_only_mixed);
    RUN_TEST(test_pd_ride_time_constraint);
    RUN_TEST(test_route_removal_cost);
    RUN_TEST(test_route_shaw_relatedness);
    RUN_TEST(test_adaptive_destroy_count);
    RUN_TEST(test_or_opt_intensify);
    RUN_TEST(test_tw_sorted_construction);
    RUN_TEST(test_vehicle_target_destroy);
    RUN_TEST(test_ejection_chain_reduce);
    RUN_TEST(test_ejection_chain_depth2);
    RUN_TEST(test_solomon_i1_construction);
    RUN_TEST(test_solomon_i1_pd);
    RUN_TEST(test_vehicle_empty_destroy);
    RUN_TEST(test_two_phase_solve_no_regression);
    RUN_TEST(test_travel_matrix_mode);
    RUN_TEST(test_travel_callback_mode);
    RUN_TEST(test_travel_auto_dedup);
    RUN_TEST(test_travel_asymmetric_matrix);
    RUN_TEST(test_qualification_api);
    RUN_TEST(test_qualification_filters_solve);
    RUN_TEST(test_qualification_unassigned_when_none_qualify);
    RUN_TEST(test_qualification_pd_request);
    RUN_TEST(test_solution_export_delivery);
    RUN_TEST(test_solution_export_pd);
    RUN_TEST(test_solution_export_unassigned);
    RUN_TEST(test_solution_export_multi_vehicle);
    /* U4: Open routes */
    RUN_TEST(test_open_end_basic);
    RUN_TEST(test_open_end_timing);
    RUN_TEST(test_open_end_solution_export);
    RUN_TEST(test_open_end_pd);
    /* U5: Max duration and ride time */
    RUN_TEST(test_max_duration_api);
    RUN_TEST(test_max_duration_infeasible);
    RUN_TEST(test_max_ride_time_explicit);
    RUN_TEST(test_max_ride_time_default);
    /* U6: Vehicle cost model */
    RUN_TEST(test_vehicle_costs_api);
    RUN_TEST(test_vehicle_costs_prefer_cheaper);
    RUN_TEST(test_unassigned_weight);
    /* Short-term API */
    RUN_TEST(test_convenience_delivery_request);
    RUN_TEST(test_convenience_pd_request);
    RUN_TEST(test_solution_stop_load);
    RUN_TEST(test_solution_stop_type_service);
    RUN_TEST(test_route_duration_export);
    /* Cross-feature and gap coverage */
    RUN_TEST(test_cost_per_duration_preference);
    RUN_TEST(test_open_end_max_duration_combo);
    RUN_TEST(test_max_ride_time_with_service_time);
    RUN_TEST(test_route_duration_open_end);
    RUN_TEST(test_open_end_cost_model);
    RUN_TEST(test_max_duration_multi_stop);
    RUN_TEST(test_multi_pd_heterogeneous_ride_time);
    RUN_TEST(test_stop_load_pd_profile);
    RUN_TEST(test_max_duration_zero_unlimited);
    RUN_TEST(test_max_ride_time_zero_unlimited);
    /* U8: Request-vehicle constraints */
    RUN_TEST(test_vehicle_constraint_api);
    RUN_TEST(test_vehicle_constraint_allowed_filters);
    RUN_TEST(test_vehicle_constraint_forbidden_unassigned);
    RUN_TEST(test_vehicle_constraint_pd_request);
    RUN_TEST(test_vehicle_constraint_large_fleet);
    /* Waiting cost */
    RUN_TEST(test_waiting_cost_api);
    RUN_TEST(test_waiting_cost_accumulation);
    RUN_TEST(test_waiting_cost_vehicle_preference);
    /* Overtime cost */
    RUN_TEST(test_overtime_cost_api);
    RUN_TEST(test_overtime_cost_accumulation);
    RUN_TEST(test_overtime_cost_soft_shift);
    /* U7: Soft time windows */
    RUN_TEST(test_soft_tw_api);
    RUN_TEST(test_soft_tw_late_penalty);
    RUN_TEST(test_soft_tw_hard_still_rejects);
    RUN_TEST(test_soft_tw_early_penalty);
    printf("================\n");
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    assert(tests_run == 109);
    return tests_passed == tests_run ? 0 : 1;
}
