#include "surge.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int id;
    double x;
    double y;
    double demand;
    int ready_time;
    int due_time;
    int service_time;
} SGSolomonRow;

typedef struct {
    int id;
    double x;
    double y;
    double demand;
    int ready_time;
    int due_time;
    int service_time;
    int pickup_ref;
    int delivery_ref;
} SGLiLimRow;

static const char *sg_skip_ws(const char *s) {
    while (s && *s && isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

static int sg_contains_token(const char *line, const char *token) {
    if (!line || !token) {
        return 0;
    }
    return strstr(line, token) != NULL;
}

static int sg_parse_vehicle_line(const char *line, int *vehicle_count, double *capacity) {
    int count = 0;
    double cap = 0.0;

    if (!line || !vehicle_count || !capacity) {
        return 0;
    }
    if (sscanf(line, "%d %lf", &count, &cap) == 2 && count > 0 && isfinite(cap) && cap > 0.0) {
        *vehicle_count = count;
        *capacity = cap;
        return 1;
    }
    return 0;
}

static int sg_parse_customer_line(const char *line, SGSolomonRow *row) {
    int matched;
    SGSolomonRow parsed;

    if (!line || !row) {
        return 0;
    }

    memset(&parsed, 0, sizeof(parsed));
    matched = sscanf(line, "%d %lf %lf %lf %d %d %d", &parsed.id, &parsed.x, &parsed.y,
                     &parsed.demand, &parsed.ready_time, &parsed.due_time,
                     &parsed.service_time);
    if (matched != 7) {
        return 0;
    }
    if (!isfinite(parsed.x) || !isfinite(parsed.y) || !isfinite(parsed.demand)) {
        return 0;
    }
    if (parsed.due_time < parsed.ready_time || parsed.service_time < 0) {
        return 0;
    }

    *row = parsed;
    return 1;
}

static int sg_parse_li_lim_vehicle_line(const char *line, int *vehicle_count, double *capacity) {
    int count = 0;
    double cap = 0.0;
    double speed = 0.0;
    int matched = 0;

    if (!line || !vehicle_count || !capacity) {
        return 0;
    }
    matched = sscanf(line, "%d %lf %lf", &count, &cap, &speed);
    if ((matched == 2 || matched == 3) && count > 0 && isfinite(cap) && cap > 0.0) {
        *vehicle_count = count;
        *capacity = cap;
        return 1;
    }
    return 0;
}

static int sg_parse_li_lim_row(const char *line, SGLiLimRow *row) {
    int matched;
    SGLiLimRow parsed;

    if (!line || !row) {
        return 0;
    }

    memset(&parsed, 0, sizeof(parsed));
    matched = sscanf(line, "%d %lf %lf %lf %d %d %d %d %d",
                     &parsed.id, &parsed.x, &parsed.y, &parsed.demand,
                     &parsed.ready_time, &parsed.due_time, &parsed.service_time,
                     &parsed.pickup_ref, &parsed.delivery_ref);
    if (matched != 9) {
        return 0;
    }
    if (!isfinite(parsed.x) || !isfinite(parsed.y) || !isfinite(parsed.demand)) {
        return 0;
    }
    if (parsed.due_time < parsed.ready_time || parsed.service_time < 0) {
        return 0;
    }
    if (parsed.pickup_ref < 0 || parsed.delivery_ref < 0) {
        return 0;
    }

    *row = parsed;
    return 1;
}

static int sg_find_li_lim_row_index(const SGLiLimRow *rows, size_t row_count,
                                    int node_id, size_t *idx_out) {
    size_t i;

    if (!rows || !idx_out) {
        return 0;
    }

    for (i = 0; i < row_count; i++) {
        if (rows[i].id == node_id) {
            *idx_out = i;
            return 1;
        }
    }
    return 0;
}

SGStatus sg_load_solomon_vrptw(SGContext *ctx, const char *file_path) {
    enum {
        SG_PARSE_SEARCH_VEHICLE = 0,
        SG_PARSE_READ_VEHICLE,
        SG_PARSE_SEARCH_CUSTOMER,
        SG_PARSE_READ_CUSTOMERS
    } parse_state = SG_PARSE_SEARCH_VEHICLE;

    FILE *fp = NULL;
    SGStatus status = SG_STATUS_OK;
    SGSolomonRow *rows = NULL;
    size_t row_count = 0;
    size_t row_capacity = 0;
    int vehicle_count = 0;
    double vehicle_capacity = 0.0;
    char line[512];
    size_t i;
    SGDemandSignConvention demand_convention;

    if (!ctx || !file_path) {
        return SG_STATUS_INVALID_ARG;
    }
    if (sg_get_request_count(ctx) != 0) {
        return SG_STATUS_INVALID_ARG;
    }

    status = sg_set_dimension_count(ctx, 1);
    if (status != SG_STATUS_OK) {
        return status;
    }

    fp = fopen(file_path, "r");
    if (!fp) {
        return SG_STATUS_INVALID_ARG;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        const char *trim = sg_skip_ws(line);

        if (!trim || *trim == '\0') {
            continue;
        }

        switch (parse_state) {
            case SG_PARSE_SEARCH_VEHICLE:
                if (sg_contains_token(trim, "VEHICLE")) {
                    parse_state = SG_PARSE_READ_VEHICLE;
                }
                break;

            case SG_PARSE_READ_VEHICLE:
                if (sg_parse_vehicle_line(trim, &vehicle_count, &vehicle_capacity)) {
                    parse_state = SG_PARSE_SEARCH_CUSTOMER;
                }
                break;

            case SG_PARSE_SEARCH_CUSTOMER:
                if (sg_contains_token(trim, "CUSTOMER")) {
                    parse_state = SG_PARSE_READ_CUSTOMERS;
                }
                break;

            case SG_PARSE_READ_CUSTOMERS: {
                SGSolomonRow row;
                if (sg_parse_customer_line(trim, &row)) {
                    SGSolomonRow *new_rows;
                    if (row_count == row_capacity) {
                        size_t new_capacity = row_capacity > 0 ? row_capacity * 2 : 128;
                        if (new_capacity < row_capacity ||
                            new_capacity > SIZE_MAX / sizeof(*rows)) {
                            status = SG_STATUS_OUT_OF_MEMORY;
                            goto done;
                        }
                        new_rows = (SGSolomonRow *)realloc(rows, new_capacity * sizeof(*rows));
                        if (!new_rows) {
                            status = SG_STATUS_OUT_OF_MEMORY;
                            goto done;
                        }
                        rows = new_rows;
                        row_capacity = new_capacity;
                    }
                    rows[row_count++] = row;
                }
                break;
            }
        }
    }

    if (vehicle_count <= 0 || vehicle_capacity <= 0.0 || row_count < 2) {
        status = SG_STATUS_INVALID_ARG;
        goto done;
    }

    {
        const SGSolomonRow *depot = &rows[0];
        uint32_t depot_id = sg_add_depot(ctx);
        double cap[1];

        if (depot_id == UINT32_MAX) {
            status = SG_STATUS_OUT_OF_MEMORY;
            goto done;
        }
        status = sg_depot_set_location(ctx, depot_id, depot->x, depot->y);
        if (status != SG_STATUS_OK) {
            goto done;
        }
        status = sg_depot_set_time_window(ctx, depot_id, depot->ready_time, depot->due_time);
        if (status != SG_STATUS_OK) {
            goto done;
        }

        cap[0] = vehicle_capacity;
        for (i = 0; i < (size_t)vehicle_count; i++) {
            uint32_t vehicle_id = sg_add_vehicle(ctx);
            if (vehicle_id == UINT32_MAX) {
                status = SG_STATUS_OUT_OF_MEMORY;
                goto done;
            }
            status = sg_vehicle_set_depots(ctx, vehicle_id, depot_id, depot_id);
            if (status != SG_STATUS_OK) {
                goto done;
            }
            status = sg_vehicle_set_shift_time_window(ctx, vehicle_id,
                                                      depot->ready_time, depot->due_time);
            if (status != SG_STATUS_OK) {
                goto done;
            }
            status = sg_vehicle_set_capacity(ctx, vehicle_id, cap, 1);
            if (status != SG_STATUS_OK) {
                goto done;
            }
        }
    }

    demand_convention = sg_get_demand_sign_convention(ctx);
    for (i = 1; i < row_count; i++) {
        const SGSolomonRow *row = &rows[i];
        uint32_t request_id;
        uint32_t task_id;
        double demand[1];

        request_id = sg_add_request(ctx);
        if (request_id == UINT32_MAX) {
            status = SG_STATUS_OUT_OF_MEMORY;
            goto done;
        }

        task_id = sg_add_task(ctx, SG_TASK_DELIVERY);
        if (task_id == UINT32_MAX) {
            status = SG_STATUS_OUT_OF_MEMORY;
            goto done;
        }

        status = sg_task_set_location(ctx, task_id, row->x, row->y);
        if (status != SG_STATUS_OK) {
            goto done;
        }
        status = sg_task_set_time_window(ctx, task_id, row->ready_time, row->due_time);
        if (status != SG_STATUS_OK) {
            goto done;
        }
        status = sg_task_set_service_seconds(ctx, task_id, row->service_time);
        if (status != SG_STATUS_OK) {
            goto done;
        }

        demand[0] = fabs(row->demand);
        if (demand_convention == SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE) {
            demand[0] = -demand[0];
        }
        status = sg_task_set_demand(ctx, task_id, demand, 1);
        if (status != SG_STATUS_OK) {
            goto done;
        }
        status = sg_request_bind_delivery_task(ctx, request_id, task_id);
        if (status != SG_STATUS_OK) {
            goto done;
        }
        status = sg_request_set_time_window_hint(ctx, request_id,
                                                 row->ready_time, row->due_time);
        if (status != SG_STATUS_OK) {
            goto done;
        }
    }

done:
    if (fp) {
        fclose(fp);
        fp = NULL;
    }
    free(rows);
    return status;
}

SGStatus sg_load_li_lim_pdptw(SGContext *ctx, const char *file_path) {
    FILE *fp = NULL;
    SGStatus status = SG_STATUS_OK;
    SGLiLimRow *rows = NULL;
    uint8_t *consumed = NULL;
    size_t row_count = 0;
    size_t row_capacity = 0;
    int vehicle_count = 0;
    double vehicle_capacity = 0.0;
    char line[512];
    size_t i;
    size_t depot_idx = SIZE_MAX;
    SGDemandSignConvention demand_convention;

    if (!ctx || !file_path) {
        return SG_STATUS_INVALID_ARG;
    }
    if (sg_get_request_count(ctx) != 0) {
        return SG_STATUS_INVALID_ARG;
    }

    status = sg_set_dimension_count(ctx, 1);
    if (status != SG_STATUS_OK) {
        return status;
    }

    fp = fopen(file_path, "r");
    if (!fp) {
        return SG_STATUS_INVALID_ARG;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        const char *trim = sg_skip_ws(line);

        if (!trim || *trim == '\0') {
            continue;
        }
        if (vehicle_count <= 0 || vehicle_capacity <= 0.0) {
            if (sg_parse_li_lim_vehicle_line(trim, &vehicle_count, &vehicle_capacity)) {
                continue;
            }
        }

        {
            SGLiLimRow row;
            if (sg_parse_li_lim_row(trim, &row)) {
                SGLiLimRow *new_rows;
                if (row_count == row_capacity) {
                    size_t new_capacity = row_capacity > 0 ? row_capacity * 2U : 128U;
                    if (new_capacity < row_capacity ||
                        new_capacity > SIZE_MAX / sizeof(*rows)) {
                        status = SG_STATUS_OUT_OF_MEMORY;
                        goto done;
                    }
                    new_rows = (SGLiLimRow *)realloc(rows, new_capacity * sizeof(*rows));
                    if (!new_rows) {
                        status = SG_STATUS_OUT_OF_MEMORY;
                        goto done;
                    }
                    rows = new_rows;
                    row_capacity = new_capacity;
                }
                rows[row_count++] = row;
            }
        }
    }

    if (vehicle_count <= 0 || vehicle_capacity <= 0.0 || row_count < 3) {
        status = SG_STATUS_INVALID_ARG;
        goto done;
    }

    for (i = 0; i < row_count; i++) {
        if (rows[i].id == 0) {
            depot_idx = i;
            break;
        }
    }
    if (depot_idx == SIZE_MAX) {
        depot_idx = 0;
    }

    {
        const SGLiLimRow *depot = &rows[depot_idx];
        uint32_t depot_id = sg_add_depot(ctx);
        double cap[1];

        if (depot_id == UINT32_MAX) {
            status = SG_STATUS_OUT_OF_MEMORY;
            goto done;
        }
        status = sg_depot_set_location(ctx, depot_id, depot->x, depot->y);
        if (status != SG_STATUS_OK) {
            goto done;
        }
        status = sg_depot_set_time_window(ctx, depot_id, depot->ready_time, depot->due_time);
        if (status != SG_STATUS_OK) {
            goto done;
        }

        cap[0] = vehicle_capacity;
        for (i = 0; i < (size_t)vehicle_count; i++) {
            uint32_t vehicle_id = sg_add_vehicle(ctx);
            if (vehicle_id == UINT32_MAX) {
                status = SG_STATUS_OUT_OF_MEMORY;
                goto done;
            }
            status = sg_vehicle_set_depots(ctx, vehicle_id, depot_id, depot_id);
            if (status != SG_STATUS_OK) {
                goto done;
            }
            status = sg_vehicle_set_shift_time_window(ctx, vehicle_id,
                                                      depot->ready_time, depot->due_time);
            if (status != SG_STATUS_OK) {
                goto done;
            }
            status = sg_vehicle_set_capacity(ctx, vehicle_id, cap, 1);
            if (status != SG_STATUS_OK) {
                goto done;
            }
        }
    }

    consumed = (uint8_t *)calloc(row_count, sizeof(uint8_t));
    if (!consumed) {
        status = SG_STATUS_OUT_OF_MEMORY;
        goto done;
    }
    consumed[depot_idx] = 1;

    demand_convention = sg_get_demand_sign_convention(ctx);
    for (i = 0; i < row_count; i++) {
        const SGLiLimRow *pickup_row;
        const SGLiLimRow *delivery_row;
        size_t delivery_idx;
        uint32_t request_id;
        uint32_t pickup_task_id;
        uint32_t delivery_task_id;
        int32_t hint_early;
        int32_t hint_late;
        double quantity;
        double pickup_demand[1];
        double delivery_demand[1];

        if (i == depot_idx || consumed[i]) {
            continue;
        }
        pickup_row = &rows[i];

        if (pickup_row->pickup_ref != 0 && pickup_row->delivery_ref == 0) {
            /* Delivery nodes are attached when their pickup row is processed. */
            continue;
        }
        if (pickup_row->pickup_ref != 0 || pickup_row->delivery_ref <= 0) {
            status = SG_STATUS_INVALID_ARG;
            goto done;
        }
        if (!sg_find_li_lim_row_index(rows, row_count, pickup_row->delivery_ref, &delivery_idx)) {
            status = SG_STATUS_INVALID_ARG;
            goto done;
        }
        if (delivery_idx == i || delivery_idx == depot_idx || consumed[delivery_idx]) {
            status = SG_STATUS_INVALID_ARG;
            goto done;
        }
        delivery_row = &rows[delivery_idx];
        if (delivery_row->pickup_ref != pickup_row->id || delivery_row->delivery_ref != 0) {
            status = SG_STATUS_INVALID_ARG;
            goto done;
        }

        quantity = fabs(pickup_row->demand);
        if (quantity <= 1e-9) {
            quantity = fabs(delivery_row->demand);
        }
        if (!isfinite(quantity) || quantity <= 1e-9) {
            status = SG_STATUS_INVALID_ARG;
            goto done;
        }

        request_id = sg_add_request(ctx);
        if (request_id == UINT32_MAX) {
            status = SG_STATUS_OUT_OF_MEMORY;
            goto done;
        }

        pickup_task_id = sg_add_task(ctx, SG_TASK_PICKUP);
        delivery_task_id = sg_add_task(ctx, SG_TASK_DELIVERY);
        if (pickup_task_id == UINT32_MAX || delivery_task_id == UINT32_MAX) {
            status = SG_STATUS_OUT_OF_MEMORY;
            goto done;
        }

        status = sg_task_set_location(ctx, pickup_task_id, pickup_row->x, pickup_row->y);
        if (status != SG_STATUS_OK) {
            goto done;
        }
        status = sg_task_set_time_window(ctx, pickup_task_id,
                                         pickup_row->ready_time, pickup_row->due_time);
        if (status != SG_STATUS_OK) {
            goto done;
        }
        status = sg_task_set_service_seconds(ctx, pickup_task_id, pickup_row->service_time);
        if (status != SG_STATUS_OK) {
            goto done;
        }

        status = sg_task_set_location(ctx, delivery_task_id, delivery_row->x, delivery_row->y);
        if (status != SG_STATUS_OK) {
            goto done;
        }
        status = sg_task_set_time_window(ctx, delivery_task_id,
                                         delivery_row->ready_time, delivery_row->due_time);
        if (status != SG_STATUS_OK) {
            goto done;
        }
        status = sg_task_set_service_seconds(ctx, delivery_task_id, delivery_row->service_time);
        if (status != SG_STATUS_OK) {
            goto done;
        }

        if (demand_convention == SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE) {
            pickup_demand[0] = quantity;
            delivery_demand[0] = -quantity;
        } else {
            pickup_demand[0] = -quantity;
            delivery_demand[0] = quantity;
        }
        status = sg_task_set_demand(ctx, pickup_task_id, pickup_demand, 1);
        if (status != SG_STATUS_OK) {
            goto done;
        }
        status = sg_task_set_demand(ctx, delivery_task_id, delivery_demand, 1);
        if (status != SG_STATUS_OK) {
            goto done;
        }

        status = sg_request_bind_pickup_delivery_tasks(ctx, request_id,
                                                       pickup_task_id, delivery_task_id);
        if (status != SG_STATUS_OK) {
            goto done;
        }

        hint_early = pickup_row->ready_time < delivery_row->ready_time
                     ? pickup_row->ready_time
                     : delivery_row->ready_time;
        hint_late = pickup_row->due_time > delivery_row->due_time
                    ? pickup_row->due_time
                    : delivery_row->due_time;
        status = sg_request_set_time_window_hint(ctx, request_id, hint_early, hint_late);
        if (status != SG_STATUS_OK) {
            goto done;
        }

        consumed[i] = 1;
        consumed[delivery_idx] = 1;
    }

    for (i = 0; i < row_count; i++) {
        if (!consumed[i]) {
            status = SG_STATUS_INVALID_ARG;
            goto done;
        }
    }

done:
    if (fp) {
        fclose(fp);
        fp = NULL;
    }
    free(consumed);
    free(rows);
    return status;
}

/* Cordeau DARP format:
 *   Header: K  n  T  Q  L
 *   Rows:   id  x  y  service  demand  tw_early  tw_late
 *   Node 0 = depot, 1..n = pickups, n+1..2n = deliveries.
 *   Pickup i pairs with delivery n+i. */

typedef struct {
    int id;
    double x;
    double y;
    int service;
    double demand;
    int tw_early;
    int tw_late;
} SGCordeauRow;

SGStatus sg_load_cordeau_darp(SGContext *ctx, const char *file_path) {
    FILE *fp = NULL;
    SGStatus status = SG_STATUS_OK;
    SGCordeauRow *rows = NULL;
    size_t row_count = 0;
    size_t row_capacity = 0;
    int K = 0, n = 0, T = 0;
    double Q = 0.0, L = 0.0;
    char line[512];
    int header_parsed = 0;
    int i;
    SGDemandSignConvention demand_convention;

    if (!ctx || !file_path) {
        return SG_STATUS_INVALID_ARG;
    }
    if (sg_get_request_count(ctx) != 0) {
        return SG_STATUS_INVALID_ARG;
    }

    status = sg_set_dimension_count(ctx, 1);
    if (status != SG_STATUS_OK) {
        return status;
    }

    fp = fopen(file_path, "r");
    if (!fp) {
        return SG_STATUS_INVALID_ARG;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        const char *trim = sg_skip_ws(line);
        if (!trim || *trim == '\0') continue;

        if (!header_parsed) {
            if (sscanf(trim, "%d %d %d %lf %lf", &K, &n, &T, &Q, &L) == 5 &&
                K > 0 && n > 0) {
                header_parsed = 1;
            }
            continue;
        }

        {
            SGCordeauRow row;
            memset(&row, 0, sizeof(row));
            if (sscanf(trim, "%d %lf %lf %d %lf %d %d",
                        &row.id, &row.x, &row.y, &row.service,
                        &row.demand, &row.tw_early, &row.tw_late) == 7) {
                SGCordeauRow *new_rows;
                if (row_count == row_capacity) {
                    size_t nc = row_capacity > 0 ? row_capacity * 2 : 128;
                    new_rows = (SGCordeauRow *)realloc(rows, nc * sizeof(*rows));
                    if (!new_rows) {
                        status = SG_STATUS_OUT_OF_MEMORY;
                        goto cdone;
                    }
                    rows = new_rows;
                    row_capacity = nc;
                }
                rows[row_count++] = row;
            }
        }
    }

    if (!header_parsed || K <= 0 || n <= 0 || row_count < (size_t)(2 * n + 1)) {
        status = SG_STATUS_INVALID_ARG;
        goto cdone;
    }

    /* Depot = rows[0] (id 0) */
    {
        const SGCordeauRow *depot = &rows[0];
        uint32_t depot_id = sg_add_depot(ctx);
        double cap[1];

        if (depot_id == UINT32_MAX) {
            status = SG_STATUS_OUT_OF_MEMORY;
            goto cdone;
        }
        status = sg_depot_set_location(ctx, depot_id, depot->x, depot->y);
        if (status != SG_STATUS_OK) goto cdone;
        status = sg_depot_set_time_window(ctx, depot_id, depot->tw_early, depot->tw_late);
        if (status != SG_STATUS_OK) goto cdone;

        cap[0] = Q;
        for (i = 0; i < K; i++) {
            uint32_t v = sg_add_vehicle(ctx);
            if (v == UINT32_MAX) {
                status = SG_STATUS_OUT_OF_MEMORY;
                goto cdone;
            }
            status = sg_vehicle_set_depots(ctx, v, depot_id, depot_id);
            if (status != SG_STATUS_OK) goto cdone;
            status = sg_vehicle_set_shift_time_window(ctx, v, depot->tw_early, depot->tw_late);
            if (status != SG_STATUS_OK) goto cdone;
            status = sg_vehicle_set_capacity(ctx, v, cap, 1);
            if (status != SG_STATUS_OK) goto cdone;
            if (T > 0) {
                status = sg_vehicle_set_max_duration(ctx, v, T);
                if (status != SG_STATUS_OK) goto cdone;
            }
            /* DARP objective: minimize total route duration across the fleet.
               fixed_cost=0 (fleet size is given, not minimized),
               cost_per_distance=0 (distance is part of duration),
               cost_per_duration=1 (the actual DARP objective). */
            status = sg_vehicle_set_costs(ctx, v, 0.0, 0.0, 1.0);
            if (status != SG_STATUS_OK) goto cdone;
        }
    }

    /* Create n PD requests: pickup i (rows[i]) pairs with delivery n+i (rows[n+i]) */
    demand_convention = sg_get_demand_sign_convention(ctx);
    for (i = 1; i <= n; i++) {
        const SGCordeauRow *p_row = &rows[i];
        const SGCordeauRow *d_row = &rows[n + i];
        uint32_t request_id, p_task, d_task;
        double quantity = fabs(p_row->demand);
        double pickup_demand[1], delivery_demand[1];
        int32_t hint_early, hint_late;

        if (quantity <= 1e-9) quantity = fabs(d_row->demand);
        if (quantity <= 1e-9) quantity = 1.0;

        request_id = sg_add_request(ctx);
        if (request_id == UINT32_MAX) {
            status = SG_STATUS_OUT_OF_MEMORY;
            goto cdone;
        }

        p_task = sg_add_task(ctx, SG_TASK_PICKUP);
        d_task = sg_add_task(ctx, SG_TASK_DELIVERY);
        if (p_task == UINT32_MAX || d_task == UINT32_MAX) {
            status = SG_STATUS_OUT_OF_MEMORY;
            goto cdone;
        }

        status = sg_task_set_location(ctx, p_task, p_row->x, p_row->y);
        if (status != SG_STATUS_OK) goto cdone;
        status = sg_task_set_time_window(ctx, p_task, p_row->tw_early, p_row->tw_late);
        if (status != SG_STATUS_OK) goto cdone;
        status = sg_task_set_service_seconds(ctx, p_task, p_row->service);
        if (status != SG_STATUS_OK) goto cdone;

        status = sg_task_set_location(ctx, d_task, d_row->x, d_row->y);
        if (status != SG_STATUS_OK) goto cdone;
        status = sg_task_set_time_window(ctx, d_task, d_row->tw_early, d_row->tw_late);
        if (status != SG_STATUS_OK) goto cdone;
        status = sg_task_set_service_seconds(ctx, d_task, d_row->service);
        if (status != SG_STATUS_OK) goto cdone;

        if (demand_convention == SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE) {
            pickup_demand[0] = quantity;
            delivery_demand[0] = -quantity;
        } else {
            pickup_demand[0] = -quantity;
            delivery_demand[0] = quantity;
        }
        status = sg_task_set_demand(ctx, p_task, pickup_demand, 1);
        if (status != SG_STATUS_OK) goto cdone;
        status = sg_task_set_demand(ctx, d_task, delivery_demand, 1);
        if (status != SG_STATUS_OK) goto cdone;

        status = sg_request_bind_pickup_delivery_tasks(ctx, request_id, p_task, d_task);
        if (status != SG_STATUS_OK) goto cdone;

        if (L > 0.0) {
            status = sg_request_set_max_ride_time(ctx, request_id, (int32_t)L);
            if (status != SG_STATUS_OK) goto cdone;
        }

        hint_early = p_row->tw_early < d_row->tw_early ? p_row->tw_early : d_row->tw_early;
        hint_late = p_row->tw_late > d_row->tw_late ? p_row->tw_late : d_row->tw_late;
        status = sg_request_set_time_window_hint(ctx, request_id, hint_early, hint_late);
        if (status != SG_STATUS_OK) goto cdone;
    }

cdone:
    if (fp) {
        fclose(fp);
    }
    free(rows);
    return status;
}
