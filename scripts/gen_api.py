#!/usr/bin/env python3
"""
gen_api.py - Generate api.html from C header annotations and config

Usage:
    python3 scripts/gen_api.py              # Generate site/api.html
    python3 scripts/gen_api.py --check      # Verify api.html is up-to-date

Annotation format in C headers:
    /*@api
     * method: GET
     * path: /tiles/{z}/{x}/{y}.png
     * description: Raster tile (PNG)
     * params:
     *   - name: z
     *     type: int
     *     required: true
     *     description: Zoom level (0-18)
     * example: curl http://localhost:8081/tiles/14/8529/5974.png -o tile.png
     * response: PNG image (512x512). Content-Type: image/png
     * wasm_demo: carta-png
     */

    /*@wasm carta-png
     * title: Generate a tile using Carta WASM
     * inputs:
     *   - id: carta-z, label: z, type: number, default: 14, min: 0, max: 18
     *   - id: carta-x, label: x, type: number, default: 8529
     *   - id: carta-y, label: y, type: number, default: 5974
     * output: image
     */
"""

import json
import re
import sys
from pathlib import Path


# Project root
ROOT = Path(__file__).parent.parent
SITE_DIR = ROOT / "site"
CONFIG_FILE = SITE_DIR / "api-config.json"
TEMPLATE_FILE = SITE_DIR / "api-template.html"
OUTPUT_FILE = SITE_DIR / "api.html"


def parse_api_annotation(text: str) -> dict:
    """Parse a /*@api ... */ block into a structured dict."""
    result = {
        "method": "GET",
        "path": "",
        "description": "",
        "params": [],
        "query_params": [],
        "example": "",
        "example_comment": "",
        "response": "",
        "response_json": None,
        "wasm_demo": None,
        "extra_sections": [],
    }

    lines = text.strip().split("\n")
    current_key = None
    current_list = None

    for line in lines:
        # Remove leading " * " from comment lines
        line = re.sub(r"^\s*\*\s?", "", line)

        # Key-value pairs
        if match := re.match(r"^(\w+):\s*(.*)$", line):
            key, value = match.groups()
            key = key.lower()

            if key == "params" or key == "query_params":
                current_key = key
                current_list = []
                result[key] = current_list
            elif key == "response_json":
                # Multi-line JSON starts
                current_key = "response_json"
                result["response_json"] = value
            else:
                result[key] = value
                current_key = key
                current_list = None

        # List items (for params)
        elif line.strip().startswith("- ") and current_list is not None:
            # Parse: - name: z, type: int, required: true, description: Zoom level
            item_text = line.strip()[2:]  # Remove "- "
            item = {}
            for part in item_text.split(","):
                if ":" in part:
                    k, v = part.split(":", 1)
                    item[k.strip()] = v.strip()
            current_list.append(item)

        # Continuation lines
        elif current_key == "response_json" and line.strip():
            result["response_json"] += "\n" + line

    return result


def parse_wasm_annotation(text: str) -> dict:
    """Parse a /*@wasm name ... */ block into a structured dict."""
    result = {
        "id": "",
        "title": "",
        "description": "",
        "inputs": [],
        "output": "json",
    }

    lines = text.strip().split("\n")
    for line in lines:
        line = re.sub(r"^\s*\*\s?", "", line)

        if match := re.match(r"^(\w+):\s*(.*)$", line):
            key, value = match.groups()
            result[key.lower()] = value
        elif line.strip().startswith("- ") and "inputs" in result:
            # Parse input definition
            item_text = line.strip()[2:]
            item = {}
            for part in item_text.split(","):
                if ":" in part:
                    k, v = part.split(":", 1)
                    item[k.strip()] = v.strip()
            result["inputs"].append(item)

    return result


def parse_header_file(filepath: Path) -> tuple[list, list]:
    """Parse a C header file for @api and @wasm annotations."""
    if not filepath.exists():
        return [], []

    content = filepath.read_text()

    # Find all /*@api ... */ blocks
    api_pattern = r"/\*@api\s*(.*?)\*/"
    api_matches = re.findall(api_pattern, content, re.DOTALL)
    apis = [parse_api_annotation(m) for m in api_matches]

    # Find all /*@wasm name ... */ blocks
    wasm_pattern = r"/\*@wasm\s+(\w+)\s*(.*?)\*/"
    wasm_matches = re.findall(wasm_pattern, content, re.DOTALL)
    wasms = []
    for name, body in wasm_matches:
        w = parse_wasm_annotation(body)
        w["id"] = name
        wasms.append(w)

    return apis, wasms


def generate_endpoint_html(api: dict, wasm_demos: dict) -> str:
    """Generate HTML for a single endpoint."""
    method_lower = api["method"].lower()

    html = f'''                <div class="endpoint">
                    <div class="endpoint-header">
                        <span class="method {method_lower}">{api["method"]}</span>
                        <span class="path">{api["path"]}</span>
                        <span class="endpoint-desc">{api["description"]}</span>
                    </div>
                    <div class="endpoint-body">'''

    # Path/Query parameters
    for param_type, param_list in [("Path Parameters", api["params"]), ("Query Parameters", api["query_params"])]:
        if param_list:
            html += f'''
                        <div class="endpoint-section">
                            <h4>{param_type}</h4>
                            <table class="params-table">
                                <tr>
                                    <th>Name</th>
                                    <th>Type</th>
                                    <th>Description</th>
                                </tr>'''
            for p in param_list:
                required = ' <span class="param-required">required</span>' if p.get("required", "").lower() == "true" else ""
                html += f'''
                                <tr>
                                    <td><span class="param-name">{p.get("name", "")}</span>{required}</td>
                                    <td><span class="param-type">{p.get("type", "")}</span></td>
                                    <td>{p.get("description", "")}</td>
                                </tr>'''
            html += '''
                            </table>
                        </div>'''

    # Example
    if api["example"]:
        comment_html = f'<span class="comment"># {api["example_comment"]}</span>\n' if api["example_comment"] else ""
        html += f'''
                        <div class="endpoint-section">
                            <h4>Example</h4>
                            <div class="code-block">
<pre>{comment_html}<span class="command">curl</span> <span class="url">{api["example"]}</span></pre>
                            </div>
                        </div>'''

    # Response
    if api["response"]:
        if api["response_json"]:
            # Format JSON with syntax highlighting
            html += f'''
                        <div class="endpoint-section">
                            <h4>Response</h4>
                            <div class="code-block">
<pre>{format_json_html(api["response_json"])}</pre>
                            </div>
                        </div>'''
        else:
            html += f'''
                        <div class="endpoint-section">
                            <h4>Response</h4>
                            <p style="color: var(--text-muted); font-size: 13px;">{api["response"]}</p>
                        </div>'''

    # WASM demo
    if api["wasm_demo"] and api["wasm_demo"] in wasm_demos:
        wasm = wasm_demos[api["wasm_demo"]]
        html += generate_wasm_demo_html(wasm)

    html += '''
                    </div>
                </div>
'''
    return html


def format_json_html(json_str: str) -> str:
    """Convert a JSON string to syntax-highlighted HTML."""
    # Simple regex-based highlighting
    result = json_str

    # Keys: "key":
    result = re.sub(r'"(\w+)":', r'<span class="key">"\1"</span>:', result)

    # String values (after colon, not a key)
    result = re.sub(r': "([^"]*)"', r': <span class="string">"\1"</span>', result)

    # Numbers
    result = re.sub(r': (\d+\.?\d*)', r': <span class="number">\1</span>', result)

    # Booleans
    result = re.sub(r': (true|false)', r': <span class="number">\1</span>', result)

    return result


def generate_wasm_demo_html(wasm: dict) -> str:
    """Generate HTML for a WASM demo section."""
    demo_id = wasm["id"]

    html = f'''
                        <div class="wasm-demo" id="{demo_id}-demo">
                            <div class="wasm-demo-header">
                                <span class="wasm-badge">WASM</span>
                                <span class="wasm-demo-title">Try it in browser</span>
                            </div>'''

    if wasm.get("description"):
        html += f'''
                            <p style="color: var(--text-muted); font-size: 13px; margin-bottom: 12px;">
                                {wasm["description"]}
                            </p>'''

    # Input fields
    if wasm["inputs"]:
        html += '''
                            <div style="display: flex; gap: 12px; align-items: center; flex-wrap: wrap;">'''
        for inp in wasm["inputs"]:
            inp_id = inp.get("id", "")
            label = inp.get("label", "")
            inp_type = inp.get("type", "text")
            default = inp.get("default", "")
            min_val = f' min="{inp["min"]}"' if "min" in inp else ""
            max_val = f' max="{inp["max"]}"' if "max" in inp else ""
            width = "50px" if inp_type == "number" and len(str(default)) <= 2 else "70px"

            html += f'''
                                <label style="font-size: 13px;">
                                    {label}: <input type="{inp_type}" id="{inp_id}" value="{default}"{min_val}{max_val} style="width: {width}; padding: 4px 8px; background: var(--bg-secondary); border: 1px solid var(--border); border-radius: 4px; color: var(--text);">
                                </label>'''

        html += f'''
                                <button class="try-btn" id="{demo_id}-try-btn" disabled>Loading WASM...</button>
                            </div>'''
    else:
        html += f'''
                            <button class="try-btn" id="{demo_id}-try-btn" disabled>Loading WASM...</button>'''

    # Output area
    if wasm["output"] == "image":
        html += f'''
                            <div class="demo-output" id="{demo_id}-output">
                                <img id="{demo_id}-img" alt="Generated output">
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
    return html


def render_template(config: dict, endpoints: dict, wasm_init: dict, wasm_error: dict, handlers: str) -> str:
    """Render the Jinja2 template with the given data.

    Note: We use a simple string replacement approach to avoid requiring Jinja2.
    For a production system, you'd want to use Jinja2 properly.
    """
    template = TEMPLATE_FILE.read_text()

    # For now, since we don't have Jinja2, we'll generate the full HTML directly
    # using the current api.html as reference. This is Step 1 - just setting up
    # the structure. Step 3 will implement full Jinja2 rendering.

    # Simple replacements for demonstration
    result = template

    # Replace config values
    result = result.replace("{{ config.title }}", config.get("title", ""))
    result = result.replace("{{ config.description }}", config.get("description", ""))
    result = result.replace("{{ config.canonical_url }}", config.get("canonical_url", ""))
    result = result.replace("{{ config.github_url }}", config.get("github_url", ""))

    return result


def main():
    """Main entry point."""
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
    all_wasm_demos = {}

    for module in config["modules"]:
        header_path = ROOT / module["header_file"]
        if header_path.exists():
            apis, wasms = parse_header_file(header_path)
            print(f"  {module['id']}: {len(apis)} endpoints, {len(wasms)} WASM demos")
            all_endpoints[module["id"]] = apis
            for w in wasms:
                all_wasm_demos[w["id"]] = w
        else:
            print(f"  {module['id']}: header not found ({header_path})")
            all_endpoints[module["id"]] = []

    # For now, just report what we found
    # Full template rendering will be implemented in Step 3
    print(f"\nTotal: {sum(len(e) for e in all_endpoints.values())} endpoints, {len(all_wasm_demos)} WASM demos")

    if check_mode:
        print("\n--check mode: Would verify api.html is up-to-date")
        # In a full implementation, we'd render and compare
    else:
        print("\nNote: Full template rendering not yet implemented.")
        print("Run 'python3 scripts/gen_api.py' after Step 3 to generate api.html")


if __name__ == "__main__":
    main()
