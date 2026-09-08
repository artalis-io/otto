# Transport-Agnostic API Manifesto

## 0. What This Means

**OTTO APIs are transport-agnostic.** The core logic is pure C functions that take input and produce output. HTTP (Keel), WebAssembly, Unix sockets, embedded calls - these are all just thin wrappers over the same core.

This is the backend equivalent of the [ClayShards Manifesto](../clayshards/clay-shards/MANIFESTO.md):

| ClayShards (Frontend) | OTTO APIs (Backend) |
|----------------------|---------------------|
| UI code is renderer-agnostic | API code is transport-agnostic |
| WebGL, TUI, framebuffer | HTTP, WASM, socket, embedded |
| Same C code, different renderers | Same C code, different transports |

---

## 1. First Principles

### 1) The core is a pure function

Every OTTO API endpoint is fundamentally:

```c
int carta_render_tile(int z, int x, int y, uint8_t **out, size_t *out_len);
int vl_route(VLGraph *g, int from, int to, VLRoute *route);
int lc_search(LCIndex *idx, const char *query, LCResult *results);
int fw_optimize(FWProblem *p, FWSolution *s);
```

No HTTP. No request objects. No response writers. Just input → output.

### 2) Transport is a thin wrapper

Keel HTTP is one transport:

```c
static void handle_tile(struct mg_connection *c, struct mg_http_message *hm) {
    int z = parse_int(hm, "z");
    int x = parse_int(hm, "x");
    int y = parse_int(hm, "y");

    uint8_t *png; size_t len;
    int status = carta_render_tile(z, x, y, &png, &len);

    mg_http_reply(c, status == 0 ? 200 : 500,
                  "Content-Type: image/png\r\n", "%.*s", len, png);
}
```

WASM is another transport:

```c
EMSCRIPTEN_KEEPALIVE
int carta_api_handle(const char *path, uint8_t **out, size_t *out_len) {
    int z, x, y;
    parse_tile_path(path, &z, &x, &y);
    return carta_render_tile(z, x, y, out, out_len);
}
```

The core function is identical. Only the wrapper changes.

### 3) The demo IS the product

Because everything compiles to WASM:
- The browser demo runs the actual tile renderer
- The browser demo runs the actual routing engine
- The browser demo runs the actual geocoder
- The browser demo runs the actual optimizer

Not a simulation. Not a mockup. The real algorithms, in the browser.

### 4) Zero-infrastructure evaluation

Send a prospect a single HTML file with embedded WASM and Monaco PBF data:
- No Docker
- No AWS credentials
- No server to provision
- No network required

They open it, click "Try it", and the real API runs locally.

---

## 2. Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         Application                              │
│  React UI │ CLI tool │ Browser demo │ Embedded system           │
├─────────────────────────────────────────────────────────────────┤
│                    Transport Layer (thin)                        │
│  Keel HTTP     │ WASM+JS │ Unix socket │ Direct C call          │
├─────────────────────────────────────────────────────────────────┤
│                    Core API (pure C)                             │
│  carta_render_tile() │ vl_route() │ lc_search() │ fw_optimize() │
├─────────────────────────────────────────────────────────────────┤
│                    Domain Libraries                              │
│  Carta (tiles) │ Velo (routing) │ Locus (geocoding) │ FuelWise  │
├─────────────────────────────────────────────────────────────────┤
│                    Core Libraries                                │
│  Ralph (LP/MIP) │ Shared (geo, protobuf, compression)           │
└─────────────────────────────────────────────────────────────────┘
```

The transport layer is ~50-100 lines per API. The core is thousands.

---

## 3. Implementation Pattern

### API Function Signature

```c
// Core function: transport-agnostic
// Returns: 0 on success, error code on failure
// Output: via pointer parameters (caller allocates or function allocates)
int module_operation(
    const InputStruct *in,    // Input (owned by caller)
    OutputStruct *out,        // Output (owned by caller or allocated)
    size_t *out_len           // Output size if dynamic
);
```

### HTTP Wrapper Pattern

```c
static void handle_operation(struct mg_connection *c, struct mg_http_message *hm) {
    // 1. Parse HTTP → Input struct
    InputStruct in;
    if (parse_request(hm, &in) != 0) {
        mg_http_reply(c, 400, "", "Bad Request");
        return;
    }

    // 2. Call transport-agnostic core
    OutputStruct out;
    int result = module_operation(&in, &out, NULL);

    // 3. Output struct → HTTP response
    if (result == 0) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "%s", serialize_output(&out));
    } else {
        mg_http_reply(c, 500, "", "Internal Error");
    }
}
```

### WASM Wrapper Pattern

```c
EMSCRIPTEN_KEEPALIVE
int module_api_handle(const char *request_json, char **response_json) {
    // 1. Parse JSON → Input struct
    InputStruct in;
    if (parse_json(request_json, &in) != 0) {
        *response_json = strdup("{\"error\":\"Bad Request\"}");
        return 400;
    }

    // 2. Call transport-agnostic core (SAME FUNCTION)
    OutputStruct out;
    int result = module_operation(&in, &out, NULL);

    // 3. Output struct → JSON response
    *response_json = serialize_output(&out);
    return result == 0 ? 200 : 500;
}
```

---

## 4. Benefits

### For Sales
- **Demo without infrastructure** - Prospects try the real product instantly
- **Proof by demonstration** - "Will it work on edge?" → "It works in your browser"
- **No trust required** - They can inspect network tab: zero server calls

### For Development
- **Test without servers** - Unit test the core functions directly
- **Debug in browser** - Full DevTools access to the actual algorithms
- **Single codebase** - No separate "demo mode" or "lite version"

### For Deployment
- **Edge-ready by design** - If it runs in WASM, it runs anywhere
- **Consistent behavior** - Same code path in all environments
- **Reduced attack surface** - Core has no network dependencies

### For Architecture
- **Clean separation** - Transport concerns don't leak into business logic
- **Easy to add transports** - gRPC, WebSocket, IPC = just another wrapper
- **Testable** - Core functions are pure, deterministic, mockable

---

## 5. The OTTO Promise

If you build with OTTO's transport-agnostic pattern:

- **The same API code runs across transports.**
- Your API will run as **HTTP server**, **WASM module**, and **embedded library** without rewriting.
- Your business logic remains in **pure C**.
- Your API remains **simple to test** because the core is transport-free.
- Your transport remains **replaceable** because it's just a thin wrapper.

---

## 6. Comparison with Traditional Approaches

### Traditional REST Framework
```python
@app.route('/tiles/<z>/<x>/<y>.png')
def get_tile(z, x, y):
    # Business logic deeply coupled to Flask
    tile = render_tile(z, x, y)
    return Response(tile, mimetype='image/png')
```

Can't run this in a browser. Can't embed it. Can't unit test without mocking Flask.

### OTTO Approach
```c
// Core: runs anywhere
int carta_render_tile(int z, int x, int y, uint8_t **out, size_t *len);

// HTTP wrapper: 10 lines
// WASM wrapper: 10 lines
// Embedded wrapper: 5 lines
// Unit test: direct call
```

---

## 7. See Also

- [ClayShards Manifesto](../clayshards/clay-shards/MANIFESTO.md) - The frontend equivalent
- [API Documentation](../site/api.html) - Live WASM demos
- [API Codegen Plan](API_CODEGEN_PLAN.md) - Generating docs from annotations
