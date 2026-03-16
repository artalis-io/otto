#include "lp_error.h"

#include <string.h>
#include <stdio.h>

static void lp_error_reset_struct(RalphAPIError *error) {
    if (!error) return;
    memset(error, 0, sizeof(*error));
    error->domain = RALPH_ERROR_DOMAIN_NONE;
    error->code = RALPH_ERROR_CODE_NONE;
    error->status_hint = RALPH_STATUS_UNKNOWN;
    error->api_id = RALPH_ERROR_API_NONE;
}

static void lp_error_fill_struct(RalphAPIError *error,
                                 RalphErrorDomain domain,
                                 RalphErrorCode code,
                                 RalphStatus status_hint,
                                 RalphErrorAPIId api_id,
                                 int detail_i0,
                                 int detail_i1,
                                 const char *message) {
    if (!error) return;
    lp_error_reset_struct(error);
    error->domain = domain;
    error->code = code;
    error->status_hint = status_hint;
    error->api_id = api_id;
    error->detail_i0 = detail_i0;
    error->detail_i1 = detail_i1;
    if (message && message[0] != '\0') {
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
}

void lp_error_state_init(LPAPIErrorState *state) {
    if (!state) return;
    lp_error_reset_struct(&state->error);
    state->valid = 0;
}

void lp_error_state_clear(LPAPIErrorState *state) {
    if (!state) return;
    lp_error_reset_struct(&state->error);
    state->valid = 0;
}

int lp_error_state_get(const LPAPIErrorState *state, RalphAPIError *out) {
    if (!state || !out) return -1;
    if (!state->valid) return -1;
    *out = state->error;
    return 0;
}

void lp_error_state_set(LPAPIErrorState *state,
                        RalphErrorDomain domain,
                        RalphErrorCode code,
                        RalphStatus status_hint,
                        RalphErrorAPIId api_id,
                        int detail_i0,
                        int detail_i1,
                        const char *message) {
    if (!state) return;
    lp_error_fill_struct(&state->error,
                         domain,
                         code,
                         status_hint,
                         api_id,
                         detail_i0,
                         detail_i1,
                         message);
    state->valid = 1;
}

static _Thread_local RalphAPIError g_lp_error_tls;
static _Thread_local int g_lp_error_tls_valid = 0;

void lp_error_tls_clear(void) {
    lp_error_reset_struct(&g_lp_error_tls);
    g_lp_error_tls_valid = 0;
}

int lp_error_tls_get(RalphAPIError *out) {
    if (!out) return -1;
    if (!g_lp_error_tls_valid) return -1;
    *out = g_lp_error_tls;
    return 0;
}

void lp_error_tls_set(RalphErrorDomain domain,
                      RalphErrorCode code,
                      RalphStatus status_hint,
                      RalphErrorAPIId api_id,
                      int detail_i0,
                      int detail_i1,
                      const char *message) {
    lp_error_fill_struct(&g_lp_error_tls,
                         domain,
                         code,
                         status_hint,
                         api_id,
                         detail_i0,
                         detail_i1,
                         message);
    g_lp_error_tls_valid = 1;
}

const char* lp_error_domain_string(RalphErrorDomain domain) {
    switch (domain) {
        case RALPH_ERROR_DOMAIN_NONE: return "NONE";
        case RALPH_ERROR_DOMAIN_ARGUMENT: return "ARGUMENT";
        case RALPH_ERROR_DOMAIN_STATE: return "STATE";
        case RALPH_ERROR_DOMAIN_RANGE: return "RANGE";
        case RALPH_ERROR_DOMAIN_PARAMETER: return "PARAMETER";
        case RALPH_ERROR_DOMAIN_MEMORY: return "MEMORY";
        case RALPH_ERROR_DOMAIN_IO: return "IO";
        case RALPH_ERROR_DOMAIN_PARSE: return "PARSE";
        case RALPH_ERROR_DOMAIN_SOLVER: return "SOLVER";
        case RALPH_ERROR_DOMAIN_EXTERNAL: return "EXTERNAL";
        case RALPH_ERROR_DOMAIN_INTERNAL: return "INTERNAL";
        default: return "UNKNOWN";
    }
}

const char* lp_error_code_string(RalphErrorCode code) {
    switch (code) {
        case RALPH_ERROR_CODE_NONE: return "NONE";
        case RALPH_ERROR_CODE_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case RALPH_ERROR_CODE_NULL_POINTER: return "NULL_POINTER";
        case RALPH_ERROR_CODE_OUT_OF_RANGE: return "OUT_OF_RANGE";
        case RALPH_ERROR_CODE_NOT_AVAILABLE: return "NOT_AVAILABLE";
        case RALPH_ERROR_CODE_UNKNOWN_PARAMETER: return "UNKNOWN_PARAMETER";
        case RALPH_ERROR_CODE_PARAMETER_SCOPE_MISMATCH: return "PARAMETER_SCOPE_MISMATCH";
        case RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID: return "PARAMETER_VALUE_INVALID";
        case RALPH_ERROR_CODE_ALLOCATION_FAILED: return "ALLOCATION_FAILED";
        case RALPH_ERROR_CODE_IO_OPEN_FAILED: return "IO_OPEN_FAILED";
        case RALPH_ERROR_CODE_IO_READ_FAILED: return "IO_READ_FAILED";
        case RALPH_ERROR_CODE_IO_WRITE_FAILED: return "IO_WRITE_FAILED";
        case RALPH_ERROR_CODE_PARSE_FAILED: return "PARSE_FAILED";
        case RALPH_ERROR_CODE_SOLVE_FAILED: return "SOLVE_FAILED";
        case RALPH_ERROR_CODE_EXTERNAL_DISPATCH_FAILED: return "EXTERNAL_DISPATCH_FAILED";
        case RALPH_ERROR_CODE_EXTERNAL_EXECUTION_FAILED: return "EXTERNAL_EXECUTION_FAILED";
        case RALPH_ERROR_CODE_NUMERICAL_FAILURE: return "NUMERICAL_FAILURE";
        case RALPH_ERROR_CODE_LIMIT_REACHED: return "LIMIT_REACHED";
        case RALPH_ERROR_CODE_INTERNAL_FAILURE: return "INTERNAL_FAILURE";
        default: return "UNKNOWN";
    }
}

const char* lp_error_api_string(RalphErrorAPIId api_id) {
    switch (api_id) {
        case RALPH_ERROR_API_NONE: return "NONE";
        case RALPH_ERROR_API_LIFECYCLE: return "LIFECYCLE";
        case RALPH_ERROR_API_MODEL_BUILD: return "MODEL_BUILD";
        case RALPH_ERROR_API_MODEL_EDIT: return "MODEL_EDIT";
        case RALPH_ERROR_API_SOLVE: return "SOLVE";
        case RALPH_ERROR_API_SOLUTION_QUERY: return "SOLUTION_QUERY";
        case RALPH_ERROR_API_PARAMETER: return "PARAMETER";
        case RALPH_ERROR_API_BASIS: return "BASIS";
        case RALPH_ERROR_API_IO: return "IO";
        case RALPH_ERROR_API_EXTERNAL: return "EXTERNAL";
        case RALPH_ERROR_API_PARSE: return "PARSE";
        case RALPH_ERROR_API_BENDERS: return "BENDERS";
        default: return "UNKNOWN";
    }
}

const char* lp_error_message(const RalphAPIError *error) {
    if (!error) return "";
    return error->message;
}
