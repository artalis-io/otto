#ifndef SURGE_H
#define SURGE_H

#include <stdint.h>

#include "sg_types.h"

typedef struct SGContext SGContext;

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
SGStatus sg_validate_model(const SGContext *ctx);
SGStatus sg_load_solomon_vrptw(SGContext *ctx, const char *file_path);
SGStatus sg_load_li_lim_pdptw(SGContext *ctx, const char *file_path);

SGStatus sg_solve(SGContext *ctx);

double sg_get_total_cost(const SGContext *ctx);
double sg_get_total_distance(const SGContext *ctx);
uint32_t sg_get_unassigned(const SGContext *ctx);
uint32_t sg_get_used_vehicle_count(const SGContext *ctx);
uint32_t sg_get_request_count(const SGContext *ctx);
void sg_get_stats(const SGContext *ctx, SGStats *stats);

#endif /* SURGE_H */
