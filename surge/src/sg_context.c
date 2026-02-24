#include "sg_internal.h"

#include <stdarg.h>
#include <stdio.h>

void sg_set_error(SGContext *ctx, const char *fmt, ...) {
    va_list ap;
    if (!ctx || !fmt) return;
    va_start(ap, fmt);
    vsnprintf(ctx->last_error, sizeof(ctx->last_error), fmt, ap);
    va_end(ap);
}

void sg_clear_error(SGContext *ctx) {
    if (ctx) ctx->last_error[0] = '\0';
}

const char *sg_get_last_error(const SGContext *ctx) {
    if (!ctx) return "";
    return ctx->last_error;
}

SGRequestHint sg_request_hint_default(void) {
    SGRequestHint hint;

    memset(&hint, 0, sizeof(hint));
    hint.priority = 50;
    return hint;
}

SGRequestRecord sg_request_record_default(void) {
    SGRequestRecord record;

    memset(&record, 0, sizeof(record));
    record.kind = SG_REQUEST_KIND_UNBOUND;
    return record;
}

int sg_task_type_valid(SGTaskType type) {
    return (type == SG_TASK_PICKUP || type == SG_TASK_DELIVERY || type == SG_TASK_SERVICE);
}

void sg_vehicle_records_free(SGVehicleRecord *vehicles, uint32_t count) {
    uint32_t i;

    if (!vehicles) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(vehicles[i].capacity);
        vehicles[i].capacity = NULL;
        free(vehicles[i].initial_load);
        vehicles[i].initial_load = NULL;
    }
    free(vehicles);
}

void sg_request_records_free(SGRequestRecord *requests, uint32_t count) {
    uint32_t i;

    if (!requests) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(requests[i].allowed_vehicles);
        requests[i].allowed_vehicles = NULL;
        free(requests[i].forbidden_vehicles);
        requests[i].forbidden_vehicles = NULL;
        free(requests[i].exclusion_group_ids);
        requests[i].exclusion_group_ids = NULL;
    }
    free(requests);
}

void sg_task_records_free(SGTaskRecord *tasks, uint32_t count) {
    uint32_t i;

    if (!tasks) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(tasks[i].demand);
        tasks[i].demand = NULL;
        free(tasks[i].time_windows);
        tasks[i].time_windows = NULL;
    }
    free(tasks);
}

int sg_priority_policy_valid(SGPriorityRemovalPolicy policy) {
    return (policy == SG_PRIORITY_REMOVE_LOWER_FIRST ||
            policy == SG_PRIORITY_REMOVE_HIGHER_FIRST);
}

int sg_demand_sign_convention_valid(SGDemandSignConvention convention) {
    return (convention == SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE ||
            convention == SG_DEMAND_PICKUP_NEGATIVE_DELIVERY_POSITIVE);
}

int sg_config_valid(const SGConfig *config) {
    if (!config) {
        return 0;
    }
    if (config->max_iterations <= 0 || config->max_time_seconds < 0) {
        return 0;
    }
    if (config->segment_size <= 0 || config->q_min < 0 || config->q_max < config->q_min) {
        return 0;
    }
    if (config->accept_type != SG_ACCEPT_SA &&
        config->accept_type != SG_ACCEPT_RRT &&
        config->accept_type != SG_ACCEPT_IMPROVING) {
        return 0;
    }
    return sg_priority_policy_valid(config->priority_removal_policy);
}

void sg_zone_matrix_clear(SGContext *ctx) {
    if (!ctx) {
        return;
    }

    free(ctx->zone_distance_matrix);
    ctx->zone_distance_matrix = NULL;
    ctx->zone_count = 0;
}

int sg_task_ready_for_model(const SGTaskRecord *task) {
    return task && task->has_location && task->has_time_window && task->service_seconds >= 0;
}

int sg_request_pd_demands_valid(const SGTaskRecord *pickup, const SGTaskRecord *delivery,
                                uint32_t dimension_count,
                                SGDemandSignConvention convention) {
    uint32_t dim;
    int has_non_zero_flow = 0;

    if (!pickup || !delivery || !pickup->demand || !delivery->demand || !pickup->has_demand ||
        !delivery->has_demand) {
        return 0;
    }

    for (dim = 0; dim < dimension_count; dim++) {
        double pick = pickup->demand[dim];
        double drop = delivery->demand[dim];

        if (!isfinite(pick) || !isfinite(drop)) {
            return 0;
        }
        if (convention == SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE) {
            if (pick < -SG_DEMAND_TOLERANCE || drop > SG_DEMAND_TOLERANCE) {
                return 0;
            }
        } else {
            if (pick > SG_DEMAND_TOLERANCE || drop < -SG_DEMAND_TOLERANCE) {
                return 0;
            }
        }
        if (fabs(pick + drop) > SG_DEMAND_TOLERANCE) {
            return 0;
        }
        if (fabs(pick) > SG_DEMAND_TOLERANCE || fabs(drop) > SG_DEMAND_TOLERANCE) {
            has_non_zero_flow = 1;
        }
    }

    return has_non_zero_flow;
}

int sg_delivery_task_demand_valid(const SGTaskRecord *delivery, uint32_t dimension_count,
                                  SGDemandSignConvention convention) {
    uint32_t dim;
    int has_non_zero = 0;

    if (!delivery || !delivery->demand || !delivery->has_demand) {
        return 0;
    }

    for (dim = 0; dim < dimension_count; dim++) {
        double value = delivery->demand[dim];
        if (!isfinite(value)) {
            return 0;
        }
        if (convention == SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE) {
            if (value > SG_DEMAND_TOLERANCE) {
                return 0;
            }
        } else {
            if (value < -SG_DEMAND_TOLERANCE) {
                return 0;
            }
        }
        if (fabs(value) > SG_DEMAND_TOLERANCE) {
            has_non_zero = 1;
        }
    }

    return has_non_zero;
}

uint32_t sg_count_unbound_requests(const SGContext *ctx) {
    uint32_t i;
    uint32_t count = 0;

    if (!ctx || ctx->num_requests == 0) {
        return 0;
    }
    if (!ctx->requests) {
        return ctx->num_requests;
    }

    for (i = 0; i < ctx->num_requests; i++) {
        if (ctx->requests[i].kind == SG_REQUEST_KIND_UNBOUND) {
            count++;
        }
    }
    return count;
}

const char *sg_version(void) {
    return "0.1.0-dev";
}

void sg_config_default(SGConfig *config) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->max_iterations = 1000;
    config->max_time_seconds = 0;
    config->segment_size = 100;
    config->q_min = 4;
    config->q_max = 20;
    config->seed = 0xDEADBEEF;
    config->deterministic = true;
    config->require_bound_requests_at_solve = true;
    config->priority_removal_policy = SG_PRIORITY_REMOVE_LOWER_FIRST;
    config->lexicographic_objective = false;
    config->accept_type = SG_ACCEPT_SA;
    config->adaptive_q = false;
}

SGContext *sg_create(void) {
    SGContext *ctx = (SGContext *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->op_rng = sh_rng_create_default();
    if (!ctx->op_rng) {
        free(ctx);
        return NULL;
    }

    sg_config_default(&ctx->config);
    ctx->demand_sign_convention = SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE;
    ctx->dimension_count = 1;
    ctx->unassigned_weight = SG_ROUTE_OBJECTIVE_UNASSIGNED_WEIGHT;
    return ctx;
}

void sg_free(SGContext *ctx) {
    uint32_t i;

    if (!ctx) {
        return;
    }

    free(ctx->depots);
    ctx->depots = NULL;
    ctx->num_depots = 0;

    sg_request_records_free(ctx->requests, ctx->num_requests);
    ctx->requests = NULL;

    free(ctx->request_hints);
    ctx->request_hints = NULL;
    ctx->num_requests = 0;

    sg_vehicle_records_free(ctx->vehicles, ctx->num_vehicles);
    ctx->vehicles = NULL;
    ctx->num_vehicles = 0;

    sg_task_records_free(ctx->tasks, ctx->num_tasks);
    ctx->tasks = NULL;
    ctx->num_tasks = 0;

    sg_zone_matrix_clear(ctx);

    free(ctx->commodity_conflicts);
    ctx->commodity_conflicts = NULL;
    ctx->num_commodities = 0;
    ctx->num_exclusion_groups = 0;

    free(ctx->setup_time_matrix);
    ctx->setup_time_matrix = NULL;
    ctx->num_setup_classes = 0;

    free(ctx->destroy_op_stats);
    ctx->destroy_op_stats = NULL;
    ctx->num_destroy_ops = 0;
    free(ctx->repair_op_stats);
    ctx->repair_op_stats = NULL;
    ctx->num_repair_ops = 0;

    free(ctx->location_coords);
    ctx->location_coords = NULL;
    free(ctx->travel_distance_matrix);
    ctx->travel_distance_matrix = NULL;
    free(ctx->travel_duration_matrix);
    ctx->travel_duration_matrix = NULL;
    ctx->num_locations = 0;
    ctx->travel_callback = NULL;
    ctx->travel_callback_data = NULL;
    atomic_store(&ctx->travel_prepared, 0);

    if (ctx->final_solution) {
        sg_route_solution_free(ctx->final_solution, NULL);
        ctx->final_solution = NULL;
    }

    free(ctx->violations);
    ctx->violations = NULL;
    ctx->num_violations = 0;
    ctx->violations_capacity = 0;

    free(ctx->initial_route_vehicle_ids);
    ctx->initial_route_vehicle_ids = NULL;
    free(ctx->initial_route_request_ids);
    ctx->initial_route_request_ids = NULL;
    free(ctx->initial_route_lengths);
    ctx->initial_route_lengths = NULL;
    ctx->num_initial_routes = 0;
    ctx->total_initial_requests = 0;

    /* Free speed profiles */
    if (ctx->speed_profiles) {
        for (i = 0; i < ctx->num_speed_profiles; i++) {
            sh_step_free(ctx->speed_profiles[i]);
        }
        free(ctx->speed_profiles);
        ctx->speed_profiles = NULL;
    }
    ctx->num_speed_profiles = 0;
    ctx->global_speed_profile_id = 0;
    ctx->has_speed_profiles = 0;

    /* Free travel profiles */
    if (ctx->travel_profiles) {
        for (i = 0; i < ctx->num_travel_profiles; i++) {
            free(ctx->travel_profiles[i].distance_matrix);
            free(ctx->travel_profiles[i].duration_matrix);
            if (ctx->travel_profiles[i].time_brackets) {
                uint32_t j;
                for (j = 0; j < ctx->travel_profiles[i].num_time_brackets; j++) {
                    free(ctx->travel_profiles[i].time_brackets[j].distance_matrix);
                    free(ctx->travel_profiles[i].time_brackets[j].duration_matrix);
                }
                free(ctx->travel_profiles[i].time_brackets);
            }
        }
        free(ctx->travel_profiles);
        ctx->travel_profiles = NULL;
    }
    ctx->num_travel_profiles = 0;
    ctx->has_travel_profiles = 0;

    /* Free global time brackets */
    if (ctx->travel_time_brackets) {
        for (i = 0; i < ctx->num_travel_time_brackets; i++) {
            free(ctx->travel_time_brackets[i].distance_matrix);
            free(ctx->travel_time_brackets[i].duration_matrix);
        }
        free(ctx->travel_time_brackets);
        ctx->travel_time_brackets = NULL;
    }
    ctx->num_travel_time_brackets = 0;
    ctx->has_travel_time_brackets = 0;

    free(ctx->request_locks);
    ctx->request_locks = NULL;
    ctx->has_committed = 0;
    ctx->has_frozen = 0;
    free(ctx->frozen_vehicle_map);
    ctx->frozen_vehicle_map = NULL;

    sh_rng_free(ctx->op_rng);
    ctx->op_rng = NULL;
    free(ctx);
}

SGStatus sg_set_config(SGContext *ctx, const SGConfig *config) {
    if (!ctx || !sg_config_valid(config)) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->config = *config;
    return SG_STATUS_OK;
}

SGStatus sg_set_require_bound_requests_at_solve(SGContext *ctx, bool require_bound) {
    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->config.require_bound_requests_at_solve = require_bound;
    return SG_STATUS_OK;
}

bool sg_get_require_bound_requests_at_solve(const SGContext *ctx) {
    if (!ctx) {
        return true;
    }
    return ctx->config.require_bound_requests_at_solve;
}

SGStatus sg_set_demand_sign_convention(SGContext *ctx, SGDemandSignConvention convention) {
    if (!ctx || !sg_demand_sign_convention_valid(convention)) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->demand_sign_convention = convention;
    return SG_STATUS_OK;
}

SGDemandSignConvention sg_get_demand_sign_convention(const SGContext *ctx) {
    if (!ctx) {
        return SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE;
    }
    return ctx->demand_sign_convention;
}

SGStatus sg_set_dimension_count(SGContext *ctx, uint32_t dimension_count) {
    if (!ctx || dimension_count == 0) {
        return SG_STATUS_INVALID_ARG;
    }
    if (ctx->num_vehicles > 0 || ctx->num_tasks > 0) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->dimension_count = dimension_count;
    return SG_STATUS_OK;
}

uint32_t sg_get_dimension_count(const SGContext *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->dimension_count;
}

uint32_t sg_add_depot(SGContext *ctx) {
    SGDepotRecord *new_depots;
    size_t next_count;
    uint32_t id;

    if (!ctx) {
        return UINT32_MAX;
    }

    id = ctx->num_depots;
    next_count = (size_t)ctx->num_depots + 1;
    if (next_count > SIZE_MAX / sizeof(*ctx->depots)) {
        return UINT32_MAX;
    }

    new_depots = (SGDepotRecord *)realloc(ctx->depots, next_count * sizeof(*ctx->depots));
    if (!new_depots) {
        return UINT32_MAX;
    }

    ctx->depots = new_depots;
    memset(&ctx->depots[id], 0, sizeof(ctx->depots[id]));
    ctx->depots[id].location_id = UINT32_MAX;
    ctx->num_depots++;
    return id;
}

SGStatus sg_depot_set_location(SGContext *ctx, uint32_t depot_id, double x, double y) {
    SGDepotRecord *depot;

    if (!ctx || depot_id >= ctx->num_depots || !isfinite(x) || !isfinite(y)) {
        return SG_STATUS_INVALID_ARG;
    }

    depot = &ctx->depots[depot_id];
    depot->x = x;
    depot->y = y;
    depot->has_location = 1;
    return SG_STATUS_OK;
}

SGStatus sg_depot_set_time_window(SGContext *ctx, uint32_t depot_id, int32_t early,
                                  int32_t late) {
    SGDepotRecord *depot;

    if (!ctx || depot_id >= ctx->num_depots || late < early) {
        return SG_STATUS_INVALID_ARG;
    }

    depot = &ctx->depots[depot_id];
    depot->tw_early = early;
    depot->tw_late = late;
    depot->has_time_window = 1;
    return SG_STATUS_OK;
}

SGStatus sg_depot_set_max_simultaneous(SGContext *ctx, uint32_t depot_id,
                                        uint32_t max_simultaneous) {
    if (!ctx || depot_id >= ctx->num_depots) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->depots[depot_id].max_simultaneous = max_simultaneous;
    if (max_simultaneous > 0) {
        ctx->has_depot_capacity = 1;
    }
    return SG_STATUS_OK;
}

uint32_t sg_add_request(SGContext *ctx) {
    SGRequestHint *new_hints;
    SGRequestRecord *new_requests;
    size_t next_count;
    uint32_t id;

    if (!ctx) {
        return UINT32_MAX;
    }

    id = ctx->num_requests;
    next_count = (size_t)ctx->num_requests + 1;
    if (next_count > SIZE_MAX / sizeof(*ctx->request_hints) ||
        next_count > SIZE_MAX / sizeof(*ctx->requests)) {
        return UINT32_MAX;
    }

    new_hints = (SGRequestHint *)realloc(ctx->request_hints,
                                         next_count * sizeof(*ctx->request_hints));
    if (!new_hints) {
        return UINT32_MAX;
    }
    ctx->request_hints = new_hints;

    new_requests = (SGRequestRecord *)realloc(ctx->requests,
                                              next_count * sizeof(*ctx->requests));
    if (!new_requests) {
        return UINT32_MAX;
    }
    ctx->requests = new_requests;

    ctx->request_hints[id] = sg_request_hint_default();
    ctx->requests[id] = sg_request_record_default();
    ctx->num_requests++;
    return id;
}

uint32_t sg_add_vehicle(SGContext *ctx) {
    SGVehicleRecord *new_vehicles;
    SGVehicleRecord *vehicle;
    size_t next_count;
    uint32_t id;

    if (!ctx || ctx->dimension_count == 0) {
        return UINT32_MAX;
    }

    id = ctx->num_vehicles;
    next_count = (size_t)ctx->num_vehicles + 1;
    if (next_count > SIZE_MAX / sizeof(*ctx->vehicles) ||
        (size_t)ctx->dimension_count > SIZE_MAX / sizeof(double)) {
        return UINT32_MAX;
    }

    new_vehicles = (SGVehicleRecord *)realloc(ctx->vehicles,
                                              next_count * sizeof(*ctx->vehicles));
    if (!new_vehicles) {
        return UINT32_MAX;
    }
    ctx->vehicles = new_vehicles;

    vehicle = &ctx->vehicles[id];
    memset(vehicle, 0, sizeof(*vehicle));
    vehicle->start_location_id = UINT32_MAX;
    vehicle->end_location_id = UINT32_MAX;
    vehicle->fixed_cost = SG_ROUTE_OBJECTIVE_VEHICLE_WEIGHT;
    vehicle->cost_per_distance = 1.0;
    vehicle->cost_per_duration = 0.0;
    vehicle->max_trips = 1;  /* Default: single trip */
    vehicle->capacity = (double *)calloc((size_t)ctx->dimension_count, sizeof(double));
    if (!vehicle->capacity) {
        return UINT32_MAX;
    }
    vehicle->initial_load = (double *)calloc((size_t)ctx->dimension_count, sizeof(double));
    if (!vehicle->initial_load) {
        free(vehicle->capacity);
        vehicle->capacity = NULL;
        return UINT32_MAX;
    }

    ctx->num_vehicles++;
    return id;
}

uint32_t sg_add_task(SGContext *ctx, SGTaskType type) {
    SGTaskRecord *new_tasks;
    SGTaskRecord *task;
    size_t next_count;
    uint32_t id;

    if (!ctx || !sg_task_type_valid(type) || ctx->dimension_count == 0) {
        return UINT32_MAX;
    }

    id = ctx->num_tasks;
    next_count = (size_t)ctx->num_tasks + 1;
    if (next_count > SIZE_MAX / sizeof(*ctx->tasks) ||
        (size_t)ctx->dimension_count > SIZE_MAX / sizeof(double)) {
        return UINT32_MAX;
    }

    new_tasks = (SGTaskRecord *)realloc(ctx->tasks, next_count * sizeof(*ctx->tasks));
    if (!new_tasks) {
        return UINT32_MAX;
    }
    ctx->tasks = new_tasks;

    task = &ctx->tasks[id];
    memset(task, 0, sizeof(*task));
    task->type = type;
    task->location_id = UINT32_MAX;
    task->demand = (double *)calloc((size_t)ctx->dimension_count, sizeof(double));
    if (!task->demand) {
        return UINT32_MAX;
    }

    ctx->num_tasks++;
    return id;
}

SGStatus sg_vehicle_set_depots(SGContext *ctx, uint32_t vehicle_id, uint32_t start_depot_id,
                               uint32_t end_depot_id) {
    SGVehicleRecord *vehicle;

    if (!ctx || vehicle_id >= ctx->num_vehicles || start_depot_id >= ctx->num_depots ||
        end_depot_id >= ctx->num_depots) {
        return SG_STATUS_INVALID_ARG;
    }

    vehicle = &ctx->vehicles[vehicle_id];
    vehicle->start_depot_id = start_depot_id;
    vehicle->end_depot_id = end_depot_id;
    vehicle->has_depots = 1;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_shift_time_window(SGContext *ctx, uint32_t vehicle_id, int32_t early,
                                          int32_t late) {
    SGVehicleRecord *vehicle;

    if (!ctx || vehicle_id >= ctx->num_vehicles || late < early) {
        return SG_STATUS_INVALID_ARG;
    }

    vehicle = &ctx->vehicles[vehicle_id];
    vehicle->shift_early = early;
    vehicle->shift_late = late;
    vehicle->has_shift_time_window = 1;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_capacity(SGContext *ctx, uint32_t vehicle_id, const double *capacity,
                                 uint32_t capacity_count) {
    SGVehicleRecord *vehicle;
    uint32_t i;

    if (!ctx || vehicle_id >= ctx->num_vehicles || !capacity ||
        capacity_count != ctx->dimension_count || !ctx->vehicles[vehicle_id].capacity) {
        return SG_STATUS_INVALID_ARG;
    }

    vehicle = &ctx->vehicles[vehicle_id];
    for (i = 0; i < capacity_count; i++) {
        if (!isfinite(capacity[i]) || capacity[i] < 0.0) {
            return SG_STATUS_INVALID_ARG;
        }
    }
    memcpy(vehicle->capacity, capacity, (size_t)capacity_count * sizeof(double));
    vehicle->has_capacity = 1;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_initial_load(SGContext *ctx, uint32_t vehicle_id,
                                     const double *initial_load, uint32_t load_count) {
    SGVehicleRecord *vehicle;
    uint32_t i;

    if (!ctx || vehicle_id >= ctx->num_vehicles || !initial_load ||
        load_count != ctx->dimension_count || !ctx->vehicles[vehicle_id].initial_load) {
        return SG_STATUS_INVALID_ARG;
    }

    vehicle = &ctx->vehicles[vehicle_id];
    for (i = 0; i < load_count; i++) {
        if (!isfinite(initial_load[i]) || initial_load[i] < 0.0) {
            return SG_STATUS_INVALID_ARG;
        }
        if (vehicle->has_capacity && vehicle->capacity &&
            initial_load[i] > vehicle->capacity[i]) {
            return SG_STATUS_INVALID_ARG;
        }
    }
    memcpy(vehicle->initial_load, initial_load, (size_t)load_count * sizeof(double));
    vehicle->has_initial_load = 1;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_pd_policy(SGContext *ctx, uint32_t vehicle_id, SGPDPolicy policy) {
    if (!ctx || vehicle_id >= ctx->num_vehicles) {
        return SG_STATUS_INVALID_ARG;
    }
    if (policy != SG_PD_POLICY_NONE && policy != SG_PD_POLICY_LIFO &&
        policy != SG_PD_POLICY_FIFO) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].pd_policy = (uint8_t)policy;
    if (policy != SG_PD_POLICY_NONE) {
        ctx->has_pd_policy = 1;
    }
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_backhaul(SGContext *ctx, uint32_t vehicle_id, int backhaul) {
    if (!ctx || vehicle_id >= ctx->num_vehicles) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].backhaul = backhaul ? 1 : 0;
    if (backhaul) {
        ctx->has_backhaul = 1;
    }
    return SG_STATUS_OK;
}

SGStatus sg_request_set_lock(SGContext *ctx, uint32_t request_id, SGRequestLock lock) {
    if (!ctx || request_id >= ctx->num_requests) {
        return SG_STATUS_INVALID_ARG;
    }
    if (lock != SG_LOCK_NONE && lock != SG_LOCK_COMMITTED && lock != SG_LOCK_FROZEN) {
        return SG_STATUS_INVALID_ARG;
    }

    if (lock == SG_LOCK_NONE && !ctx->request_locks) {
        return SG_STATUS_OK;  /* Already all NONE, nothing to do */
    }

    /* Lazy allocation on first non-NONE lock */
    if (!ctx->request_locks) {
        ctx->request_locks = (uint8_t *)calloc((size_t)ctx->num_requests, sizeof(uint8_t));
        if (!ctx->request_locks) {
            return SG_STATUS_OUT_OF_MEMORY;
        }
    }

    ctx->request_locks[request_id] = (uint8_t)lock;

    /* Update fast-path flags */
    if (lock >= SG_LOCK_COMMITTED) {
        ctx->has_committed = 1;
    }
    if (lock == SG_LOCK_FROZEN) {
        ctx->has_frozen = 1;
    }

    return SG_STATUS_OK;
}

SGStatus sg_task_set_location(SGContext *ctx, uint32_t task_id, double x, double y) {
    SGTaskRecord *task;

    if (!ctx || task_id >= ctx->num_tasks || !isfinite(x) || !isfinite(y)) {
        return SG_STATUS_INVALID_ARG;
    }

    task = &ctx->tasks[task_id];
    task->x = x;
    task->y = y;
    task->has_location = 1;
    return SG_STATUS_OK;
}

SGStatus sg_task_set_time_window(SGContext *ctx, uint32_t task_id, int32_t early, int32_t late) {
    SGTaskRecord *task;

    if (!ctx || task_id >= ctx->num_tasks || late < early) {
        return SG_STATUS_INVALID_ARG;
    }

    task = &ctx->tasks[task_id];
    task->tw_early = early;
    task->tw_late = late;
    task->has_time_window = 1;
    free(task->time_windows);
    task->time_windows = NULL;
    task->num_time_windows = 0;
    return SG_STATUS_OK;
}

SGStatus sg_task_set_service_seconds(SGContext *ctx, uint32_t task_id, int32_t service_seconds) {
    SGTaskRecord *task;

    if (!ctx || task_id >= ctx->num_tasks || service_seconds < 0) {
        return SG_STATUS_INVALID_ARG;
    }

    task = &ctx->tasks[task_id];
    task->service_seconds = service_seconds;
    return SG_STATUS_OK;
}

SGStatus sg_task_set_demand(SGContext *ctx, uint32_t task_id, const double *demand,
                            uint32_t demand_count) {
    SGTaskRecord *task;
    uint32_t i;

    if (!ctx || task_id >= ctx->num_tasks || !demand || demand_count != ctx->dimension_count ||
        !ctx->tasks[task_id].demand) {
        return SG_STATUS_INVALID_ARG;
    }

    task = &ctx->tasks[task_id];
    for (i = 0; i < demand_count; i++) {
        if (!isfinite(demand[i])) {
            return SG_STATUS_INVALID_ARG;
        }
    }

    memcpy(task->demand, demand, (size_t)demand_count * sizeof(double));
    task->has_demand = 1;
    return SG_STATUS_OK;
}

SGStatus sg_request_bind_delivery_task(SGContext *ctx, uint32_t request_id,
                                       uint32_t delivery_task_id) {
    SGRequestRecord *request;
    SGTaskRecord *delivery;

    if (!ctx || request_id >= ctx->num_requests || delivery_task_id >= ctx->num_tasks) {
        return SG_STATUS_INVALID_ARG;
    }

    request = &ctx->requests[request_id];
    delivery = &ctx->tasks[delivery_task_id];
    if (delivery->type != SG_TASK_DELIVERY && delivery->type != SG_TASK_SERVICE) {
        return SG_STATUS_INVALID_ARG;
    }

    request->kind = SG_REQUEST_KIND_DELIVERY_ONLY;
    request->delivery_task_id = delivery_task_id;
    request->has_delivery_task = 1;
    request->pickup_task_id = 0;
    request->has_pickup_task = 0;
    return SG_STATUS_OK;
}

SGStatus sg_request_bind_pickup_delivery_tasks(SGContext *ctx, uint32_t request_id,
                                               uint32_t pickup_task_id,
                                               uint32_t delivery_task_id) {
    SGRequestRecord *request;
    SGTaskRecord *pickup;
    SGTaskRecord *delivery;

    if (!ctx || request_id >= ctx->num_requests || pickup_task_id >= ctx->num_tasks ||
        delivery_task_id >= ctx->num_tasks || pickup_task_id == delivery_task_id) {
        return SG_STATUS_INVALID_ARG;
    }

    request = &ctx->requests[request_id];
    pickup = &ctx->tasks[pickup_task_id];
    delivery = &ctx->tasks[delivery_task_id];
    if (pickup->type != SG_TASK_PICKUP || delivery->type != SG_TASK_DELIVERY) {
        return SG_STATUS_INVALID_ARG;
    }

    request->kind = SG_REQUEST_KIND_PICKUP_DELIVERY;
    request->pickup_task_id = pickup_task_id;
    request->delivery_task_id = delivery_task_id;
    request->has_pickup_task = 1;
    request->has_delivery_task = 1;
    return SG_STATUS_OK;
}

SGStatus sg_request_set_priority_hint(SGContext *ctx, uint32_t request_id, int32_t priority) {
    if (!ctx || request_id >= ctx->num_requests || !ctx->request_hints) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->request_hints[request_id].priority = sg_clamp_priority(priority);
    return SG_STATUS_OK;
}

SGStatus sg_request_set_time_window_hint(SGContext *ctx, uint32_t request_id,
                                         int32_t early, int32_t late) {
    if (!ctx || request_id >= ctx->num_requests || !ctx->request_hints || late < early) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->request_hints[request_id].tw_early = early;
    ctx->request_hints[request_id].tw_late = late;
    ctx->request_hints[request_id].has_time_window = 1;
    return SG_STATUS_OK;
}

SGStatus sg_request_set_zone_hint(SGContext *ctx, uint32_t request_id, uint32_t zone_id) {
    if (!ctx || request_id >= ctx->num_requests || !ctx->request_hints) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->request_hints[request_id].zone_id = zone_id;
    ctx->request_hints[request_id].has_zone = 1;
    return SG_STATUS_OK;
}

SGStatus sg_set_priority_removal_policy(SGContext *ctx, SGPriorityRemovalPolicy policy) {
    if (!ctx || !sg_priority_policy_valid(policy)) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->config.priority_removal_policy = policy;
    return SG_STATUS_OK;
}

SGStatus sg_set_zone_distance_matrix(SGContext *ctx, uint32_t zone_count,
                                     const double *matrix_row_major) {
    double *copy = NULL;
    size_t i;
    size_t j;
    size_t n;
    size_t total;

    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }

    if (zone_count == 0) {
        if (matrix_row_major != NULL) {
            return SG_STATUS_INVALID_ARG;
        }
        sg_zone_matrix_clear(ctx);
        return SG_STATUS_OK;
    }

    if (!matrix_row_major) {
        return SG_STATUS_INVALID_ARG;
    }

    n = (size_t)zone_count;
    if (n > SIZE_MAX / n) {
        return SG_STATUS_INVALID_ARG;
    }
    total = n * n;
    if (total > SIZE_MAX / sizeof(double)) {
        return SG_STATUS_INVALID_ARG;
    }

    copy = (double *)malloc(total * sizeof(double));
    if (!copy) {
        return SG_STATUS_OUT_OF_MEMORY;
    }

    for (i = 0; i < total; i++) {
        double value = matrix_row_major[i];
        if (!isfinite(value) || value < 0.0) {
            free(copy);
            return SG_STATUS_INVALID_ARG;
        }
        copy[i] = value;
    }

    for (i = 0; i < n; i++) {
        copy[i * n + i] = 0.0;
    }

    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            double a = copy[i * n + j];
            double b = copy[j * n + i];
            if (fabs(a - b) > 1e-9) {
                free(copy);
                return SG_STATUS_INVALID_ARG;
            }
        }
    }

    sg_zone_matrix_clear(ctx);
    ctx->zone_distance_matrix = copy;
    ctx->zone_count = zone_count;
    return SG_STATUS_OK;
}

SGStatus sg_clear_zone_distance_matrix(SGContext *ctx) {
    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }

    sg_zone_matrix_clear(ctx);
    return SG_STATUS_OK;
}

SGStatus sg_validate_model(SGContext *ctx) {
    uint32_t i;
    uint32_t d;

    if (!ctx || ctx->dimension_count == 0) {
        return SG_STATUS_INVALID_ARG;
    }

    sg_clear_error(ctx);

    if ((ctx->num_depots > 0 && !ctx->depots) ||
        (ctx->num_vehicles > 0 && !ctx->vehicles) ||
        (ctx->num_tasks > 0 && !ctx->tasks) ||
        (ctx->num_requests > 0 && (!ctx->requests || !ctx->request_hints))) {
        sg_set_error(ctx, "internal data arrays not allocated");
        return SG_STATUS_INFEASIBLE;
    }

    for (i = 0; i < ctx->num_depots; i++) {
        const SGDepotRecord *depot = &ctx->depots[i];
        if (!depot->has_location) {
            sg_set_error(ctx, "depot %u: location not set", i);
            return SG_STATUS_INFEASIBLE;
        }
        if (depot->has_time_window && depot->tw_late < depot->tw_early) {
            sg_set_error(ctx, "depot %u: tw_late < tw_early", i);
            return SG_STATUS_INFEASIBLE;
        }
    }

    for (i = 0; i < ctx->num_vehicles; i++) {
        const SGVehicleRecord *vehicle = &ctx->vehicles[i];

        if (!vehicle->capacity) {
            sg_set_error(ctx, "vehicle %u: no capacity array", i);
            return SG_STATUS_INFEASIBLE;
        }
        if (ctx->num_depots > 0 && !vehicle->has_depots) {
            sg_set_error(ctx, "vehicle %u: depots not set (have %u depots)", i, ctx->num_depots);
            return SG_STATUS_INFEASIBLE;
        }
        if (vehicle->has_depots &&
            (vehicle->start_depot_id >= ctx->num_depots || vehicle->end_depot_id >= ctx->num_depots)) {
            sg_set_error(ctx, "vehicle %u: depot id out of range", i);
            return SG_STATUS_INFEASIBLE;
        }
        if (vehicle->has_shift_time_window && vehicle->shift_late < vehicle->shift_early) {
            sg_set_error(ctx, "vehicle %u: shift_late < shift_early", i);
            return SG_STATUS_INFEASIBLE;
        }
        if (vehicle->depot_loading_seconds < 0 || vehicle->depot_unloading_seconds < 0) {
            sg_set_error(ctx, "vehicle %u: negative depot service time", i);
            return SG_STATUS_INFEASIBLE;
        }

        for (d = 0; d < ctx->dimension_count; d++) {
            double cap = vehicle->capacity[d];
            if (!isfinite(cap) || cap < 0.0) {
                sg_set_error(ctx, "vehicle %u: invalid capacity[%u]", i, d);
                return SG_STATUS_INFEASIBLE;
            }
        }
        if (vehicle->has_travel_profile) {
            if (vehicle->travel_profile_id == 0 ||
                vehicle->travel_profile_id > ctx->num_travel_profiles) {
                sg_set_error(ctx, "vehicle %u: travel_profile_id out of range", i);
                return SG_STATUS_INFEASIBLE;
            }
        }
    }

    /* Validate travel profiles */
    for (i = 0; i < ctx->num_travel_profiles; i++) {
        const SGTravelProfile *tp = &ctx->travel_profiles[i];
        if (tp->has_speed_profile) {
            if (tp->speed_profile_id == 0 ||
                tp->speed_profile_id > ctx->num_speed_profiles) {
                sg_set_error(ctx, "travel_profile %u: speed_profile_id out of range", i);
                return SG_STATUS_INFEASIBLE;
            }
        }
        if (tp->has_distance_matrix && !tp->distance_matrix) {
            sg_set_error(ctx, "travel_profile %u: distance matrix flag set but NULL", i);
            return SG_STATUS_INFEASIBLE;
        }
        if (tp->has_duration_matrix && !tp->duration_matrix) {
            sg_set_error(ctx, "travel_profile %u: duration matrix flag set but NULL", i);
            return SG_STATUS_INFEASIBLE;
        }
        if (tp->has_time_brackets) {
            uint32_t b;
            if (tp->num_time_brackets == 0) {
                sg_set_error(ctx, "travel_profile %u: has_time_brackets but count=0", i);
                return SG_STATUS_INFEASIBLE;
            }
            for (b = 0; b < tp->num_time_brackets; b++) {
                if (!tp->time_brackets[b].duration_matrix) {
                    sg_set_error(ctx, "travel_profile %u: bracket %u has NULL duration", i, b);
                    return SG_STATUS_INFEASIBLE;
                }
                if (b > 0 && tp->time_brackets[b].start_time <= tp->time_brackets[b - 1].start_time) {
                    sg_set_error(ctx, "travel_profile %u: brackets not strictly ascending", i);
                    return SG_STATUS_INFEASIBLE;
                }
            }
        }
    }

    /* Validate global time brackets */
    if (ctx->has_travel_time_brackets) {
        uint32_t b;
        if (ctx->num_travel_time_brackets == 0) {
            sg_set_error(ctx, "has_travel_time_brackets but count=0");
            return SG_STATUS_INFEASIBLE;
        }
        for (b = 0; b < ctx->num_travel_time_brackets; b++) {
            if (!ctx->travel_time_brackets[b].duration_matrix) {
                sg_set_error(ctx, "global time bracket %u has NULL duration", b);
                return SG_STATUS_INFEASIBLE;
            }
            if (b > 0 && ctx->travel_time_brackets[b].start_time <=
                          ctx->travel_time_brackets[b - 1].start_time) {
                sg_set_error(ctx, "global time brackets not strictly ascending");
                return SG_STATUS_INFEASIBLE;
            }
        }
    }

    /* Validate global speed profile reference */
    if (ctx->global_speed_profile_id > 0 &&
        ctx->global_speed_profile_id > ctx->num_speed_profiles) {
        sg_set_error(ctx, "global speed_profile_id out of range");
        return SG_STATUS_INFEASIBLE;
    }

    for (i = 0; i < ctx->num_tasks; i++) {
        const SGTaskRecord *task = &ctx->tasks[i];

        if (!sg_task_ready_for_model(task)) {
            sg_set_error(ctx, "task %u: not ready (location=%d tw=%d svc=%d)",
                         i, task->has_location, task->has_time_window, task->service_seconds);
            return SG_STATUS_INFEASIBLE;
        }
        if (task->tw_late < task->tw_early) {
            sg_set_error(ctx, "task %u: tw_late < tw_early", i);
            return SG_STATUS_INFEASIBLE;
        }
        if (task->num_time_windows >= 2) {
            uint32_t w;
            if (!task->time_windows) {
                sg_set_error(ctx, "task %u: disjunct tw array null", i);
                return SG_STATUS_INFEASIBLE;
            }
            for (w = 0; w < task->num_time_windows; w++) {
                if (task->time_windows[w].late < task->time_windows[w].early) {
                    sg_set_error(ctx, "task %u: disjunct tw[%u] late < early", i, w);
                    return SG_STATUS_INFEASIBLE;
                }
                if (w > 0 && task->time_windows[w].early <= task->time_windows[w - 1].late) {
                    sg_set_error(ctx, "task %u: disjunct tw[%u] overlaps tw[%u]", i, w, w - 1);
                    return SG_STATUS_INFEASIBLE;
                }
            }
            if (task->tw_early != task->time_windows[0].early ||
                task->tw_late != task->time_windows[task->num_time_windows - 1].late) {
                sg_set_error(ctx, "task %u: disjunct tw outer bounds mismatch", i);
                return SG_STATUS_INFEASIBLE;
            }
        }
        if (!task->demand) {
            sg_set_error(ctx, "task %u: demand array null", i);
            return SG_STATUS_INFEASIBLE;
        }
        if (task->type != SG_TASK_SERVICE && !task->has_demand) {
            sg_set_error(ctx, "task %u: demand not set", i);
            return SG_STATUS_INFEASIBLE;
        }
        if (task->has_demand) {
            for (d = 0; d < ctx->dimension_count; d++) {
                if (!isfinite(task->demand[d])) {
                    sg_set_error(ctx, "task %u: demand[%u] not finite", i, d);
                    return SG_STATUS_INFEASIBLE;
                }
            }
        }
        if (task->has_soft_time_window) {
            if (!task->has_time_window) {
                sg_set_error(ctx, "task %u: soft tw without hard tw", i);
                return SG_STATUS_INFEASIBLE;
            }
            if (task->soft_tw_early < task->tw_early ||
                task->soft_tw_late > task->tw_late ||
                task->soft_tw_late < task->soft_tw_early) {
                sg_set_error(ctx, "task %u: soft tw outside hard tw bounds", i);
                return SG_STATUS_INFEASIBLE;
            }
        }
    }

    for (i = 0; i < ctx->num_requests; i++) {
        const SGRequestRecord *request = &ctx->requests[i];

        if (request->kind == SG_REQUEST_KIND_UNBOUND) {
            continue;
        }

        if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY) {
            const SGTaskRecord *delivery;
            if (!request->has_delivery_task || request->delivery_task_id >= ctx->num_tasks) {
                sg_set_error(ctx, "request %u: delivery task not set or out of range", i);
                return SG_STATUS_INFEASIBLE;
            }
            delivery = &ctx->tasks[request->delivery_task_id];
            if (delivery->type != SG_TASK_DELIVERY && delivery->type != SG_TASK_SERVICE) {
                sg_set_error(ctx, "request %u: delivery task %u is not delivery/service type",
                             i, request->delivery_task_id);
                return SG_STATUS_INFEASIBLE;
            }
            if (delivery->type == SG_TASK_DELIVERY &&
                !sg_delivery_task_demand_valid(delivery, ctx->dimension_count,
                                               ctx->demand_sign_convention)) {
                sg_set_error(ctx, "request %u: delivery task %u has invalid demand",
                             i, request->delivery_task_id);
                return SG_STATUS_INFEASIBLE;
            }
            continue;
        }

        if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
            const SGTaskRecord *pickup;
            const SGTaskRecord *delivery;

            if (!request->has_pickup_task || !request->has_delivery_task ||
                request->pickup_task_id >= ctx->num_tasks ||
                request->delivery_task_id >= ctx->num_tasks ||
                request->pickup_task_id == request->delivery_task_id) {
                sg_set_error(ctx, "request %u: pickup/delivery task ids invalid", i);
                return SG_STATUS_INFEASIBLE;
            }

            pickup = &ctx->tasks[request->pickup_task_id];
            delivery = &ctx->tasks[request->delivery_task_id];
            if (pickup->type != SG_TASK_PICKUP || delivery->type != SG_TASK_DELIVERY) {
                sg_set_error(ctx, "request %u: wrong task types (pickup=%d delivery=%d)",
                             i, pickup->type, delivery->type);
                return SG_STATUS_INFEASIBLE;
            }
            if (pickup->tw_early > delivery->tw_late) {
                sg_set_error(ctx, "request %u: pickup tw_early > delivery tw_late", i);
                return SG_STATUS_INFEASIBLE;
            }
            if (!sg_request_pd_demands_valid(pickup, delivery, ctx->dimension_count,
                                             ctx->demand_sign_convention)) {
                sg_set_error(ctx, "request %u: pickup/delivery demands don't balance", i);
                return SG_STATUS_INFEASIBLE;
            }
            continue;
        }

        sg_set_error(ctx, "request %u: unknown kind %d", i, request->kind);
        return SG_STATUS_INFEASIBLE;
    }

    /* Validate frozen requests: each must appear in initial_routes */
    if (ctx->has_frozen && ctx->request_locks) {
        for (i = 0; i < ctx->num_requests; i++) {
            if (ctx->request_locks[i] == SG_LOCK_FROZEN) {
                int found = 0;
                uint32_t r, offset = 0;
                for (r = 0; r < ctx->num_initial_routes; r++) {
                    uint32_t j;
                    for (j = 0; j < ctx->initial_route_lengths[r]; j++) {
                        if (ctx->initial_route_request_ids[offset + j] == i) {
                            found = 1;
                            break;
                        }
                    }
                    offset += ctx->initial_route_lengths[r];
                    if (found) break;
                }
                if (!found) {
                    sg_set_error(ctx, "request %u: FROZEN lock requires initial_routes assignment", i);
                    return SG_STATUS_INFEASIBLE;
                }
            }
        }
    }

    return SG_STATUS_OK;
}

uint32_t sg_add_location(SGContext *ctx) {
    double *new_coords;
    size_t next_count;
    uint32_t id;

    if (!ctx) {
        return UINT32_MAX;
    }

    id = ctx->num_locations;
    next_count = (size_t)ctx->num_locations + 1;
    if (next_count > SIZE_MAX / (2 * sizeof(double))) {
        return UINT32_MAX;
    }

    new_coords = (double *)realloc(ctx->location_coords, next_count * 2 * sizeof(double));
    if (!new_coords) {
        return UINT32_MAX;
    }

    ctx->location_coords = new_coords;
    ctx->location_coords[id * 2] = 0.0;
    ctx->location_coords[id * 2 + 1] = 0.0;
    ctx->num_locations++;
    return id;
}

SGStatus sg_location_set_coords(SGContext *ctx, uint32_t location_id, double x, double y) {
    if (!ctx || location_id >= ctx->num_locations || !isfinite(x) || !isfinite(y)) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->location_coords[location_id * 2] = x;
    ctx->location_coords[location_id * 2 + 1] = y;
    return SG_STATUS_OK;
}

SGStatus sg_depot_set_location_id(SGContext *ctx, uint32_t depot_id, uint32_t location_id) {
    if (!ctx || depot_id >= ctx->num_depots || location_id >= ctx->num_locations) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->depots[depot_id].location_id = location_id;
    ctx->depots[depot_id].has_location = 1;
    if (ctx->location_coords) {
        ctx->depots[depot_id].x = ctx->location_coords[location_id * 2];
        ctx->depots[depot_id].y = ctx->location_coords[location_id * 2 + 1];
    }
    return SG_STATUS_OK;
}

SGStatus sg_task_set_location_id(SGContext *ctx, uint32_t task_id, uint32_t location_id) {
    if (!ctx || task_id >= ctx->num_tasks || location_id >= ctx->num_locations) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->tasks[task_id].location_id = location_id;
    ctx->tasks[task_id].has_location = 1;
    if (ctx->location_coords) {
        ctx->tasks[task_id].x = ctx->location_coords[location_id * 2];
        ctx->tasks[task_id].y = ctx->location_coords[location_id * 2 + 1];
    }
    return SG_STATUS_OK;
}

SGStatus sg_set_travel_matrix(SGContext *ctx, uint32_t location_count,
                               const double *distance_matrix_row_major,
                               const double *duration_matrix_row_major) {
    double *dist_copy = NULL;
    double *dur_copy = NULL;
    size_t n;
    size_t total;
    size_t i;

    if (!ctx || location_count == 0 || !distance_matrix_row_major ||
        !duration_matrix_row_major) {
        return SG_STATUS_INVALID_ARG;
    }

    n = (size_t)location_count;
    if (n > SIZE_MAX / n) {
        return SG_STATUS_INVALID_ARG;
    }
    total = n * n;
    if (total > SIZE_MAX / sizeof(double)) {
        return SG_STATUS_INVALID_ARG;
    }

    dist_copy = (double *)malloc(total * sizeof(double));
    dur_copy = (double *)malloc(total * sizeof(double));
    if (!dist_copy || !dur_copy) {
        free(dist_copy);
        free(dur_copy);
        return SG_STATUS_OUT_OF_MEMORY;
    }

    for (i = 0; i < total; i++) {
        if (!isfinite(distance_matrix_row_major[i]) || distance_matrix_row_major[i] < 0.0 ||
            !isfinite(duration_matrix_row_major[i]) || duration_matrix_row_major[i] < 0.0) {
            free(dist_copy);
            free(dur_copy);
            return SG_STATUS_INVALID_ARG;
        }
        dist_copy[i] = distance_matrix_row_major[i];
        dur_copy[i] = duration_matrix_row_major[i];
    }

    free(ctx->travel_distance_matrix);
    free(ctx->travel_duration_matrix);
    ctx->travel_distance_matrix = dist_copy;
    ctx->travel_duration_matrix = dur_copy;
    return SG_STATUS_OK;
}

SGStatus sg_set_travel_callback(SGContext *ctx, SGTravelCallback callback, void *user_data) {
    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->travel_callback = callback;
    ctx->travel_callback_data = user_data;
    return SG_STATUS_OK;
}

/* ---- Speed profiles ---- */

uint32_t sg_add_speed_profile(SGContext *ctx) {
    SHStepFunc **new_arr;
    SHStepFunc *sf;
    uint32_t id;

    if (!ctx) {
        return UINT32_MAX;
    }

    sf = sh_step_create(1.0, 4);
    if (!sf) {
        return UINT32_MAX;
    }

    new_arr = (SHStepFunc **)realloc(ctx->speed_profiles,
                                      ((size_t)ctx->num_speed_profiles + 1) * sizeof(SHStepFunc *));
    if (!new_arr) {
        sh_step_free(sf);
        return UINT32_MAX;
    }

    id = ctx->num_speed_profiles;
    ctx->speed_profiles = new_arr;
    ctx->speed_profiles[id] = sf;
    ctx->num_speed_profiles++;
    ctx->has_speed_profiles = 1;
    return id;
}

SGStatus sg_speed_profile_add_entry(SGContext *ctx, uint32_t profile_id,
                                     double start_time, double multiplier) {
    if (!ctx || profile_id >= ctx->num_speed_profiles) {
        return SG_STATUS_INVALID_ARG;
    }
    if (multiplier <= 0.0 || !isfinite(multiplier) || !isfinite(start_time)) {
        return SG_STATUS_INVALID_ARG;
    }
    if (sh_step_set(ctx->speed_profiles[profile_id], start_time, multiplier) != 0) {
        return SG_STATUS_INVALID_ARG;
    }
    return SG_STATUS_OK;
}

SGStatus sg_set_global_speed_profile(SGContext *ctx, uint32_t speed_profile_id) {
    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }
    if (speed_profile_id >= ctx->num_speed_profiles) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->global_speed_profile_id = speed_profile_id + 1;  /* 1-based internally */
    return SG_STATUS_OK;
}

/* ---- Travel profiles ---- */

uint32_t sg_add_travel_profile(SGContext *ctx) {
    SGTravelProfile *new_arr;
    uint32_t id;

    if (!ctx) {
        return UINT32_MAX;
    }

    new_arr = (SGTravelProfile *)realloc(ctx->travel_profiles,
                                          ((size_t)ctx->num_travel_profiles + 1) * sizeof(SGTravelProfile));
    if (!new_arr) {
        return UINT32_MAX;
    }

    id = ctx->num_travel_profiles;
    ctx->travel_profiles = new_arr;
    memset(&ctx->travel_profiles[id], 0, sizeof(SGTravelProfile));
    ctx->num_travel_profiles++;
    ctx->has_travel_profiles = 1;
    return id;
}

SGStatus sg_travel_profile_set_matrices(SGContext *ctx, uint32_t profile_id,
                                         uint32_t location_count,
                                         const double *distance_matrix,
                                         const double *duration_matrix) {
    SGTravelProfile *tp;
    size_t total;
    size_t i;

    if (!ctx || profile_id >= ctx->num_travel_profiles) {
        return SG_STATUS_INVALID_ARG;
    }
    if (location_count == 0) {
        return SG_STATUS_INVALID_ARG;
    }
    if (!distance_matrix && !duration_matrix) {
        return SG_STATUS_INVALID_ARG;
    }

    total = (size_t)location_count * location_count;
    tp = &ctx->travel_profiles[profile_id];

    if (distance_matrix) {
        double *copy = (double *)malloc(total * sizeof(double));
        if (!copy) return SG_STATUS_OUT_OF_MEMORY;
        for (i = 0; i < total; i++) {
            if (!isfinite(distance_matrix[i]) || distance_matrix[i] < 0.0) {
                free(copy);
                return SG_STATUS_INVALID_ARG;
            }
            copy[i] = distance_matrix[i];
        }
        free(tp->distance_matrix);
        tp->distance_matrix = copy;
        tp->has_distance_matrix = 1;
    }

    if (duration_matrix) {
        double *copy = (double *)malloc(total * sizeof(double));
        if (!copy) return SG_STATUS_OUT_OF_MEMORY;
        for (i = 0; i < total; i++) {
            if (!isfinite(duration_matrix[i]) || duration_matrix[i] < 0.0) {
                free(copy);
                return SG_STATUS_INVALID_ARG;
            }
            copy[i] = duration_matrix[i];
        }
        free(tp->duration_matrix);
        tp->duration_matrix = copy;
        tp->has_duration_matrix = 1;
    }

    return SG_STATUS_OK;
}

SGStatus sg_travel_profile_set_speed_profile(SGContext *ctx, uint32_t profile_id,
                                              uint32_t speed_profile_id) {
    if (!ctx || profile_id >= ctx->num_travel_profiles) {
        return SG_STATUS_INVALID_ARG;
    }
    if (speed_profile_id >= ctx->num_speed_profiles) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->travel_profiles[profile_id].speed_profile_id = speed_profile_id + 1;  /* 1-based */
    ctx->travel_profiles[profile_id].has_speed_profile = 1;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_travel_profile(SGContext *ctx, uint32_t vehicle_id,
                                        uint32_t profile_id) {
    if (!ctx || vehicle_id >= ctx->num_vehicles) {
        return SG_STATUS_INVALID_ARG;
    }
    if (profile_id >= ctx->num_travel_profiles) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].travel_profile_id = profile_id + 1;  /* 1-based */
    ctx->vehicles[vehicle_id].has_travel_profile = 1;
    return SG_STATUS_OK;
}

/* ---- Time-indexed travel brackets ---- */

SGStatus sg_set_travel_time_bracket(SGContext *ctx, double start_time,
                                     uint32_t location_count,
                                     const double *distance_matrix,
                                     const double *duration_matrix) {
    SGTravelTimeBracket *new_arr;
    size_t total, i;
    uint32_t insert_pos, k;
    double *dist_copy = NULL;
    double *dur_copy = NULL;

    if (!ctx || !duration_matrix || location_count == 0 || !isfinite(start_time)) {
        return SG_STATUS_INVALID_ARG;
    }

    /* Check for duplicate start_time */
    for (k = 0; k < ctx->num_travel_time_brackets; k++) {
        if (ctx->travel_time_brackets[k].start_time == start_time) {
            return SG_STATUS_INVALID_ARG;
        }
    }

    total = (size_t)location_count * location_count;

    /* Copy duration matrix (required) */
    dur_copy = (double *)malloc(total * sizeof(double));
    if (!dur_copy) return SG_STATUS_OUT_OF_MEMORY;
    for (i = 0; i < total; i++) {
        if (!isfinite(duration_matrix[i]) || duration_matrix[i] < 0.0) {
            free(dur_copy);
            return SG_STATUS_INVALID_ARG;
        }
        dur_copy[i] = duration_matrix[i];
    }

    /* Copy distance matrix (optional) */
    if (distance_matrix) {
        dist_copy = (double *)malloc(total * sizeof(double));
        if (!dist_copy) {
            free(dur_copy);
            return SG_STATUS_OUT_OF_MEMORY;
        }
        for (i = 0; i < total; i++) {
            if (!isfinite(distance_matrix[i]) || distance_matrix[i] < 0.0) {
                free(dist_copy);
                free(dur_copy);
                return SG_STATUS_INVALID_ARG;
            }
            dist_copy[i] = distance_matrix[i];
        }
    }

    /* Realloc bracket array */
    new_arr = (SGTravelTimeBracket *)realloc(ctx->travel_time_brackets,
        ((size_t)ctx->num_travel_time_brackets + 1) * sizeof(SGTravelTimeBracket));
    if (!new_arr) {
        free(dist_copy);
        free(dur_copy);
        return SG_STATUS_OUT_OF_MEMORY;
    }
    ctx->travel_time_brackets = new_arr;

    /* Find insertion position to maintain sorted order */
    insert_pos = ctx->num_travel_time_brackets;
    while (insert_pos > 0 && ctx->travel_time_brackets[insert_pos - 1].start_time > start_time) {
        ctx->travel_time_brackets[insert_pos] = ctx->travel_time_brackets[insert_pos - 1];
        insert_pos--;
    }

    ctx->travel_time_brackets[insert_pos].start_time = start_time;
    ctx->travel_time_brackets[insert_pos].distance_matrix = dist_copy;
    ctx->travel_time_brackets[insert_pos].duration_matrix = dur_copy;
    ctx->num_travel_time_brackets++;
    ctx->has_travel_time_brackets = 1;

    return SG_STATUS_OK;
}

SGStatus sg_travel_profile_add_time_bracket(SGContext *ctx, uint32_t profile_id,
                                             double start_time,
                                             uint32_t location_count,
                                             const double *distance_matrix,
                                             const double *duration_matrix) {
    SGTravelProfile *tp;
    SGTravelTimeBracket *new_arr;
    size_t total, i;
    uint32_t insert_pos, k;
    double *dist_copy = NULL;
    double *dur_copy = NULL;

    if (!ctx || profile_id >= ctx->num_travel_profiles) {
        return SG_STATUS_INVALID_ARG;
    }
    if (!duration_matrix || location_count == 0 || !isfinite(start_time)) {
        return SG_STATUS_INVALID_ARG;
    }

    tp = &ctx->travel_profiles[profile_id];

    /* Check for duplicate start_time */
    for (k = 0; k < tp->num_time_brackets; k++) {
        if (tp->time_brackets[k].start_time == start_time) {
            return SG_STATUS_INVALID_ARG;
        }
    }

    total = (size_t)location_count * location_count;

    /* Copy duration matrix (required) */
    dur_copy = (double *)malloc(total * sizeof(double));
    if (!dur_copy) return SG_STATUS_OUT_OF_MEMORY;
    for (i = 0; i < total; i++) {
        if (!isfinite(duration_matrix[i]) || duration_matrix[i] < 0.0) {
            free(dur_copy);
            return SG_STATUS_INVALID_ARG;
        }
        dur_copy[i] = duration_matrix[i];
    }

    /* Copy distance matrix (optional) */
    if (distance_matrix) {
        dist_copy = (double *)malloc(total * sizeof(double));
        if (!dist_copy) {
            free(dur_copy);
            return SG_STATUS_OUT_OF_MEMORY;
        }
        for (i = 0; i < total; i++) {
            if (!isfinite(distance_matrix[i]) || distance_matrix[i] < 0.0) {
                free(dist_copy);
                free(dur_copy);
                return SG_STATUS_INVALID_ARG;
            }
            dist_copy[i] = distance_matrix[i];
        }
    }

    /* Realloc bracket array */
    new_arr = (SGTravelTimeBracket *)realloc(tp->time_brackets,
        ((size_t)tp->num_time_brackets + 1) * sizeof(SGTravelTimeBracket));
    if (!new_arr) {
        free(dist_copy);
        free(dur_copy);
        return SG_STATUS_OUT_OF_MEMORY;
    }
    tp->time_brackets = new_arr;

    /* Find insertion position to maintain sorted order */
    insert_pos = tp->num_time_brackets;
    while (insert_pos > 0 && tp->time_brackets[insert_pos - 1].start_time > start_time) {
        tp->time_brackets[insert_pos] = tp->time_brackets[insert_pos - 1];
        insert_pos--;
    }

    tp->time_brackets[insert_pos].start_time = start_time;
    tp->time_brackets[insert_pos].distance_matrix = dist_copy;
    tp->time_brackets[insert_pos].duration_matrix = dur_copy;
    tp->num_time_brackets++;
    tp->has_time_brackets = 1;

    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_qualifications(SGContext *ctx, uint32_t vehicle_id,
                                        uint64_t qualification_flags) {
    if (!ctx || vehicle_id >= ctx->num_vehicles) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].qualifications = qualification_flags;
    return SG_STATUS_OK;
}

SGStatus sg_request_set_required_qualifications(SGContext *ctx, uint32_t request_id,
                                                 uint64_t qualification_flags) {
    if (!ctx || request_id >= ctx->num_requests) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->requests[request_id].required_qualifications = qualification_flags;
    return SG_STATUS_OK;
}

SGStatus sg_request_add_allowed_vehicle(SGContext *ctx, uint32_t request_id,
                                         uint32_t vehicle_id) {
    SGRequestRecord *req;
    uint32_t word = vehicle_id >> 6;
    uint16_t needed = (uint16_t)(word + 1);

    if (!ctx || request_id >= ctx->num_requests) {
        return SG_STATUS_INVALID_ARG;
    }
    req = &ctx->requests[request_id];

    if (needed > req->allowed_vc_words) {
        uint64_t *p = (uint64_t *)realloc(req->allowed_vehicles,
                                           (size_t)needed * sizeof(uint64_t));
        if (!p) return SG_STATUS_OUT_OF_MEMORY;
        memset(p + req->allowed_vc_words, 0,
               ((size_t)needed - req->allowed_vc_words) * sizeof(uint64_t));
        req->allowed_vehicles = p;
        req->allowed_vc_words = needed;
    }

    req->allowed_vehicles[word] |= (1ULL << (vehicle_id & 63));
    return SG_STATUS_OK;
}

SGStatus sg_request_add_forbidden_vehicle(SGContext *ctx, uint32_t request_id,
                                           uint32_t vehicle_id) {
    SGRequestRecord *req;
    uint32_t word = vehicle_id >> 6;
    uint16_t needed = (uint16_t)(word + 1);

    if (!ctx || request_id >= ctx->num_requests) {
        return SG_STATUS_INVALID_ARG;
    }
    req = &ctx->requests[request_id];

    if (needed > req->forbidden_vc_words) {
        uint64_t *p = (uint64_t *)realloc(req->forbidden_vehicles,
                                           (size_t)needed * sizeof(uint64_t));
        if (!p) return SG_STATUS_OUT_OF_MEMORY;
        memset(p + req->forbidden_vc_words, 0,
               ((size_t)needed - req->forbidden_vc_words) * sizeof(uint64_t));
        req->forbidden_vehicles = p;
        req->forbidden_vc_words = needed;
    }

    req->forbidden_vehicles[word] |= (1ULL << (vehicle_id & 63));
    return SG_STATUS_OK;
}

SGStatus sg_add_commodity(SGContext *ctx, uint32_t *commodity_id_out) {
    uint64_t *new_conflicts;
    uint32_t id;

    if (!ctx || !commodity_id_out) {
        return SG_STATUS_INVALID_ARG;
    }
    if (ctx->num_commodities >= 64) {
        return SG_STATUS_INVALID_ARG;
    }

    id = ctx->num_commodities;
    new_conflicts = (uint64_t *)realloc(ctx->commodity_conflicts,
                                         (size_t)(id + 1) * sizeof(uint64_t));
    if (!new_conflicts) {
        return SG_STATUS_OUT_OF_MEMORY;
    }
    ctx->commodity_conflicts = new_conflicts;
    ctx->commodity_conflicts[id] = 0;
    ctx->num_commodities = id + 1;
    *commodity_id_out = id + 1;  /* 1-indexed */
    return SG_STATUS_OK;
}

SGStatus sg_commodity_set_conflict(SGContext *ctx, uint32_t commodity_a, uint32_t commodity_b) {
    if (!ctx || commodity_a == 0 || commodity_b == 0 ||
        commodity_a > ctx->num_commodities || commodity_b > ctx->num_commodities ||
        commodity_a == commodity_b) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->commodity_conflicts[commodity_a - 1] |= (1ULL << (commodity_b - 1));
    ctx->commodity_conflicts[commodity_b - 1] |= (1ULL << (commodity_a - 1));
    return SG_STATUS_OK;
}

SGStatus sg_request_set_commodity(SGContext *ctx, uint32_t request_id, uint32_t commodity_id) {
    if (!ctx || request_id >= ctx->num_requests) {
        return SG_STATUS_INVALID_ARG;
    }
    if (commodity_id > ctx->num_commodities) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->requests[request_id].commodity_id = commodity_id;
    return SG_STATUS_OK;
}

SGStatus sg_add_exclusion_group(SGContext *ctx, uint32_t *group_id_out) {
    if (!ctx || !group_id_out) {
        return SG_STATUS_INVALID_ARG;
    }
    *group_id_out = ctx->num_exclusion_groups;
    ctx->num_exclusion_groups++;
    return SG_STATUS_OK;
}

SGStatus sg_request_add_exclusion_group(SGContext *ctx, uint32_t request_id, uint32_t group_id) {
    SGRequestRecord *req;
    uint32_t *new_ids;
    uint16_t i;

    if (!ctx || request_id >= ctx->num_requests || group_id >= ctx->num_exclusion_groups) {
        return SG_STATUS_INVALID_ARG;
    }
    req = &ctx->requests[request_id];

    /* Check for duplicate */
    for (i = 0; i < req->num_exclusion_groups; i++) {
        if (req->exclusion_group_ids[i] == group_id) {
            return SG_STATUS_OK;
        }
    }

    new_ids = (uint32_t *)realloc(req->exclusion_group_ids,
                                   (size_t)(req->num_exclusion_groups + 1) * sizeof(uint32_t));
    if (!new_ids) {
        return SG_STATUS_OUT_OF_MEMORY;
    }
    req->exclusion_group_ids = new_ids;
    req->exclusion_group_ids[req->num_exclusion_groups] = group_id;
    req->num_exclusion_groups++;
    return SG_STATUS_OK;
}

SGStatus sg_set_num_setup_classes(SGContext *ctx, uint32_t count) {
    double *matrix;
    size_t total;

    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }
    if (count == 0) {
        free(ctx->setup_time_matrix);
        ctx->setup_time_matrix = NULL;
        ctx->num_setup_classes = 0;
        return SG_STATUS_OK;
    }

    total = (size_t)count * (size_t)count;
    if (total / count != count) {
        return SG_STATUS_INVALID_ARG;
    }
    matrix = (double *)calloc(total, sizeof(double));
    if (!matrix) {
        return SG_STATUS_OUT_OF_MEMORY;
    }

    free(ctx->setup_time_matrix);
    ctx->setup_time_matrix = matrix;
    ctx->num_setup_classes = count;
    return SG_STATUS_OK;
}

SGStatus sg_set_setup_time(SGContext *ctx, uint32_t from_class, uint32_t to_class,
                            double seconds) {
    if (!ctx || from_class == 0 || to_class == 0 ||
        from_class > ctx->num_setup_classes || to_class > ctx->num_setup_classes ||
        !isfinite(seconds) || seconds < 0.0) {
        return SG_STATUS_INVALID_ARG;
    }
    if (!ctx->setup_time_matrix) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->setup_time_matrix[(size_t)(from_class - 1) * ctx->num_setup_classes + (to_class - 1)] = seconds;
    return SG_STATUS_OK;
}

SGStatus sg_request_set_setup_class(SGContext *ctx, uint32_t request_id, uint32_t class_id) {
    if (!ctx || request_id >= ctx->num_requests) {
        return SG_STATUS_INVALID_ARG;
    }
    if (class_id > ctx->num_setup_classes) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->requests[request_id].setup_class_id = class_id;
    return SG_STATUS_OK;
}

/* Per-operator telemetry getters */

uint32_t sg_get_destroy_operator_count(const SGContext *ctx) {
    if (!ctx) return 0;
    return ctx->num_destroy_ops;
}

uint32_t sg_get_repair_operator_count(const SGContext *ctx) {
    if (!ctx) return 0;
    return ctx->num_repair_ops;
}

SGStatus sg_get_destroy_operator_stats(const SGContext *ctx, uint32_t index,
                                        SGOperatorStats *out) {
    if (!ctx || !out || index >= ctx->num_destroy_ops || !ctx->destroy_op_stats) {
        return SG_STATUS_INVALID_ARG;
    }
    *out = ctx->destroy_op_stats[index];
    return SG_STATUS_OK;
}

SGStatus sg_get_repair_operator_stats(const SGContext *ctx, uint32_t index,
                                       SGOperatorStats *out) {
    if (!ctx || !out || index >= ctx->num_repair_ops || !ctx->repair_op_stats) {
        return SG_STATUS_INVALID_ARG;
    }
    *out = ctx->repair_op_stats[index];
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_open_end(SGContext *ctx, uint32_t vehicle_id, int open) {
    if (!ctx || vehicle_id >= ctx->num_vehicles) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].open_end = open ? 1 : 0;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_open_start(SGContext *ctx, uint32_t vehicle_id, int open) {
    if (!ctx || vehicle_id >= ctx->num_vehicles) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].open_start = open ? 1 : 0;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_max_duration(SGContext *ctx, uint32_t vehicle_id,
                                      int32_t max_seconds) {
    if (!ctx || vehicle_id >= ctx->num_vehicles || max_seconds < 0) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].max_duration_seconds = max_seconds;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_max_tasks(SGContext *ctx, uint32_t vehicle_id, uint32_t max_tasks) {
    if (!ctx || vehicle_id >= ctx->num_vehicles) return SG_STATUS_INVALID_ARG;
    ctx->vehicles[vehicle_id].max_tasks = max_tasks;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_max_distance(SGContext *ctx, uint32_t vehicle_id, double max_distance) {
    if (!ctx || vehicle_id >= ctx->num_vehicles || max_distance < 0.0 || !isfinite(max_distance))
        return SG_STATUS_INVALID_ARG;
    ctx->vehicles[vehicle_id].max_distance = max_distance;
    return SG_STATUS_OK;
}

SGStatus sg_request_set_max_ride_time(SGContext *ctx, uint32_t request_id,
                                       int32_t max_seconds) {
    if (!ctx || request_id >= ctx->num_requests || max_seconds < 0) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->requests[request_id].max_ride_time_seconds = max_seconds;
    ctx->requests[request_id].has_max_ride_time = max_seconds > 0 ? 1 : 0;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_costs(SGContext *ctx, uint32_t vehicle_id,
                               double fixed_cost, double cost_per_distance,
                               double cost_per_duration) {
    if (!ctx || vehicle_id >= ctx->num_vehicles ||
        !isfinite(fixed_cost) || fixed_cost < 0.0 ||
        !isfinite(cost_per_distance) || cost_per_distance < 0.0 ||
        !isfinite(cost_per_duration) || cost_per_duration < 0.0) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].fixed_cost = fixed_cost;
    ctx->vehicles[vehicle_id].cost_per_distance = cost_per_distance;
    ctx->vehicles[vehicle_id].cost_per_duration = cost_per_duration;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_waiting_cost(SGContext *ctx, uint32_t vehicle_id,
                                      double cost_per_waiting) {
    if (!ctx || vehicle_id >= ctx->num_vehicles ||
        !isfinite(cost_per_waiting) || cost_per_waiting < 0.0) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].cost_per_waiting = cost_per_waiting;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_overtime_cost(SGContext *ctx, uint32_t vehicle_id,
                                       double cost_per_overtime) {
    if (!ctx || vehicle_id >= ctx->num_vehicles ||
        !isfinite(cost_per_overtime) || cost_per_overtime < 0.0) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].cost_per_overtime = cost_per_overtime;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_depot_loading_seconds(SGContext *ctx, uint32_t vehicle_id,
                                               int32_t seconds) {
    if (!ctx || vehicle_id >= ctx->num_vehicles || seconds < 0) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].depot_loading_seconds = seconds;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_depot_unloading_seconds(SGContext *ctx, uint32_t vehicle_id,
                                                 int32_t seconds) {
    if (!ctx || vehicle_id >= ctx->num_vehicles || seconds < 0) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].depot_unloading_seconds = seconds;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_break_policy(SGContext *ctx, uint32_t vehicle_id,
                                      int32_t max_work_seconds,
                                      int32_t break_duration_seconds) {
    SGVehicleRecord *vehicle;

    if (!ctx || vehicle_id >= ctx->num_vehicles) {
        return SG_STATUS_INVALID_ARG;
    }
    if (max_work_seconds < 0 || break_duration_seconds < 0) {
        return SG_STATUS_INVALID_ARG;
    }

    vehicle = &ctx->vehicles[vehicle_id];
    vehicle->break_max_work_seconds = max_work_seconds;
    vehicle->break_duration_seconds = break_duration_seconds;
    vehicle->has_break_policy = (max_work_seconds > 0 && break_duration_seconds > 0) ? 1 : 0;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_max_total_work(SGContext *ctx, uint32_t vehicle_id,
                                        int32_t max_total_work_seconds) {
    if (!ctx || vehicle_id >= ctx->num_vehicles || max_total_work_seconds < 0) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].max_total_work_seconds = max_total_work_seconds;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_max_trips(SGContext *ctx, uint32_t vehicle_id, uint32_t max_trips) {
    if (!ctx || vehicle_id >= ctx->num_vehicles) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].max_trips = max_trips;
    ctx->vehicles[vehicle_id].has_multi_trip = (max_trips != 1) ? 1 : 0;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_trip_reload_seconds(SGContext *ctx, uint32_t vehicle_id, int32_t seconds) {
    if (!ctx || vehicle_id >= ctx->num_vehicles || seconds < 0) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->vehicles[vehicle_id].trip_reload_seconds = seconds;
    return SG_STATUS_OK;
}

SGStatus sg_task_set_soft_time_window(SGContext *ctx, uint32_t task_id,
                                      int32_t early, int32_t late,
                                      double early_penalty, double late_penalty) {
    SGTaskRecord *task;

    if (!ctx || task_id >= ctx->num_tasks || late < early ||
        !isfinite(early_penalty) || early_penalty < 0.0 ||
        !isfinite(late_penalty) || late_penalty < 0.0) {
        return SG_STATUS_INVALID_ARG;
    }

    task = &ctx->tasks[task_id];
    if (!task->has_time_window) {
        return SG_STATUS_INVALID_ARG;
    }
    if (early < task->tw_early || late > task->tw_late) {
        return SG_STATUS_INVALID_ARG;
    }

    task->soft_tw_early = early;
    task->soft_tw_late = late;
    task->tw_early_penalty = early_penalty;
    task->tw_late_penalty = late_penalty;
    task->has_soft_time_window = 1;
    return SG_STATUS_OK;
}

SGStatus sg_task_add_time_window(SGContext *ctx, uint32_t task_id,
                                 int32_t early, int32_t late) {
    SGTaskRecord *task;
    SGTimeWindow *new_arr;
    uint8_t n;
    uint8_t insert_pos;
    uint8_t i;

    if (!ctx || task_id >= ctx->num_tasks || late < early) {
        return SG_STATUS_INVALID_ARG;
    }

    task = &ctx->tasks[task_id];

    /* First call when no TW set at all: initialize single window */
    if (!task->has_time_window) {
        task->tw_early = early;
        task->tw_late = late;
        task->has_time_window = 1;
        new_arr = (SGTimeWindow *)malloc(sizeof(SGTimeWindow));
        if (!new_arr) return SG_STATUS_OUT_OF_MEMORY;
        new_arr[0].early = early;
        new_arr[0].late = late;
        task->time_windows = new_arr;
        task->num_time_windows = 1;
        return SG_STATUS_OK;
    }

    /* Promote existing single TW (set via set_time_window) into array */
    if (task->num_time_windows == 0) {
        new_arr = (SGTimeWindow *)malloc(sizeof(SGTimeWindow));
        if (!new_arr) return SG_STATUS_OUT_OF_MEMORY;
        new_arr[0].early = task->tw_early;
        new_arr[0].late = task->tw_late;
        task->time_windows = new_arr;
        task->num_time_windows = 1;
    }

    n = task->num_time_windows;
    if (n >= 254) {
        return SG_STATUS_INVALID_ARG; /* uint8_t overflow guard */
    }

    /* Find sorted insertion position */
    insert_pos = n;
    for (i = 0; i < n; i++) {
        if (early < task->time_windows[i].early) {
            insert_pos = i;
            break;
        }
    }

    /* Reject overlapping windows: early <= existing.late && late >= existing.early */
    if (insert_pos > 0) {
        if (early <= task->time_windows[insert_pos - 1].late) {
            return SG_STATUS_INVALID_ARG;
        }
    }
    if (insert_pos < n) {
        if (late >= task->time_windows[insert_pos].early) {
            return SG_STATUS_INVALID_ARG;
        }
    }

    new_arr = (SGTimeWindow *)realloc(task->time_windows,
                                       (size_t)(n + 1) * sizeof(SGTimeWindow));
    if (!new_arr) return SG_STATUS_OUT_OF_MEMORY;
    task->time_windows = new_arr;

    /* Shift elements right to make room */
    if (insert_pos < n) {
        memmove(&new_arr[insert_pos + 1], &new_arr[insert_pos],
                (size_t)(n - insert_pos) * sizeof(SGTimeWindow));
    }
    new_arr[insert_pos].early = early;
    new_arr[insert_pos].late = late;
    task->num_time_windows = n + 1;

    /* Update outer bounds */
    task->tw_early = task->time_windows[0].early;
    task->tw_late = task->time_windows[task->num_time_windows - 1].late;

    return SG_STATUS_OK;
}

SGStatus sg_set_unassigned_weight(SGContext *ctx, double weight) {
    if (!ctx || !isfinite(weight) || weight < 0.0) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->unassigned_weight = weight;
    return SG_STATUS_OK;
}

SGStatus sg_set_span_cost_duration(SGContext *ctx, double cost) {
    if (!ctx || !isfinite(cost) || cost < 0.0) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->span_cost_duration = cost;
    return SG_STATUS_OK;
}

SGStatus sg_set_span_cost_distance(SGContext *ctx, double cost) {
    if (!ctx || !isfinite(cost) || cost < 0.0) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->span_cost_distance = cost;
    return SG_STATUS_OK;
}

uint32_t sg_add_delivery_request(SGContext *ctx, double x, double y,
                                  int32_t tw_early, int32_t tw_late,
                                  int32_t service_seconds, double demand) {
    uint32_t req;
    uint32_t task;

    if (!ctx) {
        return UINT32_MAX;
    }

    req = sg_add_request(ctx);
    if (req == UINT32_MAX) return UINT32_MAX;

    task = sg_add_task(ctx, SG_TASK_DELIVERY);
    if (task == UINT32_MAX) return UINT32_MAX;

    if (sg_task_set_location(ctx, task, x, y) != SG_STATUS_OK ||
        sg_task_set_time_window(ctx, task, tw_early, tw_late) != SG_STATUS_OK ||
        sg_task_set_service_seconds(ctx, task, service_seconds) != SG_STATUS_OK ||
        sg_task_set_demand(ctx, task, &demand, 1) != SG_STATUS_OK ||
        sg_request_bind_delivery_task(ctx, req, task) != SG_STATUS_OK) {
        return UINT32_MAX;
    }

    return req;
}

uint32_t sg_add_pd_request(SGContext *ctx,
                            double px, double py, int32_t p_early, int32_t p_late,
                            int32_t p_svc,
                            double dx, double dy, int32_t d_early, int32_t d_late,
                            int32_t d_svc, double demand) {
    uint32_t req;
    uint32_t p_task;
    uint32_t d_task;
    double neg_demand;

    if (!ctx) {
        return UINT32_MAX;
    }

    neg_demand = -demand;
    req = sg_add_request(ctx);
    if (req == UINT32_MAX) return UINT32_MAX;

    p_task = sg_add_task(ctx, SG_TASK_PICKUP);
    if (p_task == UINT32_MAX) return UINT32_MAX;

    d_task = sg_add_task(ctx, SG_TASK_DELIVERY);
    if (d_task == UINT32_MAX) return UINT32_MAX;

    if (sg_task_set_location(ctx, p_task, px, py) != SG_STATUS_OK ||
        sg_task_set_time_window(ctx, p_task, p_early, p_late) != SG_STATUS_OK ||
        sg_task_set_service_seconds(ctx, p_task, p_svc) != SG_STATUS_OK ||
        sg_task_set_demand(ctx, p_task, &demand, 1) != SG_STATUS_OK ||
        sg_task_set_location(ctx, d_task, dx, dy) != SG_STATUS_OK ||
        sg_task_set_time_window(ctx, d_task, d_early, d_late) != SG_STATUS_OK ||
        sg_task_set_service_seconds(ctx, d_task, d_svc) != SG_STATUS_OK ||
        sg_task_set_demand(ctx, d_task, &neg_demand, 1) != SG_STATUS_OK ||
        sg_request_bind_pickup_delivery_tasks(ctx, req, p_task, d_task) != SG_STATUS_OK) {
        return UINT32_MAX;
    }

    return req;
}

static uint32_t sg_find_or_create_location(SGContext *ctx, double x, double y) {
    uint32_t i;
    uint32_t loc;

    /* Search existing locations for exact (x,y) match */
    for (i = 0; i < ctx->num_locations; i++) {
        if (ctx->location_coords[i * 2] == x && ctx->location_coords[i * 2 + 1] == y) {
            return i;
        }
    }

    /* Create new location */
    loc = sg_add_location(ctx);
    if (loc != UINT32_MAX) {
        sg_location_set_coords(ctx, loc, x, y);
    }
    return loc;
}

SGStatus sg_prepare_travel(SGContext *ctx) {
    uint32_t i;

    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }
    if (atomic_load(&ctx->travel_prepared)) return SG_STATUS_OK;

    /* Step 1: Auto-assign location_ids to depots/tasks without one */
    for (i = 0; i < ctx->num_depots; i++) {
        SGDepotRecord *depot = &ctx->depots[i];
        if (depot->location_id == UINT32_MAX && depot->has_location) {
            depot->location_id = sg_find_or_create_location(ctx, depot->x, depot->y);
            if (depot->location_id == UINT32_MAX) {
                return SG_STATUS_OUT_OF_MEMORY;
            }
        }
    }

    for (i = 0; i < ctx->num_tasks; i++) {
        SGTaskRecord *task = &ctx->tasks[i];
        if (task->location_id == UINT32_MAX && task->has_location) {
            task->location_id = sg_find_or_create_location(ctx, task->x, task->y);
            if (task->location_id == UINT32_MAX) {
                return SG_STATUS_OUT_OF_MEMORY;
            }
        }
    }

    /* Step 2: Cache start/end location_ids on vehicles */
    for (i = 0; i < ctx->num_vehicles; i++) {
        SGVehicleRecord *vehicle = &ctx->vehicles[i];
        if (vehicle->has_depots) {
            if (vehicle->start_depot_id < ctx->num_depots) {
                vehicle->start_location_id = ctx->depots[vehicle->start_depot_id].location_id;
            }
            if (vehicle->end_depot_id < ctx->num_depots) {
                vehicle->end_location_id = ctx->depots[vehicle->end_depot_id].location_id;
            }
        }
    }

    /* Step 3: If no matrix and no callback and no time brackets, compute Euclidean matrices */
    if (!ctx->travel_distance_matrix && !ctx->travel_duration_matrix
        && !ctx->travel_callback && !ctx->has_travel_time_brackets) {
        uint32_t n = ctx->num_locations;
        if (n > 0) {
            size_t total = (size_t)n * n;
            uint32_t r, c;

            ctx->travel_distance_matrix = (double *)malloc(total * sizeof(double));
            ctx->travel_duration_matrix = (double *)malloc(total * sizeof(double));
            if (!ctx->travel_distance_matrix || !ctx->travel_duration_matrix) {
                free(ctx->travel_distance_matrix);
                free(ctx->travel_duration_matrix);
                ctx->travel_distance_matrix = NULL;
                ctx->travel_duration_matrix = NULL;
                return SG_STATUS_OUT_OF_MEMORY;
            }

            for (r = 0; r < n; r++) {
                double rx = ctx->location_coords[r * 2];
                double ry = ctx->location_coords[r * 2 + 1];
                for (c = 0; c < n; c++) {
                    double cx_val = ctx->location_coords[c * 2];
                    double cy_val = ctx->location_coords[c * 2 + 1];
                    double dx = rx - cx_val;
                    double dy = ry - cy_val;
                    double dist = sqrt(dx * dx + dy * dy);
                    size_t idx = (size_t)r * n + c;
                    ctx->travel_distance_matrix[idx] = dist;
                    ctx->travel_duration_matrix[idx] = dist;
                }
            }
        }
    }

    /* Step 4: Validate */
    for (i = 0; i < ctx->num_depots; i++) {
        if (ctx->depots[i].has_location && ctx->depots[i].location_id >= ctx->num_locations) {
            return SG_STATUS_INFEASIBLE;
        }
    }
    for (i = 0; i < ctx->num_tasks; i++) {
        if (ctx->tasks[i].has_location && ctx->tasks[i].location_id >= ctx->num_locations) {
            return SG_STATUS_INFEASIBLE;
        }
    }
    for (i = 0; i < ctx->num_vehicles; i++) {
        if (ctx->vehicles[i].has_depots) {
            if (ctx->vehicles[i].start_location_id >= ctx->num_locations ||
                ctx->vehicles[i].end_location_id >= ctx->num_locations) {
                return SG_STATUS_INFEASIBLE;
            }
        }
    }

    atomic_store(&ctx->travel_prepared, 1);
    return SG_STATUS_OK;
}

SGStatus sg_request_set_unassigned_penalty(SGContext *ctx, uint32_t request_id, double penalty) {
    if (!ctx || request_id >= ctx->num_requests ||
        !isfinite(penalty) || penalty < 0.0) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->requests[request_id].unassigned_penalty = penalty;
    ctx->requests[request_id].has_unassigned_penalty = 1;
    return SG_STATUS_OK;
}

SGStatus sg_set_initial_routes(SGContext *ctx,
                                uint32_t num_routes,
                                const uint32_t *vehicle_ids,
                                const uint32_t *route_lengths,
                                const uint32_t *request_ids) {
    uint32_t total = 0;
    uint32_t i;
    if (!ctx) return SG_STATUS_INVALID_ARG;
    if (num_routes == 0) {
        free(ctx->initial_route_vehicle_ids);
        free(ctx->initial_route_request_ids);
        free(ctx->initial_route_lengths);
        ctx->initial_route_vehicle_ids = NULL;
        ctx->initial_route_request_ids = NULL;
        ctx->initial_route_lengths = NULL;
        ctx->num_initial_routes = 0;
        ctx->total_initial_requests = 0;
        return SG_STATUS_OK;
    }
    if (!vehicle_ids || !route_lengths || !request_ids) {
        return SG_STATUS_INVALID_ARG;
    }
    for (i = 0; i < num_routes; i++) {
        total += route_lengths[i];
    }

    {
        uint32_t *vids = (uint32_t *)malloc((size_t)num_routes * sizeof(uint32_t));
        uint32_t *rlens = (uint32_t *)malloc((size_t)num_routes * sizeof(uint32_t));
        uint32_t *rids = total > 0 ? (uint32_t *)malloc((size_t)total * sizeof(uint32_t)) : NULL;
        if (!vids || !rlens || (total > 0 && !rids)) {
            free(vids); free(rlens); free(rids);
            return SG_STATUS_OUT_OF_MEMORY;
        }
        memcpy(vids, vehicle_ids, (size_t)num_routes * sizeof(uint32_t));
        memcpy(rlens, route_lengths, (size_t)num_routes * sizeof(uint32_t));
        if (total > 0) {
            memcpy(rids, request_ids, (size_t)total * sizeof(uint32_t));
        }
        free(ctx->initial_route_vehicle_ids);
        free(ctx->initial_route_request_ids);
        free(ctx->initial_route_lengths);
        ctx->initial_route_vehicle_ids = vids;
        ctx->initial_route_request_ids = rids;
        ctx->initial_route_lengths = rlens;
        ctx->num_initial_routes = num_routes;
        ctx->total_initial_requests = total;
    }
    return SG_STATUS_OK;
}

SGStatus sg_set_progress_callback(SGContext *ctx, SGProgressCallback cb, void *user_data) {
    if (!ctx) return SG_STATUS_INVALID_ARG;
    ctx->progress_callback = cb;
    ctx->progress_callback_data = user_data;
    return SG_STATUS_OK;
}

SGStatus sg_cancel(SGContext *ctx) {
    if (!ctx) return SG_STATUS_INVALID_ARG;
    ctx->cancel_requested = 1;
    return SG_STATUS_OK;
}

double sg_get_total_cost(const SGContext *ctx) {
    if (!ctx) {
        return 0.0;
    }
    return ctx->stats.total_cost;
}

double sg_get_total_distance(const SGContext *ctx) {
    if (!ctx) {
        return 0.0;
    }
    return ctx->stats.total_distance;
}

uint32_t sg_get_unassigned(const SGContext *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->stats.unassigned;
}

uint32_t sg_get_used_vehicle_count(const SGContext *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->stats.vehicles_used;
}

uint32_t sg_get_request_count(const SGContext *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->num_requests;
}

void sg_get_stats(const SGContext *ctx, SGStats *stats) {
    if (!ctx || !stats) {
        return;
    }
    *stats = ctx->stats;
}

/* ---------- Solution route/stop export ---------- */

uint32_t sg_solution_get_route_count(const SGContext *ctx) {
    const SGRouteSolution *sol;
    uint32_t count;
    uint32_t v;

    if (!ctx || !ctx->final_solution) {
        return 0;
    }
    sol = ctx->final_solution;
    count = 0;
    for (v = 0; v < sol->num_vehicles; v++) {
        if (sol->route_stop_lengths[v] > 0) {
            count++;
        }
    }
    return count;
}

/* Map route_index (0..route_count-1) to vehicle_id (skip empty routes). */
static uint32_t sg_route_index_to_vehicle(const SGRouteSolution *sol, uint32_t route_index) {
    uint32_t count = 0;
    uint32_t v;

    for (v = 0; v < sol->num_vehicles; v++) {
        if (sol->route_stop_lengths[v] > 0) {
            if (count == route_index) {
                return v;
            }
            count++;
        }
    }
    return UINT32_MAX;
}

uint32_t sg_solution_get_route_vehicle_id(const SGContext *ctx, uint32_t route_index) {
    if (!ctx || !ctx->final_solution) {
        return UINT32_MAX;
    }
    return sg_route_index_to_vehicle(ctx->final_solution, route_index);
}

double sg_solution_get_route_distance(const SGContext *ctx, uint32_t route_index) {
    uint32_t vid;

    if (!ctx || !ctx->final_solution) {
        return 0.0;
    }
    vid = sg_route_index_to_vehicle(ctx->final_solution, route_index);
    if (vid == UINT32_MAX) {
        return 0.0;
    }
    return ctx->final_solution->route_distance[vid];
}

uint32_t sg_solution_get_route_trip_count(const SGContext *ctx, uint32_t route_index) {
    uint32_t vid;

    if (!ctx || !ctx->final_solution) {
        return 0;
    }
    vid = sg_route_index_to_vehicle(ctx->final_solution, route_index);
    if (vid == UINT32_MAX) {
        return 0;
    }
    if (ctx->final_solution->route_trip_count) {
        return ctx->final_solution->route_trip_count[vid];
    }
    return ctx->final_solution->route_stop_lengths[vid] > 0 ? 1 : 0;
}

uint32_t sg_solution_get_route_stop_count(const SGContext *ctx, uint32_t route_index) {
    uint32_t vid;

    if (!ctx || !ctx->final_solution) {
        return 0;
    }
    vid = sg_route_index_to_vehicle(ctx->final_solution, route_index);
    if (vid == UINT32_MAX) {
        return 0;
    }
    return ctx->final_solution->route_stop_lengths[vid];
}

SGStatus sg_solution_get_route_stop(const SGContext *ctx, uint32_t route_index,
                                     uint32_t stop_index, SGSolutionStop *stop_out) {
    uint32_t vid;
    const SGRouteStop *stops;
    const SGRouteStop *s;

    if (!ctx || !ctx->final_solution || !stop_out) {
        return SG_STATUS_INVALID_ARG;
    }
    vid = sg_route_index_to_vehicle(ctx->final_solution, route_index);
    if (vid == UINT32_MAX) {
        return SG_STATUS_INVALID_ARG;
    }
    if (stop_index >= ctx->final_solution->route_stop_lengths[vid]) {
        return SG_STATUS_INVALID_ARG;
    }
    stops = sg_route_vehicle_stop_ptr_const(ctx->final_solution, vid);
    s = &stops[stop_index];
    stop_out->request_id = s->request_id;
    stop_out->task_id = s->task_id;
    if (s->task_id < ctx->num_tasks && ctx->tasks[s->task_id].type == SG_TASK_SERVICE) {
        stop_out->stop_type = SG_STOP_TYPE_SERVICE;
    } else {
        stop_out->stop_type = s->is_pickup ? SG_STOP_TYPE_PICKUP : SG_STOP_TYPE_DELIVERY;
    }
    stop_out->arrival = s->arrival;
    stop_out->service_start = s->service_start;
    stop_out->departure = s->depart;
    /* Compute trip_index by counting trip_start flags up to this stop */
    {
        uint32_t trip_idx = 0;
        uint32_t si;
        for (si = 1; si <= stop_index; si++) {
            if (stops[si].trip_start) {
                trip_idx++;
            }
        }
        stop_out->trip_index = trip_idx;
    }
    return SG_STATUS_OK;
}

uint32_t sg_solution_get_unassigned_request(const SGContext *ctx, uint32_t index) {
    const SGRouteSolution *sol;

    if (!ctx || !ctx->final_solution) {
        return UINT32_MAX;
    }
    sol = ctx->final_solution;
    if (index >= sol->base.num_unassigned) {
        return UINT32_MAX;
    }
    return sol->base.unassigned_ids[index];
}

double sg_solution_get_route_duration(const SGContext *ctx, uint32_t route_index) {
    uint32_t vid;

    if (!ctx || !ctx->final_solution || !ctx->final_solution->route_duration) {
        return 0.0;
    }
    vid = sg_route_index_to_vehicle(ctx->final_solution, route_index);
    if (vid == UINT32_MAX) {
        return 0.0;
    }
    return ctx->final_solution->route_duration[vid];
}

double sg_solution_get_route_waiting(const SGContext *ctx, uint32_t route_index) {
    uint32_t vid;

    if (!ctx || !ctx->final_solution || !ctx->final_solution->route_waiting) {
        return 0.0;
    }
    vid = sg_route_index_to_vehicle(ctx->final_solution, route_index);
    if (vid == UINT32_MAX) {
        return 0.0;
    }
    return ctx->final_solution->route_waiting[vid];
}

double sg_solution_get_route_overtime(const SGContext *ctx, uint32_t route_index) {
    uint32_t vid;

    if (!ctx || !ctx->final_solution || !ctx->final_solution->route_overtime) {
        return 0.0;
    }
    vid = sg_route_index_to_vehicle(ctx->final_solution, route_index);
    if (vid == UINT32_MAX) {
        return 0.0;
    }
    return ctx->final_solution->route_overtime[vid];
}

double sg_solution_get_route_tw_penalty(const SGContext *ctx, uint32_t route_index) {
    uint32_t vid;

    if (!ctx || !ctx->final_solution || !ctx->final_solution->route_tw_penalty) {
        return 0.0;
    }
    vid = sg_route_index_to_vehicle(ctx->final_solution, route_index);
    if (vid == UINT32_MAX) {
        return 0.0;
    }
    return ctx->final_solution->route_tw_penalty[vid];
}

SGStatus sg_solution_get_route_stop_load(const SGContext *ctx, uint32_t route_index,
                                          uint32_t stop_index, uint32_t dimension,
                                          double *load_out) {
    uint32_t vid;
    const SGRouteSolution *sol;
    size_t idx;

    if (!ctx || !ctx->final_solution || !load_out) {
        return SG_STATUS_INVALID_ARG;
    }
    sol = ctx->final_solution;
    if (!sol->route_stop_load || ctx->dimension_count == 0) {
        return SG_STATUS_INVALID_ARG;
    }
    if (dimension >= ctx->dimension_count) {
        return SG_STATUS_INVALID_ARG;
    }
    vid = sg_route_index_to_vehicle(sol, route_index);
    if (vid == UINT32_MAX) {
        return SG_STATUS_INVALID_ARG;
    }
    if (stop_index >= sol->route_stop_lengths[vid]) {
        return SG_STATUS_INVALID_ARG;
    }
    /* Load array layout: [vehicle * (stop_stride+1) * dim_count + stop * dim_count + d]
       Internal storage uses prefix-sum convention (starting from 0).
       Export adds initial_load offset for the first trip to show actual vehicle load. */
    idx = (size_t)vid * ((size_t)sol->stop_stride + 1U) * (size_t)ctx->dimension_count +
          (size_t)stop_index * (size_t)ctx->dimension_count + (size_t)dimension;
    {
        double prefix = sol->route_stop_load[idx];
        const SGVehicleRecord *vehicle = &ctx->vehicles[vid];
        /* Determine if this stop is on the first trip (before any trip_start boundary) */
        uint8_t first_trip = 1;
        if (stop_index > 0) {
            const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(sol, vid);
            uint32_t s;
            for (s = 1; s <= stop_index; s++) {
                if (stops[s].trip_start) { first_trip = 0; break; }
            }
        }
        if (first_trip && vehicle->has_initial_load && vehicle->initial_load) {
            prefix += vehicle->initial_load[dimension];
        }
        *load_out = prefix;
    }
    return SG_STATUS_OK;
}

double sg_solution_get_route_break_time(const SGContext *ctx, uint32_t route_index) {
    uint32_t vid;

    if (!ctx || !ctx->final_solution || !ctx->final_solution->route_break_time) {
        return 0.0;
    }
    vid = sg_route_index_to_vehicle(ctx->final_solution, route_index);
    if (vid == UINT32_MAX) {
        return 0.0;
    }
    return ctx->final_solution->route_break_time[vid];
}

uint32_t sg_solution_get_route_break_count(const SGContext *ctx, uint32_t route_index) {
    uint32_t vid;

    if (!ctx || !ctx->final_solution || !ctx->final_solution->route_break_count) {
        return 0;
    }
    vid = sg_route_index_to_vehicle(ctx->final_solution, route_index);
    if (vid == UINT32_MAX) {
        return 0;
    }
    return ctx->final_solution->route_break_count[vid];
}

SGStatus sg_solution_get_route_break(const SGContext *ctx, uint32_t route_index,
                                      uint32_t break_index,
                                      uint32_t *after_stop_index_out,
                                      double *start_out, double *duration_out) {
    uint32_t vid;
    const SGRouteSolution *sol;
    const SGRouteBreak *brk;

    if (!ctx || !ctx->final_solution) {
        return SG_STATUS_INVALID_ARG;
    }
    sol = ctx->final_solution;
    if (!sol->route_breaks || !sol->route_break_count) {
        return SG_STATUS_INVALID_ARG;
    }
    vid = sg_route_index_to_vehicle(sol, route_index);
    if (vid == UINT32_MAX) {
        return SG_STATUS_INVALID_ARG;
    }
    if (break_index >= sol->route_break_count[vid]) {
        return SG_STATUS_INVALID_ARG;
    }
    brk = &sol->route_breaks[(size_t)vid * sol->break_stride + break_index];
    if (after_stop_index_out) *after_stop_index_out = brk->after_stop_index;
    if (start_out) *start_out = brk->start_time;
    if (duration_out) *duration_out = brk->duration;
    return SG_STATUS_OK;
}

double sg_solution_get_route_total_work(const SGContext *ctx, uint32_t route_index) {
    uint32_t vid;

    if (!ctx || !ctx->final_solution || !ctx->final_solution->route_total_work) {
        return 0.0;
    }
    vid = sg_route_index_to_vehicle(ctx->final_solution, route_index);
    if (vid == UINT32_MAX) {
        return 0.0;
    }
    return ctx->final_solution->route_total_work[vid];
}

/* ============================================================================
 * Plan Validation
 * ============================================================================ */

SGStatus sg_validate_plan(SGContext *ctx, uint32_t num_routes, const SGPlanRoute *routes) {
    SGStatus status;

    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }

    sg_clear_error(ctx);

    status = sg_validate_model(ctx);
    if (status != SG_STATUS_OK) {
        return status;
    }

    return sg_validate_plan_impl(ctx, num_routes, routes);
}

uint32_t sg_get_violation_count(const SGContext *ctx) {
    if (!ctx) return 0;
    return ctx->num_violations;
}

SGStatus sg_get_violation(const SGContext *ctx, uint32_t index, SGViolation *out) {
    if (!ctx || !out || index >= ctx->num_violations) {
        return SG_STATUS_INVALID_ARG;
    }
    *out = ctx->violations[index];
    return SG_STATUS_OK;
}
