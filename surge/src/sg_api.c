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
 * JSON Model Builder
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

static int build_depots(SGContext *ctx, const ShJsonValue *depots_arr) {
    size_t i;
    size_t count;

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

        v = sh_json_get(d, "tw_early");
        if (v) {
            int32_t early = sh_json_as_int(v, 0);
            int32_t late = sh_json_as_int(sh_json_get(d, "tw_late"), 0);
            if (sg_depot_set_time_window(ctx, id, early, late) != SG_STATUS_OK) {
                return -1;
            }
        }
    }

    return 0;
}

static int build_vehicles(SGContext *ctx, const ShJsonValue *vehicles_arr) {
    size_t i;
    size_t count;

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
    }

    return 0;
}

static int build_tasks(SGContext *ctx, const ShJsonValue *tasks_arr) {
    size_t i;
    size_t count;

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
    }

    return 0;
}

static int build_requests(SGContext *ctx, const ShJsonValue *requests_arr) {
    size_t i;
    size_t count;

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
    }

    return 0;
}

/* ============================================================================
 * Solve Handler
 * ============================================================================ */

char *sg_api_solve(const char *json_body, size_t body_len,
                   int *status_code, size_t *out_len) {
    SHArena *arena = NULL;
    ShJsonValue *root = NULL;
    ShJsonStatus parse_status;
    SGContext *ctx = NULL;
    SGStatus solve_status;
    ShJsonValue *v;
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

    v = sh_json_get(root, "dimension_count");
    if (v) {
        int dim = sh_json_as_int(v, 1);
        if (dim > 0 && sg_set_dimension_count(ctx, (uint32_t)dim) != SG_STATUS_OK) {
            sg_free(ctx);
            sh_arena_free(arena);
            if (status_code) *status_code = 400;
            return make_error_json(400, "invalid dimension_count", out_len);
        }
    }

    v = sh_json_get(root, "demand_sign_convention");
    if (v) {
        int conv = sh_json_as_int(v, 0);
        sg_set_demand_sign_convention(ctx, (SGDemandSignConvention)conv);
    }

    if (build_config(ctx, sh_json_get(root, "config")) != SG_STATUS_OK ||
        build_depots(ctx, sh_json_get(root, "depots")) != 0 ||
        build_vehicles(ctx, sh_json_get(root, "vehicles")) != 0 ||
        build_tasks(ctx, sh_json_get(root, "tasks")) != 0 ||
        build_requests(ctx, sh_json_get(root, "requests")) != 0) {
        sg_free(ctx);
        sh_arena_free(arena);
        if (status_code) *status_code = 400;
        return make_error_json(400, "failed to build model", out_len);
    }

    sh_arena_free(arena);
    arena = NULL;

    if (sg_validate_model(ctx) != SG_STATUS_OK) {
        sg_free(ctx);
        if (status_code) *status_code = 400;
        return make_error_json(400, "model validation failed", out_len);
    }

    solve_status = sg_solve(ctx);

    sh_json_buf_init(&jb);
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);

    if (solve_status == SG_STATUS_OK || solve_status == SG_STATUS_LIMIT) {
        SGStats stats;
        sg_get_stats(ctx, &stats);

        sh_json_write_kv_string(&w, "status",
                                solve_status == SG_STATUS_OK ? "ok" : "limit");

        sh_json_write_key(&w, "stats");
        sh_json_write_object_start(&w);
        sh_json_write_kv_int(&w, "iterations", stats.iterations);
        sh_json_write_kv_double_fmt(&w, "total_cost", stats.total_cost, 2);
        sh_json_write_kv_double_fmt(&w, "total_distance", stats.total_distance, 2);
        sh_json_write_kv_int(&w, "unassigned", stats.unassigned);
        sh_json_write_kv_int(&w, "vehicles_used", stats.vehicles_used);
        sh_json_write_object_end(&w);
    } else if (solve_status == SG_STATUS_INFEASIBLE) {
        sh_json_write_kv_string(&w, "status", "infeasible");
    } else {
        sh_json_write_kv_string(&w, "status", "error");
    }

    sh_json_write_object_end(&w);

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
