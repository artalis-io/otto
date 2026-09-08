# Iris — Wireable LLM Orchestration Engine

## Context

Arqh ($3.8M pre-seed) is getting funded for "LLM + optimizer" UX. OTTO has superior engines but no NL interface. Iris closes that gap — not as a wrapper for any specific engine, but as a **generic left-brain/right-brain dispatch engine** where optimization backends register themselves.

Iris is Priority 3 in STRATEGY.md. This plan accelerates it.

## Architecture Overview

Iris is a **wireable orchestrator**. Engines are not hardcoded — they register capabilities, domain schemas, and handlers. The LLM (right brain) interprets intent; the engine registry (left brain) routes to the right solver. A sharp process boundary ensures the LLM never touches computation.

```
Natural Language                  Structured Input (UI/API/WASM)
        |                                 |
  ┌─────▼──────────────┐                  │
  │ ir_parse() [C]      │  NON-DET        │
  │ Right Brain:        │                  │
  │ • Build prompt from │                  │
  │   registry schemas  │                  │
  │ • HTTP POST to LLM  │                  │
  │   (Keel HTTP client)│                  │
  │ • Parse response    │                  │
  └─────┬──────────────┘                  │
        │ IRIntent (domain params)        │
        ├─────────────────────────────────┘
        ▼
  ┌──────────────────────────────────────────────────────┐
  │ ir_dispatch() [C]                  DETERMINISTIC      │
  │ Left Brain:                                          │
  │                                                      │
  │  1. Find engine for capability (explicit binding)    │
  │  2. Engine declares requires: ["fleet_data", ...]    │
  │  3. Resolve from bound providers (session cache):    │
  │       "fleet_data" → Nexus / SQL / SAP TM / Oracle   │
  │       "travel_matrix" → Velo (computation, long TTL) │
  │  4. Validate provider data against canonical schemas │
  │  5. Apply session overrides (JSON patches)           │
  │  6. Engine adapter: domain params → solver calls     │
  │  7. Engine's explain() → structured JSON summary     │
  │  8. Cache result in session (for replan, what-if)    │
  │                                                      │
  │  Optional: ir_narrate → structured JSON to prose     │
  └──────────────────────────────────────────────────────┘

Single C binary. WASM build excludes ir_parse + HTTP client (structured input only).
```

**The LLM outputs domain-level intent (what a dispatcher would say), never solver parameters.** Engine adapters translate domain intent → solver calls deterministically. Iris core has zero knowledge of vehicles, routes, VRP, PDPTW, or any logistics domain — it's a generic intent → data → solver → explain pipeline.

**No hardcoded pipeline steps.** If an engine needs geocoding, it declares `requires: ["geocode"]` and Iris finds the bound provider. If no provider is bound, Iris reports which requirement couldn't be met.

### The Key Insight: Everything Is Wireable

Iris has two types of registerable components:

1. **Engines** — solve problems (Surge, FuelWise, Ralph). Each exports a **domain schema** (what the LLM sees) and translates domain params → solver calls internally.
2. **Providers** — supply data from any source (Nexus files, SQL databases, SAP TM, Oracle OTM, REST APIs, Locus geocoding, Velo travel matrices). Each declares **traits** (cacheable, TTL, latency).

Both register the same way: declare capabilities, schemas, and a handler. An engine declares what **data it requires** — Iris auto-resolves requirements from bound providers, caches results in the session, and applies overrides before calling the engine.

```c
/* iris/include/ir_engine.h — the wireable component interface */

/* A component is either an engine (solves) or a provider (supplies data) */
typedef enum {
    IR_COMPONENT_ENGINE = 0,    /* Handles optimization/computation */
    IR_COMPONENT_PROVIDER = 1   /* Supplies data to engines */
} IRComponentType;

typedef struct {
    const char *name;               /* For logging only */
    const char *version;
    IRComponentType type;

    /* What this component can do */
    const char **capabilities;      /* NULL-terminated */
    uint32_t capability_count;

    /* What this component needs before it can run (provider dependencies) */
    const char **requires;          /* NULL-terminated: {"fleet_data", "travel_matrix", NULL} */
    uint32_t requires_count;

    /* --- Schemas (two layers) --- */

    /* Domain schema: what the LLM sees. Domain-level concepts a human would say.
       Example (Surge): {problem_class: enum, time_budget: enum, overrides: [...]}
       Example (FuelWise): {scope: "all_routes"|"single", vehicle_ref: string}
       The LLM prompt is generated from this. */
    const char *domain_schema_json;

    /* Output schema: what the engine returns (for downstream chaining) */
    const char *output_schema_json;

    /* Few-shot examples: valid domain intents for prompt generation.
       JSON array of {input: "NL text", output: {domain params}} */
    const char *examples_json;

    /* --- Handler --- */

    /* Engine handler: receives domain params + resolved provider data.
       The adapter translates domain params → solver calls internally.
       The LLM never constructs solver-specific JSON. */
    int (*handle)(const char *domain_params_json, size_t domain_len,
                  const char *resolved_data_json, size_t data_len,
                  char **output_json, size_t *output_len,
                  void *ctx);

    /* Explainer (optional): engine result → structured summary JSON.
       Returns structured data (counts, costs, violations), NOT prose.
       The NL narration layer (optional, separate) converts this to text. */
    int (*explain)(const char *result_json, size_t result_len,
                   char **summary_json, size_t *summary_len,
                   void *ctx);

    /* --- Provider traits (ignored for engines) --- */
    struct {
        bool cacheable;              /* Can results be cached across calls? */
        uint32_t ttl_seconds;        /* Cache TTL (0 = session-scoped) */
        bool idempotent;             /* Same input always gives same output? */
        uint32_t expected_latency_ms;/* Hint for dispatch ordering */
    } traits;

    void (*free_result)(char *json, void *ctx);
    void *ctx;
} IRComponent;

typedef struct IRRegistry IRRegistry;

IRRegistry *ir_registry_create(void);
void ir_registry_free(IRRegistry *reg);
int ir_registry_add(IRRegistry *reg, const IRComponent *component);
int ir_registry_remove(IRRegistry *reg, const char *name);

/* Find by capability. Returns component bound to this capability
   in the deployment config (deterministic, not order-dependent). */
const IRComponent *ir_registry_resolve(const IRRegistry *reg,
                                        const char *capability);

/* List all capabilities grouped by type (for LLM prompt generation) */
uint32_t ir_registry_engine_capabilities(const IRRegistry *reg,
                                          const char **out, uint32_t max_out);
uint32_t ir_registry_provider_capabilities(const IRRegistry *reg,
                                            const char **out, uint32_t max_out);
```

### Data Resolution Is Automatic

When an engine declares `requires: ["fleet_data", "travel_matrix"]`, Iris resolves those from registered providers before calling the engine:

```c
/* ir_dispatch.c — automatic data resolution */
int ir_dispatch(const IRRegistry *reg, IRSession *session, const IRIntent *intent) {
    /* 1. Find engine for the requested capability */
    const IRComponent *engine = ir_registry_resolve(reg, intent->capability);
    if (!engine) return IR_NO_ENGINE_FOR_CAPABILITY;

    /* 2. Resolve engine's data requirements from providers */
    /* Each provider gets a typed request with filters + session context */
    for (uint32_t i = 0; i < engine->requires_count; i++) {
        /* Check session cache first (respects provider TTL) */
        if (ir_session_has_fresh(session, engine->requires[i])) continue;

        const IRComponent *provider = ir_registry_resolve(reg, engine->requires[i]);
        if (!provider) return IR_UNMET_REQUIREMENT;

        /* Build provider request with filters from intent */
        IRProviderRequest req = {
            .capability = engine->requires[i],
            .filters_json = intent->filters_json,  /* e.g. {date: "2026-03-10"} */
            .filters_len = intent->filters_len,
            .session = session                      /* access to overrides, state */
        };
        provider->handle(&req, &data_json, &data_len, provider->ctx);

        /* Validate provider output against canonical schema */
        ir_validate_provider_output(engine->requires[i], data_json, data_len, &issues);

        /* Cache in session (respects provider TTL) */
        ir_session_set_data(session, engine->requires[i], data_json, data_len);
    }

    /* 3. Merge all resolved data + apply session overrides */
    char *resolved = ir_session_build_resolved(session, engine->requires,
                                                engine->requires_count);

    /* 4. Call engine with domain params + resolved data */
    engine->handle(intent->domain_params_json, intent->domain_params_len,
                   resolved, resolved_len,
                   &result_json, &result_len, engine->ctx);

    /* 5. Cache result in session (for replan, what-if, explain) */
    ir_session_set_result(session, intent->capability, result_json, result_len);
}
```

### Session State

Multi-turn workflows require deterministic state. `IRSession` holds cached provider data, solve results, and overrides:

```c
/* iris/include/ir_session.h */
typedef struct IRSession IRSession;

IRSession *ir_session_create(const IRRegistry *reg);
void ir_session_free(IRSession *s);

/* Provider data cache (respects TTL from provider traits) */
int ir_session_set_data(IRSession *s, const char *capability,
                        const char *data_json, size_t len);
const char *ir_session_get_data(const IRSession *s, const char *capability,
                                 size_t *len);
bool ir_session_has_fresh(const IRSession *s, const char *capability);

/* Solve result cache (for replan baseline, what-if comparison) */
int ir_session_set_result(IRSession *s, const char *capability,
                          const char *result_json, size_t len);
const char *ir_session_get_result(const IRSession *s, const char *capability,
                                   size_t *len);

/* Domain-level overrides (vehicle offline, temporary additions, etc.)
   These are JSON patches applied to provider data before engine call. */
int ir_session_add_override(IRSession *s, const char *override_json, size_t len);
const char *ir_session_get_overrides(const IRSession *s, size_t *len);

/* Build merged data: cached provider data + applied overrides */
char *ir_session_build_resolved(const IRSession *s,
                                 const char **capabilities, uint32_t count);

/* Clone session for what-if scenarios */
IRSession *ir_session_clone(const IRSession *s);
```

Multi-turn example:
```
1. "Load today's orders"   → provider fetches, session caches order_data
2. "Take truck 3 offline"  → session stores override {action: "exclude", vehicle: "truck-3"}
3. "Plan routes"            → dispatch uses cached data + override → solve → session caches result
4. "Freeze routes 1,2"     → session stores lock state
5. "Add rush order ORD-99" → session stores new data
6. "Replan"                 → dispatch uses cached + locks + new data → solve → delta vs. step 3
7. "What if we had 1 more truck?" → clone session → modify → re-solve → diff
```

### Provider Resolution Is Explicit

When multiple providers can serve the same capability, the deployment config determines which one is used — not registration order:

```json
/* iris/config/provider_bindings.json — per-deployment */
{
  "bindings": {
    "fleet_data":     "sap-tm-fleet",
    "customer_data":  "sap-tm-fleet",
    "order_data":     "sql-orders",
    "station_data":   "nexus-stations",
    "travel_matrix":  "velo",
    "point_route":    "velo",
    "geocode":        "locus"
  }
}
```

`ir_registry_resolve("fleet_data")` looks up the binding, finds `"sap-tm-fleet"`, returns that component. Deterministic. No ambiguity.

### Example: Wiring a Complete Deployment

```c
/* OTTO computation engines */
ir_registry_add(reg, &(IRComponent){
    .name = "surge",
    .type = IR_COMPONENT_ENGINE,
    .capabilities = (const char*[]){"route_optimize", "validate_plan", "replan", NULL},
    .requires = (const char*[]){"fleet_data", "order_data", "travel_matrix", NULL},
    .handle = surge_engine_handle,
    .explain = surge_explain_handle,
    .ctx = surge_ctx
});

ir_registry_add(reg, &(IRComponent){
    .name = "fuelwise",
    .type = IR_COMPONENT_ENGINE,
    .capabilities = (const char*[]){"refuel", "filter_stations", NULL},
    .requires = (const char*[]){"station_data", "point_route", NULL},
    .handle = fuelwise_engine_handle,
    .explain = fuelwise_explain_handle,
    .ctx = fw_ctx
});

/* OTTO computation providers */
ir_registry_add(reg, &(IRComponent){
    .name = "locus", .type = IR_COMPONENT_PROVIDER,
    .capabilities = (const char*[]){"geocode", "reverse_geocode", NULL},
    .handle = locus_provider_handle, .ctx = locus_ctx
});

ir_registry_add(reg, &(IRComponent){
    .name = "velo", .type = IR_COMPONENT_PROVIDER,
    .capabilities = (const char*[]){"travel_matrix", "point_route", NULL},
    .handle = velo_provider_handle, .ctx = velo_ctx
});

/* Data providers — customer-specific, configured via JSON (see Section 2) */
ir_registry_add(reg, &(IRComponent){
    .name = "sap-tm", .type = IR_COMPONENT_PROVIDER,
    .capabilities = (const char*[]){"fleet_data", "customer_data", NULL},
    .handle = sap_tm_provider_handle, .ctx = sap_ctx
});

ir_registry_add(reg, &(IRComponent){
    .name = "sql-orders", .type = IR_COMPONENT_PROVIDER,
    .capabilities = (const char*[]){"order_data", NULL},
    .handle = sql_provider_handle, .ctx = sql_ctx
});
```

**Adding a new engine or data source is one `ir_registry_add()` call.** No Iris core code changes. No new enums. No new switch cases.

### LLM Prompt Is Generated From Registry

See Section 1 for the prompt generation architecture. The key insight:

- The C pipeline exports registry metadata: `ir_pipeline --export-registry > registry.json`
- `ir_parse()` builds the system prompt from registry domain schemas, examples, and capability lists
- **Wire in a new engine → the LLM prompt automatically knows about it.** No code changes.
- The prompt includes **domain schemas** (what the LLM can output) and **few-shot examples** (from each engine adapter), not just capability names. This prevents hallucinated parameters.

## Key Design Principles

| Principle | How |
|-----------|-----|
| Engines are plugins, not hardcoded | Registry pattern with capability matching |
| LLM prompt adapts to available engines | Generated from registry domain schemas + examples |
| Master data is external | Any provider: Nexus files, SQL, SAP TM, Oracle OTM, REST APIs |
| No magic constants | All config is in JSON files, not code |
| LLM is provider-agnostic | C abstraction via HTTP client: Claude, OpenAI, Ollama, HuggingFace, custom |
| Sharp LLM boundary | `ir_parse()` (non-deterministic LLM call) → IRIntent → `ir_dispatch()` (deterministic) |
| LLM never constructs solver params | LLM outputs domain intent; engine adapters translate |
| Iris core is domain-agnostic | No vehicle/route/fleet/VRP concepts in core — all in engine adapters |
| Session state is explicit | IRSession holds cached data, overrides, results for multi-turn workflows |
| Provider resolution is deterministic | Explicit bindings in config, not registration-order-dependent |

## 1. LLM Boundary: All C, Single Binary

The LLM call is the ONLY non-deterministic operation in Iris. Everything after the IRIntent is pure deterministic C. This boundary is a function boundary within one process — not a process boundary.

### Architecture: Single C Binary

```
┌──────────────────────────────────────────────────────────────────┐
│  ir_pipeline (single C binary)                                   │
│                                                                  │
│  ┌─────────────────────────┐    ┌──────────────────────────────┐ │
│  │ ir_parse()              │    │ ir_dispatch()                │ │
│  │                         │    │                              │ │
│  │ • Build prompt from     │    │ • Resolve providers          │ │
│  │   registry schemas      │    │ • Validate provider data     │ │
│  │ • HTTP POST to LLM API  │    │ • Apply session overrides    │ │
│  │   (Keel HTTP client)    │    │ • Call engine adapter        │ │
│  │ • Parse JSON response   │──▶ │ • Engine explain()           │ │
│  │ • Build IRIntent        │    │ • Cache result in session    │ │
│  │                         │    │                              │ │
│  │ NON-DETERMINISTIC       │    │ DETERMINISTIC                │ │
│  └─────────────────────────┘    └──────────────────────────────┘ │
│                                                                  │
│  Structured input (UI/API) skips ir_parse() entirely             │
│  WASM build excludes ir_parse() + HTTP client (no LLM in browser)│
└──────────────────────────────────────────────────────────────────┘
```

- **`ir_parse()`** builds the prompt from registry metadata, calls the LLM via Keel's HTTP client, parses the JSON response into an `IRIntent`. This is the only non-deterministic code path.
- **`ir_dispatch()`** takes an `IRIntent` and runs the deterministic pipeline: resolve providers, validate data, apply overrides, call engine, explain, cache.
- **Structured input** (from a UI, API, or test) skips `ir_parse()` entirely — constructs `IRIntent` directly. Same deterministic pipeline.
- **WASM build** excludes `ir_parse.c` and the HTTP client. In-browser Iris uses structured input only — the browser/server handles the LLM call and feeds `IRIntent` JSON to the WASM pipeline.

### LLM Provider Abstraction (C)

LLM API calls are HTTP POSTs with JSON bodies. Each provider differs in:
- Request body shape (Claude tool_use vs OpenAI function_calling vs Ollama JSON mode)
- Response body shape (where the structured output lives in the JSON)
- Authentication header format

```c
/* iris/include/ir_llm.h */

typedef enum {
    IR_LLM_CLAUDE = 0,     /* Anthropic: tool_use */
    IR_LLM_OPENAI = 1,     /* OpenAI/compatible: function_calling */
    IR_LLM_OLLAMA = 2,     /* Ollama: /api/chat + JSON mode */
    IR_LLM_CUSTOM = 3      /* Any OpenAI-compatible endpoint */
} IRLLMProvider;

typedef struct {
    IRLLMProvider provider;
    const char *endpoint;       /* env: IRIS_LLM_ENDPOINT */
    const char *api_key;        /* env: IRIS_LLM_API_KEY */
    const char *model;          /* env: IRIS_LLM_MODEL */
    double temperature;         /* env: IRIS_LLM_TEMPERATURE */
    uint32_t max_tokens;        /* env: IRIS_LLM_MAX_TOKENS */
    uint32_t timeout_ms;        /* env: IRIS_LLM_TIMEOUT (default: 30000) */
} IRLLMConfig;

typedef struct {
    int status;             /* HTTP status code */
    char *content;          /* Parsed structured output (JSON) */
    size_t content_len;
    double latency_ms;
} IRLLMResponse;

/* Single LLM call: build provider-specific request, POST via Keel, parse response.
   Uses kl_client_request() (sync) or kl_h2_client_request() (HTTP/2 multiplexed). */
int ir_llm_call(const IRLLMConfig *config,
                const char *system_prompt,
                const char *user_prompt,
                const char *tool_schema_json,  /* IRIntent JSON Schema */
                IRLLMResponse *response);

void ir_llm_response_free(IRLLMResponse *resp);
```

Internally, `ir_llm_call` does:
1. Build provider-specific JSON request body via `sh_json` writer
2. `kl_client_request(alloc, &cfg, "POST", endpoint, headers, n, body, body_len, &resp)` — Keel sync HTTP client
3. Parse `resp.body` differently per provider (extract structured output from JSON)

Each provider adapter builds the HTTP request body differently:

```c
/* Internal: provider-specific request builders (different JSON shapes) */

/* Claude: POST https://api.anthropic.com/v1/messages
   Headers: x-api-key, anthropic-version
   Body: {model, system, messages, tools} → response.content[].type=="tool_use" */
static int ir_llm_build_claude(ShJsonWriter *w, const char *system,
                                const char *user, const char *tool_schema);

/* OpenAI: POST https://api.openai.com/v1/chat/completions
   Headers: Authorization: Bearer
   Body: {model, messages, tools} → response.choices[].message.tool_calls[] */
static int ir_llm_build_openai(ShJsonWriter *w, const char *system,
                                const char *user, const char *tool_schema);

/* Ollama: POST http://localhost:11434/api/chat
   Body: {model, messages, format: "json"} → response.message.content (parse as JSON) */
static int ir_llm_build_ollama(ShJsonWriter *w, const char *system,
                                const char *user, const char *tool_schema);
```

### HTTP/2 for Ensemble Voting

Claude and OpenAI APIs support HTTP/2. For ensemble voting (N parallel LLM calls for confidence), Iris uses Keel's HTTP/2 client to fire N requests as **multiplexed streams over a single TCP+TLS connection** — no per-request handshake overhead:

```c
/* ir_llm.c — ensemble via HTTP/2 multiplexing */
int ir_llm_ensemble(const IRLLMConfig *config, uint32_t n_runs,
                    const char *system_prompt, const char *user_prompt,
                    const char *tool_schema_json,
                    IRLLMResponse *responses) {
    /* 1. Single H2 connection to LLM API */
    KlH2ClientConn *conn = kl_h2_client_connect(ev_ctx, alloc, &h2_cfg,
                                                  config->endpoint, on_error, ctx);

    /* 2. Fire N requests as multiplexed streams */
    for (uint32_t i = 0; i < n_runs; i++) {
        kl_h2_client_request(conn, "POST", path, headers, n_headers,
                              body, body_len, on_response, &responses[i]);
    }

    /* 3. Event loop drives all streams concurrently on one connection */
    /* 4. on_response called for each completion → parse into IRLLMResponse */
    /* 5. Vote per field across N responses */
}
```

**Config via env vars** (following OTTO `sh_args` pattern):
```
IRIS_LLM_PROVIDER=claude|openai|ollama|custom
IRIS_LLM_ENDPOINT=http://localhost:11434  (auto-set for known providers)
IRIS_LLM_API_KEY=sk-...
IRIS_LLM_MODEL=claude-sonnet-4-6
IRIS_LLM_TEMPERATURE=0.0
IRIS_LLM_TIMEOUT=30000
```

### Prompt Generation From Registry (C)

`ir_parse()` builds the system prompt from registry metadata using `sh_json`:

```c
/* ir_parse.c — prompt generation + LLM call */

/* Build system prompt from registered engine domain schemas + examples */
static char *ir_build_prompt(const IRRegistry *reg, const IRConfig *config,
                              Arena *arena) {
    ShJsonWriter w;
    sh_json_writer_init(&w, arena);

    /* For each registered engine: append capability + domain schema + examples */
    uint32_t cap_count;
    const char **caps = ir_registry_engine_capabilities(reg, ...);
    for (uint32_t i = 0; i < cap_count; i++) {
        const IRComponent *engine = ir_registry_resolve(reg, caps[i]);
        /* Append: "Capability: route_optimize\n" */
        /* Append: "Domain parameters: {domain_schema_json}\n" */
        /* Append: "Examples:\n{examples_json}\n" */
    }

    /* Append config: time vocabulary, units, data capabilities */
    /* ... */

    return arena_strdup(arena, prompt_buf);
}

/* Full parse: NL → IRIntent */
int ir_parse(const IRRegistry *reg, const IRLLMConfig *llm_config,
             const IRConfig *config, const char *user_input,
             IRIntent *intent, Arena *arena) {
    char *prompt = ir_build_prompt(reg, config, arena);
    char *tool_schema = ir_build_intent_schema(reg, arena);

    IRLLMResponse resp;
    int rc = ir_llm_call(llm_config, prompt, user_input, tool_schema, &resp);
    if (rc != 0) return rc;

    /* Parse LLM response JSON → IRIntent fields */
    rc = ir_intent_from_json(resp.content, resp.content_len, intent, arena);
    ir_llm_response_free(&resp);
    return rc;
}
```

The prompt is auto-generated from whatever engines are registered. Wire in a new engine → the prompt includes its domain schema and examples automatically.

## 2. Master Data from Any Source

Dispatchers don't describe their fleet from scratch every time. Fleet definitions, depot lists, and customer databases are **master data** — and they live in different systems depending on the customer.

### The Provider Abstraction

Iris doesn't care where data comes from. A provider for `"fleet_data"` can be:

| Provider | Source | Use Case |
|----------|--------|----------|
| Nexus | XLSX/PDF/CSV → canonical JSON | Greenfield, no TMS |
| SQL | PostgreSQL, MySQL, MSSQL | Custom databases, data warehouses |
| SAP TM | RFC/OData API | Enterprise SAP customers |
| Oracle OTM | REST API | Oracle Transportation Management |
| BluJay/E2open | REST API | Cloud TMS |
| REST/HTTP | Any JSON API | Generic integration |

**All providers output the same canonical JSON format.** The provider is the adapter — it translates from the source system's schema to Iris's canonical schema. Engines never know or care where the data came from.

```
┌──────────────────────────────────────────────────────────────────┐
│  Data Sources (customer-specific)                                │
│                                                                  │
│  ┌─────────┐  ┌──────────┐  ┌───────────┐  ┌────────────────┐  │
│  │  Nexus   │  │ Postgres │  │  SAP TM   │  │  Oracle OTM    │  │
│  │ (files)  │  │  (SQL)   │  │  (OData)  │  │  (REST API)    │  │
│  └────┬─────┘  └────┬─────┘  └─────┬─────┘  └──────┬─────────┘  │
│       │             │              │               │             │
│       ▼             ▼              ▼               ▼             │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │  IRComponent.handle() → canonical JSON                   │    │
│  │  (each provider translates to the same output format)    │    │
│  └──────────────────────────────────┬───────────────────────┘    │
└─────────────────────────────────────┼────────────────────────────┘
                                      │
                                      ▼
                               Engine receives
                             canonical fleet/order
                              JSON — source-agnostic
```

### Canonical Schemas

Regardless of source, providers emit these canonical formats:

**A. `"fleet"` — Vehicle definitions**
```json
{
  "output_type": "fleet",
  "records": [
    {
      "id": "truck-large-01",
      "type": "large",
      "depot_id": "depot-a",
      "capacity_weight_kg": 24000,
      "capacity_volume_m3": 80,
      "capacity_pallets": 33,
      "shift_start": "06:00",
      "shift_end": "18:00",
      "qualifications": ["refrigerated", "adr"],
      "max_trips": 2,
      "cost_fixed": 250,
      "cost_per_km": 1.20
    }
  ]
}
```

**B. `"customer"` — Customer/location database**
```json
{
  "output_type": "customer",
  "records": [
    {
      "id": "cust-tesco-budapest",
      "name": "TESCO Mammut",
      "address": "1024 Budapest, Mammut 1",
      "lat": 47.508333,
      "lon": 19.030000,
      "time_windows": [{"start": "08:00", "end": "12:00"}, {"start": "14:00", "end": "17:00"}],
      "service_minutes": 30,
      "dock_capacity": 2,
      "required_quals": ["tail_lift"]
    }
  ]
}
```

**C. `"order"` — Daily order set**
```json
{
  "output_type": "order",
  "records": [
    {
      "id": "ORD-2026-03-10-001",
      "type": "delivery",
      "customer_id": "cust-tesco-budapest",
      "demand_pallets": 5,
      "demand_weight_kg": 1200,
      "commodity": "food_chilled",
      "priority": "high",
      "time_window_start": "08:00",
      "time_window_end": "12:00"
    },
    {
      "id": "ORD-2026-03-10-002",
      "type": "pickup_delivery",
      "pickup_customer_id": "warehouse-south",
      "delivery_customer_id": "cust-aldi-debrecen",
      "demand_pallets": 8,
      "commodity": "general"
    }
  ]
}
```

### Provider Examples

**Nexus provider** — loads from Nexus canonical JSON files (greenfield customers, no TMS):
```c
ir_registry_add(reg, &(IRComponent){
    .name = "nexus",
    .type = IR_COMPONENT_PROVIDER,
    .capabilities = (const char*[]){"fleet_data", "order_data", "customer_data",
                                     "station_data", "facility_data", NULL},
    .handle = nexus_provider_handle,  /* reads Nexus canonical JSON files */
    .ctx = nexus_ctx
});
```

**SQL provider** — queries PostgreSQL/MySQL/MSSQL directly:
```c
ir_registry_add(reg, &(IRComponent){
    .name = "sql-fleet",
    .type = IR_COMPONENT_PROVIDER,
    .capabilities = (const char*[]){"fleet_data", "order_data", "customer_data", NULL},
    .handle = sql_provider_handle,    /* executes SQL, maps rows → canonical JSON */
    .ctx = sql_ctx                    /* connection string, query templates */
});
```

**SAP TM provider** — calls SAP Transportation Management via OData/RFC:
```c
ir_registry_add(reg, &(IRComponent){
    .name = "sap-tm",
    .type = IR_COMPONENT_PROVIDER,
    .capabilities = (const char*[]){"fleet_data", "order_data", "customer_data", NULL},
    .handle = sap_tm_provider_handle, /* OData calls → canonical JSON */
    .ctx = sap_ctx                    /* SAP endpoint, credentials, mapping config */
});
```

**Oracle OTM provider** — calls Oracle Transportation Management REST API:
```c
ir_registry_add(reg, &(IRComponent){
    .name = "oracle-otm",
    .type = IR_COMPONENT_PROVIDER,
    .capabilities = (const char*[]){"fleet_data", "order_data", "customer_data", NULL},
    .handle = otm_provider_handle,    /* REST API → canonical JSON */
    .ctx = otm_ctx                    /* OTM endpoint, auth, field mapping */
});
```

**Generic REST provider** — bridges any JSON API (BluJay, E2open, Descartes, custom TMS):
```c
ir_registry_add(reg, &(IRComponent){
    .name = "rest-fleet",
    .type = IR_COMPONENT_PROVIDER,
    .capabilities = (const char*[]){"fleet_data", NULL},
    .handle = rest_provider_handle,   /* HTTP GET/POST → jq-like mapping → canonical JSON */
    .ctx = rest_ctx                   /* URL, auth, JSONPath mapping config */
});
```

### Provider Configuration (Not Code)

Each provider is configured via JSON — **no C code changes per customer**:

```json
/* iris/config/providers/sql-waberers.json */
{
  "provider": "sql",
  "connection": "env:IRIS_SQL_DSN",
  "capabilities": {
    "fleet_data": {
      "query": "SELECT vehicle_id AS id, vehicle_type AS type, depot_code AS depot_id, max_weight AS capacity_weight_kg, max_volume AS capacity_volume_m3 FROM fleet WHERE active = true",
      "field_map": {
        "vehicle_id": "id",
        "vehicle_type": "type",
        "depot_code": "depot_id"
      }
    },
    "order_data": {
      "query": "SELECT order_no AS id, order_type AS type, customer_code AS customer_id, pallet_count AS demand_pallets FROM daily_orders WHERE plan_date = :today",
      "field_map": {}
    }
  }
}
```

```json
/* iris/config/providers/sap-tm-girteka.json */
{
  "provider": "sap_tm",
  "endpoint": "env:IRIS_SAP_ENDPOINT",
  "auth": {
    "type": "oauth2",
    "token_url": "env:IRIS_SAP_TOKEN_URL",
    "client_id": "env:IRIS_SAP_CLIENT_ID",
    "client_secret": "env:IRIS_SAP_CLIENT_SECRET"
  },
  "capabilities": {
    "fleet_data": {
      "odata_entity": "/sap/opu/odata/sap/TM_FLEET_SRV/FleetSet",
      "field_map": {
        "VehicleID": "id",
        "VehicleType": "type",
        "DepotID": "depot_id",
        "MaxWeight": "capacity_weight_kg"
      }
    },
    "order_data": {
      "odata_entity": "/sap/opu/odata/sap/TM_ORDER_SRV/FreightOrderSet",
      "filter": "PlanDate eq datetime':today:'",
      "field_map": {
        "FreightOrderID": "id",
        "OrderType": "type",
        "ShipToParty": "customer_id"
      }
    }
  }
}
```

### Composite Providers

A single deployment can mix data sources. Iris resolves each capability independently:

```c
/* Waberer's deployment: fleet from SAP TM, orders from SQL, stations from Nexus */
ir_registry_add(reg, &(IRComponent){
    .name = "sap-tm-fleet",
    .type = IR_COMPONENT_PROVIDER,
    .capabilities = (const char*[]){"fleet_data", "customer_data", NULL},
    .handle = sap_tm_provider_handle,
    .ctx = sap_fleet_ctx
});

ir_registry_add(reg, &(IRComponent){
    .name = "sql-orders",
    .type = IR_COMPONENT_PROVIDER,
    .capabilities = (const char*[]){"order_data", NULL},
    .handle = sql_provider_handle,
    .ctx = sql_order_ctx
});

ir_registry_add(reg, &(IRComponent){
    .name = "nexus-stations",
    .type = IR_COMPONENT_PROVIDER,
    .capabilities = (const char*[]){"station_data", NULL},
    .handle = nexus_provider_handle,
    .ctx = nexus_station_ctx
});
```

When Surge requests `["fleet_data", "order_data", "travel_matrix"]`:
- `fleet_data` → resolved from `sap-tm-fleet` (SAP OData call)
- `order_data` → resolved from `sql-orders` (PostgreSQL query)
- `travel_matrix` → resolved from `velo` (computed in-memory)

**The engine doesn't know. The LLM doesn't know. Only the provider config knows.**

### Data Flow

```
Dispatcher: "Plan today's routes"
        |
  [ir_parse] → capability: "route_optimize", order_source: "today's orders"
        |
  [ir_dispatch]:
    1. Find engine: "route_optimize" → Surge component
    2. Surge declares requires: ["fleet_data", "order_data", "travel_matrix"]
    3. Auto-resolve from registered providers (source-agnostic):
       fleet_data    → provider for "fleet_data" (could be Nexus, SQL, SAP, Oracle)
                       → returns canonical fleet JSON
       order_data    → provider for "order_data" (could be different source)
                       → returns canonical order JSON
       travel_matrix → Velo provider computes N×N distances
                       (or Euclidean fallback if Velo not registered)
    4. Surge engine handler builds sg_api_solve() JSON + calls solver
```

**The dispatcher says "plan today's routes" — NOT "I have 3 trucks with 20 pallets each..."** Master data handles the "each" part.

## 3. Capability-Based Dispatch (No Hardcoded Engines)

IRIntent carries a **capability string** and **domain-level parameters** — never engine-specific solver params.

```c
/* The LLM outputs a capability + domain params. Engine adapters translate. */
typedef struct {
    const char *capability;          /* "route_optimize", "refuel", etc. (arena-allocated) */

    /* Domain-level parameters (what a dispatcher would say, not solver internals).
       Schema defined per engine adapter via domain_schema_json. */
    const char *domain_params_json;  /* e.g. {problem_class: "delivery", time_budget: "standard"} */
    size_t domain_params_len;

    /* Data scope/filters (what subset of master data to fetch) */
    const char *filters_json;        /* e.g. {date: "2026-03-10", depot_ids: ["A","B"]} */
    size_t filters_len;

    /* Confidence + audit (from LLM ensemble) */
    double confidence;
    const char *audit_json;          /* LLM reasoning trace for review */
    size_t audit_len;
} IRIntent;
```

**Dispatch logic** (`ir_dispatch.c`):
```c
IRDispatchResult ir_dispatch(const IRRegistry *reg, const IRIntent *intent) {
    const IREngine *engine = NULL;
    uint32_t found = ir_registry_find(reg, intent->capability, &engine, 1);
    if (found == 0)
        return (IRDispatchResult){.status = IR_NO_ENGINE_FOR_CAPABILITY};

    char *result = NULL;
    size_t result_len = 0;
    int rc = engine->handle(intent->params_json, intent->params_len,
                            &result, &result_len, engine->engine_ctx);
    /* ... */
}
```

### Example: Wiring OTTO Engines at Startup

```c
/* main.c or ir_pipeline.c — wire engines at startup */
IRRegistry *reg = ir_registry_create();

/* Each engine provides an adapter — these are in separate .c files */
ir_register_surge(reg, surge_ctx);      /* registers: route_optimize, validate_plan, replan */
ir_register_fuelwise(reg, fw_ctx);      /* registers: refuel, filter_stations */
ir_register_velo(reg, velo_ctx);        /* registers: point_route, travel_matrix */
ir_register_locus(reg, locus_ctx);      /* registers: geocode, reverse_geocode */
ir_register_ralph(reg, ralph_ctx);      /* registers: lp_solve, assignment */
ir_register_nexus(reg, nexus_ctx);      /* registers: ingest, discover_schema */

/* Future: just add a line */
ir_register_hose(reg, hose_ctx);        /* registers: hos_check, break_schedule */
ir_register_sigma(reg, sigma_ctx);      /* registers: fleet_select */

/* Or a third-party engine */
ir_register_custom(reg, &(IREngine){
    .name = "external_optimizer",
    .capabilities = (const char*[]){"warehouse_slotting", NULL},
    .handle = my_http_bridge,           /* forwards to external HTTP API */
    .engine_ctx = &my_config
});
```

### Standard Capabilities (Convention, Not Enum)

Capabilities are strings, not enum values. Convention:

| Capability | Example Engine | What It Does |
|-----------|---------------|--------------|
| `route_optimize` | Surge | VRP/PDPTW/DARP optimization |
| `validate_plan` | Surge | Check a proposed plan for violations |
| `replan` | Surge | Freeze routes + add orders + re-solve |
| `whatif` | Iris (generic pattern) | Clone session, modify, re-dispatch any engine, diff results |
| `refuel` | FuelWise | Optimize fuel stops along a route |
| `filter_stations` | FuelWise | Snap stations to route |
| `point_route` | Velo | A-to-B routing with geometry |
| `travel_matrix` | Velo | N×N distance/duration matrix |
| `geocode` | Locus | Address → coordinates |
| `reverse_geocode` | Locus | Coordinates → address |
| `ingest` | Nexus | Load XLSX/PDF/CSV → structured data |
| `lp_solve` | Ralph | Linear/integer programming |
| `assignment` | Ralph | Bipartite matching (LAP) |
| `hos_check` | HoSE (future) | Hours of service validation |
| `fleet_select` | Sigma (future) | Fleet plan selection (SCP/MIP) |

New capabilities can be added by registering new engines — zero changes to Iris core.

### Chained Capabilities

Some workflows require multiple engines in sequence. Chains are **predefined pipeline templates** (in config), not LLM-generated at runtime. The LLM selects which chain to invoke; it does not construct chains.

```json
/* iris/config/chains/plan_and_fuel.json — predefined template */
{
  "name": "plan_and_fuel",
  "description": "Optimize routes, then optimize fuel for each route",
  "steps": [
    {"capability": "route_optimize", "output_as": "plan"},
    {"capability": "refuel", "input_from": "plan", "for_each": "$.result.routes[*]"},
    {"capability": "hos_check", "input_from": "plan", "for_each": "$.result.routes[*]"}
  ]
}
```

- `for_each` uses **JSONPath** (`$.result.routes[*]`), not domain concepts ("route"). Iris doesn't know what a route is.
- `output_as` / `input_from` are session-scoped named slots.
- The LLM can request `{capability: "chain", chain_name: "plan_and_fuel"}` — but never constructs the chain itself.

Iris executes the chain, piping outputs to inputs. Each step resolves via the registry. Chain templates are deterministic and auditable.

## 4. Problem Classification: Engine Adapter's Job, Not the LLM's

The LLM expresses intent in **domain terms** — the engine adapter translates.

| Dispatcher says | LLM domain intent | Surge adapter translates to |
|----------------|-------------------|-----------------------------|
| "Plan delivery routes" | `{problem_class: "delivery"}` | `sg_add_delivery_request()` |
| "Schedule pickups and deliveries" | `{problem_class: "pickup_delivery"}` | `sg_add_pd_request()` |
| "Dial-a-ride for passengers" | `{problem_class: "pickup_delivery", constraint: "max_ride_time"}` | `sg_add_pd_request()` + DARP params |

The LLM outputs `problem_class: "delivery"` — a domain concept any dispatcher understands. The Surge adapter decides which Surge API calls to make. **Iris core has no knowledge of VRP, PDPTW, delivery, or pickup.** These are domain concepts defined in the Surge adapter's `domain_schema_json`.

```json
/* Surge adapter's domain schema (exposed to LLM via prompt) */
{
  "problem_class": {"enum": ["delivery", "pickup_delivery"]},
  "time_budget": {"enum": ["quick", "standard", "thorough"]},
  "optimization_priority": {"enum": ["minimize_vehicles", "minimize_distance", "balance_workload"]},
  "overrides": {"type": "array", "items": {"$ref": "#/definitions/override"}}
}
```

The adapter's `handle()` translates internally:

```c
/* Inside ir_engine_surge.c — NOT in Iris core */
static int surge_handle(const char *domain_json, size_t domain_len,
                        const char *data_json, size_t data_len,
                        char **out, size_t *out_len, void *ctx) {
    /* Parse domain params */
    const char *problem_class = json_get_string(domain_json, "problem_class");

    /* Translate to Surge API calls — adapter's responsibility */
    if (strcmp(problem_class, "delivery") == 0) {
        /* Build delivery-only requests from order data */
        for (...) sg_add_delivery_request(sg, ...);
    } else if (strcmp(problem_class, "pickup_delivery") == 0) {
        /* Build paired P-D requests */
        for (...) sg_add_pd_request(sg, ...);
    }

    /* Translate time_budget → solver profile (from solver_profiles.json config) */
    const char *profile = lookup_solver_profile(domain_json, config);
    sg_api_solve(sg, profile, ...);
}
```

Rich constraints (multi-dim capacity, qualifications, compartments, LIFO/FIFO, backhaul, multi-trip) are configured in the adapter based on domain params and master data — the LLM never touches solver parameters.

## 5. Master Data Overrides (Domain-Agnostic)

"2 large trucks at Depot A, 3 medium trucks at Depot B" — this comes from master data, not per-request NL parsing.

### Master Data Flow
```
Provider (Nexus/SQL/SAP) → session cache → canonical JSON
Engine adapter reads canonical JSON → builds solver model
```

### NL Overrides via Session

Dispatchers modify master data ad-hoc. These are **generic JSON patches** stored in the session — Iris has no knowledge of what a "vehicle" or "truck" is:

- "Take truck 3 offline" → `{action: "exclude", target: "truck-3"}`
- "Add a rental truck, 20 pallets, from Depot A" → `{action: "add", data: {type: "rental", depot: "depot-a", capacity_pallets: 20}}`
- "Truck 2 is refrigerated today" → `{action: "modify", target: "truck-2", patch: {qualifications: ["refrigerated"]}}`

```c
/* Generic override — Iris core. No vehicle/fleet/route knowledge. */
/* Overrides are JSON objects stored in the session. The engine adapter
   interprets them against the canonical data in its domain context. */

int ir_session_add_override(IRSession *s, const char *override_json, size_t len);

/* When building resolved data, the session applies overrides as JSON patches.
   The engine adapter receives the already-patched canonical data. */
char *ir_session_build_resolved(IRSession *s, const char **caps, uint32_t count);
```

**Iris doesn't know what `{action: "exclude", target: "truck-3"}` means.** It stores the override in the session. When the engine adapter calls `ir_session_build_resolved()`, the session applies it as a JSON patch to the cached `fleet_data`. The Surge adapter then reads the patched fleet data and builds its model — truck-3 is simply absent.

This pattern works for any domain: exclude a warehouse from slotting, remove a container from shipping, take a train out of scheduling.

## 6. Workflow Examples (Engine-Agnostic)

These workflows use Surge as an example, but the patterns are generic. Iris core has no Surge-specific logic — everything below is expressed through the generic dispatch/session/explain interfaces.

### 6.1 Plan from Scratch
```
NL: "Plan delivery routes for tomorrow"
ir_parse → {capability: "route_optimize",
             domain_params: {problem_class: "delivery", time_budget: "standard"},
             filters: {date: "2026-03-11"}}
ir_dispatch(session):
  → resolve engine: "route_optimize" → Surge adapter
  → auto-resolve requires from providers:
     fleet_data → provider fetches/caches in session
     order_data → provider fetches (filtered by date), caches
     travel_matrix → Velo computes, caches (long TTL)
  → Surge adapter translates domain params → solver calls internally
  → Surge adapter's explain() returns structured summary JSON
```

### 6.2 Freeze and Replan
```
NL: "Freeze routes 1 and 2, add rush order ORD-99, replan"
ir_parse → {capability: "replan",
             domain_params: {frozen: ["route-1", "route-2"],
                             add_orders: ["ORD-99"]}}
ir_dispatch(session):
  → session has baseline result from previous solve (step 6.1)
  → session has overrides: frozen route IDs + new order IDs
  → Surge adapter reads baseline + overrides
  → Surge adapter INTERNALLY translates:
     frozen routes → SG_LOCK_FROZEN (Surge concept, not Iris concept)
     remaining → SG_LOCK_COMMITTED
     new orders → SG_LOCK_NONE
     builds initial_routes for warm start
  → Surge adapter's explain() returns delta vs. baseline
```

### 6.3 Validate a Plan
```
NL: "Check if this plan is feasible"
ir_parse → {capability: "validate_plan",
             domain_params: {proposed: [{vehicle: "truck-01", orders: ["ORD-1","ORD-2"]}]}}
ir_dispatch(session):
  → Surge adapter resolves IDs against session's cached fleet/order data
  → Surge adapter INTERNALLY calls sg_validate_plan()
  → Surge adapter's explain() returns structured violations list
```

### 6.4 What-If Scenarios (Generic Pattern)
```
NL: "What if we add a truck at Depot B?"
ir_parse → {capability: "whatif",
             domain_params: {modification: {action: "add", data: {type: "large", depot: "depot-b"}},
                             rerun: "route_optimize"}}
ir_whatif(session):
  1. Clone session → session_b
  2. Apply override to session_b: add vehicle
  3. Re-dispatch "route_optimize" with session_b
  4. Diff: session.result vs session_b.result (structured JSON diff)
     → {baseline_vehicles: 5, modified_vehicles: 5, baseline_distance: 1240,
        modified_distance: 1180, delta_cost: -12400}
```

What-if is a **generic Iris pattern** (clone-modify-resolve-diff), not engine-specific. It works for any capability: what-if different fuel prices (FuelWise), different constraints (Ralph), different fleet composition (Surge).

### 6.5 Monitor Long Solves
```c
/* Engine adapters register a progress callback. Iris streams it to the client.
   The callback receives structured JSON, not engine-specific types. */
typedef struct {
    double elapsed_seconds;
    double best_objective;
    double improvement_rate;     /* for plateau detection */
    const char *status;          /* "running", "converged", "time_limit" */
} IRProgress;

/* Iris progress handler — engine-agnostic */
typedef int (*ir_progress_fn)(const IRProgress *progress, void *ud);
```

Engine adapters translate their internal progress types (e.g., `SGStats`) to `IRProgress` inside their `handle()` — Iris never sees `SGStats`.

### 6.6 Explain Solutions

The `explain()` callback on each engine returns **structured JSON**, not prose:

```json
/* Surge adapter's explain() output — structured data */
{
  "summary": {
    "total_routes": 5,
    "total_distance_km": 1240,
    "total_cost": 186000,
    "unassigned_orders": 0,
    "violations": []
  },
  "routes": [
    {"vehicle": "truck-01", "stops": 8, "distance_km": 312, "utilization": 0.87}
  ]
}
```

An optional **narration layer** (LLM or template-based) converts structured JSON to prose for the dispatcher. This is a presentation concern, not a computation concern:

```c
/* ir_narrate.c — optional, converts structured explain JSON to prose */
/* Can use LLM (for natural prose) or templates (for deterministic text) */
int ir_narrate(const char *explain_json, size_t len,
               const IRNarrateConfig *config,  /* template vs LLM, language, verbosity */
               char **prose, size_t *prose_len);
```

## 7. FuelWise Workflows (Detailed)

FuelWise optimizes refueling along a fixed route. Iris integrates it in three patterns:

### 7.1 Post-Surge: Optimize Fuel for Planned Routes

**Dispatcher**: *"Optimize fuel stops for today's plan"*

This is a **chained workflow**: Surge produces routes → FuelWise optimizes refueling per route.

```
ir_parse → {capability: "refuel", domain_params: {scope: "all_routes", source: "current_plan"}}
ir_dispatch:
  → engine: FuelWise (refuel)
  → FuelWise declares requires: ["station_data", "point_route", "fleet_data"]
  → auto-resolve: station_data (provider), point_route (Velo), fleet_data (provider)
  → FuelWise handler:
      For each route in current plan:
        - Extract route geometry, get road geometry via point_route (Velo)
        - Vehicle fuel params from fleet_data (provider)
        - Station list from station_data (provider)
        - fw_optimize → per-route refueling solution
FuelWise adapter's explain() returns structured JSON:
  {"routes": [
    {"vehicle": "truck-large-01", "stops": [
      {"station": "MOL Szolnok", "km": 112, "liters": 45, "cost": 26505}],
     "total_cost": 44865, "arrival_fuel_L": 25},
    {"vehicle": "truck-medium-03", "stops": [], "total_cost": 0, "arrival_fuel_L": 85}
  ]}
ir_narrate (optional) → prose for dispatcher
```

### 7.2 Single Route Refueling

**Dispatcher**: *"Where should truck 2 refuel on the Budapest-Vienna run?"*

```
ir_parse → {capability: "refuel", domain_params: {scope: "single", vehicle_ref: "truck-02", route_ref: "Budapest→Vienna"}}
ir_dispatch:
  → engine: FuelWise (refuel)
  → auto-resolve: fleet_data (provider), point_route (Velo), station_data (provider)
FuelWise engine handler builds /api/v1/optimize request:
  {
    "route": [[47.497, 19.040], ..., [48.208, 16.373]],
    "tank_capacity": 300,
    "current_fuel": 120,
    "consumption": 32,      /* L/100km */
    "minimum_fuel": 50,
    "stations": [/* nearby stations with prices */]
  }
fw_optimize → solution
FuelWise adapter's explain() → structured JSON:
  {"recommended": {"station": "OMV Győr", "km": 121, "liters": 60, "cost": 35100},
   "alternatives": [{"station": "MOL Hegyeshalom", "km": 167, "liters": 45, "cost": 25740}],
   "without_stop": {"arrival_fuel_L": 42, "below_minimum": true}}
ir_narrate (optional) → prose for dispatcher
```

### 7.3 Piecewise Consumption (Variable Load)

**Dispatcher**: *"Optimize fuel for the Debrecen run — truck is full outbound, empty return"*

FuelWise supports route segments with different consumption rates (loaded vs. empty):

```
ir_parse → {capability: "refuel", domain_params: {scope: "single", vehicle_ref: "truck-01",
             route_ref: "Budapest→Debrecen→Budapest",
             segments: [{leg: "outbound", load: "full"}, {leg: "return", load: "empty"}]}}
FuelWise engine handler:
  "segments": [
    {"start": 0,      "consumption": 35, "weight": 24000},   /* loaded outbound */
    {"start": 231000, "consumption": 22, "weight": 8000}     /* empty return */
  ]
fw_optimize → solution accounting for variable consumption
```

### 7.4 Fleet-Wide Fuel Budget

**Dispatcher**: *"What's today's total fuel cost?"*

Chained: solve all routes, then FuelWise each one, aggregate.

```
For each route in today's plan:
  fw_optimize → per-route cost
FuelWise adapter's explain() → structured JSON:
  {"fleet_total_cost": 147405, "route_costs": [44865, 0, 31200, 52440, 18900],
   "naive_cost": 198000, "savings_pct": 25.5}
ir_narrate (optional) → prose for dispatcher
```

### 7.5 FuelWise Integration Data Flow

```
Surge solution (route geometry + stops)
        |
  ┌─────▼──────────────────────────────┐
  │ For each route:                     │
  │  1. Extract stop coordinates        │
  │  2. Velo: stop→stop road geometry  │
  │  3. Concatenate into full polyline  │
  │  4. Load vehicle fuel params        │
  │     (tank, consumption, current)    │
  │  5. Load nearby fuel stations       │
  │  6. fw_optimize() or POST /optimize │
  └─────┬──────────────────────────────┘
        │
  ┌─────▼──────────────────────────────┐
  │ Aggregate:                          │
  │  - Per-route fuel cost              │
  │  - Per-route stop count             │
  │  - Fleet total                      │
  │  - Savings vs. naive strategy       │
  └────────────────────────────────────┘
```

### 7.6 New Nexus Schema: Fuel Stations

```json
{
  "output_type": "fuel_station",
  "records": [
    {
      "id": "mol-szolnok-m5",
      "name": "MOL Szolnok M5",
      "lat": 47.1622,
      "lon": 20.1825,
      "brand": "MOL",
      "fuel_types": ["diesel", "adblue"],
      "price_diesel_per_liter": 589,
      "currency": "HUF",
      "amenities": ["parking", "shower", "restaurant"],
      "truck_accessible": true,
      "last_price_update": "2026-03-10"
    }
  ]
}
```

### 7.7 Non-Surge Workflows

```
"How far from Budapest to Vienna by truck?" → Velo (direct, no Surge)
"Load this Excel of orders" → Nexus (ingest, then available for planning)
"Match drivers to today's routes" → Ralph LAP
"Where is this address?" → Locus geocoding
```

## 8. Confidence + Audit + Data Validation

### LLM Confidence (ir_parse layer)

Following Nexus ensemble pattern. All thresholds are configurable, not hardcoded:

```c
/* iris/include/ir_confidence.h */

typedef struct {
    double auto_approve;     /* above this: no review needed */
    double warn_threshold;   /* above this but below auto_approve: proceed with warning */
                             /* below warn_threshold: block, require human review */
} IRConfidenceThresholds;

/* Load from config/defaults.json or env vars:
   IRIS_CONF_AUTO_APPROVE, IRIS_CONF_WARN_THRESHOLD */
void ir_confidence_load(IRConfidenceThresholds *t, const char *config_path);
```

Ensemble voting works across all LLM providers (same IRIntent JSON schema, different HTTP request builders in `ir_llm.c`).

Audit trail example:
```
  "plan today's routes"    → capability: "route_optimize"          [high]
  "take truck 3 offline"   → override: {exclude: "truck-3"}       [high]
  "morning delivery"       → filters: {time_window: (time vocab)} [medium] ⚠
```

### Provider Data Validation (ir_pipeline layer)

Provider data is validated against canonical schemas BEFORE reaching engines. This is deterministic validation, not LLM confidence:

```c
/* ir_validate.c — validate provider output against canonical schema */
typedef struct {
    uint32_t record_count;
    uint32_t warning_count;    /* e.g. missing optional fields */
    uint32_t error_count;      /* e.g. missing required fields, out-of-range values */
    const char *issues_json;   /* NxIssueList-style structured issues */
} IRValidationResult;

int ir_validate_provider_output(const char *capability,
                                 const char *data_json, size_t data_len,
                                 IRValidationResult *result);
```

If a SQL query returns stale data (no records for today's date) or SAP TM returns incomplete fleet records (missing capacity fields), the validation catches it before the engine produces a mathematically valid but operationally wrong plan.

## 9. No Magic Constants

All interpretive mappings are configuration, not code.

### Time Vocabulary

The LLM prompt does NOT contain hardcoded time ranges. Instead, Iris loads a **time vocabulary** config that the operator can customize per deployment:

```json
/* iris/config/time_vocab.json — operator-configurable */
{
  "locale": "hu-HU",
  "time_zone": "Europe/Budapest",
  "vocabulary": {
    "morning":    {"start": "06:00", "end": "12:00"},
    "afternoon":  {"start": "12:00", "end": "17:00"},
    "evening":    {"start": "17:00", "end": "21:00"},
    "business_hours": {"start": "08:00", "end": "17:00"},
    "full_day":   {"start": "06:00", "end": "22:00"},
    "overnight":  {"start": "22:00", "end": "06:00"}
  },
  "formats": ["HH:MM", "h:mm AM/PM", "HHmm"],
  "default_service_minutes": 30,
  "time_unit": "seconds_from_midnight"
}
```

The LLM prompt is *generated* from this config — not the other way around. If the operator says "morning means 7am-11am for us", they change the config, and the system prompt updates.

### Unit System

```json
/* iris/config/units.json */
{
  "distance": "km",
  "weight": "kg",
  "volume": "m3",
  "fuel_volume": "L",
  "fuel_consumption": "L/100km",
  "currency": "HUF",
  "temperature": "C"
}
```

All internal computation uses SI. Display units are configurable per operator. FuelWise API accepts both metric and imperial — Iris normalizes at the boundary.

### Solver Profiles

Profile selection is not hardcoded to order counts. It's configurable:

```json
/* iris/config/solver_profiles.json */
{
  "auto_select": {
    "rules": [
      {"max_orders": 50,  "profile": "FAST",         "max_time_seconds": 10},
      {"max_orders": 200, "profile": "NEAR_OPTIMAL",  "max_time_seconds": 60},
      {"max_orders": 500, "profile": "NEAR_OPTIMAL",  "max_time_seconds": 120},
      {"default": true,   "profile": "BEST",          "max_time_seconds": 300}
    ]
  }
}
```

### Capacity Dimension Labels

Not hardcoded. Defined in fleet master data and propagated:

```json
/* In Nexus fleet schema */
{
  "capacity_dimensions": [
    {"index": 0, "label": "weight_kg", "unit": "kg"},
    {"index": 1, "label": "volume_m3", "unit": "m3"},
    {"index": 2, "label": "pallets",   "unit": "count"}
  ]
}
```

### Qualification Names

Mapped from master data, not enum ordinals:

```json
/* In Nexus fleet schema */
{
  "qualifications": {
    "bit_0": "refrigerated",
    "bit_1": "adr_hazmat",
    "bit_2": "tail_lift",
    "bit_3": "crane",
    "bit_4": "mega_trailer"
  }
}
```

### String Storage

All strings use arena allocation (like Surge and Nexus), not fixed buffers. No `char field[256]` in structs — instead `const char *field` pointing into the arena. This eliminates buffer size constants entirely.

Capacity dimension count is configurable in the fleet schema (no hardcoded max). Qualification bits use `uint64_t` (64 bits available, names defined in fleet schema).

## 10. Directory Structure

```
iris/
  include/
    ir_types.h          # IRIntent, IRProgress — domain-agnostic types
    ir_engine.h         # IRComponent, IRRegistry — the wireable interface
    ir_llm.h            # LLM provider abstraction (HTTP-based, provider-agnostic)
    ir_session.h        # IRSession — multi-turn state, cache, overrides
    ir_validate.h       # Provider output + intent validation
    ir_dispatch.h       # Capability-based dispatch + auto-requirement resolution
    ir_chain.h          # Predefined multi-step pipeline execution
    ir_explain.h        # Aggregates component explain() callbacks
    ir_narrate.h        # Optional: structured JSON → prose (template or LLM call)
    ir_whatif.h         # Generic clone-modify-resolve-diff pattern
    ir_confidence.h     # LLM confidence thresholds
  src/
    ir_engine.c         # Registry: add, remove, resolve by capability
    ir_llm.c            # LLM HTTP calls: Claude, OpenAI, Ollama adapters (Keel HTTP client)
    ir_parse.c          # NL → IRIntent: prompt generation + LLM call + response parsing
    ir_session.c        # Session: data cache (TTL-aware), overrides, result cache
    ir_validate.c       # Validate provider data against canonical schemas
    ir_dispatch.c       # Resolve capability → resolve requires → call engine
    ir_chain.c          # Execute predefined pipeline templates
    ir_explain.c        # Aggregate component explainers into summary
    ir_narrate.c        # Optional: structured → prose (templates or LLM call)
    ir_whatif.c         # Clone session → apply mod → re-dispatch → diff
    ir_confidence.c     # Confidence threshold logic
  components/           # Wireable engines + providers (each registers capabilities)
    engines/
      ir_engine_surge.c   # Engine: route_optimize, validate_plan, replan
      ir_engine_fw.c      # Engine: refuel, filter_stations
      ir_engine_ralph.c   # Engine: lp_solve, assignment
      ir_engine_http.c    # Generic HTTP bridge (for external engines)
    providers/
      ir_prov_nexus.c     # Provider: fleet_data, order_data, etc. from Nexus JSON files
      ir_prov_sql.c       # Provider: fleet_data, order_data, etc. from SQL databases
      ir_prov_sap_tm.c    # Provider: fleet_data, order_data from SAP TM (OData/RFC)
      ir_prov_otm.c       # Provider: fleet_data, order_data from Oracle OTM (REST)
      ir_prov_rest.c      # Provider: generic REST/HTTP API bridge (any TMS)
      ir_prov_velo.c      # Provider: travel_matrix, point_route
      ir_prov_locus.c     # Provider: geocode, reverse_geocode
  config/
    time_vocab.json     # Operator-configurable time vocabulary
    units.json          # Unit system (metric/imperial)
    solver_profiles.json # Auto-profile selection rules
    defaults.json       # Business defaults (service times, penalties, etc.)
    providers/          # Per-deployment data source configs (JSON, not code)
      nexus-default.json    # Nexus file paths
      sql-example.json      # SQL connection + query templates
      sap-tm-example.json   # SAP TM OData endpoint + field mapping
      otm-example.json      # Oracle OTM REST config
      rest-example.json     # Generic REST API config
  tools/
    ir_pipeline         # C CLI: single binary (parse + dispatch + explain)
  tests/
    test_registry.c       # Registry: add/remove/resolve/capabilities/bindings
    test_llm.c            # LLM request builders per provider (mock HTTP, no real LLM)
    test_parse.c          # Prompt generation from registry metadata (deterministic)
    test_session.c        # Session: cache, TTL, overrides, clone, build_resolved
    test_dispatch.c       # Capability resolution + provider data resolution
    test_validate.c       # Provider output validation against canonical schemas
    test_chain.c          # Predefined pipeline template execution
    test_whatif.c         # Clone-modify-resolve-diff pattern
    test_engine_surge.c   # Surge adapter: domain params → solver calls
    test_engine_fw.c      # FuelWise adapter
    test_prov_nexus.c     # Nexus file provider
    test_prov_sql.c       # SQL provider (mock DB)
    test_prov_rest.c      # REST/HTTP provider (mock server)
    test_prov_velo.c      # Velo computation provider (cacheable, long TTL)
    test_composite.c      # Mixed data sources (fleet from SQL + orders from REST)
    test_explain.c        # Explain aggregation (structured JSON, not prose)
    test_roundtrip.c      # End-to-end: IRIntent JSON → registry → resolve → solve → explain
  examples/
    master_fleet.json       # Example Nexus fleet output
    master_customers.json   # Example Nexus customer output
    master_stations.json    # Example Nexus fuel station output
    orders_daily.json       # Example Nexus order output
    intent_vrp.json         # VRP capability example
    intent_pdptw.json       # PDPTW capability example
    intent_refuel.json      # Refuel capability example
    chain_plan_and_fuel.json # Chained: route_optimize → refuel
    chain_replan.json       # Chained: replan → refuel → hos_check
  Makefile
  CLAUDE.md
```

## 11. Phased Delivery

### Phase 1: Core Pipeline + Surge Adapter + Nexus/SQL Providers + LLM (5-6 weeks)
- `ir_engine.c` — component registry (add/remove/resolve, explicit bindings)
- `ir_session.c` — session state (data cache with TTL, overrides, result cache, clone)
- `ir_dispatch.c` — capability resolution + provider data resolution + session integration
- `ir_validate.c` — validate provider output against canonical schemas
- `ir_explain.c` — aggregate engine explain() callbacks (structured JSON)
- `ir_engine_surge.c` — first engine adapter: domain params → Surge solver calls
  - Surge adapter exports `domain_schema_json` + `examples_json`
  - Adapter internally translates: problem_class, time_budget, overrides → sg_api_solve()
- `ir_prov_nexus.c` — first data provider (Nexus JSON files, TTL=session)
- `ir_prov_sql.c` — second data provider (Postgres/MySQL, configurable TTL)
- `ir_llm.c` — LLM HTTP calls: Claude + OpenAI + Ollama adapters (Keel HTTP client)
- `ir_parse.c` — prompt generation from registry metadata + LLM call + response → IRIntent
- Canonical schemas: fleet, customer, order (provider-agnostic format)
- Provider config JSON: connection strings, query templates, field mappings
- Provider bindings config: explicit capability → provider mapping
- `config/` — time_vocab, units, solver_profiles, defaults
- Euclidean distances (Velo provider in Phase 3)
- ~70 tests (registry + session + dispatch + validate + engine + providers + roundtrip)

### Phase 2: FuelWise + TMS Providers + Chains + Ensemble (4-5 weeks)
- `ir_engine_fw.c` — FuelWise adapter (domain params → fw_optimize)
- `ir_prov_locus.c` — geocode provider (computation, long TTL)
- `ir_prov_sap_tm.c` — SAP TM provider (OData/RFC → canonical JSON, short TTL)
- `ir_prov_otm.c` — Oracle OTM provider (REST → canonical JSON)
- `ir_prov_rest.c` — generic REST provider (any TMS, configurable)
- `ir_chain.c` — predefined pipeline templates (route_optimize → refuel, JSONPath iteration)
- `ir_narrate.c` — optional structured JSON → prose (templates first, LLM later)
- Fuel station data from Nexus or SQL providers
- Ollama + HuggingFace LLM adapters in `ir_llm.c`
- Ensemble voting, `ir_confidence.c`
- Session overrides from NL (domain-agnostic JSON patches)
- ~110 tests

### Phase 3: Velo + Replan + What-If + Rich Constraints (5-6 weeks)
- `ir_prov_velo.c` — travel_matrix, point_route (computation provider, long TTL, high latency)
- `ir_engine_ralph.c` — Ralph adapter (lp_solve, assignment)
- `ir_whatif.c` — generic clone-modify-resolve-diff (works with any engine)
- Surge replan capability (freeze/replan entirely in Surge adapter — Iris sees it as `{capability: "replan"}`)
- Rich constraints handled inside Surge adapter (multi-dim, quals, compartments, LIFO/FIFO)
- FuelWise piecewise consumption (inside FuelWise adapter)
- `IRProgress` streaming from engine adapters (engine-agnostic progress type)
- ~140 tests

### Phase 4: WASM + UX + HTTP Bridge (3-4 weeks)
- WASM build (registry + components, not LLM layer)
- `ir_engine_http.c` — generic HTTP bridge for external engines/providers
- Browser demo: structured input → registry → solve → explain
- ClayShards TUI interface
- Multi-turn session state

### Phase 5: Production (3-4 weeks)
- API server (port from IRIS_PORT env var)
- Forge async integration
- HoSE/Sigma components (when those engines ship — one `ir_registry_add()` call each)
- LLM caching, rate limiting, graceful degradation

## 12. Critical Dependencies

### Iris Core (domain-agnostic, all C, single binary)
| File | Why |
|------|-----|
| `shared/include/sh_json.h` | JSON parser/writer |
| `shared/include/sh_args.h` | CLI/env arg parsing |
| `nexus/include/nx_issue.h` | Reuse NxIssueList for validation errors |
| Keel HTTP client (`kl_client_request`, `kl_h2_client_request`) | LLM API calls + REST provider calls |
| Keel mbedTLS integration (`kl_tls_*`) | HTTPS for LLM APIs + external TMS endpoints |
| Keel server (`kl_server`) | HTTP server across OTTO |

### Per-Component (each component owns its dependency)
| Component | Dependency | What It Must Match |
|-----------|-----------|-------------------|
| `ir_engine_surge.c` | `surge/src/sg_api.c`, `surge/include/sg_types.h` | JSON build model fields + enums |
| `ir_engine_fw.c` | `fuelwise/src/fw_api.c`, `fuelwise/include/fuelwise.h` | Optimize request/response format |
| `ir_engine_ralph.c` | `ralph/include/ralph.h` | Model format |
| `ir_prov_nexus.c` | `nexus/include/nx_emit.h`, `nexus/schemas/` | Canonical JSON format |
| `ir_prov_sql.c` | libpq / mysql-connector / ODBC | SQL dialect, connection pooling |
| `ir_prov_sap_tm.c` | SAP OData v2/v4 | Entity sets, field names, auth flow |
| `ir_prov_otm.c` | Oracle OTM REST API | Endpoints, JSON response format |
| `ir_prov_rest.c` | Hull HTTP client | Generic JSON mapping via config |
| `ir_prov_velo.c` | `velo/include/velo.h` | `vl_route_coords()` params |
| `ir_prov_locus.c` | `locus/include/locus.h` | `lc_search()` params |

## 13. Verification

```bash
# Unit tests (all C — deterministic tests don't call real LLMs)
make test-iris

# Deterministic dispatch (structured input, no LLM call)
./iris/tools/ir_pipeline --config config/ \
  --intent '{"capability":"route_optimize","domain_params":{"problem_class":"delivery","time_budget":"standard"},"filters":{"date":"2026-03-10"}}'

# NL end-to-end with Claude
IRIS_LLM_PROVIDER=claude IRIS_LLM_API_KEY=sk-... \
  ./iris/tools/ir_pipeline --config config/ \
  --nl "Plan today's delivery routes"

# Same with Ollama (local LLM, same binary)
IRIS_LLM_PROVIDER=ollama IRIS_LLM_MODEL=llama3 \
  ./iris/tools/ir_pipeline --config config/ \
  --nl "Plan today's delivery routes"

# Chained capabilities (predefined template)
./iris/tools/ir_pipeline --config config/ \
  --intent '{"capability":"chain","domain_params":{"chain_name":"plan_and_fuel"}}'

# List registered capabilities + bindings (verify wiring)
./iris/tools/ir_pipeline --list-capabilities
./iris/tools/ir_pipeline --list-bindings

# Validate provider configs
./iris/tools/ir_pipeline --validate-providers config/providers/
```
