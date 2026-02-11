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
