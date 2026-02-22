#ifndef SURGE_H
#define SURGE_H

#include <stdint.h>

#include "sg_types.h"

typedef struct SGContext SGContext;

typedef void (*SGTravelCallback)(uint32_t from_location, uint32_t to_location,
                                  uint32_t vehicle_id,
                                  double *distance_out, double *duration_out,
                                  void *user_data);

const char *sg_version(void);

SGContext *sg_create(void);
void sg_free(SGContext *ctx);

void sg_config_default(SGConfig *config);
SGStatus sg_set_config(SGContext *ctx, const SGConfig *config);
SGStatus sg_set_require_bound_requests_at_solve(SGContext *ctx, bool require_bound);
bool sg_get_require_bound_requests_at_solve(const SGContext *ctx);
SGStatus sg_set_demand_sign_convention(SGContext *ctx, SGDemandSignConvention convention);
SGDemandSignConvention sg_get_demand_sign_convention(const SGContext *ctx);
SGStatus sg_set_dimension_count(SGContext *ctx, uint32_t dimension_count);
uint32_t sg_get_dimension_count(const SGContext *ctx);

uint32_t sg_add_depot(SGContext *ctx);
SGStatus sg_depot_set_location(SGContext *ctx, uint32_t depot_id, double x, double y);
SGStatus sg_depot_set_time_window(SGContext *ctx, uint32_t depot_id, int32_t early,
                                  int32_t late);

/* Depot dock capacity */
SGStatus sg_depot_set_max_simultaneous(SGContext *ctx, uint32_t depot_id,
                                        uint32_t max_simultaneous);

uint32_t sg_add_request(SGContext *ctx);
uint32_t sg_add_vehicle(SGContext *ctx);
uint32_t sg_add_task(SGContext *ctx, SGTaskType type);

SGStatus sg_vehicle_set_depots(SGContext *ctx, uint32_t vehicle_id, uint32_t start_depot_id,
                               uint32_t end_depot_id);
SGStatus sg_vehicle_set_shift_time_window(SGContext *ctx, uint32_t vehicle_id, int32_t early,
                                          int32_t late);
SGStatus sg_vehicle_set_capacity(SGContext *ctx, uint32_t vehicle_id, const double *capacity,
                                 uint32_t capacity_count);

SGStatus sg_task_set_location(SGContext *ctx, uint32_t task_id, double x, double y);
SGStatus sg_task_set_time_window(SGContext *ctx, uint32_t task_id, int32_t early, int32_t late);
SGStatus sg_task_set_service_seconds(SGContext *ctx, uint32_t task_id, int32_t service_seconds);
SGStatus sg_task_set_demand(SGContext *ctx, uint32_t task_id, const double *demand,
                            uint32_t demand_count);

SGStatus sg_request_bind_delivery_task(SGContext *ctx, uint32_t request_id,
                                       uint32_t delivery_task_id);
SGStatus sg_request_bind_pickup_delivery_tasks(SGContext *ctx, uint32_t request_id,
                                               uint32_t pickup_task_id,
                                               uint32_t delivery_task_id);
SGStatus sg_request_set_priority_hint(SGContext *ctx, uint32_t request_id, int32_t priority);
SGStatus sg_request_set_time_window_hint(SGContext *ctx, uint32_t request_id,
                                         int32_t early, int32_t late);
SGStatus sg_request_set_zone_hint(SGContext *ctx, uint32_t request_id, uint32_t zone_id);
SGStatus sg_set_priority_removal_policy(SGContext *ctx, SGPriorityRemovalPolicy policy);
SGStatus sg_set_zone_distance_matrix(SGContext *ctx, uint32_t zone_count,
                                     const double *matrix_row_major);
SGStatus sg_clear_zone_distance_matrix(SGContext *ctx);
uint32_t sg_add_location(SGContext *ctx);
SGStatus sg_location_set_coords(SGContext *ctx, uint32_t location_id, double x, double y);
SGStatus sg_depot_set_location_id(SGContext *ctx, uint32_t depot_id, uint32_t location_id);
SGStatus sg_task_set_location_id(SGContext *ctx, uint32_t task_id, uint32_t location_id);
SGStatus sg_set_travel_matrix(SGContext *ctx, uint32_t location_count,
                               const double *distance_matrix_row_major,
                               const double *duration_matrix_row_major);
SGStatus sg_set_travel_callback(SGContext *ctx, SGTravelCallback callback, void *user_data);

SGStatus sg_vehicle_set_qualifications(SGContext *ctx, uint32_t vehicle_id,
                                        uint64_t qualification_flags);
SGStatus sg_request_set_required_qualifications(SGContext *ctx, uint32_t request_id,
                                                 uint64_t qualification_flags);

/* U8: Request-vehicle constraints */
SGStatus sg_request_add_allowed_vehicle(SGContext *ctx, uint32_t request_id, uint32_t vehicle_id);
SGStatus sg_request_add_forbidden_vehicle(SGContext *ctx, uint32_t request_id, uint32_t vehicle_id);

/* Commodity conflicts */
SGStatus sg_add_commodity(SGContext *ctx, uint32_t *commodity_id_out);
SGStatus sg_commodity_set_conflict(SGContext *ctx, uint32_t commodity_a, uint32_t commodity_b);
SGStatus sg_request_set_commodity(SGContext *ctx, uint32_t request_id, uint32_t commodity_id);

/* Exclusion groups */
SGStatus sg_add_exclusion_group(SGContext *ctx, uint32_t *group_id_out);
SGStatus sg_request_add_exclusion_group(SGContext *ctx, uint32_t request_id, uint32_t group_id);

/* Sequence-dependent setup times */
SGStatus sg_set_num_setup_classes(SGContext *ctx, uint32_t count);
SGStatus sg_set_setup_time(SGContext *ctx, uint32_t from_class, uint32_t to_class, double seconds);
SGStatus sg_request_set_setup_class(SGContext *ctx, uint32_t request_id, uint32_t class_id);

/* U4: Open routes */
SGStatus sg_vehicle_set_open_end(SGContext *ctx, uint32_t vehicle_id, int open);

/* U5: Max duration and explicit ride time */
SGStatus sg_vehicle_set_max_duration(SGContext *ctx, uint32_t vehicle_id,
                                      int32_t max_seconds);
SGStatus sg_request_set_max_ride_time(SGContext *ctx, uint32_t request_id,
                                       int32_t max_seconds);

/* U6: Vehicle cost model */
SGStatus sg_vehicle_set_costs(SGContext *ctx, uint32_t vehicle_id,
                               double fixed_cost, double cost_per_distance,
                               double cost_per_duration);
SGStatus sg_vehicle_set_waiting_cost(SGContext *ctx, uint32_t vehicle_id, double cost_per_waiting);
SGStatus sg_vehicle_set_overtime_cost(SGContext *ctx, uint32_t vehicle_id, double cost_per_overtime);

/* Vehicle depot service times */
SGStatus sg_vehicle_set_depot_loading_seconds(SGContext *ctx, uint32_t vehicle_id,
                                               int32_t seconds);
SGStatus sg_vehicle_set_depot_unloading_seconds(SGContext *ctx, uint32_t vehicle_id,
                                                 int32_t seconds);

/* Break policy */
SGStatus sg_vehicle_set_break_policy(SGContext *ctx, uint32_t vehicle_id,
                                      int32_t max_work_seconds,
                                      int32_t break_duration_seconds);
SGStatus sg_vehicle_set_max_total_work(SGContext *ctx, uint32_t vehicle_id,
                                        int32_t max_total_work_seconds);

SGStatus sg_set_unassigned_weight(SGContext *ctx, double weight);

/* Per-request drop penalty */
SGStatus sg_request_set_unassigned_penalty(SGContext *ctx, uint32_t request_id, double penalty);

/* U7: Soft time windows */
SGStatus sg_task_set_soft_time_window(SGContext *ctx, uint32_t task_id,
                                      int32_t early, int32_t late,
                                      double early_penalty, double late_penalty);

/* Disjunct time windows */
SGStatus sg_task_add_time_window(SGContext *ctx, uint32_t task_id,
                                 int32_t early, int32_t late);

/* Convenience constructors (single-dim demand) */
uint32_t sg_add_delivery_request(SGContext *ctx, double x, double y,
                                  int32_t tw_early, int32_t tw_late,
                                  int32_t service_seconds, double demand);
uint32_t sg_add_pd_request(SGContext *ctx,
                            double px, double py, int32_t p_early, int32_t p_late,
                            int32_t p_svc,
                            double dx, double dy, int32_t d_early, int32_t d_late,
                            int32_t d_svc, double demand);

/* Warm start / initial solution */
SGStatus sg_set_initial_routes(SGContext *ctx,
                                uint32_t num_routes,
                                const uint32_t *vehicle_ids,
                                const uint32_t *route_lengths,
                                const uint32_t *request_ids);

/* Progress callback + cancel */
typedef int (*SGProgressCallback)(const SGStats *stats, void *user_data);
SGStatus sg_set_progress_callback(SGContext *ctx, SGProgressCallback cb, void *user_data);
SGStatus sg_cancel(SGContext *ctx);

/* Error diagnostics */
const char *sg_get_last_error(const SGContext *ctx);

SGStatus sg_validate_model(SGContext *ctx);
SGStatus sg_load_solomon_vrptw(SGContext *ctx, const char *file_path);
SGStatus sg_load_li_lim_pdptw(SGContext *ctx, const char *file_path);
SGStatus sg_load_cordeau_darp(SGContext *ctx, const char *file_path);

SGStatus sg_solve(SGContext *ctx);

double sg_get_total_cost(const SGContext *ctx);
double sg_get_total_distance(const SGContext *ctx);
uint32_t sg_get_unassigned(const SGContext *ctx);
uint32_t sg_get_used_vehicle_count(const SGContext *ctx);
uint32_t sg_get_request_count(const SGContext *ctx);
void sg_get_stats(const SGContext *ctx, SGStats *stats);

/* Solution route/stop export (available after sg_solve returns SG_STATUS_OK) */
uint32_t sg_solution_get_route_count(const SGContext *ctx);
uint32_t sg_solution_get_route_vehicle_id(const SGContext *ctx, uint32_t route_index);
double sg_solution_get_route_distance(const SGContext *ctx, uint32_t route_index);
uint32_t sg_solution_get_route_stop_count(const SGContext *ctx, uint32_t route_index);
SGStatus sg_solution_get_route_stop(const SGContext *ctx, uint32_t route_index,
                                     uint32_t stop_index, SGSolutionStop *stop_out);
double sg_solution_get_route_duration(const SGContext *ctx, uint32_t route_index);
double sg_solution_get_route_waiting(const SGContext *ctx, uint32_t route_index);
double sg_solution_get_route_overtime(const SGContext *ctx, uint32_t route_index);
double sg_solution_get_route_tw_penalty(const SGContext *ctx, uint32_t route_index);
SGStatus sg_solution_get_route_stop_load(const SGContext *ctx, uint32_t route_index,
                                          uint32_t stop_index, uint32_t dimension,
                                          double *load_out);
uint32_t sg_solution_get_unassigned_request(const SGContext *ctx, uint32_t index);

/* Break solution export */
double sg_solution_get_route_break_time(const SGContext *ctx, uint32_t route_index);
uint32_t sg_solution_get_route_break_count(const SGContext *ctx, uint32_t route_index);
SGStatus sg_solution_get_route_break(const SGContext *ctx, uint32_t route_index,
                                      uint32_t break_index,
                                      uint32_t *after_stop_index_out,
                                      double *start_out, double *duration_out);
double sg_solution_get_route_total_work(const SGContext *ctx, uint32_t route_index);

/* Per-operator telemetry */
typedef struct {
    char name[32];
    double weight;
    int64_t selected;
    int64_t accepted;
    int64_t improvements;
    double total_seconds;
} SGOperatorStats;

uint32_t sg_get_destroy_operator_count(const SGContext *ctx);
uint32_t sg_get_repair_operator_count(const SGContext *ctx);
SGStatus sg_get_destroy_operator_stats(const SGContext *ctx, uint32_t index, SGOperatorStats *out);
SGStatus sg_get_repair_operator_stats(const SGContext *ctx, uint32_t index, SGOperatorStats *out);

#endif /* SURGE_H */
