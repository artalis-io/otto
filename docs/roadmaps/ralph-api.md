# Ralph API Implementation Plan

**Zero-dependency LP/MIP solver with transport-agnostic API**

Expose Ralph as a transport-agnostic API for solving small LP/MIP problems, primarily for WASM demos on the documentation site. Follows the patterns established by Carta, Velo, Locus.

## Rationale

### Arguments FOR a Ralph API

1. **Consistency** - Every other engine has an API (Carta:8081, Velo:8082, Locus:8083, FuelWise:8080). Ralph being API-less breaks the pattern.

2. **"The demo IS the product"** - A WASM demo where users type an LP problem and see it solved instantly would be compelling for the docs. Shows the solver actually works.

3. **Educational value** - Interactive LP solving helps users learn optimization concepts.

4. **Testing/debugging** - Developers could test formulations via curl before embedding Ralph.

5. **Completeness** - The site/api.html would cover all engines, not "all except the core solver."

### Scope Limitations

Ralph is fundamentally a library meant to be embedded (in FuelWise, Surge, etc.). The API is scoped for demos and small problems, not production solving.

| Use Case | Supported | Notes |
|----------|-----------|-------|
| WASM demo on docs | ✅ | Primary goal |
| Small LP/MIP via curl | ✅ | Testing, learning |
| MPS format input | ✅ | Standard format |
| LP format input | ✅ | Human readable |
| Large production problems | ❌ | Embed library directly |
| Warm starts | ❌ | Stateful, use library |
| Callbacks (branch & bound) | ❌ | Use library directly |

---

## Naming Convention

Ralph uses `ralph_*` prefix (not a 2-letter abbreviation like other modules). The API follows this pattern:

| Module | Core Prefix | API Prefix |
|--------|-------------|------------|
| Carta | `ct_` | `ct_api_*` |
| Velo | `vl_` | `vl_api_*` |
| Locus | `lc_` | `lc_api_*` |
| FuelWise | `fw_` | `fw_api_*` |
| **Ralph** | `ralph_` | `ralph_api_*` |

Ralph keeps its full name because it's the foundational solver that everything else builds on.

---

## API Design

### Port Assignment

| Module | Port |
|--------|------|
| FuelWise | 8080 |
| Carta | 8081 |
| Velo | 8082 |
| Locus | 8083 |
| **Ralph** | **8084** |

### Endpoints

```
POST /api/v1/solve      - Solve LP/MIP from JSON-embedded problem
GET  /api/v1/health     - Health check
GET  /api/v1/formats    - List supported input formats
```

### Request Format

```json
{
  "format": "lp",           // "lp" or "mps"
  "problem": "...",         // Problem text (LP or MPS format)
  "timeout_ms": 5000,       // Optional, default 5000
  "options": {              // Optional
    "presolve": true,
    "scaling": true
  }
}
```

### Response Format

```json
{
  "status": "optimal",      // "optimal", "infeasible", "unbounded", "timeout", "error"
  "objective": 42.5,
  "variables": {
    "x1": 10.0,
    "x2": 5.5
  },
  "solve_time_ms": 12,
  "iterations": 23,
  "message": null           // Error message if status == "error"
}
```

### Size Limits

| Limit | Value |
|-------|-------|
| Max variables (LP) | 100 |
| Max constraints (LP) | 100 |
| Max variables (MIP) | 50 |
| Max constraints (MIP) | 50 |
| Default timeout | 5000 ms |
| Max timeout | 30000 ms |

### LP Format

```
/* Production Planning Example */

/* Maximize profit */
max: 5 chairs + 3 tables;

/* Wood constraint (40 units available) */
wood: 2 chairs + 4 tables <= 40;

/* Labor constraint (24 hours available) */
labor: 3 chairs + 2 tables <= 24;

/* Market demand limits */
chairs <= 8;
tables <= 6;

/* Non-negativity */
chairs >= 0;
tables >= 0;
```

Supported syntax:
- `min:` / `max:` objective
- `<=`, `>=`, `=` constraints
- Named constraints (optional): `name: expr <= rhs;`
- Variable bounds: `x >= 0;`, `x <= 10;`
- Comments: `/* ... */` or `//`

---

## File Structure

Following `/api-audit` transport-agnostic patterns:

```
ralph/
├── include/
│   ├── ralph.h              # Existing solver API
│   └── ralph_api.h          # NEW: Transport-agnostic API handler
├── src/
│   ├── simplex.c            # Existing
│   ├── ...
│   ├── ralph_api.c          # NEW: API handler implementation
│   └── ralph_parse_lp.c     # NEW: LP format parser
├── api/
│   ├── Makefile             # NEW
│   └── src/
│       └── main.c           # NEW: Mongoose wrapper
└── wasm/
    ├── Makefile             # NEW
    └── ralph_wasm_api.c     # NEW: WASM wrapper
```

---

## Phase 1: Transport-Agnostic Handler (2-3 days)

### 1.1 Create `ralph/include/ralph_api.h`

```c
#ifndef RALPH_API_H
#define RALPH_API_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*@api
 * POST /api/v1/solve
 * Solve a linear or mixed-integer program
 *
 * Accepts LP or MPS format problems embedded in JSON. Returns optimal solution
 * or status (infeasible, unbounded, timeout).
 *
 * @body format:string:lp Input format ("lp" or "mps")
 * @body problem:string The problem in LP or MPS format
 * @body timeout_ms:number:5000 Maximum solve time in milliseconds
 *
 * @returns application/json Solution with status, objective, and variable values
 * @error 400 Invalid problem format or parse error
 * @error 413 Problem too large (max 100 vars, 100 constraints)
 * @error 408 Solve timeout exceeded
 *
 * @response_json
 * {
 *   "status": "optimal",
 *   "objective": 42.5,
 *   "variables": {"x1": 10.0, "x2": 5.5},
 *   "solve_time_ms": 12,
 *   "iterations": 23
 * }
 *
 * @example
 * curl -X POST http://localhost:8084/api/v1/solve \
 *   -H "Content-Type: application/json" \
 *   -d '{"format":"lp","problem":"min: 3x + 2y; x + y >= 4; x >= 0; y >= 0;"}'
 *
 * @demo json
 * @demo_title Solve a linear program using WASM
 * @demo_input problem:textarea:max: 5 chairs + 3 tables;\nwood: 2 chairs + 4 tables <= 40;\nlabor: 3 chairs + 2 tables <= 24;\nchairs >= 0;\ntables >= 0;
 * @demo_input format:select:lp:lp,mps
 * @demo_input timeout_ms:number:5000:100:30000
 */

/*@api
 * GET /api/v1/health
 * Health check endpoint
 *
 * @returns application/json Health status
 *
 * @response_json
 * {"status": "ok", "version": "1.0.0"}
 */

/*@api
 * GET /api/v1/formats
 * List supported input formats
 *
 * @returns application/json Supported formats with examples
 *
 * @response_json
 * {
 *   "formats": ["lp", "mps"],
 *   "examples": {
 *     "lp": "min: 3x + 2y; x + y >= 4;",
 *     "mps": "NAME example\nROWS\n..."
 *   }
 * }
 */

/*@wasm
 * @export ralph_api_init
 * @export ralph_api_free
 * @export ralph_api_handle
 * @export ralph_api_ready
 * @export ralph_response_status
 * @export ralph_response_content_type
 * @export ralph_response_body
 * @export ralph_response_body_len
 * @export ralph_response_free
 * @export malloc
 * @export free
 */

/* Opaque context */
typedef struct RalphAPIContext RalphAPIContext;

/* Request (transport-agnostic) */
typedef struct {
    const char *method;      /* "GET" or "POST" */
    const char *path;        /* "/api/v1/solve" */
    const char *query;       /* Query string (for GET) */
    const char *body;        /* Request body (for POST) */
    size_t body_len;
    const char *content_type;
} RalphAPIRequest;

/* Response */
typedef struct {
    int status_code;
    const char *content_type;
    uint8_t *body;
    size_t body_len;
} RalphAPIResponse;

/* Lifecycle */
RalphAPIContext *ralph_api_create(void);
void ralph_api_free(RalphAPIContext *ctx);
bool ralph_api_ready(RalphAPIContext *ctx);

/* Handler (transport-agnostic) */
int ralph_api_handle(RalphAPIContext *ctx,
                     const RalphAPIRequest *req,
                     RalphAPIResponse *resp);

/* Response cleanup */
void ralph_api_response_free(RalphAPIResponse *resp);

/* Configuration */
void ralph_api_set_max_vars(RalphAPIContext *ctx, int max);
void ralph_api_set_max_constraints(RalphAPIContext *ctx, int max);
void ralph_api_set_default_timeout(RalphAPIContext *ctx, int timeout_ms);

#endif /* RALPH_API_H */
```

### 1.2 Create `ralph/src/ralph_api.c`

Core handler implementation:

```c
#include "ralph_api.h"
#include "ralph.h"
#include <string.h>
#include <stdlib.h>

struct RalphAPIContext {
    int max_vars;
    int max_constraints;
    int default_timeout_ms;
};

RalphAPIContext *ralph_api_create(void) {
    RalphAPIContext *ctx = calloc(1, sizeof(RalphAPIContext));
    ctx->max_vars = 100;
    ctx->max_constraints = 100;
    ctx->default_timeout_ms = 5000;
    return ctx;
}

void ralph_api_free(RalphAPIContext *ctx) {
    free(ctx);
}

bool ralph_api_ready(RalphAPIContext *ctx) {
    return ctx != NULL;
}

int ralph_api_handle(RalphAPIContext *ctx,
                     const RalphAPIRequest *req,
                     RalphAPIResponse *resp) {

    if (strcmp(req->path, "/api/v1/health") == 0) {
        return handle_health(ctx, req, resp);
    }

    if (strcmp(req->path, "/api/v1/formats") == 0) {
        return handle_formats(ctx, req, resp);
    }

    if (strcmp(req->path, "/api/v1/solve") == 0) {
        if (strcmp(req->method, "POST") != 0) {
            return error_response(resp, 405, "Method not allowed");
        }
        return handle_solve(ctx, req, resp);
    }

    return error_response(resp, 404, "Not found");
}

static int handle_solve(RalphAPIContext *ctx,
                        const RalphAPIRequest *req,
                        RalphAPIResponse *resp) {
    /* Parse JSON request */
    char *format = NULL;
    char *problem = NULL;
    int timeout_ms = ctx->default_timeout_ms;

    if (parse_solve_request(req->body, req->body_len,
                            &format, &problem, &timeout_ms) != 0) {
        return error_response(resp, 400, "Invalid JSON request");
    }

    /* Parse problem */
    RalphModel *model = NULL;
    if (strcmp(format, "lp") == 0) {
        model = ralph_parse_lp(problem);
    } else if (strcmp(format, "mps") == 0) {
        model = ralph_parse_mps(problem);
    } else {
        free(format);
        free(problem);
        return error_response(resp, 400, "Unknown format (use 'lp' or 'mps')");
    }

    free(format);
    free(problem);

    if (!model) {
        return error_response(resp, 400, "Failed to parse problem");
    }

    /* Check size limits */
    if (ralph_get_num_vars(model) > ctx->max_vars ||
        ralph_get_num_cons(model) > ctx->max_constraints) {
        ralph_free(model);
        return error_response(resp, 413, "Problem too large");
    }

    /* Solve with timeout */
    ralph_set_timeout(model, timeout_ms);

    int64_t start = get_time_ms();
    int status = ralph_optimize(model);
    int64_t elapsed = get_time_ms() - start;

    /* Build response */
    build_solve_response(resp, model, status, elapsed);

    ralph_free(model);

    return 0;
}

static int handle_health(RalphAPIContext *ctx,
                         const RalphAPIRequest *req,
                         RalphAPIResponse *resp) {
    const char *json = "{\"status\":\"ok\",\"version\":\"1.0.0\"}";
    resp->status_code = 200;
    resp->content_type = "application/json";
    resp->body = (uint8_t *)strdup(json);
    resp->body_len = strlen(json);
    return 0;
}

static int handle_formats(RalphAPIContext *ctx,
                          const RalphAPIRequest *req,
                          RalphAPIResponse *resp) {
    const char *json =
        "{\"formats\":[\"lp\",\"mps\"],"
        "\"examples\":{"
        "\"lp\":\"min: 3x + 2y; x + y >= 4; x >= 0; y >= 0;\","
        "\"mps\":\"NAME example\\nROWS\\n N obj\\n...\"}}";
    resp->status_code = 200;
    resp->content_type = "application/json";
    resp->body = (uint8_t *)strdup(json);
    resp->body_len = strlen(json);
    return 0;
}

void ralph_api_response_free(RalphAPIResponse *resp) {
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
}
```

### 1.3 Create `ralph/src/ralph_parse_lp.c`

Simple LP format parser:

```c
/* Parse LP format:
 *
 *   min: 3 x1 + 2 x2;
 *   c1: x1 + x2 >= 4;
 *   c2: 2 x1 + x2 <= 10;
 *   x1 >= 0;
 *   x2 >= 0;
 *
 * Supports:
 *   - min/max objective
 *   - <=, >=, = constraints
 *   - Variable bounds
 *   - Comments with /* ... */ or //
 */

#include "ralph.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

RalphModel *ralph_parse_lp(const char *lp_text) {
    RalphModel *model = ralph_create();

    /* Tokenize and parse */
    /* ... implementation ... */

    return model;
}

RalphModel *ralph_parse_mps(const char *mps_text) {
    RalphModel *model = ralph_create();

    /* Parse MPS format */
    /* ... implementation ... */

    return model;
}
```

---

## Phase 2: Mongoose HTTP Server (1 day)

### 2.1 Create `ralph/api/src/main.c`

```c
#include "mongoose.h"
#include "ralph_api.h"
#include <stdio.h>
#include <signal.h>

static RalphAPIContext *s_ctx;
static volatile sig_atomic_t s_running = 1;

static void signal_handler(int sig) {
    s_running = 0;
}

static char *extract_path(struct mg_http_message *hm) {
    static char path[256];
    int len = hm->uri.len < 255 ? hm->uri.len : 255;
    memcpy(path, hm->uri.buf, len);
    path[len] = '\0';
    /* Strip query string */
    char *q = strchr(path, '?');
    if (q) *q = '\0';
    return path;
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = ev_data;

        /* Build transport-agnostic request */
        char method[8] = {0};
        snprintf(method, sizeof(method), "%.*s", (int)hm->method.len, hm->method.buf);

        RalphAPIRequest req = {
            .method = method,
            .path = extract_path(hm),
            .query = hm->query.buf,
            .body = hm->body.buf,
            .body_len = hm->body.len,
            .content_type = "application/json"
        };

        /* Call handler */
        RalphAPIResponse resp = {0};
        ralph_api_handle(s_ctx, &req, &resp);

        /* Send HTTP response */
        mg_http_reply(c, resp.status_code,
                      "Content-Type: %s\r\n"
                      "Access-Control-Allow-Origin: *\r\n",
                      resp.content_type,
                      resp.body_len, resp.body);

        ralph_api_response_free(&resp);
    }
}

int main(int argc, char *argv[]) {
    const char *port = "8084";

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = argv[++i];
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    s_ctx = ralph_api_create();

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    char url[64];
    snprintf(url, sizeof(url), "http://0.0.0.0:%s", port);
    mg_http_listen(&mgr, url, ev_handler, NULL);

    printf("Ralph API server running on http://localhost:%s\n", port);
    printf("Endpoints:\n");
    printf("  POST /api/v1/solve   - Solve LP/MIP\n");
    printf("  GET  /api/v1/health  - Health check\n");
    printf("  GET  /api/v1/formats - List formats\n");

    while (s_running) {
        mg_mgr_poll(&mgr, 1000);
    }

    mg_mgr_free(&mgr);
    ralph_api_free(s_ctx);

    return 0;
}
```

### 2.2 Create `ralph/api/Makefile`

```makefile
CC = gcc
CFLAGS = -O2 -Wall -I../include -I../../shared/include -I../../vendor/mongoose

SOURCES = src/main.c \
          ../src/ralph_api.c \
          ../src/ralph_parse_lp.c \
          ../../vendor/mongoose/mongoose.c

TARGET = ralph-solver

all: $(TARGET)

$(TARGET): $(SOURCES) ../build/libralph.a
	$(CC) $(CFLAGS) -o $@ $(SOURCES) -L../build -lralph -lm -lpthread

clean:
	rm -f $(TARGET)

.PHONY: all clean
```

---

## Phase 3: WASM Build (1 day)

### 3.1 Create `ralph/wasm/ralph_wasm_api.c`

```c
#include <emscripten.h>
#include "ralph_api.h"
#include <string.h>
#include <stdlib.h>

static RalphAPIContext *g_ctx = NULL;
static RalphAPIResponse g_last_response = {0};

EMSCRIPTEN_KEEPALIVE
int ralph_api_init(void) {
    g_ctx = ralph_api_create();
    ralph_api_set_default_timeout(g_ctx, 5000);
    return g_ctx ? 0 : -1;
}

EMSCRIPTEN_KEEPALIVE
void ralph_api_free_wasm(void) {
    if (g_ctx) {
        ralph_api_free(g_ctx);
        g_ctx = NULL;
    }
}

EMSCRIPTEN_KEEPALIVE
int ralph_api_ready_wasm(void) {
    return g_ctx != NULL;
}

EMSCRIPTEN_KEEPALIVE
int ralph_api_handle_wasm(const char *method, const char *path,
                          const char *body, int body_len) {
    if (g_last_response.body) {
        ralph_api_response_free(&g_last_response);
        memset(&g_last_response, 0, sizeof(g_last_response));
    }

    RalphAPIRequest req = {
        .method = method,
        .path = path,
        .query = NULL,
        .body = body,
        .body_len = body_len,
        .content_type = "application/json"
    };

    return ralph_api_handle(g_ctx, &req, &g_last_response);
}

EMSCRIPTEN_KEEPALIVE
int ralph_response_status(void) {
    return g_last_response.status_code;
}

EMSCRIPTEN_KEEPALIVE
const char *ralph_response_content_type(void) {
    return g_last_response.content_type;
}

EMSCRIPTEN_KEEPALIVE
const uint8_t *ralph_response_body(void) {
    return g_last_response.body;
}

EMSCRIPTEN_KEEPALIVE
int ralph_response_body_len(void) {
    return (int)g_last_response.body_len;
}

EMSCRIPTEN_KEEPALIVE
void ralph_response_free(void) {
    ralph_api_response_free(&g_last_response);
}
```

### 3.2 Create `ralph/wasm/Makefile`

```makefile
EMCC = emcc
CFLAGS = -O2 -I../include -I../../shared/include

SOURCES = ralph_wasm_api.c \
          ../src/ralph_api.c \
          ../src/ralph_parse_lp.c \
          ../src/simplex.c \
          ../src/lu.c \
          ../src/presolve.c \
          ../src/branch_bound.c \
          ../src/scaling.c \
          ../src/model.c

TARGET = build/ralph-api-demo.js

EXPORTS = _ralph_api_init,_ralph_api_free_wasm,_ralph_api_ready_wasm,_ralph_api_handle_wasm,\
          _ralph_response_status,_ralph_response_content_type,_ralph_response_body,\
          _ralph_response_body_len,_ralph_response_free,_malloc,_free

all: build $(TARGET)

build:
	mkdir -p build

$(TARGET): $(SOURCES)
	$(EMCC) $(CFLAGS) $^ -o $@ \
		-s EXPORTED_FUNCTIONS='["$(EXPORTS)"]' \
		-s EXPORTED_RUNTIME_METHODS='["UTF8ToString","stringToUTF8","lengthBytesUTF8"]' \
		-s MODULARIZE=1 \
		-s EXPORT_NAME='RalphAPIDemo' \
		-s ALLOW_MEMORY_GROWTH=1 \
		-s INITIAL_MEMORY=16777216

clean:
	rm -rf build

.PHONY: all clean
```

---

## Phase 4: JavaScript Handler (1 day)

### 4.1 Create `site/js/handlers/ralph.js`

```javascript
let ralphDemo = null;

async function initRalphDemo() {
    enableBtn('ralph-solve-try-btn', 'Solve');
    const output = document.getElementById('ralph-output');
    if (output) {
        output.textContent = 'Ready. Enter an LP problem above and click Solve.';
    }
}

function handleRalphError(err) {
    disableBtn('ralph-solve-try-btn');
    const output = document.getElementById('ralph-output');
    if (output) {
        output.textContent = 'WASM failed to load: ' + err;
    }
}

async function solveLP() {
    if (!ralphDemo || !ralphDemo.isReady()) {
        console.error('Ralph WASM not ready');
        return;
    }

    const problemEl = document.getElementById('ralph-problem');
    const formatEl = document.getElementById('ralph-format');
    const timeoutEl = document.getElementById('ralph-timeout_ms');
    const output = document.getElementById('ralph-output');

    if (!problemEl || !output) return;

    const problem = problemEl.value;
    const format = formatEl ? formatEl.value : 'lp';
    const timeout = timeoutEl ? parseInt(timeoutEl.value) || 5000 : 5000;

    output.textContent = 'Solving...';

    try {
        const request = JSON.stringify({
            format: format,
            problem: problem,
            timeout_ms: timeout
        });

        const result = await ralphDemo.solve(request);

        if (result.status === 'optimal') {
            let text = `Status: OPTIMAL\n`;
            text += `Objective: ${result.objective.toFixed(6)}\n`;
            text += `Solve time: ${result.solve_time_ms} ms\n`;
            text += `Iterations: ${result.iterations}\n\n`;
            text += `Variables:\n`;
            for (const [name, value] of Object.entries(result.variables)) {
                text += `  ${name} = ${value.toFixed(6)}\n`;
            }
            output.textContent = text;
        } else if (result.status === 'infeasible') {
            output.textContent = 'Status: INFEASIBLE\n\nThe problem has no feasible solution.';
        } else if (result.status === 'unbounded') {
            output.textContent = 'Status: UNBOUNDED\n\nThe objective can be improved indefinitely.';
        } else if (result.status === 'timeout') {
            output.textContent = `Status: TIMEOUT\n\nSolve exceeded ${timeout} ms limit.`;
        } else {
            output.textContent = `Status: ${result.status.toUpperCase()}\n${result.message || ''}`;
        }
    } catch (err) {
        output.textContent = 'Error: ' + err.message;
    }
}
```

### 4.2 Create `site/js/ralph-api-demo.js` (WASM wrapper)

```javascript
class RalphDemo {
    constructor(module) {
        this.module = module;
        this.ready = false;
    }

    async init() {
        const result = this.module._ralph_api_init();
        this.ready = (result === 0);
        return this.ready;
    }

    isReady() {
        return this.ready && this.module._ralph_api_ready_wasm();
    }

    async solve(requestJson) {
        const method = "POST";
        const path = "/api/v1/solve";

        // Allocate strings in WASM memory
        const methodLen = this.module.lengthBytesUTF8(method) + 1;
        const pathLen = this.module.lengthBytesUTF8(path) + 1;
        const bodyLen = this.module.lengthBytesUTF8(requestJson) + 1;

        const methodPtr = this.module._malloc(methodLen);
        const pathPtr = this.module._malloc(pathLen);
        const bodyPtr = this.module._malloc(bodyLen);

        this.module.stringToUTF8(method, methodPtr, methodLen);
        this.module.stringToUTF8(path, pathPtr, pathLen);
        this.module.stringToUTF8(requestJson, bodyPtr, bodyLen);

        try {
            this.module._ralph_api_handle_wasm(methodPtr, pathPtr, bodyPtr, bodyLen - 1);

            const status = this.module._ralph_response_status();
            const bodyPtrResult = this.module._ralph_response_body();
            const bodyLenResult = this.module._ralph_response_body_len();

            const responseText = this.module.UTF8ToString(bodyPtrResult, bodyLenResult);

            this.module._ralph_response_free();

            if (status !== 200) {
                throw new Error(`HTTP ${status}: ${responseText}`);
            }

            return JSON.parse(responseText);
        } finally {
            this.module._free(methodPtr);
            this.module._free(pathPtr);
            this.module._free(bodyPtr);
        }
    }

    free() {
        if (this.module) {
            this.module._ralph_api_free_wasm();
            this.ready = false;
        }
    }
}

// Factory function for consistent API with other demos
async function createRalphDemo() {
    const module = await RalphAPIDemo();
    const demo = new RalphDemo(module);
    await demo.init();
    return demo;
}
```

### 4.3 Update `site/api-config.json`

Add Ralph entry:

```json
{
  "id": "ralph",
  "name": "Ralph Solver",
  "icon": "📐",
  "port": 8084,
  "description": "Zero-dependency LP/MIP solver (Revised Simplex, Branch & Bound)",
  "header_file": "ralph/include/ralph_api.h",
  "wasm": {
    "enabled": true,
    "script": "wasm/ralph-api-demo.js",
    "wrapper": "js/ralph-api-demo.js",
    "factory_name": "RalphAPIDemo",
    "class_name": "RalphDemo",
    "handlers_file": "js/handlers/ralph.js",
    "init_function": "initRalphDemo",
    "error_function": "handleRalphError",
    "buttons": {
      "ralph-solve-try-btn": "solveLP"
    }
  }
}
```

---

## Phase 5: Demo Example (0.5 day)

### Default LP Problem for Demo

```
/* Production Planning Example
 *
 * A factory makes chairs and tables.
 * Each chair needs 2 units of wood and 3 hours of labor.
 * Each table needs 4 units of wood and 2 hours of labor.
 * We have 40 units of wood and 24 hours of labor.
 * Chairs sell for $5 profit, tables for $3.
 *
 * How many of each should we make?
 */

max: 5 chairs + 3 tables;

/* Resource constraints */
wood:  2 chairs + 4 tables <= 40;
labor: 3 chairs + 2 tables <= 24;

/* Non-negativity */
chairs >= 0;
tables >= 0;
```

### Expected Output

```
Status: OPTIMAL
Objective: 46.000000
Solve time: 2 ms
Iterations: 3

Variables:
  chairs = 4.000000
  tables = 8.000000
```

---

## Phase 6: Integration & Testing (1 day)

### 6.1 Update Root Makefile

```makefile
# Add to root Makefile
ralph-api: ralph
	$(MAKE) -C ralph/api

ralph-wasm:
	$(MAKE) -C ralph/wasm

test-ralph-api: ralph-api
	./scripts/test-ralph-api.sh
```

### 6.2 Update README.md

Change Ralph entry in Core Engines table:

```markdown
| [**Ralph**](ralph/) | Zero-dependency LP/MIP solver (Revised Simplex, Branch & Bound) | 8084 |
```

### 6.3 Regenerate API Docs

```bash
make api-docs  # Regenerates site/api.html with Ralph
```

### 6.4 Test Script

Create `scripts/test-ralph-api.sh`:

```bash
#!/bin/bash
set -e

echo "Testing Ralph API..."

# Health check
curl -s http://localhost:8084/api/v1/health | grep -q '"status":"ok"'
echo "✓ Health check passed"

# Formats
curl -s http://localhost:8084/api/v1/formats | grep -q '"lp"'
echo "✓ Formats endpoint passed"

# Simple LP solve
RESULT=$(curl -s -X POST http://localhost:8084/api/v1/solve \
  -H "Content-Type: application/json" \
  -d '{"format":"lp","problem":"min: x + y; x + y >= 2; x >= 0; y >= 0;"}')

echo "$RESULT" | grep -q '"status":"optimal"'
echo "✓ LP solve passed"

echo "All tests passed!"
```

---

## Summary

| Phase | Duration | Deliverables |
|-------|----------|--------------|
| 1. Handler | 2-3 days | `ralph_api.h`, `ralph_api.c`, `ralph_parse_lp.c` |
| 2. HTTP Server | 1 day | `ralph/api/src/main.c`, Makefile |
| 3. WASM | 1 day | `ralph/wasm/ralph_wasm_api.c`, Makefile |
| 4. JS Handler | 1 day | `site/js/handlers/ralph.js`, config |
| 5. Demo | 0.5 day | Example LP, documentation |
| 6. Testing | 1 day | Integration, api-docs, test script |

**Total: ~6-7 days**

---

## TODOs

- [ ] Phase 1: Create `ralph_api.h` with annotations
- [ ] Phase 1: Implement `ralph_api.c` handler
- [ ] Phase 1: Implement `ralph_parse_lp.c` LP parser
- [ ] Phase 1: Implement MPS parser (or defer)
- [ ] Phase 2: Create Mongoose server `main.c`
- [ ] Phase 2: Create `ralph/api/Makefile`
- [ ] Phase 3: Create WASM wrapper `ralph_wasm_api.c`
- [ ] Phase 3: Create `ralph/wasm/Makefile`
- [ ] Phase 4: Create `site/js/handlers/ralph.js`
- [ ] Phase 4: Create `site/js/ralph-api-demo.js`
- [ ] Phase 4: Update `site/api-config.json`
- [ ] Phase 5: Write demo example LP
- [ ] Phase 6: Update root Makefile
- [ ] Phase 6: Update README.md
- [ ] Phase 6: Run `make api-docs`
- [ ] Phase 6: Create test script
