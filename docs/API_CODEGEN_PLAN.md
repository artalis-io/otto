# API Documentation Codegen Plan

Generate api.html entirely from C annotations + config. Preserve the CRT terminal style.

## Design Principles

1. **Single source of truth** - Endpoints defined in C headers only
2. **Simple annotations** - Minimal syntax, no redundancy
3. **Module config** - One file lists which modules to include in api.html
4. **Style preserved** - CSS/HTML structure stays exactly as-is, just templated
5. **Python scripts** - Simple, no dependencies beyond stdlib

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         api-config.json                         │
│  { "modules": ["carta", "velo", "locus", "fuelwise"] }         │
└─────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│                    scripts/gen_api.py                           │
│  1. Parse annotations from each module's ct_api.h / vl_api.h   │
│  2. Generate api.html from template                             │
│  3. Generate wasm exports.mk for each module                    │
│  4. Generate JS wrapper methods                                 │
└─────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌──────────────┬──────────────┬──────────────┬──────────────────────┐
│ site/        │ carta/wasm/  │ velo/wasm/   │ site/js/             │
│ api.html     │ exports.mk   │ exports.mk   │ *-api-gen.js         │
└──────────────┴──────────────┴──────────────┴──────────────────────┘
```

## File Structure

```
otto/
├── site/
│   ├── api-config.json        # Which modules, order, metadata
│   ├── api-template.html      # Full HTML template with placeholders
│   ├── api.html               # Generated (don't edit!)
│   ├── style.css              # Unchanged - the beautiful CRT style
│   └── js/
│       ├── carta-api-demo.js  # Hand-written WASM loader
│       └── api-demo-gen.js    # Generated convenience methods
├── scripts/
│   └── gen_api.py             # Single script does everything
├── carta/
│   └── include/ct_api.h       # Annotated endpoints
├── velo/
│   └── include/vl_api.h       # Annotated endpoints
└── ...
```

## Configuration: api-config.json

```json
{
  "title": "OTTO API Documentation",
  "description": "REST API documentation for OTTO platform services",
  "modules": [
    {
      "id": "carta",
      "name": "Carta Tile Server",
      "icon": "🗺️",
      "port": 8081,
      "description": "Serves vector tiles (MVT), raster tiles (PNG), and ASCII art tiles from OSM PBF files.",
      "header": "carta/include/ct_api.h",
      "wasm": true,
      "wasm_demo_data": "Monaco PBF"
    },
    {
      "id": "velo",
      "name": "Velo Route Server",
      "icon": "🛣️",
      "port": 8082,
      "description": "Route planning with vehicle profiles and optimization modes.",
      "header": "velo/include/vl_api.h",
      "wasm": false
    },
    {
      "id": "locus",
      "name": "Locus Geocoder",
      "icon": "📍",
      "port": 8083,
      "description": "Forward geocoding, autocomplete, and reverse geocoding.",
      "header": "locus/include/lc_api.h",
      "wasm": false
    },
    {
      "id": "fuelwise",
      "name": "FuelWise Optimizer",
      "icon": "⛽",
      "port": 8080,
      "description": "Refueling optimization using Linear Programming.",
      "header": "fuelwise/include/fw_api.h",
      "wasm": false
    }
  ],
  "common_endpoints": ["health", "stats", "metrics"]
}
```

## Annotation Format

Keep it minimal. One comment block per endpoint.

```c
// ct_api.h

/*@api
 * GET /tiles/{z}/{x}/{y}.png
 * Raster tile (PNG)
 *
 * @path z:int Zoom level (0-18)
 * @path x:int Tile X coordinate
 * @path y:int Tile Y coordinate
 *
 * @returns image/png PNG image (512x512 by default)
 * @error 400 Invalid coordinates
 * @error 404 Outside bounds
 *
 * @demo image
 * @demo_defaults z=14 x=8529 y=5974
 */

/*@api
 * GET /tiles/{z}/{x}/{y}.mvt
 * Vector tile (MVT/Protobuf)
 *
 * @path z:int Zoom level (0-18)
 * @path x:int Tile X coordinate
 * @path y:int Tile Y coordinate
 *
 * @returns application/x-protobuf Mapbox Vector Tile
 */

/*@api
 * GET /tiles/{z}/{x}/{y}.txt
 * ASCII art tile
 *
 * @path z:int Zoom level
 * @path x:int Tile X coordinate
 * @path y:int Tile Y coordinate
 * @query width:int=80 Output width in characters (20-400)
 * @query height:int=0 Output height (0=auto from aspect ratio)
 * @query charset:string=extended simple, extended, blocks, braille
 * @query invert:bool=0 1 for light background terminals
 * @query color:bool=0 1 for ANSI 256-color output
 *
 * @returns text/plain ASCII art
 */

/*@api
 * GET /tiles.json
 * TileJSON metadata
 *
 * @returns application/json TileJSON 2.2.0 object
 * @example {"tilejson":"2.2.0","name":"OTTO Carta","minzoom":0,"maxzoom":18}
 *
 * @demo json
 */

/*@api common
 * GET /api/v1/health
 * Health check (bypasses rate limit and queue)
 *
 * @returns application/json Health status
 * @example {"status":"healthy","service":"carta","version":"1.0.0"}
 *
 * @demo json
 */

/*@api common
 * GET /api/v1/stats
 * Server statistics (bypasses queue)
 *
 * @returns application/json Server stats including work_queue and rate_limit
 *
 * @demo json
 */

/*@api common
 * GET /metrics
 * Prometheus metrics
 *
 * @returns text/plain Prometheus exposition format
 */

/*@wasm
 * @export carta_api_init
 * @export carta_api_free
 * @export carta_api_ready
 * @export carta_api_handle
 * @export carta_response_status
 * @export carta_response_content_type
 * @export carta_response_body
 * @export carta_response_body_len
 * @export carta_api_pbf_size
 * @export carta_api_version
 * @export malloc
 * @export free
 */
```

## Template: api-template.html

The template preserves all existing HTML/CSS structure. Placeholders use `{{name}}` syntax.

```html
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>{{title}}</title>
    <meta name="description" content="{{description}}">
    <!-- ... rest of head unchanged ... -->

    <!-- WASM modules - only for modules with wasm:true -->
    {{#each wasm_modules}}
    <script src="wasm/{{id}}-api-demo.js"></script>
    {{/each}}
    <script src="js/api-demo-gen.js"></script>

    <link rel="stylesheet" href="style.css">
    <!-- api.html specific styles -->
    <style>
        /* ... existing api-specific styles preserved exactly ... */
    </style>
</head>
<body>
    <div class="container">
        <header><!-- unchanged --></header>

        <main>
            <section class="hero">
                <h1>REST API Documentation</h1>
                <p>All OTTO services expose RESTful APIs with JSON responses...</p>
            </section>

            <!-- Service Navigation - generated from modules -->
            <nav class="service-nav">
                {{#each modules}}
                <a href="#{{id}}">
                    <span>{{icon}}</span>
                    <span>{{name}}</span>
                    <span class="port">:{{port}}</span>
                </a>
                {{/each}}
            </nav>

            <!-- Common Endpoints -->
            <section class="service-section">
                <div class="common-endpoints">
                    <h3>Common Endpoints (All Services)</h3>
                    <div class="common-list">
                        {{#each common_endpoints}}
                        <div class="common-item">
                            <span class="method get">{{method}}</span>
                            <span class="path">{{path}}</span>
                        </div>
                        {{/each}}
                    </div>
                </div>

                {{#each common_endpoints}}
                {{> endpoint}}
                {{/each}}
            </section>

            <!-- Module Sections - generated for each module -->
            {{#each modules}}
            <section id="{{id}}" class="service-section">
                <div class="service-header">
                    <div class="service-icon">{{icon}}</div>
                    <div>
                        <div class="service-title">{{name}}</div>
                        <div class="service-port">Default port: {{port}}</div>
                    </div>
                </div>
                <p class="service-desc">{{description}}</p>

                {{#each endpoints}}
                {{> endpoint}}
                {{/each}}
            </section>
            {{/each}}

            <!-- Status Codes - static -->
            <section class="service-section">
                <h2 style="font-size: 18px; margin-bottom: 24px;">HTTP Status Codes</h2>
                <div class="status-codes">
                    <div class="status-code success"><span class="code">200</span> Success</div>
                    <!-- ... rest unchanged ... -->
                </div>
            </section>
        </main>

        <footer><!-- unchanged --></footer>
    </div>

    <!-- WASM Demo Script - generated -->
    <script>
    {{wasm_demo_script}}
    </script>
</body>
</html>
```

### Endpoint Partial

```html
<!-- endpoint partial: renders one endpoint with optional WASM demo -->
<div class="endpoint">
    <div class="endpoint-header">
        <span class="method {{method_lower}}">{{method}}</span>
        <span class="path">{{path}}</span>
        <span class="endpoint-desc">{{summary}}</span>
    </div>
    <div class="endpoint-body">
        {{#if path_params}}
        <div class="endpoint-section">
            <h4>Path Parameters</h4>
            <table class="params-table">
                <tr><th>Name</th><th>Type</th><th>Description</th></tr>
                {{#each path_params}}
                <tr>
                    <td><span class="param-name">{{name}}</span> <span class="param-required">required</span></td>
                    <td><span class="param-type">{{type}}</span></td>
                    <td>{{description}}</td>
                </tr>
                {{/each}}
            </table>
        </div>
        {{/if}}

        {{#if query_params}}
        <div class="endpoint-section">
            <h4>Query Parameters</h4>
            <table class="params-table">
                <tr><th>Name</th><th>Type</th><th>Description</th></tr>
                {{#each query_params}}
                <tr>
                    <td><span class="param-name">{{name}}</span></td>
                    <td><span class="param-type">{{type}}</span></td>
                    <td>{{description}}{{#if default}} (default: {{default}}){{/if}}</td>
                </tr>
                {{/each}}
            </table>
        </div>
        {{/if}}

        <div class="endpoint-section">
            <h4>Response</h4>
            {{#if example}}
            <div class="code-block">
                <pre>{{example_highlighted}}</pre>
            </div>
            {{else}}
            <p style="color: var(--text-muted); font-size: 13px;">{{returns_description}}</p>
            {{/if}}
        </div>

        {{#if demo}}
        <div class="wasm-demo" id="{{demo_id}}">
            <div class="wasm-demo-header">
                <span class="wasm-badge">WASM</span>
                <span class="wasm-demo-title">Try it in browser</span>
            </div>
            {{#if demo_inputs}}
            <div style="display: flex; gap: 12px; align-items: center; flex-wrap: wrap; margin-bottom: 12px;">
                {{#each demo_inputs}}
                <label style="font-size: 13px;">
                    {{name}}: <input type="{{input_type}}" id="{{input_id}}" value="{{default}}"
                        style="width: {{width}}; padding: 4px 8px; background: var(--bg-secondary); border: 1px solid var(--border); border-radius: 4px; color: var(--text);">
                </label>
                {{/each}}
            </div>
            {{/if}}
            <button class="try-btn" id="{{btn_id}}" disabled>Loading WASM...</button>
            <div class="demo-output" id="{{output_id}}">
                {{#if demo_type_image}}
                <img id="{{img_id}}" alt="Generated output">
                {{else}}
                <div class="code-block">
                    <pre id="{{result_id}}"></pre>
                </div>
                {{/if}}
                <div class="demo-status" id="{{status_id}}"></div>
            </div>
        </div>
        {{/if}}
    </div>
</div>
```

## Generator: scripts/gen_api.py

Single Python script, no dependencies.

```python
#!/usr/bin/env python3
"""
Generate api.html and WASM exports from annotated C headers.

Usage:
    python scripts/gen_api.py

Reads:  site/api-config.json, {module}/include/*_api.h
Writes: site/api.html, {module}/wasm/exports.mk, site/js/api-demo-gen.js
"""

import json
import re
import os
from pathlib import Path

ROOT = Path(__file__).parent.parent

def parse_annotations(header_path):
    """Parse /*@api ... */ blocks from a C header."""
    content = header_path.read_text()
    endpoints = []
    wasm_exports = []

    # Match /*@api ... */ blocks
    for match in re.finditer(r'/\*@api\s*(common)?\s*\n(.*?)\*/', content, re.DOTALL):
        is_common = match.group(1) == 'common'
        block = match.group(2)
        endpoint = parse_endpoint_block(block, is_common)
        if endpoint:
            endpoints.append(endpoint)

    # Match /*@wasm ... */ block
    wasm_match = re.search(r'/\*@wasm\s*\n(.*?)\*/', content, re.DOTALL)
    if wasm_match:
        for line in wasm_match.group(1).split('\n'):
            m = re.match(r'\s*\*\s*@export\s+(\w+)', line)
            if m:
                wasm_exports.append(m.group(1))

    return endpoints, wasm_exports

def parse_endpoint_block(block, is_common):
    """Parse a single endpoint annotation block."""
    lines = [l.strip().lstrip('* ') for l in block.split('\n')]

    # First non-empty line is "METHOD /path"
    first_line = next((l for l in lines if l and not l.startswith('@')), None)
    if not first_line:
        return None

    method, path = first_line.split(' ', 1)

    # Second non-empty non-@ line is summary
    summary = next((l for l in lines[1:] if l and not l.startswith('@')), '')

    endpoint = {
        'method': method,
        'path': path,
        'summary': summary,
        'is_common': is_common,
        'path_params': [],
        'query_params': [],
        'returns': None,
        'errors': [],
        'example': None,
        'demo': None,
        'demo_defaults': {}
    }

    for line in lines:
        if line.startswith('@path '):
            # @path name:type Description
            m = re.match(r'@path\s+(\w+):(\w+)\s+(.*)', line)
            if m:
                endpoint['path_params'].append({
                    'name': m.group(1),
                    'type': m.group(2),
                    'description': m.group(3)
                })
        elif line.startswith('@query '):
            # @query name:type=default Description
            m = re.match(r'@query\s+(\w+):(\w+)(?:=([^\s]+))?\s+(.*)', line)
            if m:
                endpoint['query_params'].append({
                    'name': m.group(1),
                    'type': m.group(2),
                    'default': m.group(3),
                    'description': m.group(4)
                })
        elif line.startswith('@returns '):
            # @returns content-type Description
            m = re.match(r'@returns\s+(\S+)\s+(.*)', line)
            if m:
                endpoint['returns'] = {
                    'content_type': m.group(1),
                    'description': m.group(2)
                }
        elif line.startswith('@error '):
            # @error code Description
            m = re.match(r'@error\s+(\d+)\s+(.*)', line)
            if m:
                endpoint['errors'].append({
                    'code': int(m.group(1)),
                    'description': m.group(2)
                })
        elif line.startswith('@example '):
            endpoint['example'] = line[9:]
        elif line.startswith('@demo '):
            endpoint['demo'] = line[6:]  # 'image' or 'json'
        elif line.startswith('@demo_defaults '):
            # @demo_defaults z=14 x=8529 y=5974
            defaults = {}
            for pair in line[15:].split():
                k, v = pair.split('=')
                defaults[k] = v
            endpoint['demo_defaults'] = defaults

    return endpoint

def highlight_json(json_str):
    """Add syntax highlighting spans to JSON string."""
    # Simple regex-based highlighting
    result = json_str
    # Keys
    result = re.sub(r'"(\w+)":', r'<span class="key">"\1"</span>:', result)
    # String values
    result = re.sub(r':\s*"([^"]*)"', r': <span class="string">"\1"</span>', result)
    # Numbers
    result = re.sub(r':\s*(\d+\.?\d*)', r': <span class="number">\1</span>', result)
    # Booleans
    result = re.sub(r':\s*(true|false)', r': <span class="number">\1</span>', result)
    return result

def generate_html(config, all_endpoints, template_path, output_path):
    """Generate api.html from template and parsed endpoints."""
    template = template_path.read_text()

    # Build context for template
    # ... (template rendering logic)

    output_path.write_text(html)

def generate_wasm_exports(exports, output_path):
    """Generate exports.mk for WASM build."""
    lines = ['# Generated by gen_api.py - do not edit', '']
    exports_str = ','.join(f'_{e}' for e in exports)
    lines.append(f'API_EXPORTS = {exports_str}')
    output_path.write_text('\n'.join(lines))

def generate_js_wrapper(all_endpoints, output_path):
    """Generate JavaScript convenience methods."""
    # ... generate JS methods from endpoints
    pass

def main():
    config_path = ROOT / 'site' / 'api-config.json'
    config = json.loads(config_path.read_text())

    all_endpoints = {}
    all_exports = {}

    for module in config['modules']:
        header_path = ROOT / module['header']
        if header_path.exists():
            endpoints, exports = parse_annotations(header_path)
            all_endpoints[module['id']] = endpoints
            all_exports[module['id']] = exports

            # Generate WASM exports if module has wasm: true
            if module.get('wasm') and exports:
                exports_path = ROOT / module['id'] / 'wasm' / 'exports.mk'
                generate_wasm_exports(exports, exports_path)

    # Generate api.html
    template_path = ROOT / 'site' / 'api-template.html'
    output_path = ROOT / 'site' / 'api.html'
    generate_html(config, all_endpoints, template_path, output_path)

    # Generate JS wrapper
    js_path = ROOT / 'site' / 'js' / 'api-demo-gen.js'
    generate_js_wrapper(all_endpoints, js_path)

    print(f"Generated {output_path}")

if __name__ == '__main__':
    main()
```

## Validation

The generator validates:

1. **All endpoints have required fields** - method, path, returns
2. **WASM exports match handler functions** - if @demo, handler must be exported
3. **No duplicate paths** - within a module
4. **Common endpoints defined once** - shared across modules

Errors are printed with file:line references:

```
ERROR: carta/include/ct_api.h:42: Missing @returns for GET /tiles/{z}/{x}/{y}.png
ERROR: carta/include/ct_api.h:87: Endpoint has @demo but carta_api_handle not in @wasm exports
```

## Build Integration

```makefile
# Root Makefile
.PHONY: api-docs
api-docs:
	python3 scripts/gen_api.py

# carta/wasm/Makefile
-include exports.mk  # Generated by gen_api.py

api-demo: exports.mk $(API_OUTPUT_JS)

exports.mk: ../include/ct_api.h
	python3 ../../scripts/gen_api.py
```

## Migration Steps

### Step 1: Create Config and Template (1 hour) ✅ DONE

1. ✅ Created `site/api-config.json` with module list
2. ✅ Created `site/api-template.html` with Jinja2 placeholders
3. ✅ Created `scripts/gen_api.py` with annotation parser
4. ✅ Status codes and common endpoints in config

**Files created:**
- `site/api-config.json` - Module metadata, ports, WASM config, status codes
- `site/api-template.html` - Full HTML template with `{{ }}` placeholders
- `scripts/gen_api.py` - Parser for `/*@api ... */` annotations

### Step 2: Annotate ct_api.h (1 hour) ✅ DONE

1. ✅ Added `/*@api ... */` blocks for 4 endpoints:
   - GET /tiles/{z}/{x}/{y}.png (with WASM demo)
   - GET /tiles/{z}/{x}/{y}.mvt
   - GET /tiles/{z}/{x}/{y}.txt (with query params)
   - GET /tiles.json (with response JSON and WASM demo)
2. ✅ Added `/*@wasm ... */` block with 12 exports
3. ✅ Parser correctly extracts all annotations

**Annotation format:**
```c
/*@api
 * GET /tiles/{z}/{x}/{y}.png
 * Raster tile (PNG)
 *
 * @path z:int Zoom level (0-18)
 * @query width:int:80 Output width
 * @returns image/png PNG image
 * @demo image
 * @demo_input z:number:14:0:18
 */
```

### Step 3: Implement Generator (3 hours) ✅ DONE

1. ✅ Parser for annotations (parse_api_annotation, parse_wasm_exports, parse_header_file)
2. ✅ HTML generation with syntax highlighting (generate_endpoint_html, format_json_html)
3. ✅ Template rendering (render_template with Jinja2-like substitution)
4. ✅ Carta WASM demo handlers (generate_carta_wasm_functions)
5. ✅ --check mode for CI validation

**Generated output:**
- `site/api.html` - Complete HTML documentation (46KB)
- Supports 4 Carta endpoints from annotations
- WASM demos for PNG tiles, TileJSON, health, stats
- Proper CRT-style syntax highlighting

### Step 4: Integrate and Test (1 hour) ✅ DONE

1. ✅ Added to build system:
   - `make api-docs` - Generate API documentation
   - `make api-docs-check` - Verify up-to-date (for CI)
2. ✅ api.html is now fully generated (no hand-written version)
3. ✅ CI integration via `scripts/ci.sh lint` stage
4. ✅ Structure validation confirms all sections present

**Build integration:**
```bash
make api-docs       # Regenerate site/api.html
make api-docs-check # Verify api.html is up-to-date (exits 1 if stale)
./scripts/ci.sh lint # Includes api-docs-check in lint stage
```

### Step 5: Add Other Modules (2 hours)

1. Create vl_api.h, lc_api.h, fw_api.h stubs
2. Add annotations as APIs are implemented
3. Enable WASM for each when ready

## Total Effort: ~8 hours

## Benefits

1. **Can't forget to document** - Endpoint without annotation = build warning
2. **Can't forget WASM export** - @demo requires export
3. **Easy to add modules** - Just add to config, annotate header
4. **Style guaranteed** - Template ensures consistent look
5. **Single command** - `make api-docs` regenerates everything
