#!/usr/bin/env python3
"""
gen_api.py - Generate api.html from C header annotations and config

Usage:
    python3 scripts/gen_api.py              # Generate site/api.html
    python3 scripts/gen_api.py --check      # Verify api.html is up-to-date
    python3 scripts/gen_api.py --verbose    # Show parsed annotations

Annotation format in C headers:

    /*@api
     * GET /tiles/{z}/{x}/{y}.png
     * Raster tile (PNG)
     *
     * @path z:int Zoom level (0-18)
     * @path x:int Tile X coordinate
     * @query width:int:80 Output width (default: 80)
     *
     * @returns image/png PNG image (512x512)
     * @error 400 Invalid coordinates
     *
     * @example curl http://localhost:8081/tiles/14/8529/5974.png
     * @example_comment Get tile at zoom 14
     *
     * @demo image
     * @demo_title Generate a tile using WASM
     * @demo_input z:number:14:0:18
     */

    /*@wasm
     * @export carta_api_init
     * @export carta_api_free
     */
"""

import json
import re
import shutil
import sys
from pathlib import Path


# Project root
ROOT = Path(__file__).parent.parent
SITE_DIR = ROOT / "site"
CONFIG_FILE = SITE_DIR / "api-config.json"
TEMPLATE_FILE = SITE_DIR / "api-template.html"
OUTPUT_FILE = SITE_DIR / "api.html"

VERBOSE = False


def log(msg: str):
    """Print if verbose mode is enabled."""
    if VERBOSE:
        print(msg)


def parse_api_annotation(text: str) -> dict:
    """Parse a /*@api ... */ block into a structured dict."""
    result = {
        "method": "GET",
        "path": "",
        "summary": "",
        "is_common": False,
        "path_params": [],
        "query_params": [],
        "returns": None,
        "errors": [],
        "example": "",
        "example_comment": "",
        "response_json": None,
        "demo": None,
        "demo_title": "",
        "demo_inputs": [],
    }

    lines = text.strip().split("\n")
    in_response_json = False
    response_json_lines = []

    for line in lines:
        # Remove leading " * " from comment lines, preserve indentation for JSON
        line = re.sub(r"^\s*\*\s?", "", line)

        # Check for multi-line response_json (before stripping, to preserve indentation)
        if in_response_json:
            stripped = line.strip()
            if stripped.startswith("@") or stripped == "}":
                if stripped == "}":
                    response_json_lines.append("}")
                result["response_json"] = "\n".join(response_json_lines)
                in_response_json = False
                if stripped.startswith("@"):
                    line = stripped  # Continue processing this line below
                else:
                    continue
            else:
                response_json_lines.append(line.rstrip())  # Preserve leading whitespace
                continue

        # Strip for all other line types
        line = line.strip()

        if not line:
            continue

        # First line: "GET /path" or "common" marker
        if not result["path"]:
            if line.lower() == "common":
                result["is_common"] = True
                continue
            match = re.match(r"^(GET|POST|PUT|DELETE|PATCH)\s+(.+)$", line)
            if match:
                result["method"] = match.group(1)
                result["path"] = match.group(2)
                continue

        # Second non-@ line is summary
        if not result["summary"] and not line.startswith("@"):
            result["summary"] = line
            continue

        # @path name:type Description
        if match := re.match(r"^@path\s+(\w+):(\w+)\s+(.*)$", line):
            result["path_params"].append({
                "name": match.group(1),
                "type": match.group(2),
                "description": match.group(3),
                "required": True,
            })
            continue

        # @query name:type:default Description
        if match := re.match(r"^@query\s+(\w+):(\w+):([^\s]*)\s+(.*)$", line):
            result["query_params"].append({
                "name": match.group(1),
                "type": match.group(2),
                "default": match.group(3),
                "description": match.group(4),
            })
            continue

        # @returns content-type Description
        if match := re.match(r"^@returns\s+(\S+)\s+(.*)$", line):
            result["returns"] = {
                "content_type": match.group(1),
                "description": match.group(2),
            }
            continue

        # @error code Description
        if match := re.match(r"^@error\s+(\d+)\s+(.*)$", line):
            result["errors"].append({
                "code": int(match.group(1)),
                "description": match.group(2),
            })
            continue

        # @example curl ...
        if match := re.match(r"^@example\s+(.*)$", line):
            result["example"] = match.group(1)
            continue

        # @example_comment ...
        if match := re.match(r"^@example_comment\s+(.*)$", line):
            result["example_comment"] = match.group(1)
            continue

        # @response_json (multi-line)
        if line.startswith("@response_json"):
            in_response_json = True
            response_json_lines = []
            continue

        # @demo image|json
        if match := re.match(r"^@demo\s+(\w+)$", line):
            result["demo"] = match.group(1)
            continue

        # @demo_title ...
        if match := re.match(r"^@demo_title\s+(.*)$", line):
            result["demo_title"] = match.group(1)
            continue

        # @demo_input name:type:default:min:max
        if match := re.match(r"^@demo_input\s+(.*)$", line):
            parts = match.group(1).split(":")
            inp = {"name": parts[0], "type": parts[1] if len(parts) > 1 else "text"}
            if len(parts) > 2:
                inp["default"] = parts[2]
            if len(parts) > 3:
                inp["min"] = parts[3]
            if len(parts) > 4:
                inp["max"] = parts[4]
            result["demo_inputs"].append(inp)
            continue

    return result


def parse_wasm_exports(text: str) -> list:
    """Parse a /*@wasm ... */ block for @export lines."""
    exports = []
    for line in text.split("\n"):
        line = re.sub(r"^\s*\*\s?", "", line).strip()
        if match := re.match(r"^@export\s+(\w+)$", line):
            exports.append(match.group(1))
    return exports


def parse_header_file(filepath: Path) -> tuple[list, list]:
    """Parse a C header file for @api and @wasm annotations."""
    if not filepath.exists():
        return [], []

    content = filepath.read_text()

    # Find all /*@api ... */ blocks
    api_pattern = r"/\*@api\s*(.*?)\*/"
    api_matches = re.findall(api_pattern, content, re.DOTALL)
    apis = []
    for m in api_matches:
        api = parse_api_annotation(m)
        if api["path"]:
            apis.append(api)
            log(f"    Found endpoint: {api['method']} {api['path']}")

    # Find /*@wasm ... */ block (without name)
    wasm_pattern = r"/\*@wasm\s*(.*?)\*/"
    wasm_match = re.search(wasm_pattern, content, re.DOTALL)
    exports = []
    if wasm_match:
        exports = parse_wasm_exports(wasm_match.group(1))
        log(f"    Found {len(exports)} WASM exports")

    return apis, exports


def format_json_html(json_str: str) -> str:
    """Convert a JSON string to syntax-highlighted HTML."""
    result = json_str

    # Keys: "key":
    result = re.sub(r'"(\w+)":', r'<span class="key">"\1"</span>:', result)

    # String values (after colon)
    result = re.sub(r':\s*"([^"]*)"', r': <span class="string">"\1"</span>', result)

    # Numbers in arrays
    result = re.sub(r'\[([^\]]*)\]', lambda m: '[' + re.sub(r'(\d+\.?\d*)', r'<span class="number">\1</span>', m.group(1)) + ']', result)

    # Numbers after colon
    result = re.sub(r':\s*(\d+\.?\d*)([,\s\n\}])', r': <span class="number">\1</span>\2', result)

    # Booleans
    result = re.sub(r':\s*(true|false)', r': <span class="number">\1</span>', result)

    return result


def generate_endpoint_html(api: dict, module_id: str) -> str:
    """Generate HTML for a single endpoint."""
    method_lower = api["method"].lower()

    # Special demo IDs for carta endpoints (to match legacy JavaScript)
    if module_id == "carta" and api["path"] == "/tiles/{z}/{x}/{y}.png":
        demo_id = "carta"
        img_id = "carta-tile-img"
    elif module_id == "carta" and api["path"] == "/tiles.json":
        demo_id = "tilejson"
        img_id = None
    # Special demo IDs for velo endpoints
    elif module_id == "velo" and api["path"] == "/api/v1/route":
        demo_id = "velo-route"
        img_id = None
    elif module_id == "velo" and api["path"] == "/api/v1/health":
        demo_id = "velo-health"
        img_id = None
    elif module_id == "velo" and api["path"] == "/api/v1/stats":
        demo_id = "velo-stats"
        img_id = None
    else:
        demo_id = f"{module_id}-{api['path'].replace('/', '-').replace('{', '').replace('}', '').replace('.', '-').strip('-')}"
        img_id = f"{demo_id}-img"

    html = f'''                <div class="endpoint">
                    <div class="endpoint-header">
                        <span class="method {method_lower}">{api["method"]}</span>
                        <span class="path">{api["path"]}</span>
                        <span class="endpoint-desc">{api["summary"]}</span>
                    </div>
                    <div class="endpoint-body">'''

    # Path parameters
    if api["path_params"]:
        html += '''
                        <div class="endpoint-section">
                            <h4>Path Parameters</h4>
                            <table class="params-table">
                                <tr>
                                    <th>Name</th>
                                    <th>Type</th>
                                    <th>Description</th>
                                </tr>'''
        for p in api["path_params"]:
            required = ' <span class="param-required">required</span>' if p.get("required") else ""
            html += f'''
                                <tr>
                                    <td><span class="param-name">{p["name"]}</span>{required}</td>
                                    <td><span class="param-type">{p["type"]}</span></td>
                                    <td>{p["description"]}</td>
                                </tr>'''
        html += '''
                            </table>
                        </div>'''

    # Query parameters
    if api["query_params"]:
        html += '''
                        <div class="endpoint-section">
                            <h4>Query Parameters</h4>
                            <table class="params-table">
                                <tr>
                                    <th>Name</th>
                                    <th>Type</th>
                                    <th>Description</th>
                                </tr>'''
        for p in api["query_params"]:
            default_text = f" (default: {p['default']})" if p.get("default") else ""
            html += f'''
                                <tr>
                                    <td><span class="param-name">{p["name"]}</span></td>
                                    <td><span class="param-type">{p["type"]}</span></td>
                                    <td>{p["description"]}{default_text}</td>
                                </tr>'''
        html += '''
                            </table>
                        </div>'''

    # Example
    if api["example"]:
        comment_html = f'<span class="comment"># {api["example_comment"]}</span>\n' if api["example_comment"] else ""
        # Extract the curl command and URL
        example = api["example"]
        if example.startswith("curl "):
            url_part = example[5:]
            html += f'''
                        <div class="endpoint-section">
                            <h4>Example</h4>
                            <div class="code-block">
<pre>{comment_html}<span class="command">curl</span> <span class="url">{url_part}</span></pre>
                            </div>
                        </div>'''
        else:
            html += f'''
                        <div class="endpoint-section">
                            <h4>Example</h4>
                            <div class="code-block">
<pre>{comment_html}{example}</pre>
                            </div>
                        </div>'''

    # Response
    if api["response_json"]:
        html += f'''
                        <div class="endpoint-section">
                            <h4>Response</h4>
                            <div class="code-block">
<pre>{format_json_html(api["response_json"])}</pre>
                            </div>
                        </div>'''
    elif api["returns"]:
        html += f'''
                        <div class="endpoint-section">
                            <h4>Response</h4>
                            <p style="color: var(--text-muted); font-size: 13px;">{api["returns"]["description"]}</p>
                        </div>'''

    # WASM demo
    if api["demo"]:
        html += f'''
                        <div class="wasm-demo" id="{demo_id}-demo">
                            <div class="wasm-demo-header">
                                <span class="wasm-badge">WASM</span>
                                <span class="wasm-demo-title">Try it in browser</span>
                            </div>'''

        if api["demo_title"]:
            html += f'''
                            <p style="color: var(--text-muted); font-size: 13px; margin-bottom: 12px;">
                                {api["demo_title"]}
                            </p>'''

        # Input fields
        if api["demo_inputs"]:
            html += '''
                            <div style="display: flex; gap: 12px; align-items: center; flex-wrap: wrap;">'''
            for inp in api["demo_inputs"]:
                inp_id = f"{module_id}-{inp['name']}"
                inp_type = inp.get("type", "text")
                default = inp.get("default", "")
                min_attr = f' min="{inp["min"]}"' if "min" in inp else ""
                max_attr = f' max="{inp["max"]}"' if "max" in inp else ""
                width = "50px" if inp_type == "number" and len(str(default)) <= 2 else "70px"

                html += f'''
                                <label style="font-size: 13px;">
                                    {inp["name"]}: <input type="{inp_type}" id="{inp_id}" value="{default}"{min_attr}{max_attr} style="width: {width}; padding: 4px 8px; background: var(--bg-secondary); border: 1px solid var(--border); border-radius: 4px; color: var(--text);">
                                </label>'''

            html += f'''
                                <button class="try-btn" id="{demo_id}-try-btn" disabled>Loading WASM...</button>
                            </div>'''
        else:
            html += f'''
                            <button class="try-btn" id="{demo_id}-try-btn" disabled>Loading WASM...</button>'''

        # Output area
        if api["demo"] == "image":
            actual_img_id = img_id if img_id else f"{demo_id}-img"
            html += f'''
                            <div class="demo-output" id="{demo_id}-output">
                                <img id="{actual_img_id}" alt="Generated output">
                                <div class="demo-status" id="{demo_id}-status"></div>
                            </div>'''
        else:
            html += f'''
                            <div class="demo-output" id="{demo_id}-output">
                                <div class="code-block">
                                    <pre id="{demo_id}-result"></pre>
                                </div>
                                <div class="demo-status" id="{demo_id}-status"></div>
                            </div>'''

        html += '''
                        </div>'''

    html += '''
                    </div>
                </div>
'''
    return html


def generate_wasm_handlers(all_endpoints: dict, config: dict) -> tuple[dict, dict, str]:
    """Generate WASM init code, error code, and button handlers for each module."""
    wasm_init_code = {}
    wasm_error_code = {}
    button_handlers = []

    for module in config["modules"]:
        module_id = module["id"]
        apis = all_endpoints.get(module_id, [])

        if not module.get("wasm", {}).get("enabled"):
            continue

        # Build init code for this module
        init_lines = []
        error_lines = []

        # For carta, add special handling for the PNG demo
        if module_id == "carta":
            # Use raw strings to avoid f-string brace issues with JS template literals
            init_lines.append("                const pngBtn = document.getElementById('carta-try-btn');")
            init_lines.append("                const pngStatus = document.getElementById('carta-status');")
            init_lines.append("                pngBtn.textContent = 'Generate Tile';")
            init_lines.append("                pngBtn.disabled = false;")
            init_lines.append("                pngStatus.textContent = `Carta ${cartaDemo.getVersion()} ready (Monaco PBF: ${(cartaDemo.getPBFSize() / 1024).toFixed(0)} KB)`;")
            init_lines.append("                pngStatus.className = 'demo-status success';")
            init_lines.append("                document.getElementById('carta-output').classList.add('visible');")
            init_lines.append("                enableBtn('tilejson-try-btn', 'Fetch TileJSON');")
            init_lines.append("                enableBtn('health-try-btn', 'Check Health');")
            init_lines.append("                enableBtn('stats-try-btn', 'Get Stats');")

            error_lines.append("                pngBtn.textContent = 'WASM unavailable';")
            error_lines.append("                pngStatus.textContent = 'Failed to load WASM module: ' + err.message;")
            error_lines.append("                pngStatus.className = 'demo-status error';")
            error_lines.append("                document.getElementById('carta-output').classList.add('visible');")
            error_lines.append("                disableBtn('tilejson-try-btn');")
            error_lines.append("                disableBtn('health-try-btn');")
            error_lines.append("                disableBtn('stats-try-btn');")

            # Add carta button handlers
            button_handlers.append("            document.getElementById('carta-try-btn').addEventListener('click', generateCartaTile);")
            button_handlers.append("            document.getElementById('tilejson-try-btn').addEventListener('click', fetchTileJSON);")
            button_handlers.append("            document.getElementById('health-try-btn').addEventListener('click', fetchHealth);")
            button_handlers.append("            document.getElementById('stats-try-btn').addEventListener('click', fetchStats);")

        # For velo, add routing demo handlers
        elif module_id == "velo":
            init_lines.append("                enableBtn('velo-route-try-btn', 'Calculate Route');")
            init_lines.append("                enableBtn('velo-health-try-btn', 'Check Health');")
            init_lines.append("                enableBtn('velo-stats-try-btn', 'Get Stats');")
            init_lines.append("                const veloStatus = document.getElementById('velo-route-status');")
            init_lines.append("                if (veloStatus) {")
            init_lines.append("                    veloStatus.textContent = `Velo ${veloDemo.getVersion()} ready (Monaco: ${veloDemo.getNodeCount()} nodes)`;")
            init_lines.append("                    veloStatus.className = 'demo-status success';")
            init_lines.append("                    document.getElementById('velo-route-output').classList.add('visible');")
            init_lines.append("                }")

            error_lines.append("                disableBtn('velo-route-try-btn');")
            error_lines.append("                disableBtn('velo-health-try-btn');")
            error_lines.append("                disableBtn('velo-stats-try-btn');")
            error_lines.append("                const veloStatus = document.getElementById('velo-route-status');")
            error_lines.append("                if (veloStatus) {")
            error_lines.append("                    veloStatus.textContent = 'Failed to load WASM: ' + err.message;")
            error_lines.append("                    veloStatus.className = 'demo-status error';")
            error_lines.append("                    document.getElementById('velo-route-output').classList.add('visible');")
            error_lines.append("                }")

            button_handlers.append("            document.getElementById('velo-route-try-btn').addEventListener('click', calculateVeloRoute);")
            button_handlers.append("            document.getElementById('velo-health-try-btn').addEventListener('click', fetchVeloHealth);")
            button_handlers.append("            document.getElementById('velo-stats-try-btn').addEventListener('click', fetchVeloStats);")

        wasm_init_code[module_id] = "\n".join(init_lines) if init_lines else "                // No demo buttons to enable"
        wasm_error_code[module_id] = "\n".join(error_lines) if error_lines else "                // No error handling needed"

    return wasm_init_code, wasm_error_code, "\n".join(button_handlers)


def render_template(template: str, config: dict, endpoints: dict,
                   wasm_init_code: dict, wasm_error_code: dict, button_handlers: str) -> str:
    """Render the Jinja2-like template with actual values."""

    # Simple value substitutions
    result = template
    result = result.replace("{{ config.title }}", config["title"])
    result = result.replace("{{ config.description }}", config["description"])
    result = result.replace("{{ config.canonical_url }}", config["canonical_url"])
    result = result.replace("{{ config.github_url }}", config["github_url"])

    # Process for loops for modules
    # Find {% for module in config.modules %}...{% endfor %} blocks
    module_loop_pattern = r"{% for module in config\.modules %}\n(.*?){% endfor %}"

    def replace_module_loop(match):
        template_block = match.group(1)
        result_blocks = []

        for module in config["modules"]:
            block = template_block

            # Simple substitutions
            block = block.replace("{{ module.id }}", module["id"])
            block = block.replace("{{ module.id | upper }}", module["id"].upper())
            block = block.replace("{{ module.id | capitalize }}", module["id"].capitalize())
            block = block.replace("{{ module.name }}", module["name"])
            block = block.replace("{{ module.icon }}", module["icon"])
            block = block.replace("{{ module.port }}", str(module["port"]))
            block = block.replace("{{ module.description }}", module["description"])
            block = block.replace("{{ module.header_file }}", module["header_file"])

            # Name with filters
            name = module["name"]
            name_short = name.replace(" Server", "").replace(" Optimizer", "").replace(" Geocoder", "").replace(" Tile", "")
            block = block.replace('{{ module.name | replace(" Server", "") | replace(" Optimizer", "") | replace(" Geocoder", "") }}', name_short)

            # WASM-related
            wasm = module.get("wasm", {})
            if wasm.get("enabled"):
                block = block.replace("{{ module.wasm.script }}", wasm.get("script", ""))
                block = block.replace("{{ module.wasm.wrapper }}", wasm.get("wrapper", ""))
                block = block.replace("{{ module.wasm.class_name }}", wasm.get("class_name", ""))
                block = block.replace("{{ module.wasm.factory_name }}", wasm.get("factory_name", ""))

            # Endpoint HTML
            module_endpoints = endpoints.get(module["id"], "")
            block = block.replace("{{ endpoints[module.id] }}", module_endpoints)

            # WASM init/error code
            init_code = wasm_init_code.get(module["id"], "                // No demo buttons")
            error_code = wasm_error_code.get(module["id"], "                // No error handling")
            block = block.replace('{{ wasm_init_code[module.id] | default("                // TODO: Enable demo buttons") }}', init_code)
            block = block.replace('{{ wasm_error_code[module.id] | default("                // TODO: Handle error") }}', error_code)

            # Handle conditionals within the block
            # {% if module.wasm.enabled %}...{% endif %}
            if_wasm_pattern = r"{% if module\.wasm\.enabled %}\n?(.*?){% endif %}"
            if wasm.get("enabled"):
                block = re.sub(if_wasm_pattern, r"\1", block, flags=re.DOTALL)
            else:
                block = re.sub(if_wasm_pattern, "", block, flags=re.DOTALL)

            result_blocks.append(block)

        return "".join(result_blocks)

    result = re.sub(module_loop_pattern, replace_module_loop, result, flags=re.DOTALL)

    # Process common_endpoints loop
    common_ep_pattern = r"{% for ep in config\.common_endpoints %}\n(.*?){% endfor %}"

    def replace_common_ep_loop(match):
        template_block = match.group(1)
        result_blocks = []

        for ep in config["common_endpoints"]:
            block = template_block
            block = block.replace("{{ ep.method }}", ep["method"])
            block = block.replace("{{ ep.method | lower }}", ep["method"].lower())
            block = block.replace("{{ ep.path }}", ep["path"])
            block = block.replace("{{ ep.description }}", ep["description"])
            result_blocks.append(block)

        return "".join(result_blocks)

    result = re.sub(common_ep_pattern, replace_common_ep_loop, result, flags=re.DOTALL)

    # Process status_codes loop
    status_code_pattern = r"{% for sc in config\.status_codes %}\n(.*?){% endfor %}"

    def replace_status_code_loop(match):
        template_block = match.group(1)
        result_blocks = []

        for sc in config["status_codes"]:
            block = template_block
            block = block.replace("{{ sc.code }}", str(sc["code"]))
            block = block.replace("{{ sc.text }}", sc["text"])
            block = block.replace("{{ sc.type }}", sc["type"])
            result_blocks.append(block)

        return "".join(result_blocks)

    result = re.sub(status_code_pattern, replace_status_code_loop, result, flags=re.DOTALL)

    # Insert button handlers
    result = result.replace("{{ button_handlers }}", button_handlers)

    return result


def generate_carta_wasm_functions() -> str:
    """Generate the Carta-specific WASM helper functions."""
    return '''
        // Carta-specific WASM handlers
        async function generateCartaTile() {
            if (!cartaDemo || !cartaDemo.isReady()) return;

            const btn = document.getElementById('carta-try-btn');
            const img = document.getElementById('carta-tile-img');
            const status = document.getElementById('carta-status');
            const output = document.getElementById('carta-output');

            const z = parseInt(document.getElementById('carta-z').value) || 14;
            const x = parseInt(document.getElementById('carta-x').value) || 8529;
            const y = parseInt(document.getElementById('carta-y').value) || 5974;

            btn.disabled = true;
            btn.textContent = 'Generating...';
            output.classList.add('visible');

            try {
                const startTime = performance.now();
                const dataUrl = await cartaDemo.getTileDataURL(z, x, y);
                const elapsed = (performance.now() - startTime).toFixed(1);

                img.src = dataUrl;
                img.style.display = 'block';
                status.textContent = `Tile ${z}/${x}/${y}.png generated in ${elapsed}ms`;
                status.className = 'demo-status success';
            } catch (err) {
                console.error('Tile generation failed:', err);
                img.style.display = 'none';
                status.textContent = 'Error: ' + err.message;
                status.className = 'demo-status error';
            } finally {
                btn.disabled = false;
                btn.textContent = 'Generate Tile';
            }
        }

        async function fetchTileJSON() {
            if (!cartaDemo || !cartaDemo.isReady()) return;

            const btn = document.getElementById('tilejson-try-btn');
            const result = document.getElementById('tilejson-result');
            const status = document.getElementById('tilejson-status');
            const output = document.getElementById('tilejson-output');

            btn.disabled = true;
            btn.textContent = 'Fetching...';
            output.classList.add('visible');

            try {
                const startTime = performance.now();
                const tileJson = await cartaDemo.getTileJSON();
                const elapsed = (performance.now() - startTime).toFixed(1);

                const html = formatJsonWithHighlighting(tileJson);
                const codeBlock = result.closest('.code-block');
                if (codeBlock && window.typeAnimateContent) {
                    window.typeAnimateContent(codeBlock, html);
                } else {
                    result.innerHTML = html;
                }
                status.textContent = `Fetched in ${elapsed}ms`;
                status.className = 'demo-status success';
            } catch (err) {
                console.error('TileJSON fetch failed:', err);
                result.textContent = '';
                status.textContent = 'Error: ' + err.message;
                status.className = 'demo-status error';
            } finally {
                btn.disabled = false;
                btn.textContent = 'Fetch TileJSON';
            }
        }

        function fetchHealth() {
            fetchJsonEndpoint(cartaDemo, '/api/v1/health', 'health-try-btn', 'health-result', 'health-status', 'health-output', 'Check Health');
        }

        function fetchStats() {
            fetchJsonEndpoint(cartaDemo, '/api/v1/stats', 'stats-try-btn', 'stats-result', 'stats-status', 'stats-output', 'Get Stats');
        }
'''


def generate_velo_wasm_functions() -> str:
    """Generate the Velo-specific WASM helper functions."""
    return '''
        // Velo-specific WASM handlers
        async function calculateVeloRoute() {
            if (!veloDemo || !veloDemo.isReady()) return;

            const btn = document.getElementById('velo-route-try-btn');
            const result = document.getElementById('velo-route-result');
            const status = document.getElementById('velo-route-status');
            const output = document.getElementById('velo-route-output');

            btn.disabled = true;
            btn.textContent = 'Calculating...';
            output.classList.add('visible');

            // Monaco demo coordinates (Casino to Port)
            const from = {lat: 43.7384, lon: 7.4246};
            const to = {lat: 43.7311, lon: 7.4197};

            try {
                const startTime = performance.now();
                const route = await veloDemo.route(from, to);
                const elapsed = (performance.now() - startTime).toFixed(1);

                const html = formatJsonWithHighlighting(route);
                const codeBlock = result.closest('.code-block');
                if (codeBlock && window.typeAnimateContent) {
                    window.typeAnimateContent(codeBlock, html);
                } else {
                    result.innerHTML = html;
                }
                status.textContent = `Route calculated in ${elapsed}ms (${(route.route.distance).toFixed(0)}m, ${(route.route.duration).toFixed(0)}s)`;
                status.className = 'demo-status success';
            } catch (err) {
                console.error('Route calculation failed:', err);
                result.textContent = '';
                status.textContent = 'Error: ' + err.message;
                status.className = 'demo-status error';
            } finally {
                btn.disabled = false;
                btn.textContent = 'Calculate Route';
            }
        }

        function fetchVeloHealth() {
            fetchJsonEndpoint(veloDemo, '/api/v1/health', 'velo-health-try-btn', 'velo-health-result', 'velo-health-status', 'velo-health-output', 'Check Health');
        }

        function fetchVeloStats() {
            fetchJsonEndpoint(veloDemo, '/api/v1/stats', 'velo-stats-try-btn', 'velo-stats-result', 'velo-stats-status', 'velo-stats-output', 'Get Stats');
        }
'''


def copy_wasm_files(config: dict) -> list:
    """Copy WASM demo files to site/js/ directory."""
    copied = []
    js_dir = SITE_DIR / "js"
    js_dir.mkdir(exist_ok=True)

    for module in config["modules"]:
        wasm = module.get("wasm", {})
        if not wasm.get("enabled"):
            continue

        # Copy the main WASM script (e.g., carta-api-demo.js)
        script_name = wasm.get("script", "")
        if script_name:
            src_path = ROOT / f"{module['id']}/wasm/build/{script_name}"
            dst_path = js_dir / script_name
            if src_path.exists():
                shutil.copy2(src_path, dst_path)
                copied.append(f"{module['id']}/wasm/build/{script_name} -> site/js/{script_name}")
            else:
                print(f"  Warning: WASM script not found: {src_path}")

        # Copy the wrapper script (e.g., carta-api-demo-wrapper.js)
        wrapper_name = wasm.get("wrapper", "")
        if wrapper_name:
            src_path = SITE_DIR / "js" / wrapper_name
            # Wrapper should already be in site/js/, just verify it exists
            if not src_path.exists():
                print(f"  Warning: Wrapper not found: {src_path}")

    return copied


def main():
    """Main entry point."""
    global VERBOSE
    VERBOSE = "--verbose" in sys.argv or "-v" in sys.argv
    check_mode = "--check" in sys.argv

    # Load config
    if not CONFIG_FILE.exists():
        print(f"Error: Config file not found: {CONFIG_FILE}")
        sys.exit(1)

    with open(CONFIG_FILE) as f:
        config = json.load(f)

    print(f"Loaded config with {len(config['modules'])} modules")

    # Parse annotations from each module's header file
    all_endpoints = {}
    all_exports = {}

    for module in config["modules"]:
        header_path = ROOT / module["header_file"]
        log(f"\n  Parsing {header_path}")
        if header_path.exists():
            apis, exports = parse_header_file(header_path)
            print(f"  {module['id']}: {len(apis)} endpoints, {len(exports)} WASM exports")
            all_endpoints[module["id"]] = apis
            all_exports[module["id"]] = exports
        else:
            print(f"  {module['id']}: header not found ({header_path})")
            all_endpoints[module["id"]] = []
            all_exports[module["id"]] = []

    total_endpoints = sum(len(e) for e in all_endpoints.values())
    total_exports = sum(len(e) for e in all_exports.values())
    print(f"\nTotal: {total_endpoints} endpoints, {total_exports} WASM exports")

    # Show parsed details in verbose mode
    if VERBOSE:
        print("\n=== Parsed Endpoints ===")
        for module_id, apis in all_endpoints.items():
            if apis:
                print(f"\n{module_id}:")
                for api in apis:
                    print(f"  {api['method']} {api['path']}")
                    print(f"    Summary: {api['summary']}")
                    if api['path_params']:
                        print(f"    Path params: {[p['name'] for p in api['path_params']]}")
                    if api['query_params']:
                        print(f"    Query params: {[p['name'] for p in api['query_params']]}")
                    if api['demo']:
                        print(f"    Demo: {api['demo']}")

        print("\n=== WASM Exports ===")
        for module_id, exports in all_exports.items():
            if exports:
                print(f"\n{module_id}: {exports}")

    # Generate HTML for each module's endpoints
    endpoints_html = {}
    for module_id, apis in all_endpoints.items():
        html_parts = []
        for api in apis:
            html_parts.append(generate_endpoint_html(api, module_id))
        endpoints_html[module_id] = "\n".join(html_parts)

    # Generate WASM handlers
    wasm_init_code, wasm_error_code, button_handlers = generate_wasm_handlers(all_endpoints, config)

    # Load and render template
    if not TEMPLATE_FILE.exists():
        print(f"Error: Template file not found: {TEMPLATE_FILE}")
        sys.exit(1)

    template = TEMPLATE_FILE.read_text()
    output_html = render_template(template, config, endpoints_html,
                                  wasm_init_code, wasm_error_code, button_handlers)

    # Insert module-specific WASM functions before the "// Initialize on page load" comment
    wasm_functions = generate_carta_wasm_functions() + generate_velo_wasm_functions()
    insert_marker = "        // Initialize on page load"
    if insert_marker in output_html:
        output_html = output_html.replace(insert_marker, wasm_functions + "\n" + insert_marker)

    if check_mode:
        # Compare with existing file
        if OUTPUT_FILE.exists():
            existing = OUTPUT_FILE.read_text()
            if existing == output_html:
                print(f"\n✓ {OUTPUT_FILE.name} is up-to-date")
                sys.exit(0)
            else:
                print(f"\n✗ {OUTPUT_FILE.name} needs regeneration")
                print("  Run 'python3 scripts/gen_api.py' to update")
                sys.exit(1)
        else:
            print(f"\n✗ {OUTPUT_FILE.name} does not exist")
            sys.exit(1)
    else:
        # Copy WASM files to site/js/
        copied = copy_wasm_files(config)
        if copied:
            print("\nCopied WASM files:")
            for c in copied:
                print(f"  {c}")

        # Write output
        OUTPUT_FILE.write_text(output_html)
        print(f"\n✓ Generated {OUTPUT_FILE.name} ({len(output_html):,} bytes)")


if __name__ == "__main__":
    main()
