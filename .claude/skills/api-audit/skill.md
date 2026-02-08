---
name: api-audit
description: Audit API modules for transport-agnostic structure, annotations, and documentation. Validates against the OTTO API manifesto and generates missing docs.
user-invocable: true
---

# API Audit Skill

Audit OTTO API modules for compliance with the transport-agnostic manifesto and API documentation standards.

**Target module:** $ARGUMENTS

## Usage

```
/api-audit <module>           # Audit a module (e.g., /api-audit velo)
/api-audit <module> --fix     # Audit and apply fixes
/api-audit all                # Audit all modules
/api-audit all --fix          # Fix all modules
```

## What This Skill Checks

### 1. Transport-Agnostic Handler Structure

The OTTO API manifesto requires that API logic be transport-agnostic. The core handler should be usable by HTTP (Mongoose), WASM, Unix sockets, or direct C calls.

**Expected structure:**
```
{module}/
├── include/
│   └── {prefix}_api.h       # Handler interface (transport-agnostic)
├── src/
│   └── {prefix}_api.c       # Handler implementation
├── api/
│   └── src/
│       └── main.c           # Mongoose wrapper (thin)
└── wasm/
    └── {prefix}_wasm_api.c  # WASM wrapper (thin)
```

**Module prefixes:**
| Module | Prefix | Header | Example |
|--------|--------|--------|---------|
| carta | ct | ct_api.h | ct_api_handle() |
| velo | vl | vl_api.h | vl_api_handle() |
| locus | lc | lc_api.h | lc_api_handle() |
| fuelwise | fw | fw_api.h | fw_api_handle() |

### 2. Handler Interface Pattern

The `{prefix}_api.h` header must define:

```c
// Opaque context
typedef struct {PREFIX}APIContext {PREFIX}APIContext;

// Request/Response structs
typedef struct {
    const char *path;
    const char *query;
    const char *host;  // Optional, for URL generation
} {PREFIX}APIRequest;

typedef struct {
    int status_code;
    const char *content_type;
    uint8_t *body;      // Caller frees
    size_t body_len;
} {PREFIX}APIResponse;

// Core handler (transport-agnostic)
int {prefix}_api_handle({PREFIX}APIContext *ctx,
                        const {PREFIX}APIRequest *req,
                        {PREFIX}APIResponse *resp);

// Lifecycle
{PREFIX}APIContext *{prefix}_api_create(const uint8_t *data, size_t len, ...);
void {prefix}_api_free({PREFIX}APIContext *ctx);
void {prefix}_api_response_free({PREFIX}APIResponse *resp);
```

### 3. API Annotations

Each endpoint must have a `/*@api ... */` block in the header:

```c
/*@api
 * GET /route
 * Calculate route between waypoints
 *
 * @path param:type Description
 * @query param:type:default Description
 *
 * @returns content-type Description
 * @error 400 Description
 * @error 404 Description
 *
 * @response_json
 * { "example": "json" }
 *
 * @demo json|image
 * @demo_title Description for WASM demo
 * @demo_input from_lat:number:43.7384
 * @demo_input profile:select:car:car,truck,bike,foot
 */
```

**Annotation reference:**

| Directive | Format | Required | Description |
|-----------|--------|----------|-------------|
| First line | `METHOD /path` | Yes | HTTP method and path |
| Second line | Summary text | Yes | Brief description |
| `@path` | `name:type Description` | If path params | Path parameter |
| `@query` | `name:type:default Description` | If query params | Query parameter |
| `@returns` | `content-type Description` | Yes | Response type |
| `@error` | `code Description` | No | Error response |
| `@response_json` | JSON block | No | Example response |
| `@example` | Shell command | No | curl example |
| `@demo` | `json` or `image` | No | Enable WASM demo |
| `@demo_title` | Text | If @demo | Demo description |
| `@demo_input` | `name:type:default:min:max` | If @demo needs inputs | Demo input field (see below) |

**@demo_input types:**
| Type | Format | Example | Renders as |
|------|--------|---------|------------|
| `number` | `name:number:default:min:max` | `zoom:number:14:0:18` | `<input type="number">` |
| `text` | `name:text:default` | `query:text:Budapest` | `<input type="text">` |
| `select` | `name:select:default:opt1,opt2,...` | `profile:select:car:car,truck,bike,foot` | `<select>` dropdown |

**Reading inputs in handlers:**

Input elements are generated with ID `{module}-{input_name}`. Handlers read values like:

```javascript
// In site/js/handlers/{module}.js
const fromLat = parseFloat(document.getElementById('velo-from_lat').value);
const profile = document.getElementById('velo-profile').value;  // Works for both input and select
```

**Complete example (Velo route):**

Header annotation:
```c
/*@api
 * GET /api/v1/route
 * Calculate route between coordinates
 * ...
 * @demo json
 * @demo_title Calculate a route in Monaco using WASM
 * @demo_input from_lat:number:43.7384
 * @demo_input from_lon:number:7.4246
 * @demo_input to_lat:number:43.7311
 * @demo_input to_lon:number:7.4197
 * @demo_input profile:select:car:car,truck,bike,foot
 * @demo_input mode:select:fastest:fastest,shortest
 */
```

Handler in `site/js/handlers/velo.js`:
```javascript
async function calculateVeloRoute() {
    const from = {
        lat: parseFloat(document.getElementById('velo-from_lat').value),
        lon: parseFloat(document.getElementById('velo-from_lon').value)
    };
    const to = {
        lat: parseFloat(document.getElementById('velo-to_lat').value),
        lon: parseFloat(document.getElementById('velo-to_lon').value)
    };
    const profile = document.getElementById('velo-profile').value;
    const mode = document.getElementById('velo-mode').value;

    const route = await veloDemo.route(from, to, { profile, mode });
    // ... display result
}
```

### 4. WASM Exports

If the module has WASM enabled, the header must include a `/*@wasm ... */` block:

```c
/*@wasm
 * @export {prefix}_api_init
 * @export {prefix}_api_free
 * @export {prefix}_api_ready
 * @export {prefix}_api_handle
 * @export {prefix}_response_status
 * @export {prefix}_response_content_type
 * @export {prefix}_response_body
 * @export {prefix}_response_body_len
 * @export malloc
 * @export free
 */
```

### 5. Mongoose Integration

The `api/src/main.c` must use the handler, not inline logic:

**GOOD (transport-agnostic):**
```c
static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = ev_data;

        // Build request
        {PREFIX}APIRequest req = {
            .path = extract_path(hm),
            .query = extract_query(hm)
        };

        // Call transport-agnostic handler
        {PREFIX}APIResponse resp;
        {prefix}_api_handle(s_ctx, &req, &resp);

        // Send HTTP response
        mg_http_reply(c, resp.status_code, ...);
        {prefix}_api_response_free(&resp);
    }
}
```

**BAD (coupled to transport):**
```c
static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        // Inline logic - can't reuse in WASM!
        if (mg_match(hm->uri, mg_str("/route"), NULL)) {
            // ... all the routing logic here ...
            mg_http_reply(c, 200, ...);
        }
    }
}
```

### 6. api-config.json Entry

The module must be registered in `site/api-config.json`:

```json
{
  "id": "{module}",
  "name": "{Module} Server",
  "icon": "...",
  "port": 808X,
  "description": "...",
  "header_file": "{module}/include/{prefix}_api.h",
  "wasm": {
    "enabled": true|false,
    "script": "wasm/{module}-api-demo.js",
    "wrapper": "js/{module}-api-demo.js",
    "factory_name": "{Module}APIDemo",
    "class_name": "{Module}Demo",
    "handlers_file": "js/handlers/{module}.js",
    "init_function": "init{Module}Demo",
    "error_function": "handle{Module}Error",
    "buttons": {
      "{demo-id}-try-btn": "handlerFunctionName"
    }
  }
}
```

### 6.1 WASM Handler Files

For modules with WASM demos, handler functions live in external JS files:

**Location:** `site/js/handlers/{module}.js`

**Required functions:**
```javascript
// Handler for each button (referenced in wasm.buttons)
async function handlerFunctionName() {
    if (!{module}Demo || !{module}Demo.isReady()) return;
    // ... demo logic ...
}

// Init function (called when WASM loads successfully)
function init{Module}Demo() {
    enableBtn('{demo-id}-try-btn', 'Button Label');
    // ... enable other buttons, show status ...
}

// Error function (called when WASM fails to load)
function handle{Module}Error(err) {
    disableBtn('{demo-id}-try-btn');
    // ... disable buttons, show error ...
}
```

**Adding a new module's WASM handlers:**
1. Create `site/js/handlers/{module}.js` with handler functions
2. Add `handlers_file`, `init_function`, `error_function`, `buttons` to wasm config in api-config.json
3. Run `make api-docs` - no changes to build-api-docs.py needed

**Button ID convention:** `{demo-id}-try-btn` where demo-id is derived from the endpoint path (e.g., `velo-route-try-btn` for `/api/v1/route`)

**Existing handlers (use as reference):**
- `site/js/handlers/carta.js` - Tile generation, TileJSON
- `site/js/handlers/velo.js` - Route calculation
- `site/js/handlers/locus.js` - Search, autocomplete, reverse geocoding

### 7. Generated Documentation

The `site/api.html` is auto-generated from C header annotations and includes live WASM demos with embedded Monaco data.

**Makefile targets (root):**
```bash
make api-docs              # Regenerate site/api.html (auto-rebuilds WASM deps)
make api-docs-check        # Check if api.html is up-to-date (for CI)
make test-api-docs         # Run Playwright tests on WASM demos
make test-api-docs-install # Install Playwright + Chromium (first time)
```

**Makefile targets (site/):**
```bash
cd site && make build      # Build deployable site to build/
cd site && make serve      # Build and serve on :8000
cd site && make test       # Run WASM demo tests
cd site && make test-install # Install Playwright deps
```

**What each target does:**
| Target | Action | Use Case |
|--------|--------|----------|
| `api-docs` | Rebuilds WASM demos if needed, then runs build-api-docs.py | After changing annotations or handlers |
| `api-docs-check` | Runs build-api-docs.py --check (compares, doesn't write) | CI validation, pre-commit hooks |
| `test-api-docs` | Runs Playwright tests against api.html WASM demos | Verify demos work after changes |
| `test-api-docs-install` | npm install + playwright install chromium | First-time setup |

**Dependency chain** (all automatic):
```
data/monaco-latest.osm.pbf  (downloads if missing)
           ↓
data/monaco.vlg + data/monaco.lcx  (index files for velo/locus)
           ↓
{velo,carta,locus}/wasm/src/monaco_*.h  (embedded data headers via xxd)
           ↓
{velo,carta,locus,fuelwise}/wasm/build/*-api-demo.js  (WASM modules)
           ↓
site/api.html  (copies WASM to site/wasm/, generates HTML from annotations)
```

**Module WASM dependencies:**
| Module | Embedded Data | Dependencies |
|--------|---------------|--------------|
| velo | `monaco_vlg.h` (routing graph) | velo/src/*.c, shared |
| carta | `monaco_pbf.h` (raw PBF) | carta/src/*.c, shared |
| locus | `monaco_lcx.h` (geocoding index) | locus/src/*.c, shared |
| fuelwise | none (no embedded data) | fuelwise/src/*.c, shared, ralph |

**Key generator files:**
- `scripts/build-api-docs.py` - Parses annotations, renders template, copies WASM
- `site/api-template.html` - HTML template with Jinja2-like syntax
- `site/api-config.json` - Module configuration (ports, WASM settings)

**Force full rebuild:**
```bash
rm -f data/monaco.vlg data/monaco.lcx
rm -f velo/wasm/src/monaco_vlg.h carta/wasm/src/monaco_pbf.h locus/wasm/src/monaco_lcx.h
make api-docs
```

**What triggers auto-rebuild:**
- `velo/src/*.c` or `velo/include/*.h` changes → velo WASM rebuilt
- `carta/src/*.c` or `carta/include/*.h` changes → carta WASM rebuilt
- `locus/src/*.c` or `locus/include/*.h` changes → locus WASM rebuilt
- `fuelwise/src/*.c` or `fuelwise/include/*.h` changes → fuelwise WASM rebuilt
- `scripts/build-api-docs.py` or `site/api-template.html` changes → HTML regenerated
- Header annotation changes → HTML regenerated

## Audit Procedure

When `/api-audit <module>` is invoked:

1. **Identify module files:**
   - Header: `{module}/include/{prefix}_api.h`
   - Implementation: `{module}/src/{prefix}_api.c`
   - Server: `{module}/api/src/main.c`
   - WASM: `{module}/wasm/{prefix}_wasm_api.c`

2. **Check handler interface exists:**
   - [ ] `{PREFIX}APIContext` typedef
   - [ ] `{PREFIX}APIRequest` struct with path, query, host
   - [ ] `{PREFIX}APIResponse` struct with status, content_type, body, body_len
   - [ ] `{prefix}_api_handle()` function
   - [ ] `{prefix}_api_create()` and `{prefix}_api_free()` lifecycle

3. **Check annotations:**
   - [ ] Every endpoint has `/*@api ... */` block
   - [ ] All blocks have METHOD, path, summary, @returns
   - [ ] Path parameters documented with `@path`
   - [ ] Query parameters documented with `@query`
   - [ ] WASM-enabled endpoints have `@demo`

4. **Check WASM exports (if enabled):**
   - [ ] `/*@wasm ... */` block exists
   - [ ] All handler functions exported
   - [ ] malloc/free exported

5. **Check mongoose integration:**
   - [ ] Uses `{prefix}_api_handle()` not inline logic
   - [ ] Builds request struct from mg_http_message
   - [ ] Calls response_free after sending

6. **Check api-config.json:**
   - [ ] Module entry exists
   - [ ] header_file points to correct location
   - [ ] WASM config matches reality

7. **Check api.html:**
   - [ ] `make api-docs-check` passes
   - [ ] All endpoints from annotations appear in HTML

8. **Test WASM demos:**
   - [ ] `make test-api-docs` passes
   - [ ] All demo endpoints return expected responses

## Report Format

```markdown
## API Audit Report: {module}

**Date:** YYYY-MM-DD
**Status:** PASS | PARTIAL | FAIL

### Structure Compliance

| Check | Status | Notes |
|-------|--------|-------|
| Handler header exists | ✅/❌ | {path} |
| Handler implementation exists | ✅/❌ | {path} |
| Mongoose uses handler | ✅/❌ | Lines X-Y |
| WASM wrapper exists | ✅/❌/N/A | {path} |

### Annotation Coverage

| Endpoint | Annotated | Demo | Issues |
|----------|-----------|------|--------|
| GET /route | ✅ | ✅ | None |
| GET /health | ❌ | N/A | Missing @returns |

### WASM Exports

| Function | Annotated | Built | Match |
|----------|-----------|-------|-------|
| {prefix}_api_init | ✅ | ✅ | ✅ |
| {prefix}_api_handle | ✅ | ❌ | ❌ Missing |

### api-config.json

| Field | Status | Value |
|-------|--------|-------|
| Entry exists | ✅/❌ | |
| header_file | ✅/❌ | {current} → {expected} |
| wasm.enabled | ✅/❌ | {value} |

### Documentation

| Check | Status |
|-------|--------|
| api.html up-to-date | ✅/❌ |
| Endpoints in HTML | X/Y |

### Recommended Fixes

1. Create `{module}/include/{prefix}_api.h` with handler interface
2. Add `/*@api ... */` annotations to endpoints: ...
3. Update api-config.json header_file to ...
4. Run `make api-docs` to regenerate documentation
```

## Fix Mode (--fix)

When `--fix` is specified:

### 1. Create Missing Handler Header

If `{prefix}_api.h` doesn't exist, scaffold it from mongoose handlers:

```c
// Generated by /api-audit --fix
// TODO: Move handler logic from api/src/main.c to {prefix}_api.c

#ifndef {MODULE}_{PREFIX}_API_H
#define {MODULE}_{PREFIX}_API_H

#include <stddef.h>
#include <stdint.h>

typedef struct {PREFIX}APIContext {PREFIX}APIContext;

typedef struct {
    const char *path;
    const char *query;
    const char *host;
} {PREFIX}APIRequest;

typedef struct {
    int status_code;
    const char *content_type;
    uint8_t *body;
    size_t body_len;
} {PREFIX}APIResponse;

{PREFIX}APIContext *{prefix}_api_create(const uint8_t *data, size_t len);
void {prefix}_api_free({PREFIX}APIContext *ctx);

int {prefix}_api_handle({PREFIX}APIContext *ctx,
                        const {PREFIX}APIRequest *req,
                        {PREFIX}APIResponse *resp);

void {prefix}_api_response_free({PREFIX}APIResponse *resp);

#endif
```

### 2. Add Missing Annotations

For each unannotated endpoint found in mongoose handlers, add scaffold:

```c
/*@api
 * {METHOD} {path}
 * TODO: Add description
 *
 * @returns application/json TODO: Describe response
 */
```

### 3. Update api-config.json

If module not in config or header_file wrong:

```python
# Add or update module entry
config["modules"].append({
    "id": "{module}",
    "name": "{Module} Server",
    "icon": "❓",  # TODO: Choose icon
    "port": 808X,
    "description": "TODO: Add description",
    "header_file": "{module}/include/{prefix}_api.h",
    "wasm": {"enabled": false}
})
```

### 4. Regenerate api.html

```bash
make api-docs
```

## Examples

### Audit a single module
```
/api-audit velo
```

### Audit and fix a module
```
/api-audit locus --fix
```

### Audit all modules
```
/api-audit all
```

### Check if documentation is current
```
/api-audit docs
```

## Reference: Carta (Fully Compliant)

Carta is the reference implementation. Use it as a model:

```
carta/
├── include/
│   └── ct_api.h            # ✅ Full handler interface + annotations
├── src/
│   └── ct_api.c            # ✅ Transport-agnostic implementation
├── api/
│   └── src/
│       └── main.c          # ✅ Uses ct_api_handle()
└── wasm/
    └── ct_wasm_api.c       # ✅ WASM wrapper using ct_api_handle()
```

**Key files to reference:**
- `carta/include/ct_api.h` - Annotation format, struct definitions
- `docs/MANIFESTO.md` - Philosophy and patterns
- `docs/API_CODEGEN_PLAN.md` - Codegen details
- `scripts/build-api-docs.py` - How annotations become HTML

## WASM Demo Testing

Automated tests verify that WASM demos in api.html work correctly and return expected responses.

**Test location:** `site/tests/wasm-demos.spec.js`

**What's tested:**
| Module | Tests |
|--------|-------|
| Carta | health, stats, PNG tile, MVT tile, ASCII tile |
| Velo | health, stats, route (profiles, modes, geometry on/off) |
| Locus | health, stats, search, autocomplete, reverse geocoding |
| FuelWise | health, stats, optimize |

**Running tests:**
```bash
# First time setup
make test-api-docs-install

# Run tests (rebuilds api.html if needed)
make test-api-docs

# Or from site/ directory
cd site && make test
```

**Test structure:**
```javascript
test('velo route returns valid route', async ({ page }) => {
  const result = await page.evaluate(async () => {
    return await veloDemo.route(
      { lat: 43.7384, lon: 7.4246 },
      { lat: 43.7311, lon: 7.4197 }
    );
  });
  expect(result.route.distance).toBeGreaterThan(0);
  expect(result.route.duration).toBeGreaterThan(0);
});
```

**Adding tests for new endpoints:**
1. Add test case to `site/tests/wasm-demos.spec.js`
2. Use `page.evaluate()` to call WASM demo API
3. Assert response matches expected values from C annotations

**CI integration:**
```yaml
- name: Test WASM demos
  run: make test-api-docs
```

## Checklist for "API Compliant" Status

Before marking a module as API-compliant:

**Structure:**
- [ ] `{prefix}_api.h` header exists with full interface
- [ ] `{prefix}_api.c` implementation exists
- [ ] Mongoose server uses handler, not inline logic
- [ ] WASM wrapper exists (if WASM enabled)

**Annotations:**
- [ ] All endpoints have `/*@api ... */` blocks
- [ ] All blocks have METHOD, path, summary, @returns
- [ ] Demo endpoints have `@demo` and `@demo_title`
- [ ] `/*@wasm ... */` block lists all exports (if WASM enabled)

**Configuration:**
- [ ] Module in `site/api-config.json`
- [ ] `header_file` points to `{prefix}_api.h`
- [ ] WASM config accurate

**Documentation:**
- [ ] `make api-docs-check` passes
- [ ] All endpoints visible in `site/api.html`
- [ ] WASM demos work (if enabled)
- [ ] `make test-api-docs` passes (automated WASM tests)

## Current Module Status

| Module | Handler | Annotations | Config | api.html | Status |
|--------|---------|-------------|--------|----------|--------|
| carta | ✅ ct_api.h | ✅ 4 endpoints | ✅ | ✅ | **Compliant** |
| velo | ✅ vl_api.h | ✅ 5 endpoints | ✅ | ✅ | **Compliant** |
| locus | ✅ lc_api.h | ✅ 6 endpoints | ✅ | ✅ | **Compliant** |
| fuelwise | ❌ inline | ❌ none | ⚠️ points to main.c | ⚠️ | Needs work |

Run `/api-audit all` to get current status, `/api-audit <module> --fix` to remediate.
