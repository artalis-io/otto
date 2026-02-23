/*
 * Ralph API Handler Implementation
 *
 * Transport-agnostic request handling for LP/MIP solving.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

#include "ralph_api.h"
#include "ralph_mip.h"
#include "sh_query.h"
#include "sh_json.h"
#include "sh_arena.h"

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

#define RALPH_API_VERSION "1.0.0"
#define MAX_JSON_RESPONSE 16384
#define MAX_VAR_NAME_LEN 256

/* Content type strings */
static const char *CT_JSON = "application/json";
static const char *CT_TEXT = "text/plain";

/* ============================================================================
 * API Context
 * ============================================================================ */

struct RalphAPIContext {
    int initialized;
};

RalphAPIContext *ralph_api_create(void) {
    RalphAPIContext *ctx = (RalphAPIContext *)calloc(1, sizeof(RalphAPIContext));
    if (!ctx) return NULL;
    ctx->initialized = 1;
    return ctx;
}

void ralph_api_free(RalphAPIContext *ctx) {
    if (ctx) {
        free(ctx);
    }
}

int ralph_api_ready(RalphAPIContext *ctx) {
    return ctx && ctx->initialized;
}


/* ============================================================================
 * JSON Response Builder
 * ============================================================================ */

typedef struct {
    char *buf;
    size_t size;
    size_t pos;
} JsonBuilder;

static void json_init(JsonBuilder *jb, char *buf, size_t size) {
    jb->buf = buf;
    jb->size = size;
    jb->pos = 0;
}

static void json_append(JsonBuilder *jb, const char *fmt, ...) {
    if (jb->pos >= jb->size - 1) return;

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(jb->buf + jb->pos, jb->size - jb->pos, fmt, args);
    va_end(args);

    if (n > 0) {
        jb->pos += (size_t)n;
        if (jb->pos > jb->size - 1) jb->pos = jb->size - 1;
    }
}

/* Escape string for JSON */
static void json_append_escaped(JsonBuilder *jb, const char *s) {
    json_append(jb, "\"");
    while (*s && jb->pos < jb->size - 1) {
        switch (*s) {
            case '"':  json_append(jb, "\\\""); break;
            case '\\': json_append(jb, "\\\\"); break;
            case '\n': json_append(jb, "\\n"); break;
            case '\r': json_append(jb, "\\r"); break;
            case '\t': json_append(jb, "\\t"); break;
            default:
                if ((unsigned char)*s < 32) {
                    json_append(jb, "\\u%04x", (unsigned char)*s);
                } else {
                    jb->buf[jb->pos++] = *s;
                }
                break;
        }
        s++;
    }
    json_append(jb, "\"");
}

/* ============================================================================
 * Response Helpers
 * ============================================================================ */

static void set_response(RalphAPIResponse *resp, int status,
                         const char *content_type,
                         const char *body) {
    resp->status_code = status;
    resp->content_type = content_type;
    resp->body_len = strlen(body);
    resp->body = (uint8_t *)malloc(resp->body_len + 1);
    if (resp->body) {
        memcpy(resp->body, body, resp->body_len + 1);
    }
}

static void set_error_response(RalphAPIResponse *resp, int status,
                               const char *error_msg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", error_msg);
    set_response(resp, status, CT_JSON, buf);
}

/* ============================================================================
 * Status String Helper (lowercase for JSON)
 * ============================================================================ */

static const char *status_to_json(RalphLPStatus status) {
    switch (status) {
        case RALPH_LP_STATUS_OPTIMAL:         return "optimal";
        case RALPH_LP_STATUS_INFEASIBLE:      return "infeasible";
        case RALPH_LP_STATUS_UNBOUNDED:       return "unbounded";
        case RALPH_LP_STATUS_INF_OR_UNBD:     return "infeasible_or_unbounded";
        case RALPH_LP_STATUS_ITERATION_LIMIT: return "iteration_limit";
        case RALPH_LP_STATUS_TIME_LIMIT:      return "time_limit";
        case RALPH_LP_STATUS_NODE_LIMIT:      return "node_limit";
        case RALPH_LP_STATUS_ERROR:           return "error";
        default:                           return "unknown";
    }
}

/* ============================================================================
 * Endpoint Handlers
 * ============================================================================ */

static int handle_health(RalphAPIContext *ctx, RalphAPIResponse *resp) {
    (void)ctx;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"version\":\"%s\"}",
             RALPH_API_VERSION);
    set_response(resp, 200, CT_JSON, buf);
    return 0;
}

static int handle_formats(RalphAPIContext *ctx, RalphAPIResponse *resp) {
    (void)ctx;
    const char *json =
        "{"
        "\"formats\":["
        "{\"id\":\"lp\",\"name\":\"CPLEX LP\",\"description\":\"CPLEX LP file format with min/max, subject to, bounds\"},"
        "{\"id\":\"mps\",\"name\":\"MPS\",\"description\":\"Mathematical Programming System format\"}"
        "]"
        "}";
    set_response(resp, 200, CT_JSON, json);
    return 0;
}

/* Build JSON response from solution */
static void build_json_response(RalphLPModel *model, int is_mip, RalphLPStatus status,
                                double solve_time_ms, JsonBuilder *jb) {
    int num_vars = ralph_lp_get_num_vars(model);

    json_append(jb, "{");
    json_append(jb, "\"status\":\"%s\"", status_to_json(status));

    if (status == RALPH_LP_STATUS_OPTIMAL ||
        status == RALPH_LP_STATUS_TIME_LIMIT ||
        status == RALPH_LP_STATUS_ITERATION_LIMIT) {
        double objval = ralph_lp_get_objval(model);
        json_append(jb, ",\"objective\":%.10g", objval);

        /* Get solution values */
        double *x = (double *)malloc(num_vars * sizeof(double));
        if (x && ralph_lp_get_solution(model, x) == 0) {
            json_append(jb, ",\"variables\":{");
            int first = 1;
            for (int j = 0; j < num_vars; j++) {
                const char *name = ralph_lp_get_var_name(model, j);
                if (!first) json_append(jb, ",");
                first = 0;
                if (name && name[0]) {
                    json_append_escaped(jb, name);
                } else {
                    json_append(jb, "\"x%d\"", j);
                }
                json_append(jb, ":%.10g", x[j]);
            }
            json_append(jb, "}");
        }
        free(x);
    }

    if (status == RALPH_LP_STATUS_INFEASIBLE) {
        json_append(jb, ",\"message\":\"Problem is infeasible\"");
    } else if (status == RALPH_LP_STATUS_UNBOUNDED) {
        json_append(jb, ",\"message\":\"Problem is unbounded\"");
    } else if (status == RALPH_LP_STATUS_TIME_LIMIT) {
        json_append(jb, ",\"message\":\"Timeout exceeded\"");
    }

    json_append(jb, ",\"solve_time_ms\":%.1f", solve_time_ms);
    json_append(jb, ",\"iterations\":%d", ralph_lp_get_iterations(model));
    json_append(jb, ",\"num_vars\":%d", num_vars);
    json_append(jb, ",\"num_cons\":%d", ralph_lp_get_num_cons(model));
    if (is_mip) {
        json_append(jb, ",\"is_mip\":true");
        json_append(jb, ",\"nodes\":%d", ralph_mip_get_node_count((const RalphMIPModel *)model));
    }
    json_append(jb, "}");
}

static int handle_solve(RalphAPIContext *ctx, const RalphAPIRequest *req,
                        RalphAPIResponse *resp) {
    (void)ctx;

    if (!req->body || req->body_len == 0) {
        set_error_response(resp, 400, "Missing request body");
        return 0;
    }

    /* Check format query param: lp, mps, or json (default) */
    char format[16] = "json";  /* Default to JSON for backward compat */
    sh_query_get_str(req->query, "format", format, sizeof(format));

    /* Determine if we're using raw body or JSON-wrapped */
    int use_json = (strcmp(format, "json") == 0);
    int use_lp = (strcmp(format, "lp") == 0);
    int use_mps = (strcmp(format, "mps") == 0);

    if (!use_json && !use_lp && !use_mps) {
        set_error_response(resp, 400, "Invalid format. Use 'lp', 'mps', or 'json'");
        return 0;
    }

    char *problem = NULL;
    char problem_format[16] = {0};
    int timeout_ms = RALPH_API_DEFAULT_TIMEOUT_MS;

    if (use_json) {
        /* Parse JSON request using sh_json */
        /* Arena needs space for DOM nodes + copies of strings; use 8x input or 4KB min */
        size_t arena_size = req->body_len * 8;
        if (arena_size < 4096) arena_size = 4096;
        SHArena *arena = sh_arena_create(arena_size);
        if (!arena) {
            set_error_response(resp, 500, "Memory allocation failed");
            return 0;
        }

        ShJsonValue *root = NULL;
        ShJsonStatus json_status = sh_json_parse(req->body, req->body_len, arena, &root);
        if (json_status != SH_JSON_OK) {
            sh_arena_free(arena);
            char err_buf[256];
            snprintf(err_buf, sizeof(err_buf), "Invalid JSON: %s",
                     sh_json_status_str(json_status));
            set_error_response(resp, 400, err_buf);
            return 0;
        }

        /* Get format field */
        const char *format_str = sh_json_as_string(sh_json_get(root, "format"), NULL);
        if (!format_str) {
            sh_arena_free(arena);
            set_error_response(resp, 400, "Missing 'format' field in JSON body");
            return 0;
        }
        strncpy(problem_format, format_str, sizeof(problem_format) - 1);
        problem_format[sizeof(problem_format) - 1] = '\0';

        /* Get timeout if specified */
        int parsed_timeout = (int)sh_json_as_double(sh_json_get(root, "timeout_ms"), 0.0);
        if (parsed_timeout > 0 && parsed_timeout <= RALPH_API_MAX_TIMEOUT_MS) {
            timeout_ms = parsed_timeout;
        }

        /* Get problem string */
        const char *problem_str = sh_json_as_string(sh_json_get(root, "problem"), NULL);
        if (!problem_str) {
            sh_arena_free(arena);
            set_error_response(resp, 400, "Missing 'problem' field");
            return 0;
        }

        /* Copy problem out of arena before freeing */
        size_t problem_len = strlen(problem_str);
        problem = (char *)malloc(problem_len + 1);
        if (!problem) {
            sh_arena_free(arena);
            set_error_response(resp, 500, "Memory allocation failed");
            return 0;
        }
        memcpy(problem, problem_str, problem_len + 1);

        sh_arena_free(arena);

        /* Validate format from JSON body */
        if (strcmp(problem_format, "lp") != 0 && strcmp(problem_format, "mps") != 0) {
            free(problem);
            set_error_response(resp, 400, "Unsupported format in body. Use 'lp' or 'mps'");
            return 0;
        }
    } else {
        /* Raw LP/MPS body */
        strncpy(problem_format, format, sizeof(problem_format) - 1);
        problem_format[sizeof(problem_format) - 1] = '\0';

        /* Copy body directly */
        problem = (char *)malloc(req->body_len + 1);
        if (!problem) {
            set_error_response(resp, 500, "Memory allocation failed");
            return 0;
        }
        memcpy(problem, req->body, req->body_len);
        problem[req->body_len] = '\0';

        /* Check for timeout_ms query param */
        timeout_ms = sh_query_get_int(req->query, "timeout_ms", timeout_ms);
        if (timeout_ms > RALPH_API_MAX_TIMEOUT_MS) {
            timeout_ms = RALPH_API_MAX_TIMEOUT_MS;
        }
    }

    /* Create model and parse */
    RalphLPModel *model = ralph_lp_create();
    if (!model) {
        free(problem);
        set_error_response(resp, 500, "Failed to create model");
        return 0;
    }

    const char *parse_error = NULL;
    int parse_result;

    if (strcmp(problem_format, "lp") == 0) {
        parse_result = ralph_api_parse_lp(problem, strlen(problem), model, &parse_error);
    } else {
        parse_result = ralph_api_parse_mps(problem, strlen(problem), model, &parse_error);
    }

    free(problem);
    problem = NULL;

    if (parse_result != 0) {
        ralph_lp_free(model);
        char err_buf[256];
        snprintf(err_buf, sizeof(err_buf), "Parse error: %s",
                 parse_error ? parse_error : "unknown error");
        set_error_response(resp, 400, err_buf);
        return 0;
    }

    /* Check size limits */
    int num_vars = ralph_lp_get_num_vars(model);
    int num_cons = ralph_lp_get_num_cons(model);
    int is_mip = ralph_lp_get_num_integer_vars(model) > 0;

    int max_vars = is_mip ? RALPH_API_MAX_VARS_MIP : RALPH_API_MAX_VARS_LP;
    int max_cons = is_mip ? RALPH_API_MAX_CONS_MIP : RALPH_API_MAX_CONS_LP;

    if (num_vars > max_vars || num_cons > max_cons) {
        ralph_lp_free(model);
        char err_buf[256];
        snprintf(err_buf, sizeof(err_buf),
                 "Problem too large: %d vars, %d constraints (max: %d vars, %d cons for %s)",
                 num_vars, num_cons, max_vars, max_cons, is_mip ? "MIP" : "LP");
        set_error_response(resp, 413, err_buf);
        return 0;
    }

    /* Set timeout */
    if (is_mip) {
        ralph_mip_set_dbl_param((RalphMIPModel *)model, "time_limit", timeout_ms / 1000.0);
    } else {
        ralph_lp_set_dbl_param(model, "time_limit", timeout_ms / 1000.0);
    }

    /* Solve */
    clock_t start = clock();
    int solve_result = is_mip
        ? ralph_mip_optimize((RalphMIPModel *)model)
        : ralph_lp_optimize(model);
    clock_t end = clock();
    double solve_time_ms = (double)(end - start) / CLOCKS_PER_SEC * 1000.0;

    (void)solve_result;

    RalphLPStatus status = is_mip
        ? ralph_mip_get_status((const RalphMIPModel *)model)
        : ralph_lp_get_status(model);

    /* Build response in appropriate format */
    char *buf = (char *)malloc(MAX_JSON_RESPONSE);
    if (!buf) {
        ralph_lp_free(model);
        set_error_response(resp, 500, "Memory allocation failed");
        return 0;
    }

    size_t body_len;
    const char *content_type;

    if (use_json) {
        /* JSON output */
        JsonBuilder jb;
        json_init(&jb, buf, MAX_JSON_RESPONSE);
        build_json_response(model, is_mip, status, solve_time_ms, &jb);
        body_len = jb.pos;
        content_type = CT_JSON;
    } else {
        /* SOL format output (using core ralph function) */
        int len = ralph_lp_write_solution_buf(model, buf, MAX_JSON_RESPONSE);
        body_len = len > 0 ? (size_t)len : 0;
        content_type = CT_TEXT;
    }

    ralph_lp_free(model);

    /* Set response */
    int http_status = 200;
    if (status == RALPH_LP_STATUS_TIME_LIMIT) {
        http_status = 408;
    }

    resp->status_code = http_status;
    resp->content_type = content_type;
    resp->body_len = body_len;
    resp->body = (uint8_t *)buf;

    return 0;
}

/* ============================================================================
 * Main Request Handler
 * ============================================================================ */

int ralph_api_handle(RalphAPIContext *ctx,
                     const RalphAPIRequest *req,
                     RalphAPIResponse *resp) {
    if (!ctx || !req || !resp) return -1;

    memset(resp, 0, sizeof(*resp));

    if (!req->path) {
        set_error_response(resp, 400, "Missing path");
        return 0;
    }

    /* Route based on method and path */
    int is_get = !req->method || strcmp(req->method, "GET") == 0;
    int is_post = req->method && strcmp(req->method, "POST") == 0;

    /* GET /api/v1/health */
    if (is_get && strcmp(req->path, "/api/v1/health") == 0) {
        return handle_health(ctx, resp);
    }

    /* GET /api/v1/formats */
    if (is_get && strcmp(req->path, "/api/v1/formats") == 0) {
        return handle_formats(ctx, resp);
    }

    /* POST /api/v1/solve */
    if (is_post && strcmp(req->path, "/api/v1/solve") == 0) {
        return handle_solve(ctx, req, resp);
    }

    /* Not found */
    set_error_response(resp, 404, "Endpoint not found");
    return 0;
}

void ralph_api_response_free(RalphAPIResponse *resp) {
    if (resp && resp->body) {
        free(resp->body);
        resp->body = NULL;
        resp->body_len = 0;
    }
}

/* ============================================================================
 * WASM Helper Functions
 * ============================================================================ */

int ralph_api_response_status(const RalphAPIResponse *resp) {
    return resp ? resp->status_code : 0;
}

const char *ralph_api_response_content_type(const RalphAPIResponse *resp) {
    return resp ? resp->content_type : "";
}

const uint8_t *ralph_api_response_body(const RalphAPIResponse *resp) {
    return resp ? resp->body : NULL;
}

size_t ralph_api_response_body_len(const RalphAPIResponse *resp) {
    return resp ? resp->body_len : 0;
}

/* ============================================================================
 * Version Information
 * ============================================================================ */

const char *ralph_api_version(void) {
    return RALPH_API_VERSION;
}
