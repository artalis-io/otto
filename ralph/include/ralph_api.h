/*
 * Ralph API Handler - Transport-agnostic request handling
 *
 * This module provides request handling logic that can be used by:
 * - Keel HTTP server (REST API)
 * - WASM exports (browser demo)
 * - Direct C API calls (testing)
 *
 * The API is designed for small LP/MIP problems, primarily for WASM demos
 * on the documentation site. For production problems, embed the Ralph library
 * directly.
 */

#ifndef RALPH_RALPH_API_H
#define RALPH_RALPH_API_H

#include <stddef.h>
#include <stdint.h>
#include "ralph_lp.h"
#include "sh_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * API Endpoint Documentation
 *
 * These annotations are parsed by scripts/build-api-docs.py to generate site/api.html
 * ============================================================================ */

/*@api
 * POST /api/v1/solve
 * Solve LP/MIP problem
 *
 * Solves a linear or mixed-integer programming problem. Accepts raw LP or MPS
 * format in the request body (use format query param), or JSON wrapper.
 *
 * @query format:string:lp Input format: "lp", "mps", or "json" (default: lp)
 * @query timeout_ms:int:5000 Timeout in milliseconds (max 30000)
 *
 * @returns text/plain Solution in SOL format (for lp/mps input)
 * @error 400 Invalid problem format or syntax error
 * @error 413 Problem too large (>100 vars/constraints for LP, >50 for MIP)
 * @error 408 Timeout exceeded
 *
 * @request_body lp
 * max: 5 x + 3 y
 *
 * subject to
 * wood:  2 x + 4 y <= 40
 * labor: 3 x + 2 y <= 24
 *
 * bounds
 * x >= 0
 * y >= 0
 *
 * end
 *
 * @response_text
 * solution status: OPTIMAL
 * objective value: 40.000000
 * x 8.000000
 * y 0.000000
 *
 * @example curl -X POST "http://localhost:8084/api/v1/solve?format=lp" -d @problem.lp
 *
 * @demo json
 * @demo_title Solve a small LP problem using Ralph WASM. No server required.
 * @demo_textarea problem:lp
 * max: 5 x + 3 y
 *
 * subject to
 * wood:  2 x + 4 y <= 40
 * labor: 3 x + 2 y <= 24
 *
 * bounds
 * x >= 0
 * y >= 0
 *
 * end
 */

/*@api
 * GET /api/v1/formats
 * List supported input formats
 *
 * @returns application/json List of supported problem formats
 * @response_json
 * {
 *   "formats": [
 *     {"id": "lp", "name": "CPLEX LP", "description": "CPLEX LP file format"},
 *     {"id": "mps", "name": "MPS", "description": "Mathematical Programming System format"}
 *   ]
 * }
 *
 * @demo json
 * @demo_title Get supported formats
 * @demo_fetch /api/v1/formats
 */

/*@api
 * GET /api/v1/health
 * Health check
 *
 * @returns application/json Health status
 * @response_json
 * {"status": "ok", "version": "1.0.0"}
 */

/*@wasm
 * @export ralph_api_init
 * @export ralph_api_free
 * @export ralph_api_ready
 * @export ralph_api_handle
 * @export ralph_api_response_status
 * @export ralph_api_response_content_type
 * @export ralph_api_response_body
 * @export ralph_api_response_body_len
 * @export ralph_api_version
 * @export malloc
 * @export free
 */

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Size limits for API (larger problems should use the library directly) */
#define RALPH_API_MAX_VARS_LP    100
#define RALPH_API_MAX_CONS_LP    100
#define RALPH_API_MAX_VARS_MIP   50
#define RALPH_API_MAX_CONS_MIP   50

/* Timeout limits */
#define RALPH_API_DEFAULT_TIMEOUT_MS  5000
#define RALPH_API_MAX_TIMEOUT_MS      30000

/* ============================================================================
 * API Context
 * ============================================================================ */

/*
 * Opaque API context - stateless for Ralph (no data to preload).
 */
typedef struct RalphAPIContext RalphAPIContext;

/*
 * Create API context.
 *
 * Ralph API is stateless (unlike Carta/Velo which load map data).
 * The context exists for consistency with other OTTO APIs.
 *
 * Returns NULL on failure.
 */
RalphAPIContext *ralph_api_create(void);

/*
 * Free API context.
 */
void ralph_api_free(RalphAPIContext *ctx);

/*
 * Check if API is ready to handle requests.
 */
int ralph_api_ready(RalphAPIContext *ctx);

/* ============================================================================
 * Request/Response
 * ============================================================================ */

/*
 * Request and response are the shared, transport-agnostic types from
 * <sh_api.h>. Ralph used to define its own identical pair; so did the
 * other five modules. See docs/roadmaps/transport.md.
 */

/*
 * Handle an API request.
 *
 * Routes the request based on method and path, parses the problem,
 * solves it, and generates the response.
 *
 * Supported endpoints:
 *   POST /api/v1/solve      - Solve LP/MIP problem
 *   GET  /api/v1/formats    - List supported formats
 *   GET  /api/v1/health     - Health check
 *
 * Returns 0 on success (response filled in), -1 on internal error.
 */
int ralph_api_handle(void *ctx,
                     const ShApiRequest *req,
                     ShApiResponse *resp);

/* Response bodies are freed with sh_api_response_free() from <sh_api.h>. */

/* ============================================================================
 * WASM Helper Functions
 *
 * These functions provide easy access to response fields from JavaScript.
 * ============================================================================ */

/*
 * Get response status code.
 */
int ralph_api_response_status(const ShApiResponse *resp);

/*
 * Get response content type (static string, do not free).
 */
const char *ralph_api_response_content_type(const ShApiResponse *resp);

/*
 * Get response body pointer.
 */
const uint8_t *ralph_api_response_body(const ShApiResponse *resp);

/*
 * Get response body length.
 */
size_t ralph_api_response_body_len(const ShApiResponse *resp);

/* ============================================================================
 * LP/MPS Parsing from String
 * ============================================================================ */

/*
 * Parse LP format from string into a Ralph model.
 *
 * Returns 0 on success, -1 on parse error.
 * On error, error_msg (if not NULL) points to a static error description.
 */
int ralph_api_parse_lp(const char *lp_string, size_t len,
                       RalphLPModel *model,
                       const char **error_msg);

/*
 * Parse MPS format from string into a Ralph model.
 *
 * Returns 0 on success, -1 on parse error.
 * On error, error_msg (if not NULL) points to a static error description.
 */
int ralph_api_parse_mps(const char *mps_string, size_t len,
                        RalphLPModel *model,
                        const char **error_msg);

/* ============================================================================
 * Version Information
 * ============================================================================ */

/*
 * Get Ralph API version string.
 */
const char *ralph_api_version(void);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_RALPH_API_H */
