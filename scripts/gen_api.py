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
        # Remove leading " * " from comment lines
        line = re.sub(r"^\s*\*\s?", "", line).strip()

        if not line:
            continue

        # Check for multi-line response_json
        if in_response_json:
            if line.startswith("@") or line.startswith("}"):
                if line.startswith("}"):
                    response_json_lines.append(line)
                result["response_json"] = "\n".join(response_json_lines)
                in_response_json = False
                if line.startswith("@"):
                    pass  # Continue processing this line below
                else:
                    continue
            else:
                response_json_lines.append(line)
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
    demo_id = f"{module_id}-{api['path'].replace('/', '-').replace('{', '').replace('}', '').replace('.', '-').strip('-')}"

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

    html += '''
                    </div>
                </div>
'''
    return html


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

    # Generate HTML for endpoints
    if VERBOSE:
        print("\n=== Generated HTML (first endpoint) ===")
        for module_id, apis in all_endpoints.items():
            if apis:
                html = generate_endpoint_html(apis[0], module_id)
                print(html[:500] + "..." if len(html) > 500 else html)
                break

    if check_mode:
        print("\n--check mode: Would verify api.html is up-to-date")
    else:
        print("\nNote: Full template rendering coming in Step 3.")
        print("Run 'python3 scripts/gen_api.py --verbose' to see parsed data.")


if __name__ == "__main__":
    main()
