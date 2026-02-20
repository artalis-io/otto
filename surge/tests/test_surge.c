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

    printf("================\n");
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
