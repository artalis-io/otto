/*
 * test_neighbor.c — unit tests for SGNeighborIndex (k-nearest pruning).
 *
 * These tests exercise the neighbor index in isolation, using manually
 * constructed SGContext travel models without running the full solver.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/sg_internal.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    tests_run++; \
    printf("  %-55s", #fn); \
    fn(); \
    tests_passed++; \
    printf("OK\n"); \
} while (0)

/* ────────── Helpers ────────── */

/* Create a minimal SGContext with n locations and a global distance matrix.
   Caller must free with free_test_ctx(). */
static SGContext *make_test_ctx(uint32_t n, const double *dist_matrix) {
    SGContext *ctx = sg_create();
    assert(ctx);
    ctx->num_locations = n;
    ctx->travel_distance_matrix = (double *)malloc((size_t)n * n * sizeof(double));
    ctx->travel_duration_matrix = (double *)malloc((size_t)n * n * sizeof(double));
    assert(ctx->travel_distance_matrix && ctx->travel_duration_matrix);
    memcpy(ctx->travel_distance_matrix, dist_matrix, (size_t)n * n * sizeof(double));
    memcpy(ctx->travel_duration_matrix, dist_matrix, (size_t)n * n * sizeof(double));
    return ctx;
}

static void free_test_ctx(SGContext *ctx) {
    /* sg_free would try to free arrays we set manually, which is fine since
       we malloc'd them.  But we need to be careful about fields sg_free expects. */
    free(ctx->travel_distance_matrix);
    ctx->travel_distance_matrix = NULL;
    free(ctx->travel_duration_matrix);
    ctx->travel_duration_matrix = NULL;
    sg_free(ctx);
}

/* Build a mock SGRouteSolution with minimal fields for vehicle_has_nearby.
   Caller must free fields manually. */
typedef struct {
    SGRouteSolution sol;
    SGRouteStop *stops_buf;
} MockSolution;

static void mock_sol_init(MockSolution *ms, uint32_t num_vehicles,
                          uint32_t stop_stride) {
    memset(ms, 0, sizeof(*ms));
    ms->sol.num_vehicles = num_vehicles;
    ms->sol.stop_stride = stop_stride;
    ms->sol.route_stop_lengths = (uint32_t *)calloc(num_vehicles, sizeof(uint32_t));
    ms->sol.route_lengths = (uint32_t *)calloc(num_vehicles, sizeof(uint32_t));
    ms->stops_buf = (SGRouteStop *)calloc((size_t)num_vehicles * stop_stride,
                                           sizeof(SGRouteStop));
    ms->sol.route_stops = ms->stops_buf;
}

static void mock_sol_free(MockSolution *ms) {
    free(ms->sol.route_stop_lengths);
    free(ms->sol.route_lengths);
    free(ms->stops_buf);
}

/* ────────── Tests: Basic neighbor index (global matrix) ────────── */

/*
 * 4 locations in a line: 0--1--2--3, distance = |i - j|
 *
 *   dist[i][j]:
 *     0  1  2  3
 *     1  0  1  2
 *     2  1  0  1
 *     3  2  1  0
 */
static const double LINE4[16] = {
    0, 1, 2, 3,
    1, 0, 1, 2,
    2, 1, 0, 1,
    3, 2, 1, 0,
};

static void test_basic_4_location(void) {
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);
    sg_neighbor_init(&idx, ctx, 2);

    assert(idx.neighbors != NULL);
    assert(idx.k == 2);
    assert(idx.num_locations == 4);
    assert(idx.num_profiles == 0);

    /* Loc 0 (dist 0,1,2,3): k=2 nearest are 1,2 */
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 1) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 2) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 3) == 0);

    /* Loc 2 (dist 2,1,0,1): k=2 nearest are 1,3 */
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 2, 1) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 2, 3) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 2, 0) == 0);

    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_k_clamped_to_n_minus_1(void) {
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);
    sg_neighbor_init(&idx, ctx, 100);  /* k=100 > 3 locations available */

    assert(idx.k == 3);  /* clamped to num_locations - 1 */

    /* Every location is a neighbor of every other */
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 3) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 3, 0) == 1);

    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_k_zero_disabled(void) {
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);
    sg_neighbor_init(&idx, ctx, 0);

    assert(idx.neighbors == NULL);  /* disabled */

    /* All lookups return 1 (assume near) */
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 3) == 1);

    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_single_location(void) {
    double d = 0.0;
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(1, &d);
    sg_neighbor_init(&idx, ctx, 5);

    assert(idx.neighbors == NULL);  /* num_locations < 2 */
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 0) == 1);

    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_is_near_positive(void) {
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);
    sg_neighbor_init(&idx, ctx, 2);

    /* Loc 1 nearest: 0 (d=1) and 2 (d=1) */
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 1, 0) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 1, 2) == 1);

    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_is_near_negative(void) {
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);
    sg_neighbor_init(&idx, ctx, 1);  /* only 1 nearest */

    /* Loc 0 nearest: only 1 (d=1).  2 and 3 are NOT near. */
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 1) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 2) == 0);
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 3) == 0);

    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_asymmetric_matrix(void) {
    /*
     * Asymmetric: A→B = 1 but B→A = 10
     *   0  1  5
     *  10  0  1
     *   5  1  0
     */
    double asym[9] = {
        0,  1, 5,
        10, 0, 1,
        5,  1, 0,
    };
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(3, asym);
    sg_neighbor_init(&idx, ctx, 1);

    /* Loc 0: nearest = 1 (d=1) */
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 1) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 2) == 0);

    /* Loc 1: nearest = 2 (d=1), NOT 0 (d=10) */
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 1, 2) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 1, 0) == 0);

    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_large_k_all_neighbors(void) {
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);
    sg_neighbor_init(&idx, ctx, 3);  /* k = n-1 = 3 → every other loc is a neighbor */

    uint32_t a, b;
    for (a = 0; a < 4; a++) {
        for (b = 0; b < 4; b++) {
            assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, a, b) == 1);
        }
    }

    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_self_is_always_near(void) {
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);
    sg_neighbor_init(&idx, ctx, 1);

    /* a == b → always near */
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 0) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 3, 3) == 1);

    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

/* ────────── Tests: Travel model variations ────────── */

static void dummy_callback(uint32_t from, uint32_t to, uint32_t vehicle,
                           double dep, double *dist, double *dur, void *data) {
    (void)from; (void)to; (void)vehicle; (void)dep; (void)data;
    *dist = 1.0;
    *dur = 1.0;
}

static void test_callback_mode_disabled(void) {
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);
    ctx->travel_callback = dummy_callback;
    sg_neighbor_init(&idx, ctx, 2);

    assert(idx.neighbors == NULL);  /* disabled for callback mode */
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 3) == 1);  /* assume near */

    ctx->travel_callback = NULL;  /* restore before free */
    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_per_vehicle_profiles(void) {
    /*
     * Global matrix: same as LINE4
     * Profile 1: reverse — far locations are close
     *   0  3  2  1
     *   3  0  3  2
     *   2  3  0  3
     *   1  2  3  0
     */
    double reverse_dist[16] = {
        0, 3, 2, 1,
        3, 0, 3, 2,
        2, 3, 0, 3,
        1, 2, 3, 0,
    };
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);

    /* Set up 1 travel profile */
    SGTravelProfile tp;
    memset(&tp, 0, sizeof(tp));
    tp.distance_matrix = reverse_dist;
    tp.has_distance_matrix = 1;
    tp.duration_matrix = reverse_dist;
    tp.has_duration_matrix = 1;
    ctx->travel_profiles = &tp;
    ctx->num_travel_profiles = 1;
    ctx->has_travel_profiles = 1;

    /* Set up 2 vehicles: vehicle 0 = global, vehicle 1 = profile 1 */
    SGVehicleRecord vehicles[2];
    memset(vehicles, 0, sizeof(vehicles));
    vehicles[0].travel_profile_id = 0;  /* global */
    vehicles[1].travel_profile_id = 1;  /* profile 1 */
    ctx->vehicles = vehicles;
    ctx->num_vehicles = 2;

    sg_neighbor_init(&idx, ctx, 1);

    assert(idx.num_profiles == 1);

    /* Vehicle 0 (global): Loc 0 nearest = 1 (d=1) */
    assert(sg_neighbor_is_near(&idx, ctx, 0, 0, 1) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, 0, 0, 3) == 0);

    /* Vehicle 1 (profile 1): Loc 0 nearest = 3 (d=1 in reverse_dist) */
    assert(sg_neighbor_is_near(&idx, ctx, 1, 0, 3) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, 1, 0, 1) == 0);

    /* Clean up — don't let sg_free try to free our stack arrays */
    ctx->travel_profiles = NULL;
    ctx->num_travel_profiles = 0;
    ctx->has_travel_profiles = 0;
    ctx->vehicles = NULL;
    ctx->num_vehicles = 0;
    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_time_brackets_uses_bracket0(void) {
    /*
     * Global matrix: LINE4
     * Global time bracket[0] distance matrix: reverse distances
     * Neighbor index should use bracket[0], not the base matrix.
     */
    double reverse_dist[16] = {
        0, 3, 2, 1,
        3, 0, 3, 2,
        2, 3, 0, 3,
        1, 2, 3, 0,
    };
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);

    SGTravelTimeBracket br;
    memset(&br, 0, sizeof(br));
    br.start_time = 0.0;
    br.distance_matrix = reverse_dist;
    br.duration_matrix = ctx->travel_duration_matrix;
    ctx->travel_time_brackets = &br;
    ctx->num_travel_time_brackets = 1;
    ctx->has_travel_time_brackets = 1;

    sg_neighbor_init(&idx, ctx, 1);

    /* Loc 0 nearest = 3 (d=1 in reverse bracket[0]) */
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 3) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 1) == 0);

    ctx->travel_time_brackets = NULL;
    ctx->num_travel_time_brackets = 0;
    ctx->has_travel_time_brackets = 0;
    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_mixed_profile_and_global(void) {
    /*
     * Vehicle 0: no profile (global)
     * Vehicle 1: profile 1
     * Vehicle 2: no profile (global)
     */
    double reverse_dist[16] = {
        0, 3, 2, 1,
        3, 0, 3, 2,
        2, 3, 0, 3,
        1, 2, 3, 0,
    };
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);

    SGTravelProfile tp;
    memset(&tp, 0, sizeof(tp));
    tp.distance_matrix = reverse_dist;
    tp.has_distance_matrix = 1;
    ctx->travel_profiles = &tp;
    ctx->num_travel_profiles = 1;
    ctx->has_travel_profiles = 1;

    SGVehicleRecord vehicles[3];
    memset(vehicles, 0, sizeof(vehicles));
    vehicles[0].travel_profile_id = 0;
    vehicles[1].travel_profile_id = 1;
    vehicles[2].travel_profile_id = 0;
    ctx->vehicles = vehicles;
    ctx->num_vehicles = 3;

    sg_neighbor_init(&idx, ctx, 1);

    /* Vehicle 0 (global): Loc 0 nearest = 1 */
    assert(sg_neighbor_is_near(&idx, ctx, 0, 0, 1) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, 0, 0, 3) == 0);

    /* Vehicle 1 (profile): Loc 0 nearest = 3 */
    assert(sg_neighbor_is_near(&idx, ctx, 1, 0, 3) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, 1, 0, 1) == 0);

    /* Vehicle 2 (global): same as vehicle 0 */
    assert(sg_neighbor_is_near(&idx, ctx, 2, 0, 1) == 1);
    assert(sg_neighbor_is_near(&idx, ctx, 2, 0, 3) == 0);

    ctx->travel_profiles = NULL;
    ctx->num_travel_profiles = 0;
    ctx->has_travel_profiles = 0;
    ctx->vehicles = NULL;
    ctx->num_vehicles = 0;
    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

/* ────────── Tests: Vehicle pruning helper ────────── */

static void test_vehicle_with_nearby_stop(void) {
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);

    /* Set up 1 request (delivery-only) at location 0 */
    SGTaskRecord tasks[2];
    memset(tasks, 0, sizeof(tasks));
    tasks[0].location_id = 0; tasks[0].has_location = 1; /* delivery task */
    tasks[1].location_id = 1; tasks[1].has_location = 1; /* vehicle stop task */
    ctx->tasks = tasks;
    ctx->num_tasks = 2;

    SGRequestRecord reqs[1];
    memset(reqs, 0, sizeof(reqs));
    reqs[0].kind = SG_REQUEST_KIND_DELIVERY_ONLY;
    reqs[0].delivery_task_id = 0;
    reqs[0].has_delivery_task = 1;
    ctx->requests = reqs;
    ctx->num_requests = 1;

    SGVehicleRecord vehicles[1];
    memset(vehicles, 0, sizeof(vehicles));
    vehicles[0].start_location_id = 2;
    vehicles[0].end_location_id = 2;
    ctx->vehicles = vehicles;
    ctx->num_vehicles = 1;

    sg_neighbor_init(&idx, ctx, 2);

    /* Mock solution: vehicle 0 has 1 stop at task_id=1 (loc=1) */
    MockSolution ms;
    mock_sol_init(&ms, 1, 4);
    ms.sol.route_stop_lengths[0] = 1;
    ms.sol.route_lengths[0] = 1;
    ms.stops_buf[0].task_id = 1;  /* loc 1 */

    /* Loc 0's k=2 neighbors include loc 1 → vehicle has nearby */
    assert(sg_neighbor_vehicle_has_nearby(&idx, ctx, &ms.sol, 0, 0) == 1);

    mock_sol_free(&ms);
    ctx->tasks = NULL; ctx->num_tasks = 0;
    ctx->requests = NULL; ctx->num_requests = 0;
    ctx->vehicles = NULL; ctx->num_vehicles = 0;
    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_vehicle_with_no_nearby_stops(void) {
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);

    SGTaskRecord tasks[2];
    memset(tasks, 0, sizeof(tasks));
    tasks[0].location_id = 0; tasks[0].has_location = 1; /* delivery task */
    tasks[1].location_id = 3; tasks[1].has_location = 1; /* vehicle stop task at far end */
    ctx->tasks = tasks;
    ctx->num_tasks = 2;

    SGRequestRecord reqs[1];
    memset(reqs, 0, sizeof(reqs));
    reqs[0].kind = SG_REQUEST_KIND_DELIVERY_ONLY;
    reqs[0].delivery_task_id = 0;
    reqs[0].has_delivery_task = 1;
    ctx->requests = reqs;
    ctx->num_requests = 1;

    SGVehicleRecord vehicles[1];
    memset(vehicles, 0, sizeof(vehicles));
    vehicles[0].start_location_id = 3;
    vehicles[0].end_location_id = 3;
    ctx->vehicles = vehicles;
    ctx->num_vehicles = 1;

    sg_neighbor_init(&idx, ctx, 1);  /* k=1: loc 0's only neighbor is loc 1 */

    MockSolution ms;
    mock_sol_init(&ms, 1, 4);
    ms.sol.route_stop_lengths[0] = 1;
    ms.sol.route_lengths[0] = 1;
    ms.stops_buf[0].task_id = 1;  /* loc 3 — not near loc 0 with k=1 */

    /* Vehicle's stop loc=3 and depot loc=3 are not in loc 0's k=1 neighbors {1} */
    assert(sg_neighbor_vehicle_has_nearby(&idx, ctx, &ms.sol, 0, 0) == 0);

    mock_sol_free(&ms);
    ctx->tasks = NULL; ctx->num_tasks = 0;
    ctx->requests = NULL; ctx->num_requests = 0;
    ctx->vehicles = NULL; ctx->num_vehicles = 0;
    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_empty_vehicle_never_pruned(void) {
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);

    SGTaskRecord tasks[1];
    memset(tasks, 0, sizeof(tasks));
    tasks[0].location_id = 0; tasks[0].has_location = 1;
    ctx->tasks = tasks;
    ctx->num_tasks = 1;

    SGRequestRecord reqs[1];
    memset(reqs, 0, sizeof(reqs));
    reqs[0].kind = SG_REQUEST_KIND_DELIVERY_ONLY;
    reqs[0].delivery_task_id = 0;
    reqs[0].has_delivery_task = 1;
    ctx->requests = reqs;
    ctx->num_requests = 1;

    SGVehicleRecord vehicles[1];
    memset(vehicles, 0, sizeof(vehicles));
    vehicles[0].start_location_id = 3;
    vehicles[0].end_location_id = 3;
    ctx->vehicles = vehicles;
    ctx->num_vehicles = 1;

    sg_neighbor_init(&idx, ctx, 1);

    /* Empty vehicle (route_stop_lengths[0] = 0) → always returns 1 */
    MockSolution ms;
    mock_sol_init(&ms, 1, 4);
    ms.sol.route_stop_lengths[0] = 0;
    ms.sol.route_lengths[0] = 0;

    assert(sg_neighbor_vehicle_has_nearby(&idx, ctx, &ms.sol, 0, 0) == 1);

    mock_sol_free(&ms);
    ctx->tasks = NULL; ctx->num_tasks = 0;
    ctx->requests = NULL; ctx->num_requests = 0;
    ctx->vehicles = NULL; ctx->num_vehicles = 0;
    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_pd_request_checks_both_locations(void) {
    /*
     * PD request: pickup at loc 0, delivery at loc 3
     * Vehicle stop at loc 3 (far from pickup but near delivery)
     * With k=1, loc 0's neighbor is {1}, loc 3's neighbor is {2}
     * Stop at loc 3 is NOT in loc 0's neighbors but loc 3 IS in loc 3's neighbors (self)
     */
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);

    SGTaskRecord tasks[3];
    memset(tasks, 0, sizeof(tasks));
    tasks[0].location_id = 0; tasks[0].has_location = 1; /* pickup */
    tasks[1].location_id = 3; tasks[1].has_location = 1; /* delivery */
    tasks[2].location_id = 2; tasks[2].has_location = 1; /* vehicle stop */
    ctx->tasks = tasks;
    ctx->num_tasks = 3;

    SGRequestRecord reqs[1];
    memset(reqs, 0, sizeof(reqs));
    reqs[0].kind = SG_REQUEST_KIND_PICKUP_DELIVERY;
    reqs[0].pickup_task_id = 0;
    reqs[0].delivery_task_id = 1;
    reqs[0].has_pickup_task = 1;
    reqs[0].has_delivery_task = 1;
    ctx->requests = reqs;
    ctx->num_requests = 1;

    SGVehicleRecord vehicles[1];
    memset(vehicles, 0, sizeof(vehicles));
    vehicles[0].start_location_id = 2;
    vehicles[0].end_location_id = 2;
    ctx->vehicles = vehicles;
    ctx->num_vehicles = 1;

    sg_neighbor_init(&idx, ctx, 1);  /* k=1 */

    /* Vehicle stop at loc 2.  Loc 3 (delivery) k=1 neighbors = {2}.
       Stop loc=2 IS in delivery's neighbors → return 1 */
    MockSolution ms;
    mock_sol_init(&ms, 1, 4);
    ms.sol.route_stop_lengths[0] = 1;
    ms.sol.route_lengths[0] = 1;
    ms.stops_buf[0].task_id = 2;  /* loc 2 */

    assert(sg_neighbor_vehicle_has_nearby(&idx, ctx, &ms.sol, 0, 0) == 1);

    mock_sol_free(&ms);
    ctx->tasks = NULL; ctx->num_tasks = 0;
    ctx->requests = NULL; ctx->num_requests = 0;
    ctx->vehicles = NULL; ctx->num_vehicles = 0;
    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_disabled_index_always_returns_1(void) {
    SGNeighborIndex idx;
    memset(&idx, 0, sizeof(idx));  /* neighbors = NULL → disabled */

    SGContext *ctx = make_test_ctx(4, LINE4);

    assert(sg_neighbor_is_near(&idx, ctx, SG_NO_VEHICLE, 0, 3) == 1);

    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

static void test_depot_location_checked(void) {
    /*
     * Request at loc 0.  Vehicle has no stops but depot at loc 1.
     * k=1: loc 0's neighbor = {1}.  Depot loc=1 is in neighbors → return 1.
     *
     * However, route_stop_lengths=0 means we return 1 (empty vehicle).
     * So test with a stop at loc 3 (far away) and depot at loc 1.
     */
    SGNeighborIndex idx;
    SGContext *ctx = make_test_ctx(4, LINE4);

    SGTaskRecord tasks[2];
    memset(tasks, 0, sizeof(tasks));
    tasks[0].location_id = 0; tasks[0].has_location = 1; /* delivery */
    tasks[1].location_id = 3; tasks[1].has_location = 1; /* vehicle stop (far) */
    ctx->tasks = tasks;
    ctx->num_tasks = 2;

    SGRequestRecord reqs[1];
    memset(reqs, 0, sizeof(reqs));
    reqs[0].kind = SG_REQUEST_KIND_DELIVERY_ONLY;
    reqs[0].delivery_task_id = 0;
    reqs[0].has_delivery_task = 1;
    ctx->requests = reqs;
    ctx->num_requests = 1;

    SGVehicleRecord vehicles[1];
    memset(vehicles, 0, sizeof(vehicles));
    vehicles[0].start_location_id = 1;  /* depot near request */
    vehicles[0].end_location_id = 1;
    ctx->vehicles = vehicles;
    ctx->num_vehicles = 1;

    sg_neighbor_init(&idx, ctx, 1);  /* k=1: loc 0 neighbor = {1} */

    MockSolution ms;
    mock_sol_init(&ms, 1, 4);
    ms.sol.route_stop_lengths[0] = 1;
    ms.sol.route_lengths[0] = 1;
    ms.stops_buf[0].task_id = 1;  /* loc 3 — NOT near */

    /* Stop at loc 3 is not near loc 0.  But depot at loc 1 IS near → return 1 */
    assert(sg_neighbor_vehicle_has_nearby(&idx, ctx, &ms.sol, 0, 0) == 1);

    mock_sol_free(&ms);
    ctx->tasks = NULL; ctx->num_tasks = 0;
    ctx->requests = NULL; ctx->num_requests = 0;
    ctx->vehicles = NULL; ctx->num_vehicles = 0;
    sg_neighbor_free(&idx);
    free_test_ctx(ctx);
}

/* ────────── Main ────────── */

int main(void) {
    printf("sg_neighbor tests:\n");

    /* Basic neighbor index */
    RUN_TEST(test_basic_4_location);
    RUN_TEST(test_k_clamped_to_n_minus_1);
    RUN_TEST(test_k_zero_disabled);
    RUN_TEST(test_single_location);
    RUN_TEST(test_is_near_positive);
    RUN_TEST(test_is_near_negative);
    RUN_TEST(test_asymmetric_matrix);
    RUN_TEST(test_large_k_all_neighbors);
    RUN_TEST(test_self_is_always_near);

    /* Travel model variations */
    RUN_TEST(test_callback_mode_disabled);
    RUN_TEST(test_per_vehicle_profiles);
    RUN_TEST(test_time_brackets_uses_bracket0);
    RUN_TEST(test_mixed_profile_and_global);

    /* Vehicle pruning helper */
    RUN_TEST(test_vehicle_with_nearby_stop);
    RUN_TEST(test_vehicle_with_no_nearby_stops);
    RUN_TEST(test_empty_vehicle_never_pruned);
    RUN_TEST(test_pd_request_checks_both_locations);
    RUN_TEST(test_disabled_index_always_returns_1);
    RUN_TEST(test_depot_location_checked);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_run == tests_passed ? 0 : 1;
}
