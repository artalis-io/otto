#ifndef SURGE_SG_INTERNAL_H
#define SURGE_SG_INTERNAL_H

#include "surge.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arbor.h"
#include "sh_dist.h"

/* Constants */
#define SG_UNASSIGNED_PENALTY 10000.0
#define SG_WORST_RANDOMNESS 4.0
#define SG_SHAW_RANDOMNESS 4.0
#define SG_ROUTE_CLUSTER_RANDOMNESS 2.5
#define SG_TIME_CLUSTER_RANDOMNESS 2.0
#define SG_PD_SHAW_RANDOMNESS 3.0
#define SG_ROUTE_SHAW_RANDOMNESS 4.0
#define SG_OPERATOR_SEED_XOR 0x9E3779B97F4A7C15ULL
#define SG_DEMAND_TOLERANCE 1e-9
#define SG_NOISE_REGRET_SCALE 0.1
#define SG_CONSTRUCT_REGRET_K 3
#define SG_CONSTRUCT_DISTANCE_SECONDS 1800.0
#define SG_CONSTRUCT_FALLBACK_SECONDS 600.0
#define SG_ROUTE_OBJECTIVE_VEHICLE_WEIGHT 1000000.0
#define SG_ROUTE_OBJECTIVE_UNASSIGNED_WEIGHT 1000000000.0
#define SG_ROUTE_MAX_REGRET_K 4
#define SG_ROUTE_MAX_INTENSIFY_PASSES 8

/* Internal types */
typedef struct {
    uint32_t total_requests;
    uint32_t num_assigned;
    uint32_t num_unassigned;

    uint32_t *assigned_ids;
    uint32_t *unassigned_ids;
    uint8_t *assigned_flags;
} SGBootstrapSolution;

typedef struct {
    uint32_t request_id;
    uint32_t task_id;
    uint8_t is_pickup;
    /* Cached timing (populated by sg_route_update_timing) */
    double arrival;        /* time cursor on arrival (after travel from prev) */
    double service_start;  /* max(arrival, tw_early) */
    double depart;         /* service_start + service_seconds */
    double latest_start;   /* latest feasible service start (backward pass) */
    double forward_slack;  /* latest_start - service_start */
} SGRouteStop;

typedef struct {
    SGBootstrapSolution base;

    uint32_t num_vehicles;
    uint32_t route_stride;
    uint32_t stop_stride;
    uint32_t vehicles_used;
    double total_distance;

    uint32_t *route_lengths;
    uint32_t *route_requests;
    uint32_t *route_stop_lengths;
    SGRouteStop *route_stops;
    uint32_t *route_stop_prev;
    uint32_t *route_stop_next;
    uint32_t *request_vehicle;
    uint32_t *request_pos;
    uint32_t *request_pickup_stop_pos;
    uint32_t *request_delivery_stop_pos;
    double *route_distance;
    double *route_stop_load;   /* [vehicle * stop_stride * dim_count + stop * dim_count + d] */
} SGRouteSolution;

typedef struct {
    double *remaining_capacity;
    double *remaining_time_seconds;
} SGConstructState;

typedef struct {
    int32_t priority;
    uint32_t zone_id;
    int32_t tw_early;
    int32_t tw_late;
    uint8_t has_zone;
    uint8_t has_time_window;
} SGRequestHint;

typedef struct {
    double x;
    double y;
    int32_t tw_early;
    int32_t tw_late;
    uint8_t has_location;
    uint8_t has_time_window;
} SGDepotRecord;

typedef struct {
    uint32_t start_depot_id;
    uint32_t end_depot_id;
    int32_t shift_early;
    int32_t shift_late;
    double *capacity;
    uint8_t has_depots;
    uint8_t has_shift_time_window;
    uint8_t has_capacity;
} SGVehicleRecord;

typedef struct {
    SGTaskType type;
    double x;
    double y;
    int32_t tw_early;
    int32_t tw_late;
    int32_t service_seconds;
    double *demand;
    uint8_t has_location;
    uint8_t has_time_window;
    uint8_t has_demand;
} SGTaskRecord;

typedef struct {
    SGRequestKind kind;
    uint32_t pickup_task_id;
    uint32_t delivery_task_id;
    uint8_t has_pickup_task;
    uint8_t has_delivery_task;
} SGRequestRecord;

struct SGContext {
    SGConfig config;
    SGDemandSignConvention demand_sign_convention;
    uint32_t dimension_count;
    uint32_t num_depots;
    uint32_t num_requests;
    uint32_t num_vehicles;
    uint32_t num_tasks;
    uint32_t zone_count;
    SGStats stats;

    SGDepotRecord *depots;
    SGRequestRecord *requests;
    SGRequestHint *request_hints;
    SGVehicleRecord *vehicles;
    SGTaskRecord *tasks;
    double *zone_distance_matrix;
    SHRng *op_rng;
    void *active_solution;  /* Temporary: set during destroy ops needing route access */
};

/* sg_context.c */
SGRequestHint sg_request_hint_default(void);
SGRequestRecord sg_request_record_default(void);
int sg_task_type_valid(SGTaskType type);
void sg_vehicle_records_free(SGVehicleRecord *vehicles, uint32_t count);
void sg_task_records_free(SGTaskRecord *tasks, uint32_t count);
int sg_config_valid(const SGConfig *config);
int sg_priority_policy_valid(SGPriorityRemovalPolicy policy);
int sg_demand_sign_convention_valid(SGDemandSignConvention convention);
void sg_zone_matrix_clear(SGContext *ctx);
int sg_task_ready_for_model(const SGTaskRecord *task);
int sg_request_pd_demands_valid(const SGTaskRecord *pickup, const SGTaskRecord *delivery,
                                uint32_t dimension_count, SGDemandSignConvention convention);
int sg_delivery_task_demand_valid(const SGTaskRecord *delivery, uint32_t dimension_count,
                                  SGDemandSignConvention convention);
uint32_t sg_count_unbound_requests(const SGContext *ctx);

/* sg_solution.c */
void sg_bootstrap_solution_reset(SGBootstrapSolution *sol);
ARStatus sg_bootstrap_solution_init(SGBootstrapSolution *sol, uint32_t total_requests);
int sg_find_id(const uint32_t *ids, uint32_t count, uint32_t id);
ARStatus sg_bootstrap_assign_request(SGBootstrapSolution *sol, uint32_t id);
ARStatus sg_bootstrap_unassign_request(SGBootstrapSolution *sol, uint32_t id);
void *sg_bootstrap_copy(const void *solution, void *user_ctx);
void sg_bootstrap_free(void *solution, void *user_ctx);
double sg_bootstrap_cost(const void *solution, void *user_ctx);
int sg_bootstrap_size(const void *solution, void *user_ctx);
int sg_bootstrap_validate(const void *solution, void *user_ctx);
int sg_get_assigned_count(void *solution, void *user_ctx);
uint32_t sg_get_assigned_element(void *solution, void *user_ctx, int index);
void sg_route_solution_reset(SGRouteSolution *sol);
ARStatus sg_route_solution_init(const SGContext *ctx, SGRouteSolution *sol);
void *sg_route_solution_copy(const void *solution, void *user_ctx);
void sg_route_solution_free(void *solution, void *user_ctx);
int sg_route_solution_validate(const void *solution, void *user_ctx);
double sg_route_solution_cost(const void *solution, void *user_ctx);
int sg_route_solution_size(const void *solution, void *user_ctx);
uint32_t *sg_route_vehicle_ptr(SGRouteSolution *sol, uint32_t vehicle_id);
const uint32_t *sg_route_vehicle_ptr_const(const SGRouteSolution *sol, uint32_t vehicle_id);
SGRouteStop *sg_route_vehicle_stop_ptr(SGRouteSolution *sol, uint32_t vehicle_id);
const SGRouteStop *sg_route_vehicle_stop_ptr_const(const SGRouteSolution *sol,
                                                    uint32_t vehicle_id);
uint32_t *sg_route_vehicle_stop_prev_ptr(SGRouteSolution *sol, uint32_t vehicle_id);
uint32_t *sg_route_vehicle_stop_next_ptr(SGRouteSolution *sol, uint32_t vehicle_id);
const uint32_t *sg_route_vehicle_stop_prev_ptr_const(const SGRouteSolution *sol,
                                                      uint32_t vehicle_id);
const uint32_t *sg_route_vehicle_stop_next_ptr_const(const SGRouteSolution *sol,
                                                      uint32_t vehicle_id);
int sg_request_emit_stops(const SGContext *ctx, uint32_t request_id,
                          SGRouteStop *stops_out, uint32_t *stop_count_out);
int sg_route_rebuild_vehicle_stop_state(const SGContext *ctx, SGRouteSolution *sol,
                                        uint32_t vehicle_id);
int sg_route_splice_stop(const SGContext *ctx, SGRouteSolution *sol,
                         uint32_t vehicle_id, uint32_t at,
                         const SGRouteStop *stop);
int sg_route_excise_stop(const SGContext *ctx, SGRouteSolution *sol,
                         uint32_t vehicle_id, uint32_t at);
double sg_route_objective_cost(uint32_t unassigned, uint32_t vehicles_used,
                               double total_distance);

/* sg_cost.c */
double sg_euclid(double ax, double ay, double bx, double by);
int32_t sg_clamp_priority(int32_t priority);
int64_t sg_abs_i64(int64_t value);
const SGRequestHint *sg_get_hint(const SGContext *ctx, uint32_t request_id);
const SGRequestRecord *sg_get_request_record(const SGContext *ctx, uint32_t request_id);
const SGTaskRecord *sg_get_task_record(const SGContext *ctx, uint32_t task_id);
int sg_request_time_midpoint(const SGContext *ctx, uint32_t request_id, int64_t *midpoint);
int sg_request_tw_width(const SGContext *ctx, uint32_t request_id, int32_t *width_out);
int sg_request_time_window_bounds(const SGContext *ctx, uint32_t request_id,
                                  int32_t *early_out, int32_t *late_out);
int sg_request_centroid(const SGContext *ctx, uint32_t request_id, double *x, double *y);
double sg_request_load_magnitude(const SGContext *ctx, uint32_t request_id);
double sg_request_priority_score(const SGContext *ctx, uint32_t request_id);
SGRequestKind sg_request_kind(const SGContext *ctx, uint32_t request_id);
double sg_request_abs_demand_at_dim(const SGContext *ctx, uint32_t request_id, uint32_t dim);
int sg_vehicle_start_end_locations(const SGContext *ctx, uint32_t vehicle_id,
                                   double *sx, double *sy, double *ex, double *ey);
double sg_vehicle_request_cost(const SGContext *ctx, uint32_t vehicle_id,
                               uint32_t request_id, double noise_scale);
int sg_request_best_k_costs(const SGContext *ctx, uint32_t request_id, int k,
                            double noise_scale, double *best_cost, double *kth_cost);
int sg_zone_distance_lookup(const SGContext *ctx, uint32_t zone_a, uint32_t zone_b,
                            double *distance);
double sg_zone_similarity(const SGContext *ctx, uint32_t zone_a, uint32_t zone_b);
double sg_priority_removal_score(const SGContext *ctx, int32_t priority);
double sg_zone_density_score(const SGContext *ctx, const SGBootstrapSolution *sol,
                             uint32_t request_id);
double sg_bootstrap_removal_cost(void *ctx, void *solution, uint32_t element_id);
double sg_route_removal_cost(void *ctx, void *solution, uint32_t element_id);
double sg_bootstrap_relatedness(void *ctx, uint32_t a, uint32_t b);
double sg_route_cluster_relatedness(void *ctx, uint32_t a, uint32_t b);
double sg_time_cluster_relatedness(void *ctx, uint32_t a, uint32_t b);
double sg_time_window_relatedness(void *ctx, uint32_t a, uint32_t b);
double sg_pd_shaw_relatedness(void *ctx, uint32_t a, uint32_t b);
double sg_route_shaw_relatedness(void *ctx, uint32_t a, uint32_t b);
double sg_criticality_removal_cost(void *ctx, void *solution, uint32_t element_id);
int sg_request_time_use_for_vehicle(const SGContext *ctx, uint32_t vehicle_id,
                                    uint32_t request_id, double *time_use_seconds);
void sg_compute_solution_route_metrics(const SGContext *ctx, const SGBootstrapSolution *sol,
                                       uint32_t *vehicles_used_out, double *total_distance_out);

/* sg_feasibility.c */
int sg_route_update_timing(const SGContext *ctx, SGRouteSolution *sol, uint32_t vehicle_id);
int sg_route_update_load(const SGContext *ctx, SGRouteSolution *sol, uint32_t vehicle_id);
int sg_route_eval_insertion_cached(const SGContext *ctx, const SGRouteSolution *sol,
                                   uint32_t request_id, uint32_t vehicle_id,
                                   uint32_t pos, double *score_out,
                                   double *new_route_distance_out);
int sg_route_eval_pd_best_insertion_cached(
    const SGContext *ctx, const SGRouteSolution *sol,
    uint32_t request_id, uint32_t vehicle_id,
    double *best_score_out, uint32_t *best_pickup_pos_out,
    uint32_t *best_delivery_pos_out, double *best_route_distance_out);
int sg_route_stop_sequence_feasible(const SGContext *ctx, uint32_t vehicle_id,
                                    const SGRouteStop *stops, uint32_t stop_count,
                                    double *distance_out);
int sg_route_sequence_feasible_distance(const SGContext *ctx, uint32_t vehicle_id,
                                        const uint32_t *request_ids, uint32_t request_count,
                                        double *distance_out, double *capacity_scratch);
int sg_route_eval_insertion(const SGContext *ctx, const SGRouteSolution *sol,
                            uint32_t request_id, uint32_t vehicle_id, uint32_t pos,
                            uint32_t *candidate_route, double *capacity_scratch,
                            double *score_out, double *new_route_distance_out);
ARStatus sg_route_apply_insertion(const SGContext *ctx, SGRouteSolution *sol,
                                  uint32_t request_id, uint32_t vehicle_id,
                                  uint32_t pos, double new_route_distance);
ARStatus sg_route_apply_pd_insertion(const SGContext *ctx, SGRouteSolution *sol,
                                     uint32_t request_id, uint32_t vehicle_id,
                                     uint32_t pickup_stop_pos, uint32_t delivery_stop_pos,
                                     double new_route_distance);
ARStatus sg_route_unassign_request(const SGContext *ctx, SGRouteSolution *sol,
                                   uint32_t request_id, double *capacity_scratch);
ARStatus sg_route_unassign_removed_requests(const SGContext *ctx, SGRouteSolution *sol,
                                            const uint32_t *removed_ids, int removed_count);
void sg_route_insert_request(uint32_t *route, uint32_t *route_len, uint32_t insert_pos,
                             uint32_t request_id);

/* sg_construct.c */
ARStatus sg_construct_state_init(const SGContext *ctx, SGConstructState *state);
void sg_construct_state_reset(SGConstructState *state);
int sg_construct_select_regret_request(const SGContext *ctx, const SGConstructState *state,
                                       const SGBootstrapSolution *sol, int regret_k,
                                       uint32_t *request_id_out, uint32_t *vehicle_id_out);
ARStatus sg_construct_initial_solution(SGContext *ctx, SGBootstrapSolution *sol);

/* sg_destroy.c */
ARStatus sg_unassign_removed_requests(SGBootstrapSolution *sol,
                                      const uint32_t *removed_ids, int removed_count);
ARStatus sg_destroy_random(void *op_ctx, void *solution, int count,
                           uint32_t *removed_ids, int *removed_count);
ARStatus sg_destroy_worst(void *op_ctx, void *solution, int count,
                          uint32_t *removed_ids, int *removed_count);
ARStatus sg_destroy_shaw(void *op_ctx, void *solution, int count,
                         uint32_t *removed_ids, int *removed_count);
ARStatus sg_destroy_criticality_worst(void *op_ctx, void *solution, int count,
                                      uint32_t *removed_ids, int *removed_count);
ARStatus sg_destroy_route_cluster(void *op_ctx, void *solution, int count,
                                  uint32_t *removed_ids, int *removed_count);
ARStatus sg_destroy_time_cluster(void *op_ctx, void *solution, int count,
                                 uint32_t *removed_ids, int *removed_count);
ARStatus sg_destroy_paired_shaw(void *op_ctx, void *solution, int count,
                                uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_random(void *op_ctx, void *solution, int count,
                                 uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_worst(void *op_ctx, void *solution, int count,
                                uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_shaw(void *op_ctx, void *solution, int count,
                               uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_criticality_worst(void *op_ctx, void *solution, int count,
                                            uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_route_cluster(void *op_ctx, void *solution, int count,
                                        uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_time_cluster(void *op_ctx, void *solution, int count,
                                       uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_route_removal(void *op_ctx, void *solution, int count,
                                        uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_time_window(void *op_ctx, void *solution, int count,
                                      uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_paired_shaw(void *op_ctx, void *solution, int count,
                                      uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_vehicle_target(void *op_ctx, void *solution, int count,
                                         uint32_t *removed_ids, int *removed_count);

/* sg_repair.c */
ARStatus sg_reinsert_removed_requests(SGBootstrapSolution *sol,
                                      const uint32_t *removed_ids, int removed_count);
ARStatus sg_repair_greedy_insertion(void *op_ctx, void *solution,
                                    const uint32_t *removed_ids, int removed_count);
ARStatus sg_repair_regret2(void *op_ctx, void *solution,
                           const uint32_t *removed_ids, int removed_count);
ARStatus sg_repair_regret3(void *op_ctx, void *solution,
                           const uint32_t *removed_ids, int removed_count);
ARStatus sg_repair_regret4(void *op_ctx, void *solution,
                           const uint32_t *removed_ids, int removed_count);
ARStatus sg_repair_noise_regret(void *op_ctx, void *solution,
                                const uint32_t *removed_ids, int removed_count);
ARStatus sg_repair_pair_sync(void *op_ctx, void *solution,
                             const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_greedy(void *op_ctx, void *solution,
                                const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_regret2(void *op_ctx, void *solution,
                                 const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_regret3(void *op_ctx, void *solution,
                                 const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_regret4(void *op_ctx, void *solution,
                                 const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_noise_regret(void *op_ctx, void *solution,
                                      const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_pair_sync(void *op_ctx, void *solution,
                                   const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_fill_greedy(SGContext *ctx, SGRouteSolution *sol, double noise_scale);
ARStatus sg_route_repair_fill_regret(SGContext *ctx, SGRouteSolution *sol,
                                     int regret_k, double noise_scale);
int sg_route_rank_insertions_for_request(SGContext *ctx, const SGRouteSolution *sol,
                                         uint32_t request_id, int regret_k,
                                         double noise_scale, double *best_score_out,
                                         double *kth_score_out, uint32_t *best_vehicle_out,
                                         uint32_t *best_pos_out,
                                         uint32_t *best_pickup_stop_pos_out,
                                         uint32_t *best_delivery_stop_pos_out,
                                         double *best_route_distance_out);

/* sg_postprocess.c */
ARStatus sg_route_postprocess_reduce_vehicles(const SGContext *ctx, SGRouteSolution *sol);
ARStatus sg_route_postprocess_ejection_reduce(const SGContext *ctx, SGRouteSolution *sol);
ARStatus sg_route_postprocess_polish_distance(const SGContext *ctx, SGRouteSolution *sol);
ARStatus sg_route_postprocess_intensify(const SGContext *ctx, SGRouteSolution *sol);
int sg_route_find_best_insertion_for_request(const SGContext *ctx, const SGRouteSolution *sol,
                                             uint32_t request_id, uint32_t forbidden_vehicle,
                                             uint32_t *best_vehicle_out,
                                             uint32_t *best_pos_out,
                                             uint32_t *best_pickup_pos_out,
                                             uint32_t *best_delivery_pos_out,
                                             double *best_route_distance_out);
int sg_route_find_best_insertion_no_new_vehicle(const SGContext *ctx, const SGRouteSolution *sol,
                                                uint32_t request_id,
                                                uint32_t empty_route_ok_vehicle,
                                                uint32_t *best_vehicle_out,
                                                uint32_t *best_pos_out,
                                                uint32_t *best_pickup_pos_out,
                                                uint32_t *best_delivery_pos_out,
                                                double *best_route_distance_out);
void sg_route_restore_from_backup(SGRouteSolution *sol, SGRouteSolution *backup);

/* sg_solve.c */
void sg_adaptive_q_bounds(int num_requests, int config_q_min, int config_q_max,
                          int *q_min_out, int *q_max_out);
int sg_route_solver_eligible(const SGContext *ctx);

#endif /* SURGE_SG_INTERNAL_H */
