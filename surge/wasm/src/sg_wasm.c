/**
 * Surge WASM Low-Level Bindings
 *
 * Exposes the Surge C API directly to JavaScript/WASM callers.
 * Provides context lifecycle, entity addition, configuration, solving,
 * and result accessors for fine-grained control.
 *
 * For a simpler JSON-in/JSON-out interface, see sg_wasm_api.c.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "surge.h"
#include "sg_types.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

/* ============================================================================
 * Memory Management
 * ============================================================================ */

WASM_EXPORT
void *wasm_malloc(int size) {
    return malloc((size_t)size);
}

WASM_EXPORT
void wasm_free(void *ptr) {
    free(ptr);
}

/* ============================================================================
 * Version
 * ============================================================================ */

WASM_EXPORT
const char *wasm_version(void) {
    return sg_version();
}

/* ============================================================================
 * Context Lifecycle
 * ============================================================================ */

WASM_EXPORT
SGContext *wasm_sg_create(void) {
    return sg_create();
}

WASM_EXPORT
void wasm_sg_free(SGContext *ctx) {
    sg_free(ctx);
}

/* ============================================================================
 * Configuration
 * ============================================================================ */

WASM_EXPORT
int wasm_sg_set_dimension_count(SGContext *ctx, uint32_t count) {
    return (int)sg_set_dimension_count(ctx, count);
}

WASM_EXPORT
int wasm_sg_set_config(SGContext *ctx, uint32_t max_iterations,
                       double max_time_seconds, int deterministic,
                       uint64_t seed) {
    SGConfig config;
    sg_config_default(&config);
    config.max_iterations = max_iterations;
    config.max_time_seconds = max_time_seconds;
    config.deterministic = deterministic;
    config.seed = seed;
    return (int)sg_set_config(ctx, &config);
}

/* ============================================================================
 * Depot Management
 * ============================================================================ */

WASM_EXPORT
uint32_t wasm_sg_add_depot(SGContext *ctx) {
    return sg_add_depot(ctx);
}

WASM_EXPORT
int wasm_sg_depot_set_location(SGContext *ctx, uint32_t depot_id,
                               double x, double y) {
    return (int)sg_depot_set_location(ctx, depot_id, x, y);
}

WASM_EXPORT
int wasm_sg_depot_set_time_window(SGContext *ctx, uint32_t depot_id,
                                  int32_t early, int32_t late) {
    return (int)sg_depot_set_time_window(ctx, depot_id, early, late);
}

/* ============================================================================
 * Vehicle Management
 * ============================================================================ */

WASM_EXPORT
uint32_t wasm_sg_add_vehicle(SGContext *ctx) {
    return sg_add_vehicle(ctx);
}

WASM_EXPORT
int wasm_sg_vehicle_set_depots(SGContext *ctx, uint32_t vehicle_id,
                               uint32_t start_depot, uint32_t end_depot) {
    return (int)sg_vehicle_set_depots(ctx, vehicle_id, start_depot, end_depot);
}

WASM_EXPORT
int wasm_sg_vehicle_set_shift_time_window(SGContext *ctx, uint32_t vehicle_id,
                                          int32_t early, int32_t late) {
    return (int)sg_vehicle_set_shift_time_window(ctx, vehicle_id, early, late);
}

WASM_EXPORT
int wasm_sg_vehicle_set_capacity(SGContext *ctx, uint32_t vehicle_id,
                                 const double *capacity, uint32_t count) {
    return (int)sg_vehicle_set_capacity(ctx, vehicle_id, capacity, count);
}

/* ============================================================================
 * Task Management
 * ============================================================================ */

WASM_EXPORT
uint32_t wasm_sg_add_task(SGContext *ctx, int type) {
    return sg_add_task(ctx, (SGTaskType)type);
}

WASM_EXPORT
int wasm_sg_task_set_location(SGContext *ctx, uint32_t task_id,
                              double x, double y) {
    return (int)sg_task_set_location(ctx, task_id, x, y);
}

WASM_EXPORT
int wasm_sg_task_set_time_window(SGContext *ctx, uint32_t task_id,
                                 int32_t early, int32_t late) {
    return (int)sg_task_set_time_window(ctx, task_id, early, late);
}

WASM_EXPORT
int wasm_sg_task_set_service_seconds(SGContext *ctx, uint32_t task_id,
                                     int32_t service_seconds) {
    return (int)sg_task_set_service_seconds(ctx, task_id, service_seconds);
}

WASM_EXPORT
int wasm_sg_task_set_demand(SGContext *ctx, uint32_t task_id,
                            const double *demand, uint32_t count) {
    return (int)sg_task_set_demand(ctx, task_id, demand, count);
}

/* ============================================================================
 * Request Management
 * ============================================================================ */

WASM_EXPORT
uint32_t wasm_sg_add_request(SGContext *ctx) {
    return sg_add_request(ctx);
}

WASM_EXPORT
int wasm_sg_request_bind_delivery_task(SGContext *ctx, uint32_t request_id,
                                       uint32_t delivery_task_id) {
    return (int)sg_request_bind_delivery_task(ctx, request_id, delivery_task_id);
}

WASM_EXPORT
int wasm_sg_request_bind_pickup_delivery_tasks(SGContext *ctx,
                                               uint32_t request_id,
                                               uint32_t pickup_task_id,
                                               uint32_t delivery_task_id) {
    return (int)sg_request_bind_pickup_delivery_tasks(ctx, request_id,
                                                      pickup_task_id,
                                                      delivery_task_id);
}

WASM_EXPORT
int wasm_sg_request_set_priority_hint(SGContext *ctx, uint32_t request_id,
                                      int32_t priority) {
    return (int)sg_request_set_priority_hint(ctx, request_id, priority);
}

/* ============================================================================
 * Solve
 * ============================================================================ */

WASM_EXPORT
int wasm_sg_solve(SGContext *ctx) {
    return (int)sg_solve(ctx);
}

/* ============================================================================
 * Result Accessors
 * ============================================================================ */

WASM_EXPORT
double wasm_sg_get_total_cost(const SGContext *ctx) {
    return sg_get_total_cost(ctx);
}

WASM_EXPORT
double wasm_sg_get_total_distance(const SGContext *ctx) {
    return sg_get_total_distance(ctx);
}

WASM_EXPORT
uint32_t wasm_sg_get_unassigned(const SGContext *ctx) {
    return sg_get_unassigned(ctx);
}

WASM_EXPORT
uint32_t wasm_sg_get_used_vehicle_count(const SGContext *ctx) {
    return sg_get_used_vehicle_count(ctx);
}

WASM_EXPORT
uint32_t wasm_sg_get_request_count(const SGContext *ctx) {
    return sg_get_request_count(ctx);
}
