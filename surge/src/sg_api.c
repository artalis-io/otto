/*
 * Surge API Handler Implementation
 *
 * Transport-agnostic request handling for VRP/PDPTW solving.
 */

#include "sg_api.h"
#include "surge.h"
#include "sh_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * JSON Error Response Helper
 * ============================================================================ */

static char *make_error_json(int status_code, const char *message, size_t *out_len) {
    ShJsonBuf jb;
    ShJsonWriter w;

    sh_json_buf_init(&jb);
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "status", "error");
    sh_json_write_kv_int(&w, "code", status_code);
    sh_json_write_kv_string(&w, "error", message);
    sh_json_write_object_end(&w);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NULL;
    }

    if (out_len) {
        *out_len = jb.len;
    }
    return sh_json_buf_take(&jb);
}

/* ============================================================================
 * JSON Model Builders
 * ============================================================================ */

static SGStatus build_config(SGContext *ctx, const ShJsonValue *cfg_val) {
    SGConfig cfg;

    sg_config_default(&cfg);

    if (cfg_val && sh_json_type(cfg_val) == SH_JSON_OBJECT) {
        ShJsonValue *v;

        v = sh_json_get(cfg_val, "max_iterations");
        if (v) cfg.max_iterations = sh_json_as_int(v, cfg.max_iterations);

        v = sh_json_get(cfg_val, "max_time_seconds");
        if (v) cfg.max_time_seconds = sh_json_as_int(v, cfg.max_time_seconds);

        v = sh_json_get(cfg_val, "segment_size");
        if (v) cfg.segment_size = sh_json_as_int(v, cfg.segment_size);

        v = sh_json_get(cfg_val, "q_min");
        if (v) cfg.q_min = sh_json_as_int(v, cfg.q_min);

        v = sh_json_get(cfg_val, "q_max");
        if (v) cfg.q_max = sh_json_as_int(v, cfg.q_max);

        v = sh_json_get(cfg_val, "seed");
        if (v) cfg.seed = (uint64_t)sh_json_as_double(v, (double)cfg.seed);

        v = sh_json_get(cfg_val, "deterministic");
        if (v) cfg.deterministic = sh_json_as_bool(v, cfg.deterministic);

        v = sh_json_get(cfg_val, "accept_type");
        if (v) {
            const char *at = sh_json_as_string(v, "sa");
            if (strcmp(at, "rrt") == 0) cfg.accept_type = SG_ACCEPT_RRT;
            else if (strcmp(at, "improving") == 0) cfg.accept_type = SG_ACCEPT_IMPROVING;
            else cfg.accept_type = SG_ACCEPT_SA;
        }

        v = sh_json_get(cfg_val, "lexicographic_objective");
        if (v) cfg.lexicographic_objective = sh_json_as_bool(v, cfg.lexicographic_objective);

        v = sh_json_get(cfg_val, "adaptive_q");
        if (v) cfg.adaptive_q = sh_json_as_bool(v, cfg.adaptive_q);
    }

    return sg_set_config(ctx, &cfg);
}

static int build_locations(SGContext *ctx, const ShJsonValue *locs_arr) {
    size_t i, count;

    if (!locs_arr || sh_json_type(locs_arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(locs_arr);
    for (i = 0; i < count; i++) {
        ShJsonValue *loc = sh_json_array_get(locs_arr, i);
        uint32_t id;
        double x, y;

        if (!loc || sh_json_type(loc) != SH_JSON_OBJECT) {
            return -1;
        }

        id = sg_add_location(ctx);
        if (id == UINT32_MAX) {
            return -1;
        }

        x = sh_json_as_double(sh_json_get(loc, "x"), 0.0);
        y = sh_json_as_double(sh_json_get(loc, "y"), 0.0);
        if (sg_location_set_coords(ctx, id, x, y) != SG_STATUS_OK) {
            return -1;
        }
    }

    return 0;
}

static int build_travel(SGContext *ctx, const ShJsonValue *travel_val) {
    ShJsonValue *v;
    int location_count;
    size_t n;
    double *distances = NULL;
    double *durations = NULL;
    size_t i;
    SGStatus status;

    if (!travel_val || sh_json_type(travel_val) != SH_JSON_OBJECT) {
        return 0;
    }

    v = sh_json_get(travel_val, "location_count");
    if (!v) return -1;
    location_count = sh_json_as_int(v, 0);
    if (location_count <= 0) return -1;
    n = (size_t)location_count * (size_t)location_count;

    v = sh_json_get(travel_val, "distances");
    if (!v || sh_json_type(v) != SH_JSON_ARRAY || sh_json_array_len(v) != n) {
        return -1;
    }
    distances = (double *)malloc(n * sizeof(double));
    if (!distances) return -1;
    for (i = 0; i < n; i++) {
        distances[i] = sh_json_as_double(sh_json_array_get(v, i), 0.0);
    }

    v = sh_json_get(travel_val, "durations");
    if (!v || sh_json_type(v) != SH_JSON_ARRAY || sh_json_array_len(v) != n) {
        free(distances);
        return -1;
    }
    durations = (double *)malloc(n * sizeof(double));
    if (!durations) {
        free(distances);
        return -1;
    }
    for (i = 0; i < n; i++) {
        durations[i] = sh_json_as_double(sh_json_array_get(v, i), 0.0);
    }

    status = sg_set_travel_matrix(ctx, (uint32_t)location_count, distances, durations);
    free(distances);
    free(durations);
    return (status == SG_STATUS_OK) ? 0 : -1;
}

static int build_zones(SGContext *ctx, const ShJsonValue *zones_val) {
    ShJsonValue *v;
    int zone_count;
    size_t n, i;
    double *matrix;
    SGStatus status;

    if (!zones_val || sh_json_type(zones_val) != SH_JSON_OBJECT) {
        return 0;
    }

    v = sh_json_get(zones_val, "count");
    if (!v) return -1;
    zone_count = sh_json_as_int(v, 0);
    if (zone_count <= 0) return -1;
    n = (size_t)zone_count * (size_t)zone_count;

    v = sh_json_get(zones_val, "distances");
    if (!v || sh_json_type(v) != SH_JSON_ARRAY || sh_json_array_len(v) != n) {
        return -1;
    }

    matrix = (double *)malloc(n * sizeof(double));
    if (!matrix) return -1;
    for (i = 0; i < n; i++) {
        matrix[i] = sh_json_as_double(sh_json_array_get(v, i), 0.0);
    }

    status = sg_set_zone_distance_matrix(ctx, (uint32_t)zone_count, matrix);
    free(matrix);
    return (status == SG_STATUS_OK) ? 0 : -1;
}

static int build_commodities(SGContext *ctx, const ShJsonValue *comm_val) {
    ShJsonValue *v;
    int count, i;
    ShJsonValue *conflicts;
    size_t ci, clen;

    if (!comm_val || sh_json_type(comm_val) != SH_JSON_OBJECT) {
        return 0;
    }

    v = sh_json_get(comm_val, "count");
    if (!v) return -1;
    count = sh_json_as_int(v, 0);
    if (count <= 0) return -1;

    for (i = 0; i < count; i++) {
        uint32_t cid;
        if (sg_add_commodity(ctx, &cid) != SG_STATUS_OK) {
            return -1;
        }
    }

    conflicts = sh_json_get(comm_val, "conflicts");
    if (conflicts && sh_json_type(conflicts) == SH_JSON_ARRAY) {
        clen = sh_json_array_len(conflicts);
        for (ci = 0; ci < clen; ci++) {
            ShJsonValue *pair = sh_json_array_get(conflicts, ci);
            uint32_t a, b;
            if (!pair || sh_json_type(pair) != SH_JSON_ARRAY || sh_json_array_len(pair) != 2) {
                return -1;
            }
            a = (uint32_t)sh_json_as_int(sh_json_array_get(pair, 0), 0);
            b = (uint32_t)sh_json_as_int(sh_json_array_get(pair, 1), 0);
            if (sg_commodity_set_conflict(ctx, a, b) != SG_STATUS_OK) {
                return -1;
            }
        }
    }

    return 0;
}

static int build_exclusion_groups(SGContext *ctx, const ShJsonValue *eg_val) {
    ShJsonValue *v;
    int count, i;

    if (!eg_val || sh_json_type(eg_val) != SH_JSON_OBJECT) {
        return 0;
    }

    v = sh_json_get(eg_val, "count");
    if (!v) return -1;
    count = sh_json_as_int(v, 0);
    if (count <= 0) return -1;

    for (i = 0; i < count; i++) {
        uint32_t gid;
        if (sg_add_exclusion_group(ctx, &gid) != SG_STATUS_OK) {
            return -1;
        }
    }

    return 0;
}

static int build_setup_times(SGContext *ctx, const ShJsonValue *st_val) {
    ShJsonValue *v;
    int num_classes;
    size_t n, i, j;
    ShJsonValue *matrix_arr;

    if (!st_val || sh_json_type(st_val) != SH_JSON_OBJECT) {
        return 0;
    }

    v = sh_json_get(st_val, "num_classes");
    if (!v) return -1;
    num_classes = sh_json_as_int(v, 0);
    if (num_classes <= 0) return -1;

    if (sg_set_num_setup_classes(ctx, (uint32_t)num_classes) != SG_STATUS_OK) {
        return -1;
    }

    matrix_arr = sh_json_get(st_val, "matrix");
    if (matrix_arr && sh_json_type(matrix_arr) == SH_JSON_ARRAY) {
        n = (size_t)num_classes * (size_t)num_classes;
        if (sh_json_array_len(matrix_arr) != n) {
            return -1;
        }
        for (i = 0; i < (size_t)num_classes; i++) {
            for (j = 0; j < (size_t)num_classes; j++) {
                double secs = sh_json_as_double(
                    sh_json_array_get(matrix_arr, i * (size_t)num_classes + j), 0.0);
                if (sg_set_setup_time(ctx, (uint32_t)i + 1, (uint32_t)j + 1, secs) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

static int build_depots(SGContext *ctx, const ShJsonValue *depots_arr) {
    size_t i, count;

    if (!depots_arr || sh_json_type(depots_arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(depots_arr);
    for (i = 0; i < count; i++) {
        ShJsonValue *d = sh_json_array_get(depots_arr, i);
        uint32_t id;
        ShJsonValue *v;

        if (!d || sh_json_type(d) != SH_JSON_OBJECT) {
            return -1;
        }

        id = sg_add_depot(ctx);
        if (id == UINT32_MAX) {
            return -1;
        }

        v = sh_json_get(d, "x");
        if (v) {
            double x = sh_json_as_double(v, 0.0);
            double y = sh_json_as_double(sh_json_get(d, "y"), 0.0);
            if (sg_depot_set_location(ctx, id, x, y) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(d, "location_id");
        if (v) {
            uint32_t loc_id = (uint32_t)sh_json_as_int(v, 0);
            if (sg_depot_set_location_id(ctx, id, loc_id) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(d, "tw_early");
        if (v) {
            int32_t early = sh_json_as_int(v, 0);
            int32_t late = sh_json_as_int(sh_json_get(d, "tw_late"), 0);
            if (sg_depot_set_time_window(ctx, id, early, late) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(d, "max_simultaneous");
        if (v) {
            uint32_t ms = (uint32_t)sh_json_as_int(v, 0);
            if (sg_depot_set_max_simultaneous(ctx, id, ms) != SG_STATUS_OK) {
                return -1;
            }
        }
    }

    return 0;
}

static int build_vehicles(SGContext *ctx, const ShJsonValue *vehicles_arr) {
    size_t i, count;

    if (!vehicles_arr || sh_json_type(vehicles_arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(vehicles_arr);
    for (i = 0; i < count; i++) {
        ShJsonValue *veh = sh_json_array_get(vehicles_arr, i);
        uint32_t id;
        ShJsonValue *v;

        if (!veh || sh_json_type(veh) != SH_JSON_OBJECT) {
            return -1;
        }

        id = sg_add_vehicle(ctx);
        if (id == UINT32_MAX) {
            return -1;
        }

        v = sh_json_get(veh, "start_depot_id");
        if (v) {
            uint32_t start = (uint32_t)sh_json_as_int(v, 0);
            uint32_t end = (uint32_t)sh_json_as_int(sh_json_get(veh, "end_depot_id"), start);
            if (sg_vehicle_set_depots(ctx, id, start, end) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "shift_early");
        if (v) {
            int32_t early = sh_json_as_int(v, 0);
            int32_t late = sh_json_as_int(sh_json_get(veh, "shift_late"), 0);
            if (sg_vehicle_set_shift_time_window(ctx, id, early, late) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "capacity");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t dim = sh_json_array_len(v);
            double *cap = (double *)malloc(dim * sizeof(double));
            size_t j;
            if (!cap) return -1;
            for (j = 0; j < dim; j++) {
                cap[j] = sh_json_as_double(sh_json_array_get(v, j), 0.0);
            }
            if (sg_vehicle_set_capacity(ctx, id, cap, (uint32_t)dim) != SG_STATUS_OK) {
                free(cap);
                return -1;
            }
            free(cap);
        }

        v = sh_json_get(veh, "qualifications");
        if (v) {
            uint64_t q = (uint64_t)sh_json_as_double(v, 0.0);
            if (sg_vehicle_set_qualifications(ctx, id, q) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "open_end");
        if (v) {
            int oe = sh_json_as_bool(v, false) ? 1 : 0;
            if (sg_vehicle_set_open_end(ctx, id, oe) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "max_duration");
        if (v) {
            int32_t md = sh_json_as_int(v, 0);
            if (sg_vehicle_set_max_duration(ctx, id, md) != SG_STATUS_OK) {
                return -1;
            }
        }

        /* Cost model */
        {
            ShJsonValue *fc = sh_json_get(veh, "fixed_cost");
            ShJsonValue *cpd = sh_json_get(veh, "cost_per_distance");
            ShJsonValue *cpt = sh_json_get(veh, "cost_per_duration");
            if (fc || cpd || cpt) {
                double fixed = fc ? sh_json_as_double(fc, 0.0) : 0.0;
                double dist_c = cpd ? sh_json_as_double(cpd, 1.0) : 1.0;
                double dur_c = cpt ? sh_json_as_double(cpt, 0.0) : 0.0;
                if (sg_vehicle_set_costs(ctx, id, fixed, dist_c, dur_c) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }

        v = sh_json_get(veh, "cost_per_waiting");
        if (v) {
            if (sg_vehicle_set_waiting_cost(ctx, id, sh_json_as_double(v, 0.0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "cost_per_overtime");
        if (v) {
            if (sg_vehicle_set_overtime_cost(ctx, id, sh_json_as_double(v, 0.0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "depot_loading_seconds");
        if (v) {
            if (sg_vehicle_set_depot_loading_seconds(ctx, id, sh_json_as_int(v, 0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "depot_unloading_seconds");
        if (v) {
            if (sg_vehicle_set_depot_unloading_seconds(ctx, id, sh_json_as_int(v, 0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        /* Break policy */
        {
            ShJsonValue *bmw = sh_json_get(veh, "break_max_work_seconds");
            ShJsonValue *bds = sh_json_get(veh, "break_duration_seconds");
            if (bmw || bds) {
                int32_t mw = bmw ? sh_json_as_int(bmw, 0) : 0;
                int32_t bd = bds ? sh_json_as_int(bds, 0) : 0;
                if (sg_vehicle_set_break_policy(ctx, id, mw, bd) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }

        v = sh_json_get(veh, "max_total_work_seconds");
        if (v) {
            if (sg_vehicle_set_max_total_work(ctx, id, sh_json_as_int(v, 0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        /* Multi-trip */
        v = sh_json_get(veh, "max_trips");
        if (v) {
            if (sg_vehicle_set_max_trips(ctx, id, (uint32_t)sh_json_as_int(v, 1)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(veh, "trip_reload_seconds");
        if (v) {
            if (sg_vehicle_set_trip_reload_seconds(ctx, id, sh_json_as_int(v, 0)) != SG_STATUS_OK) {
                return -1;
            }
        }
    }

    return 0;
}

static int build_tasks(SGContext *ctx, const ShJsonValue *tasks_arr) {
    size_t i, count;

    if (!tasks_arr || sh_json_type(tasks_arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(tasks_arr);
    for (i = 0; i < count; i++) {
        ShJsonValue *t = sh_json_array_get(tasks_arr, i);
        const char *type_str;
        SGTaskType type;
        uint32_t id;
        ShJsonValue *v;

        if (!t || sh_json_type(t) != SH_JSON_OBJECT) {
            return -1;
        }

        type_str = sh_json_as_string(sh_json_get(t, "type"), "delivery");
        if (strcmp(type_str, "pickup") == 0) {
            type = SG_TASK_PICKUP;
        } else if (strcmp(type_str, "delivery") == 0) {
            type = SG_TASK_DELIVERY;
        } else if (strcmp(type_str, "service") == 0) {
            type = SG_TASK_SERVICE;
        } else {
            return -1;
        }

        id = sg_add_task(ctx, type);
        if (id == UINT32_MAX) {
            return -1;
        }

        v = sh_json_get(t, "x");
        if (v) {
            double x = sh_json_as_double(v, 0.0);
            double y = sh_json_as_double(sh_json_get(t, "y"), 0.0);
            if (sg_task_set_location(ctx, id, x, y) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(t, "location_id");
        if (v) {
            uint32_t loc_id = (uint32_t)sh_json_as_int(v, 0);
            if (sg_task_set_location_id(ctx, id, loc_id) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(t, "tw_early");
        if (v) {
            int32_t early = sh_json_as_int(v, 0);
            int32_t late = sh_json_as_int(sh_json_get(t, "tw_late"), 0);
            if (sg_task_set_time_window(ctx, id, early, late) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(t, "service_seconds");
        if (v) {
            if (sg_task_set_service_seconds(ctx, id, sh_json_as_int(v, 0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(t, "demand");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t dim = sh_json_array_len(v);
            double *demand = (double *)malloc(dim * sizeof(double));
            size_t j;
            if (!demand) return -1;
            for (j = 0; j < dim; j++) {
                demand[j] = sh_json_as_double(sh_json_array_get(v, j), 0.0);
            }
            if (sg_task_set_demand(ctx, id, demand, (uint32_t)dim) != SG_STATUS_OK) {
                free(demand);
                return -1;
            }
            free(demand);
        }

        /* Soft time window */
        v = sh_json_get(t, "soft_time_window");
        if (v && sh_json_type(v) == SH_JSON_OBJECT) {
            int32_t se = sh_json_as_int(sh_json_get(v, "early"), 0);
            int32_t sl = sh_json_as_int(sh_json_get(v, "late"), 0);
            double ep = sh_json_as_double(sh_json_get(v, "early_penalty"), 0.0);
            double lp = sh_json_as_double(sh_json_get(v, "late_penalty"), 0.0);
            if (sg_task_set_soft_time_window(ctx, id, se, sl, ep, lp) != SG_STATUS_OK) {
                return -1;
            }
        }

        /* Disjunct time windows */
        v = sh_json_get(t, "time_windows");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t twc = sh_json_array_len(v);
            size_t ti;
            for (ti = 0; ti < twc; ti++) {
                ShJsonValue *tw = sh_json_array_get(v, ti);
                if (!tw || sh_json_type(tw) != SH_JSON_OBJECT) return -1;
                int32_t twe = sh_json_as_int(sh_json_get(tw, "early"), 0);
                int32_t twl = sh_json_as_int(sh_json_get(tw, "late"), 0);
                if (sg_task_add_time_window(ctx, id, twe, twl) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

static int build_requests(SGContext *ctx, const ShJsonValue *requests_arr) {
    size_t i, count;

    if (!requests_arr || sh_json_type(requests_arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(requests_arr);
    for (i = 0; i < count; i++) {
        ShJsonValue *r = sh_json_array_get(requests_arr, i);
        uint32_t id;
        ShJsonValue *v;

        if (!r || sh_json_type(r) != SH_JSON_OBJECT) {
            return -1;
        }

        id = sg_add_request(ctx);
        if (id == UINT32_MAX) {
            return -1;
        }

        v = sh_json_get(r, "pickup_task_id");
        if (v) {
            uint32_t pickup = (uint32_t)sh_json_as_int(v, 0);
            uint32_t delivery = (uint32_t)sh_json_as_int(sh_json_get(r, "delivery_task_id"), 0);
            if (sg_request_bind_pickup_delivery_tasks(ctx, id, pickup, delivery) !=
                SG_STATUS_OK) {
                return -1;
            }
        } else {
            v = sh_json_get(r, "delivery_task_id");
            if (v) {
                uint32_t delivery = (uint32_t)sh_json_as_int(v, 0);
                if (sg_request_bind_delivery_task(ctx, id, delivery) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }

        v = sh_json_get(r, "priority");
        if (v) {
            sg_request_set_priority_hint(ctx, id, sh_json_as_int(v, 0));
        }

        v = sh_json_get(r, "zone_id");
        if (v) {
            sg_request_set_zone_hint(ctx, id, (uint32_t)sh_json_as_int(v, 0));
        }

        v = sh_json_get(r, "tw_early");
        if (v) {
            int32_t early = sh_json_as_int(v, 0);
            int32_t late = sh_json_as_int(sh_json_get(r, "tw_late"), 0);
            sg_request_set_time_window_hint(ctx, id, early, late);
        }

        v = sh_json_get(r, "required_qualifications");
        if (v) {
            uint64_t q = (uint64_t)sh_json_as_double(v, 0.0);
            if (sg_request_set_required_qualifications(ctx, id, q) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(r, "max_ride_time");
        if (v) {
            if (sg_request_set_max_ride_time(ctx, id, sh_json_as_int(v, 0)) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(r, "allowed_vehicles");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t alen = sh_json_array_len(v);
            size_t ai;
            for (ai = 0; ai < alen; ai++) {
                uint32_t vid = (uint32_t)sh_json_as_int(sh_json_array_get(v, ai), 0);
                if (sg_request_add_allowed_vehicle(ctx, id, vid) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }

        v = sh_json_get(r, "forbidden_vehicles");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t flen = sh_json_array_len(v);
            size_t fi;
            for (fi = 0; fi < flen; fi++) {
                uint32_t vid = (uint32_t)sh_json_as_int(sh_json_array_get(v, fi), 0);
                if (sg_request_add_forbidden_vehicle(ctx, id, vid) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }

        v = sh_json_get(r, "commodity");
        if (v) {
            uint32_t cid = (uint32_t)sh_json_as_int(v, 0);
            if (sg_request_set_commodity(ctx, id, cid) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(r, "exclusion_groups");
        if (v && sh_json_type(v) == SH_JSON_ARRAY) {
            size_t elen = sh_json_array_len(v);
            size_t ei;
            for (ei = 0; ei < elen; ei++) {
                uint32_t gid = (uint32_t)sh_json_as_int(sh_json_array_get(v, ei), 0);
                if (sg_request_add_exclusion_group(ctx, id, gid) != SG_STATUS_OK) {
                    return -1;
                }
            }
        }

        v = sh_json_get(r, "setup_class");
        if (v) {
            uint32_t sc = (uint32_t)sh_json_as_int(v, 0);
            if (sg_request_set_setup_class(ctx, id, sc) != SG_STATUS_OK) {
                return -1;
            }
        }

        v = sh_json_get(r, "unassigned_penalty");
        if (v) {
            double pen = sh_json_as_double(v, 0.0);
            if (sg_request_set_unassigned_penalty(ctx, id, pen) != SG_STATUS_OK) {
                return -1;
            }
        }
    }

    return 0;
}

static int build_initial_routes(SGContext *ctx, const ShJsonValue *ir_arr) {
    size_t i, count;
    uint32_t *vehicle_ids = NULL;
    uint32_t *route_lengths = NULL;
    uint32_t *request_ids = NULL;
    uint32_t total_requests = 0;
    SGStatus status;

    if (!ir_arr || sh_json_type(ir_arr) != SH_JSON_ARRAY) {
        return 0;
    }

    count = sh_json_array_len(ir_arr);
    if (count == 0) return 0;

    /* First pass: count total request IDs */
    for (i = 0; i < count; i++) {
        ShJsonValue *route = sh_json_array_get(ir_arr, i);
        ShJsonValue *rids;
        if (!route || sh_json_type(route) != SH_JSON_OBJECT) return -1;
        rids = sh_json_get(route, "request_ids");
        if (!rids || sh_json_type(rids) != SH_JSON_ARRAY) return -1;
        total_requests += (uint32_t)sh_json_array_len(rids);
    }

    vehicle_ids = (uint32_t *)malloc(count * sizeof(uint32_t));
    route_lengths = (uint32_t *)malloc(count * sizeof(uint32_t));
    request_ids = total_requests > 0 ? (uint32_t *)malloc(total_requests * sizeof(uint32_t)) : NULL;
    if (!vehicle_ids || !route_lengths || (total_requests > 0 && !request_ids)) {
        free(vehicle_ids);
        free(route_lengths);
        free(request_ids);
        return -1;
    }

    /* Second pass: fill arrays */
    {
        uint32_t ri = 0;
        for (i = 0; i < count; i++) {
            ShJsonValue *route = sh_json_array_get(ir_arr, i);
            ShJsonValue *rids = sh_json_get(route, "request_ids");
            size_t rlen = sh_json_array_len(rids);
            size_t j;

            vehicle_ids[i] = (uint32_t)sh_json_as_int(sh_json_get(route, "vehicle_id"), 0);
            route_lengths[i] = (uint32_t)rlen;

            for (j = 0; j < rlen; j++) {
                request_ids[ri++] = (uint32_t)sh_json_as_int(sh_json_array_get(rids, j), 0);
            }
        }
    }

    status = sg_set_initial_routes(ctx, (uint32_t)count, vehicle_ids, route_lengths, request_ids);
    free(vehicle_ids);
    free(route_lengths);
    free(request_ids);
    return (status == SG_STATUS_OK) ? 0 : -1;
}

/* ============================================================================
 * sg_api_build_model — build model from parsed JSON DOM
 * ============================================================================ */

SGStatus sg_api_build_model(SGContext *ctx, const ShJsonValue *root) {
    ShJsonValue *v;

    if (!ctx || !root || sh_json_type(root) != SH_JSON_OBJECT) {
        return SG_STATUS_INVALID_ARG;
    }

    /* 1. Scalars */
    v = sh_json_get(root, "dimension_count");
    if (v) {
        int dim = sh_json_as_int(v, 1);
        if (dim > 0 && sg_set_dimension_count(ctx, (uint32_t)dim) != SG_STATUS_OK) {
            return SG_STATUS_INVALID_ARG;
        }
    }

    v = sh_json_get(root, "demand_sign_convention");
    if (v) {
        sg_set_demand_sign_convention(ctx, (SGDemandSignConvention)sh_json_as_int(v, 0));
    }

    v = sh_json_get(root, "unassigned_weight");
    if (v) {
        sg_set_unassigned_weight(ctx, sh_json_as_double(v, 1000000.0));
    }

    /* 2. Config */
    if (build_config(ctx, sh_json_get(root, "config")) != SG_STATUS_OK) {
        return SG_STATUS_ERROR;
    }

    /* 3. Locations (before depots/tasks that use location_id) */
    if (build_locations(ctx, sh_json_get(root, "locations")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 4. Commodities, exclusion groups, setup times (before requests) */
    if (build_commodities(ctx, sh_json_get(root, "commodities")) != 0) {
        return SG_STATUS_ERROR;
    }
    if (build_exclusion_groups(ctx, sh_json_get(root, "exclusion_groups")) != 0) {
        return SG_STATUS_ERROR;
    }
    if (build_setup_times(ctx, sh_json_get(root, "setup_times")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 5. Depots (before vehicles) */
    if (build_depots(ctx, sh_json_get(root, "depots")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 6. Vehicles */
    if (build_vehicles(ctx, sh_json_get(root, "vehicles")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 7. Tasks */
    if (build_tasks(ctx, sh_json_get(root, "tasks")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 8. Requests (references tasks, commodities, exclusion_groups, vehicles) */
    if (build_requests(ctx, sh_json_get(root, "requests")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 9. Travel and zones (after locations) */
    if (build_travel(ctx, sh_json_get(root, "travel")) != 0) {
        return SG_STATUS_ERROR;
    }
    if (build_zones(ctx, sh_json_get(root, "zones")) != 0) {
        return SG_STATUS_ERROR;
    }

    /* 10. Initial routes (references vehicles and requests) */
    if (build_initial_routes(ctx, sh_json_get(root, "initial_routes")) != 0) {
        return SG_STATUS_ERROR;
    }

    return SG_STATUS_OK;
}

/* ============================================================================
 * sg_api_build_model_file — read JSON file and build model
 * ============================================================================ */

SGStatus sg_api_build_model_file(SGContext *ctx, const char *path) {
    FILE *f;
    long fsize;
    char *buf;
    size_t nread;
    SHArena *arena;
    ShJsonValue *root;
    ShJsonStatus parse_status;
    SGStatus result;

    if (!ctx || !path) return SG_STATUS_INVALID_ARG;

    f = fopen(path, "rb");
    if (!f) return SG_STATUS_ERROR;

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(f);
        return SG_STATUS_INVALID_ARG;
    }

    buf = (char *)malloc((size_t)fsize);
    if (!buf) {
        fclose(f);
        return SG_STATUS_OUT_OF_MEMORY;
    }

    nread = fread(buf, 1, (size_t)fsize, f);
    fclose(f);

    if ((long)nread != fsize) {
        free(buf);
        return SG_STATUS_ERROR;
    }

    arena = sh_arena_create((size_t)fsize * 2 + 4096);
    if (!arena) {
        free(buf);
        return SG_STATUS_OUT_OF_MEMORY;
    }

    parse_status = sh_json_parse(buf, (size_t)fsize, arena, &root);
    free(buf);

    if (parse_status != SH_JSON_OK || !root) {
        sh_arena_free(arena);
        return SG_STATUS_INVALID_ARG;
    }

    result = sg_api_build_model(ctx, root);
    sh_arena_free(arena);
    return result;
}

/* ============================================================================
 * sg_api_write_solution — write solution to streaming JSON writer
 * ============================================================================ */

SGStatus sg_api_write_solution(const SGContext *ctx, ShJsonWriter *w,
                                SGStatus solve_status) {
    if (!ctx || !w) return SG_STATUS_INVALID_ARG;

    sh_json_write_object_start(w);

    if (solve_status == SG_STATUS_OK || solve_status == SG_STATUS_LIMIT) {
        SGStats stats;
        uint32_t route_count, ri, unassigned_count;

        sg_get_stats(ctx, &stats);

        sh_json_write_kv_string(w, "status",
                                solve_status == SG_STATUS_OK ? "ok" : "limit");

        /* Stats */
        sh_json_write_key(w, "stats");
        sh_json_write_object_start(w);
        sh_json_write_kv_int(w, "iterations", stats.iterations);
        sh_json_write_kv_double_fmt(w, "total_cost", stats.total_cost, 2);
        sh_json_write_kv_double_fmt(w, "total_distance", stats.total_distance, 2);
        sh_json_write_kv_int(w, "unassigned", stats.unassigned);
        sh_json_write_kv_int(w, "vehicles_used", stats.vehicles_used);
        sh_json_write_kv_double_fmt(w, "total_waiting", stats.total_waiting, 2);
        sh_json_write_kv_double_fmt(w, "total_overtime", stats.total_overtime, 2);
        sh_json_write_kv_double_fmt(w, "total_tw_penalty", stats.total_tw_penalty, 2);
        sh_json_write_object_end(w);

        /* Routes */
        route_count = sg_solution_get_route_count(ctx);
        sh_json_write_key(w, "routes");
        sh_json_write_array_start(w);

        for (ri = 0; ri < route_count; ri++) {
            uint32_t vehicle_id = sg_solution_get_route_vehicle_id(ctx, ri);
            uint32_t stop_count = sg_solution_get_route_stop_count(ctx, ri);
            uint32_t si;

            sh_json_write_object_start(w);
            sh_json_write_kv_int(w, "vehicle_id", vehicle_id);
            sh_json_write_kv_double_fmt(w, "distance",
                sg_solution_get_route_distance(ctx, ri), 2);
            sh_json_write_kv_double_fmt(w, "duration",
                sg_solution_get_route_duration(ctx, ri), 2);
            sh_json_write_kv_double_fmt(w, "waiting",
                sg_solution_get_route_waiting(ctx, ri), 2);
            sh_json_write_kv_double_fmt(w, "overtime",
                sg_solution_get_route_overtime(ctx, ri), 2);
            sh_json_write_kv_double_fmt(w, "tw_penalty",
                sg_solution_get_route_tw_penalty(ctx, ri), 2);
            sh_json_write_kv_double_fmt(w, "break_time",
                sg_solution_get_route_break_time(ctx, ri), 2);
            sh_json_write_kv_int(w, "break_count",
                sg_solution_get_route_break_count(ctx, ri));
            sh_json_write_kv_double_fmt(w, "total_work",
                sg_solution_get_route_total_work(ctx, ri), 2);
            sh_json_write_kv_int(w, "trip_count",
                sg_solution_get_route_trip_count(ctx, ri));

            /* Break position records */
            {
                uint32_t bc = sg_solution_get_route_break_count(ctx, ri);
                if (bc > 0) {
                    uint32_t bi;
                    sh_json_write_key(w, "breaks");
                    sh_json_write_array_start(w);
                    for (bi = 0; bi < bc; bi++) {
                        uint32_t after_stop;
                        double bstart, bdur;
                        if (sg_solution_get_route_break(ctx, ri, bi,
                                &after_stop, &bstart, &bdur) == SG_STATUS_OK) {
                            sh_json_write_object_start(w);
                            sh_json_write_kv_int(w, "after_stop_index", after_stop);
                            sh_json_write_kv_double_fmt(w, "start", bstart, 2);
                            sh_json_write_kv_double_fmt(w, "duration", bdur, 2);
                            sh_json_write_object_end(w);
                        }
                    }
                    sh_json_write_array_end(w);
                }
            }

            sh_json_write_key(w, "stops");
            sh_json_write_array_start(w);

            for (si = 0; si < stop_count; si++) {
                SGSolutionStop stop;
                if (sg_solution_get_route_stop(ctx, ri, si, &stop) == SG_STATUS_OK) {
                    const char *type_str;
                    sh_json_write_object_start(w);
                    sh_json_write_kv_int(w, "request_id", stop.request_id);
                    sh_json_write_kv_int(w, "task_id", stop.task_id);
                    switch (stop.stop_type) {
                        case SG_STOP_TYPE_PICKUP:   type_str = "pickup"; break;
                        case SG_STOP_TYPE_DELIVERY:  type_str = "delivery"; break;
                        case SG_STOP_TYPE_SERVICE:   type_str = "service"; break;
                        default:                     type_str = "unknown"; break;
                    }
                    sh_json_write_kv_string(w, "type", type_str);
                    sh_json_write_kv_double_fmt(w, "arrival", stop.arrival, 2);
                    sh_json_write_kv_double_fmt(w, "service_start", stop.service_start, 2);
                    sh_json_write_kv_double_fmt(w, "departure", stop.departure, 2);
                    sh_json_write_kv_int(w, "trip_index", stop.trip_index);
                    sh_json_write_object_end(w);
                }
            }

            sh_json_write_array_end(w);  /* stops */
            sh_json_write_object_end(w);  /* route */
        }

        sh_json_write_array_end(w);  /* routes */

        /* Unassigned */
        unassigned_count = sg_get_unassigned(ctx);
        sh_json_write_key(w, "unassigned");
        sh_json_write_array_start(w);
        for (ri = 0; ri < unassigned_count; ri++) {
            sh_json_write_int(w, sg_solution_get_unassigned_request(ctx, ri));
        }
        sh_json_write_array_end(w);
    } else if (solve_status == SG_STATUS_INFEASIBLE) {
        sh_json_write_kv_string(w, "status", "infeasible");
    } else {
        sh_json_write_kv_string(w, "status", "error");
    }

    /* Error string (always present, empty on success) */
    {
        const char *err = sg_get_last_error(ctx);
        sh_json_write_kv_string(w, "error", err ? err : "");
    }

    sh_json_write_object_end(w);

    return sh_json_writer_error(w) ? SG_STATUS_ERROR : SG_STATUS_OK;
}

/* ============================================================================
 * Solve Handler (refactored to use build_model + write_solution)
 * ============================================================================ */

char *sg_api_solve(const char *json_body, size_t body_len,
                   int *status_code, size_t *out_len) {
    SHArena *arena = NULL;
    ShJsonValue *root = NULL;
    ShJsonStatus parse_status;
    SGContext *ctx = NULL;
    SGStatus build_status, solve_status;
    ShJsonBuf jb;
    ShJsonWriter w;
    char *result;

    if (!json_body || body_len == 0) {
        if (status_code) *status_code = 400;
        return make_error_json(400, "empty request body", out_len);
    }

    arena = sh_arena_create(body_len * 2 + 4096);
    if (!arena) {
        if (status_code) *status_code = 500;
        return make_error_json(500, "out of memory", out_len);
    }

    parse_status = sh_json_parse(json_body, body_len, arena, &root);
    if (parse_status != SH_JSON_OK || !root || sh_json_type(root) != SH_JSON_OBJECT) {
        sh_arena_free(arena);
        if (status_code) *status_code = 400;
        return make_error_json(400, "invalid JSON", out_len);
    }

    ctx = sg_create();
    if (!ctx) {
        sh_arena_free(arena);
        if (status_code) *status_code = 500;
        return make_error_json(500, "failed to create solver context", out_len);
    }

    build_status = sg_api_build_model(ctx, root);
    sh_arena_free(arena);
    arena = NULL;

    if (build_status != SG_STATUS_OK) {
        const char *err = sg_get_last_error(ctx);
        char *resp;
        if (err && err[0]) {
            resp = make_error_json(400, err, out_len);
        } else {
            resp = make_error_json(400, "failed to build model", out_len);
        }
        sg_free(ctx);
        if (status_code) *status_code = 400;
        return resp;
    }

    if (sg_validate_model(ctx) != SG_STATUS_OK) {
        const char *err = sg_get_last_error(ctx);
        char *resp;
        if (err && err[0]) {
            resp = make_error_json(400, err, out_len);
        } else {
            resp = make_error_json(400, "model validation failed", out_len);
        }
        sg_free(ctx);
        if (status_code) *status_code = 400;
        return resp;
    }

    solve_status = sg_solve(ctx);

    sh_json_buf_init(&jb);
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sg_api_write_solution(ctx, &w, solve_status);

    sg_free(ctx);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        if (status_code) *status_code = 500;
        return make_error_json(500, "failed to write response JSON", out_len);
    }

    if (status_code) {
        *status_code = (solve_status == SG_STATUS_OK || solve_status == SG_STATUS_LIMIT)
                       ? 200 : 400;
    }
    if (out_len) {
        *out_len = jb.len;
    }

    result = sh_json_buf_take(&jb);
    return result;
}

/* ============================================================================
 * Health / Version Handlers
 * ============================================================================ */

char *sg_api_health(size_t *out_len) {
    ShJsonBuf jb;
    ShJsonWriter w;

    sh_json_buf_init(&jb);
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "status", "healthy");
    sh_json_write_kv_string(&w, "service", "surge");
    sh_json_write_kv_string(&w, "version", sg_version());
    sh_json_write_object_end(&w);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NULL;
    }

    if (out_len) {
        *out_len = jb.len;
    }
    return sh_json_buf_take(&jb);
}

char *sg_api_version(size_t *out_len) {
    ShJsonBuf jb;
    ShJsonWriter w;

    sh_json_buf_init(&jb);
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "version", sg_version());
    sh_json_write_object_end(&w);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NULL;
    }

    if (out_len) {
        *out_len = jb.len;
    }
    return sh_json_buf_take(&jb);
}

/* ============================================================================
 * Router
 * ============================================================================ */

int sg_api_handle(const SGAPIRequest *req, SGAPIResponse *resp) {
    if (!req || !resp || !req->path) {
        return -1;
    }

    memset(resp, 0, sizeof(*resp));
    resp->content_type = "application/json";

    if (strcmp(req->path, "/api/v1/solve") == 0) {
        int code = 200;
        resp->body = sg_api_solve(req->body, req->body_len, &code, &resp->body_len);
        resp->status_code = code;
    } else if (strcmp(req->path, "/api/v1/health") == 0) {
        resp->body = sg_api_health(&resp->body_len);
        resp->status_code = resp->body ? 200 : 500;
    } else if (strcmp(req->path, "/api/v1/version") == 0) {
        resp->body = sg_api_version(&resp->body_len);
        resp->status_code = resp->body ? 200 : 500;
    } else {
        resp->body = make_error_json(404, "not found", &resp->body_len);
        resp->status_code = 404;
    }

    return resp->body ? 0 : -1;
}

void sg_api_response_free(SGAPIResponse *resp) {
    if (resp && resp->body) {
        free(resp->body);
        resp->body = NULL;
        resp->body_len = 0;
    }
}
