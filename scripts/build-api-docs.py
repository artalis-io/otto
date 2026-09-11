#!/usr/bin/env python3
"""
build-api-docs.py - Generate api.html from C header annotations and config

Usage:
    python3 scripts/build-api-docs.py              # Generate site/api.html
    python3 scripts/build-api-docs.py --check      # Verify api.html is up-to-date
    python3 scripts/build-api-docs.py --validate   # Validate @response_json syntax
    python3 scripts/build-api-docs.py --verbose    # Show parsed annotations

Annotation format in C headers:

    /*@api
     * GET /tiles/{z}/{x}/{y}.png
     * Raster tile (PNG)
     *
     * @path z:int Zoom level (0-18)
     * @query width:int:80 Output width (default: 80)
     * @returns image/png PNG image (512x512)
     * @error 400 Invalid coordinates
     * @example curl http://localhost:8081/tiles/14/8529/5974.png
     * @demo image
     */
"""

from __future__ import annotations

import json
import re
import shutil
import sys

# This script prints check marks and reads UTF-8 sources. On Windows both
# default to the locale codepage (cp1252), which cannot represent either --
# reading raised "charmap codec can't decode" and printing raised "can't
# encode ✗". Every open() below names its encoding explicitly; this
# handles the streams, which are not ours to pass an encoding to.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass  # not a real stream, or Python < 3.7
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


# =============================================================================
# Configuration Schema
# =============================================================================

@dataclass
class WasmConfig:
    """WASM demo configuration for a module."""
    enabled: bool = False
    script: str = ""
    wrapper: str = ""
    factory_name: str = ""
    class_name: str = ""
    handlers_file: str = ""
    init_function: str = ""
    error_function: str = ""
    buttons: dict[str, str] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> WasmConfig:
        return cls(
            enabled=data.get("enabled", False),
            script=data.get("script", ""),
            wrapper=data.get("wrapper", ""),
            factory_name=data.get("factory_name", ""),
            class_name=data.get("class_name", ""),
            handlers_file=data.get("handlers_file", ""),
            init_function=data.get("init_function", ""),
            error_function=data.get("error_function", ""),
            buttons=data.get("buttons", {}),
        )


@dataclass
class ModuleConfig:
    """Configuration for an API module."""
    id: str
    name: str
    icon: str
    port: int
    description: str
    header_file: str
    wasm: WasmConfig = field(default_factory=WasmConfig)

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> ModuleConfig:
        required = ["id", "name", "icon", "port", "description", "header_file"]
        missing = [k for k in required if k not in data]
        if missing:
            raise ConfigError(f"Module missing required fields: {missing}")

        return cls(
            id=data["id"],
            name=data["name"],
            icon=data["icon"],
            port=data["port"],
            description=data["description"],
            header_file=data["header_file"],
            wasm=WasmConfig.from_dict(data.get("wasm", {})),
        )


@dataclass
class CommonEndpoint:
    """Common endpoint shared by all modules."""
    method: str
    path: str
    description: str
    wasm_demo: bool = False

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> CommonEndpoint:
        return cls(
            method=data["method"],
            path=data["path"],
            description=data["description"],
            wasm_demo=data.get("wasm_demo", False),
        )


@dataclass
class StatusCode:
    """HTTP status code documentation."""
    code: int
    text: str
    type: str

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> StatusCode:
        return cls(code=data["code"], text=data["text"], type=data["type"])


@dataclass
class Config:
    """Complete API documentation configuration."""
    title: str
    description: str
    canonical_url: str
    github_url: str
    modules: list[ModuleConfig]
    common_endpoints: list[CommonEndpoint]
    status_codes: list[StatusCode]

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> Config:
        required = ["title", "description", "canonical_url", "github_url", "modules"]
        missing = [k for k in required if k not in data]
        if missing:
            raise ConfigError(f"Config missing required fields: {missing}")

        return cls(
            title=data["title"],
            description=data["description"],
            canonical_url=data["canonical_url"],
            github_url=data["github_url"],
            modules=[ModuleConfig.from_dict(m) for m in data["modules"]],
            common_endpoints=[CommonEndpoint.from_dict(e) for e in data.get("common_endpoints", [])],
            status_codes=[StatusCode.from_dict(s) for s in data.get("status_codes", [])],
        )


# =============================================================================
# Parsed API Types
# =============================================================================

@dataclass
class PathParam:
    """API path parameter."""
    name: str
    type: str
    description: str
    required: bool = True


@dataclass
class QueryParam:
    """API query parameter."""
    name: str
    type: str
    default: str
    description: str


@dataclass
class DemoInput:
    """WASM demo input field."""
    name: str
    type: str = "text"
    default: str = ""
    min: str | None = None
    max: str | None = None
    options: list[str] | None = None  # For select dropdowns


@dataclass
class DemoConfig:
    """Demo configuration for auto-generating handlers."""
    type: str = ""                  # json, image, text, binary
    title: str = ""
    fetch_path: str = ""            # For simple fetch demos (e.g., "/api/v1/health")
    custom_handler: str = ""        # For custom handlers (function name)
    inputs: list[DemoInput] = field(default_factory=list)


@dataclass
class RequestBody:
    """Request body example."""
    format: str = "json"  # json, lp, mps, text
    content: str = ""


@dataclass
class ResponseBody:
    """Response body example."""
    format: str = "json"  # json, text, sol
    content: str = ""


@dataclass
class ApiEndpoint:
    """Parsed API endpoint from header annotations."""
    method: str = "GET"
    path: str = ""
    summary: str = ""
    is_common: bool = False
    path_params: list[PathParam] = field(default_factory=list)
    query_params: list[QueryParam] = field(default_factory=list)
    returns_type: str | None = None
    returns_desc: str | None = None
    errors: list[tuple[int, str]] = field(default_factory=list)
    example: str = ""
    example_comment: str = ""
    request_body: RequestBody | None = None  # Input example
    response_json: str | None = None  # Legacy: JSON response
    response_body: ResponseBody | None = None  # New: any format response
    demo: str | None = None  # "image", "json", "text", "binary"
    demo_title: str = ""
    demo_inputs: list[DemoInput] = field(default_factory=list)
    demo_fetch: str = ""            # Auto-fetch path for simple demos
    demo_handler: str = ""          # Custom handler function name


# =============================================================================
# Errors
# =============================================================================

class GenApiError(Exception):
    """Base exception for gen_api errors."""
    pass


class ConfigError(GenApiError):
    """Configuration file error."""
    pass


class ParseError(GenApiError):
    """Annotation parsing error."""
    pass


class TemplateError(GenApiError):
    """Template rendering error."""
    pass


# =============================================================================
# Paths
# =============================================================================

ROOT = Path(__file__).parent.parent
SITE_DIR = ROOT / "site"
CONFIG_FILE = SITE_DIR / "api-config.json"
TEMPLATE_FILE = SITE_DIR / "api-template.html"
OUTPUT_FILE = SITE_DIR / "api.html"


# =============================================================================
# Annotation Parser
# =============================================================================

class AnnotationParser:
    """Parser for @api annotations in C headers."""

    # Patterns for annotation lines
    PATTERNS = {
        "method_path": re.compile(r"^(GET|POST|PUT|DELETE|PATCH)\s+(.+)$"),
        "path_param": re.compile(r"^@path\s+(\w+):(\w+)\s+(.*)$"),
        "query_param": re.compile(r"^@query\s+(\w+):(\w+):([^\s]*)\s+(.*)$"),
        "returns": re.compile(r"^@returns\s+(\S+)\s+(.*)$"),
        "error": re.compile(r"^@error\s+(\d+)\s+(.*)$"),
        "example": re.compile(r"^@example\s+(.*)$"),
        "example_comment": re.compile(r"^@example_comment\s+(.*)$"),
        "request_body": re.compile(r"^@request_body\s+(\w+)$"),
        "response_text": re.compile(r"^@response_text$"),
        "demo": re.compile(r"^@demo\s+(\w+)$"),
        "demo_title": re.compile(r"^@demo_title\s+(.*)$"),
        "demo_input": re.compile(r"^@demo_input\s+(.*)$"),
        "demo_fetch": re.compile(r"^@demo_fetch\s+(.*)$"),
        "demo_handler": re.compile(r"^@demo_handler\s+(\w+)$"),
    }

    def __init__(self, verbose: bool = False):
        self.verbose = verbose

    def log(self, msg: str) -> None:
        if self.verbose:
            print(msg)

    def parse_file(self, filepath: Path) -> tuple[list[ApiEndpoint], list[str]]:
        """Parse a C header file for @api and @wasm annotations."""
        if not filepath.exists():
            return [], []

        content = filepath.read_text(encoding="utf-8")

        # Find all /*@api ... */ blocks
        api_pattern = r"/\*@api\s*(.*?)\*/"
        apis = []
        for match in re.finditer(api_pattern, content, re.DOTALL):
            try:
                api = self._parse_api_block(match.group(1))
                if api.path:
                    apis.append(api)
                    self.log(f"    Found: {api.method} {api.path}")
            except ParseError as e:
                print(f"  Warning: Failed to parse annotation: {e}")

        # Find /*@wasm ... */ block
        wasm_pattern = r"/\*@wasm\s*(.*?)\*/"
        exports = []
        if wasm_match := re.search(wasm_pattern, content, re.DOTALL):
            exports = self._parse_wasm_exports(wasm_match.group(1))
            self.log(f"    Found {len(exports)} WASM exports")

        return apis, exports

    def _parse_api_block(self, text: str) -> ApiEndpoint:
        """Parse a single @api annotation block."""
        api = ApiEndpoint()
        lines = text.strip().split("\n")
        in_response_json = False
        in_request_body = False
        in_response_text = False
        response_json_lines: list[str] = []
        request_body_lines: list[str] = []
        response_text_lines: list[str] = []
        request_body_format = "lp"
        brace_depth = 0

        for line in lines:
            # Remove comment prefix
            line = re.sub(r"^\s*\*\s?", "", line)

            # Handle multi-line response_json (track nested braces/brackets)
            if in_response_json:
                stripped = line.strip()
                # Count braces and brackets to handle nested objects/arrays
                brace_depth += stripped.count("{") - stripped.count("}")
                brace_depth += stripped.count("[") - stripped.count("]")
                response_json_lines.append(line)  # Preserve indentation
                # End when we're back to depth 0 (matched all braces/brackets) or hit next annotation
                if brace_depth <= 0 or stripped.startswith("@"):
                    if stripped.startswith("@"):
                        # Remove the @line from JSON, it's the next annotation
                        response_json_lines.pop()
                        line = stripped
                    api.response_json = "\n".join(response_json_lines)
                    in_response_json = False
                    if not stripped.startswith("@"):
                        continue
                else:
                    continue

            # Handle multi-line request_body (plain text until next @annotation)
            if in_request_body:
                stripped = line.strip()
                if stripped.startswith("@"):
                    # End of request body block
                    api.request_body = RequestBody(
                        format=request_body_format,
                        content="\n".join(request_body_lines).strip()
                    )
                    in_request_body = False
                    line = stripped
                else:
                    request_body_lines.append(line)
                    continue

            # Handle multi-line response_text (plain text until next @annotation)
            if in_response_text:
                stripped = line.strip()
                if stripped.startswith("@"):
                    # End of response text block
                    api.response_body = ResponseBody(
                        format="text",
                        content="\n".join(response_text_lines).strip()
                    )
                    in_response_text = False
                    line = stripped
                else:
                    response_text_lines.append(line)
                    continue

            line = line.strip()
            if not line:
                continue

            # First line: method + path
            if not api.path:
                if line.lower() == "common":
                    api.is_common = True
                    continue
                if m := self.PATTERNS["method_path"].match(line):
                    api.method = m.group(1)
                    api.path = m.group(2)
                    continue

            # Summary (first non-@ line after path)
            if not api.summary and not line.startswith("@"):
                api.summary = line
                continue

            # @path name:type Description
            if m := self.PATTERNS["path_param"].match(line):
                api.path_params.append(PathParam(
                    name=m.group(1), type=m.group(2), description=m.group(3)
                ))
                continue

            # @query name:type:default Description
            if m := self.PATTERNS["query_param"].match(line):
                api.query_params.append(QueryParam(
                    name=m.group(1), type=m.group(2),
                    default=m.group(3), description=m.group(4)
                ))
                continue

            # @returns content-type Description
            if m := self.PATTERNS["returns"].match(line):
                api.returns_type = m.group(1)
                api.returns_desc = m.group(2)
                continue

            # @error code Description
            if m := self.PATTERNS["error"].match(line):
                api.errors.append((int(m.group(1)), m.group(2)))
                continue

            # @example curl ...
            if m := self.PATTERNS["example"].match(line):
                api.example = m.group(1)
                continue

            # @example_comment ...
            if m := self.PATTERNS["example_comment"].match(line):
                api.example_comment = m.group(1)
                continue

            # @response_json (multi-line)
            if line.startswith("@response_json"):
                in_response_json = True
                response_json_lines = []
                continue

            # @request_body format (multi-line) - starts block for input example
            if m := self.PATTERNS["request_body"].match(line):
                request_body_format = m.group(1)  # lp, mps, json, text
                in_request_body = True
                request_body_lines = []
                continue

            # @response_text (multi-line) - plain text response (SOL format, etc.)
            if line.startswith("@response_text"):
                in_response_text = True
                response_text_lines = []
                continue

            # @demo image|json
            if m := self.PATTERNS["demo"].match(line):
                api.demo = m.group(1)
                continue

            # @demo_title ...
            if m := self.PATTERNS["demo_title"].match(line):
                api.demo_title = m.group(1)
                continue

            # @demo_input name:type:default:min:max
            # @demo_input name:select:default:option1,option2,option3
            if m := self.PATTERNS["demo_input"].match(line):
                parts = m.group(1).split(":")
                inp = DemoInput(name=parts[0])
                if len(parts) > 1:
                    inp.type = parts[1]
                if len(parts) > 2:
                    inp.default = parts[2]
                if inp.type == "select" and len(parts) > 3:
                    # For select, 4th part is comma-separated options
                    inp.options = parts[3].split(",")
                elif len(parts) > 3:
                    inp.min = parts[3]
                if len(parts) > 4 and inp.type != "select":
                    inp.max = parts[4]
                api.demo_inputs.append(inp)
                continue

            # @demo_fetch /api/v1/health (auto-generate simple fetch handler)
            if m := self.PATTERNS["demo_fetch"].match(line):
                api.demo_fetch = m.group(1).strip()
                continue

            # @demo_handler customFunctionName (use custom handler)
            if m := self.PATTERNS["demo_handler"].match(line):
                api.demo_handler = m.group(1)
                continue

        # Finalize any pending multi-line blocks
        if in_request_body and request_body_lines:
            api.request_body = RequestBody(
                format=request_body_format,
                content="\n".join(request_body_lines).strip()
            )
        if in_response_text and response_text_lines:
            api.response_body = ResponseBody(
                format="text",
                content="\n".join(response_text_lines).strip()
            )

        return api

    def _parse_wasm_exports(self, text: str) -> list[str]:
        """Parse @export lines from a @wasm block."""
        exports = []
        for line in text.split("\n"):
            line = re.sub(r"^\s*\*\s?", "", line).strip()
            if m := re.match(r"^@export\s+(\w+)$", line):
                exports.append(m.group(1))
        return exports


# =============================================================================
# HTML Generator
# =============================================================================

class HtmlGenerator:
    """Generates HTML from parsed API endpoints."""

    def __init__(self, config: Config):
        self.config = config

    def format_json_html(self, json_str: str) -> str:
        """Convert JSON to syntax-highlighted HTML."""
        result = json_str
        # Keys
        result = re.sub(r'"(\w+)":', r'<span class="key">"\1"</span>:', result)
        # String values
        result = re.sub(r':\s*"([^"]*)"', r': <span class="string">"\1"</span>', result)
        # Numbers in arrays
        result = re.sub(
            r'\[([^\]]*)\]',
            lambda m: '[' + re.sub(r'(\d+\.?\d*)', r'<span class="number">\1</span>', m.group(1)) + ']',
            result
        )
        # Numbers after colon
        result = re.sub(r':\s*(\d+\.?\d*)([,\s\n\}])', r': <span class="number">\1</span>\2', result)
        # Booleans
        result = re.sub(r':\s*(true|false)', r': <span class="number">\1</span>', result)
        return result

    def generate_endpoint(self, api: ApiEndpoint, module_id: str) -> str:
        """Generate HTML for a single endpoint."""
        method_lower = api.method.lower()
        demo_id = self._get_demo_id(api, module_id)

        html = f'''                <div class="endpoint">
                    <div class="endpoint-header">
                        <span class="method {method_lower}">{api.method}</span>
                        <span class="path">{api.path}</span>
                        <span class="endpoint-desc">{api.summary}</span>
                    </div>
                    <div class="endpoint-body">'''

        # Path parameters
        if api.path_params:
            html += self._generate_params_table("Path Parameters", api.path_params, is_path=True)

        # Query parameters
        if api.query_params:
            html += self._generate_params_table("Query Parameters", api.query_params, is_path=False)

        # Example
        if api.example:
            html += self._generate_example(api)

        # Request Body (Input example - shown before response)
        if api.request_body and api.request_body.content:
            html += self._generate_request_body(api.request_body)

        # Response
        if api.response_body and api.response_body.content:
            # New: text/SOL response format
            html += self._generate_response_body(api.response_body)
        elif api.response_json:
            html += f'''
                        <div class="endpoint-section">
                            <h4>Response</h4>
                            <div class="code-block">
<pre>{self.format_json_html(api.response_json)}</pre>
                            </div>
                        </div>'''
        elif api.returns_desc:
            html += f'''
                        <div class="endpoint-section">
                            <h4>Response</h4>
                            <p style="color: var(--text-muted); font-size: 13px;">{api.returns_desc}</p>
                        </div>'''

        # WASM demo
        if api.demo:
            html += self._generate_wasm_demo(api, demo_id, module_id)

        html += '''
                    </div>
                </div>
'''
        return html

    def _get_demo_id(self, api: ApiEndpoint, module_id: str) -> str:
        """Generate a unique demo ID for an endpoint."""
        # Special cases for legacy JavaScript compatibility
        special_ids = {
            ("carta", "/tiles/{z}/{x}/{y}.png"): "carta",
            ("carta", "/tiles.json"): "tilejson",
            ("velo", "/api/v1/route"): "velo-route",
            ("velo", "/api/v1/health"): "velo-health",
            ("velo", "/api/v1/stats"): "velo-stats",
        }
        if (module_id, api.path) in special_ids:
            return special_ids[(module_id, api.path)]

        # Generate from path
        clean_path = api.path.replace("/", "-").replace("{", "").replace("}", "")
        clean_path = clean_path.replace(".", "-").strip("-")
        return f"{module_id}-{clean_path}"

    def _generate_params_table(self, title: str, params: list, is_path: bool) -> str:
        """Generate a parameters table."""
        html = f'''
                        <div class="endpoint-section">
                            <h4>{title}</h4>
                            <table class="params-table">
                                <tr>
                                    <th>Name</th>
                                    <th>Type</th>
                                    <th>Description</th>
                                </tr>'''

        for p in params:
            if is_path:
                required = ' <span class="param-required">required</span>' if getattr(p, 'required', True) else ""
                desc = p.description
            else:
                required = ""
                default_text = f" (default: {p.default})" if p.default else ""
                desc = f"{p.description}{default_text}"

            html += f'''
                                <tr>
                                    <td><span class="param-name">{p.name}</span>{required}</td>
                                    <td><span class="param-type">{p.type}</span></td>
                                    <td>{desc}</td>
                                </tr>'''

        html += '''
                            </table>
                        </div>'''
        return html

    def _generate_example(self, api: ApiEndpoint) -> str:
        """Generate example code block."""
        comment_html = f'<span class="comment"># {api.example_comment}</span>\n' if api.example_comment else ""

        if api.example.startswith("curl "):
            url_part = api.example[5:]
            return f'''
                        <div class="endpoint-section">
                            <h4>Example</h4>
                            <div class="code-block">
<pre>{comment_html}<span class="command">curl</span> <span class="url">{url_part}</span></pre>
                            </div>
                        </div>'''
        else:
            return f'''
                        <div class="endpoint-section">
                            <h4>Example</h4>
                            <div class="code-block">
<pre>{comment_html}{api.example}</pre>
                            </div>
                        </div>'''

    def _generate_request_body(self, request_body: RequestBody) -> str:
        """Generate request body example block (input)."""
        # Format label based on type
        format_labels = {
            "lp": "LP Format",
            "mps": "MPS Format",
            "json": "JSON",
            "text": "Text",
        }
        label = format_labels.get(request_body.format, request_body.format.upper())

        # For JSON, apply syntax highlighting
        if request_body.format == "json":
            content_html = self.format_json_html(request_body.content)
        else:
            # For LP/MPS/text, just escape and show as-is
            content_html = request_body.content.replace("<", "&lt;").replace(">", "&gt;")

        return f'''
                        <div class="endpoint-section">
                            <h4>Request Body <span style="color: var(--text-muted); font-weight: normal; font-size: 12px;">({label})</span></h4>
                            <div class="code-block">
<pre>{content_html}</pre>
                            </div>
                        </div>'''

    def _generate_response_body(self, response_body: ResponseBody) -> str:
        """Generate response body example block (output)."""
        # Format label based on type
        format_labels = {
            "json": "JSON",
            "text": "Text",
            "sol": "SOL Format",
        }
        label = format_labels.get(response_body.format, response_body.format.upper())

        # For JSON, apply syntax highlighting
        if response_body.format == "json":
            content_html = self.format_json_html(response_body.content)
        else:
            # For text/SOL, just escape and show as-is
            content_html = response_body.content.replace("<", "&lt;").replace(">", "&gt;")

        return f'''
                        <div class="endpoint-section">
                            <h4>Response <span style="color: var(--text-muted); font-weight: normal; font-size: 12px;">({label})</span></h4>
                            <div class="code-block">
<pre>{content_html}</pre>
                            </div>
                        </div>'''

    def _generate_wasm_demo(self, api: ApiEndpoint, demo_id: str, module_id: str) -> str:
        """Generate WASM demo section."""
        html = f'''
                        <div class="wasm-demo" id="{demo_id}-demo">
                            <div class="wasm-demo-header">
                                <span class="wasm-badge">WASM</span>
                                <span class="wasm-demo-title">Try it in browser</span>
                            </div>'''

        if api.demo_title:
            html += f'''
                            <p style="color: var(--text-muted); font-size: 13px; margin-bottom: 12px;">
                                {api.demo_title}
                            </p>'''

        # Input fields
        if api.demo_inputs:
            html += '''
                            <div style="display: flex; gap: 12px; align-items: center; flex-wrap: wrap;">'''
            for inp in api.demo_inputs:
                inp_id = f"{demo_id}-{inp.name}"

                if inp.type == "select" and inp.options:
                    # Render as dropdown
                    options_html = ""
                    for opt in inp.options:
                        selected = ' selected' if opt == inp.default else ''
                        options_html += f'<option value="{opt}"{selected}>{opt}</option>'
                    html += f'''
                                <label style="font-size: 13px;">
                                    {inp.name}: <select id="{inp_id}" style="padding: 4px 8px; background: var(--bg-secondary); border: 1px solid var(--border); border-radius: 4px; color: var(--text);">{options_html}</select>
                                </label>'''
                else:
                    # Render as input
                    min_attr = f' min="{inp.min}"' if inp.min else ""
                    max_attr = f' max="{inp.max}"' if inp.max else ""
                    width = "50px" if inp.type == "number" and len(inp.default) <= 2 else "70px"
                    html += f'''
                                <label style="font-size: 13px;">
                                    {inp.name}: <input type="{inp.type}" id="{inp_id}" value="{inp.default}"{min_attr}{max_attr} style="width: {width}; padding: 4px 8px; background: var(--bg-secondary); border: 1px solid var(--border); border-radius: 4px; color: var(--text);">
                                </label>'''

            html += f'''
                                <button class="try-btn" id="{demo_id}-try-btn" disabled>Loading WASM...</button>
                            </div>'''
        else:
            html += f'''
                            <button class="try-btn" id="{demo_id}-try-btn" disabled>Loading WASM...</button>'''

        # Output area
        if api.demo == "image":
            img_id = "carta-tile-img" if demo_id == "carta" else f"{demo_id}-img"
            html += f'''
                            <div class="demo-output" id="{demo_id}-output">
                                <div class="crt-image-wrapper">
                                    <div class="tv-static-placeholder" id="{demo_id}-placeholder"></div>
                                    <img id="{img_id}" alt="Generated output" style="display: none;">
                                </div>
                                <div class="demo-status" id="{demo_id}-status"></div>
                            </div>'''
        elif api.demo == "text":
            # ASCII/text output - no line spacing
            html += f'''
                            <div class="demo-output ascii-output" id="{demo_id}-output">
                                <div class="code-block">
                                    <pre id="{demo_id}-result"></pre>
                                </div>
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


# =============================================================================
# Handler Generator
# =============================================================================

class HandlerGenerator:
    """Generates JavaScript handler functions from API annotations."""

    def __init__(self, config: Config, endpoints: dict[str, list[ApiEndpoint]]):
        self.config = config
        self.endpoints = endpoints

    def generate(self) -> str:
        """Generate all handler functions as JavaScript code."""
        parts = [
            "/**",
            " * Auto-generated API handlers",
            " * Generated by build-api-docs.py - DO NOT EDIT",
            " */",
            "",
            "// Common utilities",
            self._generate_utilities(),
            "",
        ]

        # Generate handlers for each module
        for module in self.config.modules:
            if not module.wasm.enabled:
                continue

            module_handlers = self._generate_module_handlers(module)
            if module_handlers:
                parts.append(f"// {module.name} handlers")
                parts.append(module_handlers)
                parts.append("")

        # Generate init/error functions
        parts.append("// Module initialization")
        for module in self.config.modules:
            if module.wasm.enabled:
                parts.append(self._generate_init_function(module))
                parts.append(self._generate_error_function(module))
                parts.append("")

        return "\n".join(parts)

    def _generate_utilities(self) -> str:
        """Generate common utility functions."""
        return '''function enableBtn(id, text) {
    const btn = document.getElementById(id);
    if (btn) {
        btn.disabled = false;
        btn.textContent = text;
    }
}

function disableBtn(id) {
    const btn = document.getElementById(id);
    if (btn) {
        btn.disabled = true;
        btn.textContent = 'WASM unavailable';
    }
}

async function fetchJsonEndpoint(demo, path, btnId, resultId, statusId, outputId, btnText) {
    if (!demo || !demo.isReady()) return;

    const btn = document.getElementById(btnId);
    const result = document.getElementById(resultId);
    const status = document.getElementById(statusId);
    const output = document.getElementById(outputId);

    btn.disabled = true;
    btn.textContent = 'Loading...';
    output.classList.add('visible');

    try {
        const startTime = performance.now();
        const response = await demo.fetch(path);
        const elapsed = (performance.now() - startTime).toFixed(1);

        if (!response.ok) throw new Error(`HTTP ${response.status}`);

        const data = await response.json();
        const html = formatJsonWithHighlighting(data);
        const codeBlock = result.closest('.code-block');
        if (codeBlock && window.typeAnimateContent) {
            window.typeAnimateContent(codeBlock, html);
        } else {
            result.innerHTML = html;
        }
        status.textContent = `Completed in ${elapsed}ms`;
        status.className = 'demo-status success';
    } catch (err) {
        console.error('Fetch failed:', err);
        result.textContent = '';
        status.textContent = 'Error: ' + err.message;
        status.className = 'demo-status error';
    } finally {
        btn.disabled = false;
        btn.textContent = btnText;
    }
}

async function fetchTextEndpoint(demo, path, btnId, resultId, statusId, outputId, btnText) {
    if (!demo || !demo.isReady()) return;

    const btn = document.getElementById(btnId);
    const result = document.getElementById(resultId);
    const status = document.getElementById(statusId);
    const output = document.getElementById(outputId);

    btn.disabled = true;
    btn.textContent = 'Loading...';
    output.classList.add('visible');

    try {
        const startTime = performance.now();
        const response = await demo.fetch(path);
        const elapsed = (performance.now() - startTime).toFixed(1);

        if (!response.ok) throw new Error(`HTTP ${response.status}`);

        const text = await response.text();
        result.textContent = text;
        status.textContent = `Completed in ${elapsed}ms (${text.length} chars)`;
        status.className = 'demo-status success';
    } catch (err) {
        console.error('Fetch failed:', err);
        result.textContent = '';
        status.textContent = 'Error: ' + err.message;
        status.className = 'demo-status error';
    } finally {
        btn.disabled = false;
        btn.textContent = btnText;
    }
}

async function fetchBinaryEndpoint(demo, path, btnId, resultId, statusId, outputId, btnText, contentType) {
    if (!demo || !demo.isReady()) return;

    const btn = document.getElementById(btnId);
    const result = document.getElementById(resultId);
    const status = document.getElementById(statusId);
    const output = document.getElementById(outputId);

    btn.disabled = true;
    btn.textContent = 'Loading...';
    output.classList.add('visible');

    try {
        const startTime = performance.now();
        const response = await demo.fetch(path);
        const elapsed = (performance.now() - startTime).toFixed(1);

        if (!response.ok) throw new Error(`HTTP ${response.status}`);

        const data = await response.arrayBuffer();
        const info = {
            path: path,
            size_bytes: data.byteLength,
            size_kb: (data.byteLength / 1024).toFixed(2),
            content_type: contentType || 'application/octet-stream'
        };

        const html = formatJsonWithHighlighting(info);
        const codeBlock = result.closest('.code-block');
        if (codeBlock && window.typeAnimateContent) {
            window.typeAnimateContent(codeBlock, html);
        } else {
            result.innerHTML = html;
        }
        status.textContent = `Generated in ${elapsed}ms (${info.size_kb} KB)`;
        status.className = 'demo-status success';
    } catch (err) {
        console.error('Fetch failed:', err);
        result.textContent = '';
        status.textContent = 'Error: ' + err.message;
        status.className = 'demo-status error';
    } finally {
        btn.disabled = false;
        btn.textContent = btnText;
    }
}'''

    def _generate_module_handlers(self, module: ModuleConfig) -> str:
        """Generate handlers for a single module."""
        apis = self.endpoints.get(module.id, [])
        handlers = []

        for api in apis:
            if not api.demo:
                continue

            # If demo_handler is specified, this needs a custom handler (skip generation)
            if api.demo_handler:
                continue

            # If demo_fetch is specified, generate a simple fetch handler
            if api.demo_fetch:
                handler = self._generate_fetch_handler(module, api)
                if handler:
                    handlers.append(handler)

        return "\n\n".join(handlers)

    def _generate_fetch_handler(self, module: ModuleConfig, api: ApiEndpoint) -> str:
        """Generate a simple fetch handler."""
        demo_id = self._get_demo_id(api, module.id)
        func_name = self._get_function_name(demo_id)
        demo_var = f"{module.id}Demo"

        # Determine button text from path
        btn_text = self._get_button_text(api)

        if api.demo == "json":
            return f'''function {func_name}() {{
    fetchJsonEndpoint({demo_var}, '{api.demo_fetch}', '{demo_id}-try-btn', '{demo_id}-result', '{demo_id}-status', '{demo_id}-output', '{btn_text}');
}}'''
        elif api.demo == "text":
            return f'''function {func_name}() {{
    fetchTextEndpoint({demo_var}, '{api.demo_fetch}', '{demo_id}-try-btn', '{demo_id}-result', '{demo_id}-status', '{demo_id}-output', '{btn_text}');
}}'''
        elif api.demo == "binary":
            content_type = api.returns_type or "application/octet-stream"
            return f'''function {func_name}() {{
    fetchBinaryEndpoint({demo_var}, '{api.demo_fetch}', '{demo_id}-try-btn', '{demo_id}-result', '{demo_id}-status', '{demo_id}-output', '{btn_text}', '{content_type}');
}}'''

        return ""

    def _get_demo_id(self, api: ApiEndpoint, module_id: str) -> str:
        """Generate a unique demo ID for an endpoint."""
        # Keep consistent with HtmlGenerator
        special_ids = {
            ("carta", "/tiles/{z}/{x}/{y}.png"): "carta",
            ("carta", "/tiles.json"): "tilejson",
            ("velo", "/api/v1/route"): "velo-route",
            ("velo", "/api/v1/health"): "velo-health",
            ("velo", "/api/v1/stats"): "velo-stats",
        }
        if (module_id, api.path) in special_ids:
            return special_ids[(module_id, api.path)]

        clean_path = api.path.replace("/", "-").replace("{", "").replace("}", "")
        clean_path = clean_path.replace(".", "-").strip("-")
        return f"{module_id}-{clean_path}"

    def _get_function_name(self, demo_id: str) -> str:
        """Convert demo ID to function name."""
        # Convert kebab-case to camelCase
        parts = demo_id.split("-")
        return parts[0] + "".join(p.capitalize() for p in parts[1:])

    def _get_button_text(self, api: ApiEndpoint) -> str:
        """Determine button text from endpoint."""
        path = api.path.lower()
        if "health" in path:
            return "Check Health"
        elif "stats" in path:
            return "Get Stats"
        elif "search" in path:
            return "Search"
        elif "autocomplete" in path:
            return "Autocomplete"
        elif "reverse" in path:
            return "Reverse Geocode"
        elif "solve" in path:
            return "Solve Problem"
        elif ".mvt" in path:
            return "Generate MVT"
        elif ".txt" in path:
            return "Render ASCII"
        elif ".png" in path:
            return "Generate Tile"
        elif "tilejson" in path or "tiles.json" in path:
            return "Fetch TileJSON"
        else:
            return "Try It"

    def _generate_init_function(self, module: ModuleConfig) -> str:
        """Generate init function for a module."""
        apis = self.endpoints.get(module.id, [])
        demo_var = f"{module.id}Demo"
        func_name = f"init{module.id.capitalize()}Demo"

        # Collect buttons to enable
        buttons = []
        for api in apis:
            if not api.demo:
                continue
            demo_id = self._get_demo_id(api, module.id)
            btn_text = self._get_button_text(api)
            buttons.append(f"    enableBtn('{demo_id}-try-btn', '{btn_text}');")

        if not buttons:
            return f"function {func_name}() {{\n    // No demos configured\n}}"

        # Status message
        first_demo_id = self._get_demo_id(apis[0], module.id) if apis else module.id
        version_method = self._get_version_method(module.id)
        info_method = self._get_info_method(module.id)

        status_line = f"    const status = document.getElementById('{first_demo_id}-status');"
        status_msg = f"    if (status) {{\n        status.textContent = `{module.name.split()[0]} ${{" + f"{demo_var}.{version_method}()}}" + f" ready{info_method}`;\n        status.className = 'demo-status success';\n        document.getElementById('{first_demo_id}-output').classList.add('visible');\n    }}"

        return f"""function {func_name}() {{
{chr(10).join(buttons)}
{status_line}
{status_msg}
}}"""

    def _generate_error_function(self, module: ModuleConfig) -> str:
        """Generate error function for a module."""
        apis = self.endpoints.get(module.id, [])
        func_name = f"handle{module.id.capitalize()}Error"

        # Collect buttons to disable
        buttons = []
        for api in apis:
            if not api.demo:
                continue
            demo_id = self._get_demo_id(api, module.id)
            buttons.append(f"    disableBtn('{demo_id}-try-btn');")

        if not buttons:
            return f"function {func_name}(err) {{\n    console.error('WASM error:', err);\n}}"

        first_demo_id = self._get_demo_id(apis[0], module.id) if apis else module.id

        return f"""function {func_name}(err) {{
{chr(10).join(buttons)}
    const status = document.getElementById('{first_demo_id}-status');
    if (status) {{
        status.textContent = 'Failed to load WASM: ' + err.message;
        status.className = 'demo-status error';
        document.getElementById('{first_demo_id}-output').classList.add('visible');
    }}
}}"""

    def _get_version_method(self, module_id: str) -> str:
        """Get version method name for module."""
        return "getVersion"

    def _get_info_method(self, module_id: str) -> str:
        """Get additional info for status message."""
        if module_id == "carta":
            return " (Monaco PBF: ${(cartaDemo.getPBFSize() / 1024).toFixed(0)} KB)"
        elif module_id == "velo":
            return " (Monaco: ${veloDemo.getNodeCount()} nodes)"
        elif module_id == "locus":
            return " (Monaco: ${locusDemo.getEntityCount()} entities)"
        elif module_id == "fuelwise":
            return ""
        return ""


# =============================================================================
# Template Renderer
# =============================================================================

class TemplateRenderer:
    """Simple template renderer with Jinja-like syntax."""

    def __init__(self, config: Config, endpoints_html: dict[str, str], verbose: bool = False):
        self.config = config
        self.endpoints_html = endpoints_html
        self.verbose = verbose

    def render(self, template: str) -> str:
        """Render the template with configuration values."""
        result = template

        # Simple value substitutions
        result = self._replace_simple_vars(result)

        # Process module loops
        result = self._process_module_loops(result)

        # Process common_endpoints loops
        result = self._process_common_endpoint_loops(result)

        # Process status_codes loops
        result = self._process_status_code_loops(result)

        # Insert WASM handlers
        result = self._insert_wasm_handlers(result)

        # Verify no unprocessed template tags remain
        self._check_unprocessed_tags(result)

        return result

    def _replace_simple_vars(self, text: str) -> str:
        """Replace simple {{ config.* }} variables."""
        replacements = {
            "{{ config.title }}": self.config.title,
            "{{ config.description }}": self.config.description,
            "{{ config.canonical_url }}": self.config.canonical_url,
            "{{ config.github_url }}": self.config.github_url,
        }
        for pattern, value in replacements.items():
            text = text.replace(pattern, value)
        return text

    def _process_module_loops(self, text: str) -> str:
        """Process {% for module in config.modules %} loops."""
        pattern = r"{% for module in config\.modules %}\n(.*?){% endfor %}"

        def replace(match: re.Match) -> str:
            template_block = match.group(1)
            blocks = []

            for module in self.config.modules:
                block = self._render_module_block(template_block, module)
                blocks.append(block)

            return "".join(blocks)

        return re.sub(pattern, replace, text, flags=re.DOTALL)

    def _render_module_block(self, template: str, module: ModuleConfig) -> str:
        """Render a single module's template block."""
        block = template

        # Simple substitutions
        block = block.replace("{{ module.id }}", module.id)
        block = block.replace("{{ module.id | upper }}", module.id.upper())
        block = block.replace("{{ module.id | capitalize }}", module.id.capitalize())
        block = block.replace("{{ module.name }}", module.name)
        block = block.replace("{{ module.icon }}", module.icon)
        block = block.replace("{{ module.port }}", str(module.port))
        block = block.replace("{{ module.description }}", module.description)
        block = block.replace("{{ module.header_file }}", module.header_file)

        # Name with filters
        name_short = module.name
        for suffix in [" Server", " Optimizer", " Geocoder", " Tile"]:
            name_short = name_short.replace(suffix, "")
        block = block.replace(
            '{{ module.name | replace(" Server", "") | replace(" Optimizer", "") | replace(" Geocoder", "") }}',
            name_short
        )

        # WASM config
        if module.wasm.enabled:
            block = block.replace("{{ module.wasm.script }}", module.wasm.script)
            block = block.replace("{{ module.wasm.wrapper }}", module.wasm.wrapper)
            block = block.replace("{{ module.wasm.class_name }}", module.wasm.class_name)
            block = block.replace("{{ module.wasm.factory_name }}", module.wasm.factory_name)

        # Endpoint HTML
        block = block.replace("{{ endpoints[module.id] }}", self.endpoints_html.get(module.id, ""))

        # WASM init/error code
        init_code, error_code = self._get_wasm_code(module)
        block = block.replace(
            '{{ wasm_init_code[module.id] | default("                // TODO: Enable demo buttons") }}',
            init_code
        )
        block = block.replace(
            '{{ wasm_error_code[module.id] | default("                // TODO: Handle error") }}',
            error_code
        )

        # Handle conditionals
        if_pattern = r"{% if module\.wasm\.enabled %}\n?(.*?){% endif %}"
        if module.wasm.enabled:
            block = re.sub(if_pattern, r"\1", block, flags=re.DOTALL)
        else:
            block = re.sub(if_pattern, "", block, flags=re.DOTALL)

        return block

    def _get_wasm_code(self, module: ModuleConfig) -> tuple[str, str]:
        """Get WASM init and error code for a module from config."""
        if not module.wasm.enabled:
            return "                // WASM not enabled", "                // WASM not enabled"

        if module.wasm.init_function:
            init_code = f"                {module.wasm.init_function}();"
        else:
            init_code = "                // No init function configured"

        if module.wasm.error_function:
            error_code = f"                {module.wasm.error_function}(err);"
        else:
            error_code = "                // No error function configured"

        return init_code, error_code

    def _process_common_endpoint_loops(self, text: str) -> str:
        """Process {% for ep in config.common_endpoints %} loops."""
        pattern = r"{% for ep in config\.common_endpoints %}\n(.*?){% endfor %}"

        def replace(match: re.Match) -> str:
            template_block = match.group(1)
            blocks = []
            for ep in self.config.common_endpoints:
                block = template_block
                block = block.replace("{{ ep.method }}", ep.method)
                block = block.replace("{{ ep.method | lower }}", ep.method.lower())
                block = block.replace("{{ ep.path }}", ep.path)
                block = block.replace("{{ ep.description }}", ep.description)
                blocks.append(block)
            return "".join(blocks)

        return re.sub(pattern, replace, text, flags=re.DOTALL)

    def _process_status_code_loops(self, text: str) -> str:
        """Process {% for sc in config.status_codes %} loops."""
        pattern = r"{% for sc in config\.status_codes %}\n(.*?){% endfor %}"

        def replace(match: re.Match) -> str:
            template_block = match.group(1)
            blocks = []
            for sc in self.config.status_codes:
                block = template_block
                block = block.replace("{{ sc.code }}", str(sc.code))
                block = block.replace("{{ sc.text }}", sc.text)
                block = block.replace("{{ sc.type }}", sc.type)
                blocks.append(block)
            return "".join(blocks)

        return re.sub(pattern, replace, text, flags=re.DOTALL)

    def _insert_wasm_handlers(self, text: str) -> str:
        """Insert WASM handler functions and button bindings."""
        # Button handlers for each module
        button_handlers = []
        for module in self.config.modules:
            if not module.wasm.enabled:
                continue
            handlers = self._get_button_handlers(module)
            button_handlers.extend(handlers)

        text = text.replace("{{ button_handlers }}", "\n".join(button_handlers))

        # Insert WASM functions before "// Initialize on page load"
        wasm_functions = self._get_wasm_functions()
        insert_marker = "        // Initialize on page load"
        if insert_marker in text:
            text = text.replace(insert_marker, wasm_functions + "\n" + insert_marker)

        return text

    def _get_button_handlers(self, module: ModuleConfig) -> list[str]:
        """Get button click handler registrations for a module.

        Uses explicit buttons config if present, otherwise auto-derives from
        annotations in the endpoints.
        """
        if not module.wasm.enabled:
            return []

        handlers = []

        # First, use explicit buttons config (for backwards compatibility)
        if module.wasm.buttons:
            for btn_id, func_name in module.wasm.buttons.items():
                handlers.append(
                    f"            document.getElementById('{btn_id}').addEventListener('click', {func_name});"
                )

        return handlers

    def _get_wasm_functions(self) -> str:
        """Load WASM handler functions from external files specified in config."""
        parts = []
        loaded_files = set()

        for module in self.config.modules:
            if not module.wasm.enabled or not module.wasm.handlers_file:
                continue

            handlers_path = SITE_DIR / module.wasm.handlers_file
            if handlers_path in loaded_files:
                continue

            if handlers_path.exists():
                content = handlers_path.read_text(encoding="utf-8")
                # Strip the comment header and add module comment
                lines = content.split('\n')
                # Skip initial comment block
                start = 0
                for i, line in enumerate(lines):
                    if line.strip() and not line.strip().startswith('*') and not line.strip().startswith('/*'):
                        start = i
                        break
                    if line.strip() == '*/':
                        start = i + 1
                        break

                code = '\n'.join(lines[start:]).strip()
                if code:
                    parts.append(f"\n        // {module.id.capitalize()} handlers (from {module.wasm.handlers_file})")
                    # Indent each line for inline JS
                    indented = '\n'.join('        ' + line if line.strip() else '' for line in code.split('\n'))
                    parts.append(indented)
                    loaded_files.add(handlers_path)
            else:
                if self.verbose:
                    print(f"  Warning: Handlers file not found: {handlers_path}")

        return '\n'.join(parts) if parts else ""

    def _check_unprocessed_tags(self, text: str) -> None:
        """Check for unprocessed template tags and warn."""
        # Find any remaining {{ ... }} or {% ... %}
        var_pattern = r"\{\{[^}]+\}\}"
        tag_pattern = r"\{%[^%]+%\}"

        vars_found = re.findall(var_pattern, text)
        tags_found = re.findall(tag_pattern, text)

        if vars_found and self.verbose:
            print(f"  Warning: Unprocessed variables: {vars_found[:5]}")
        if tags_found and self.verbose:
            print(f"  Warning: Unprocessed tags: {tags_found[:5]}")


# =============================================================================
# File Operations
# =============================================================================

def copy_wasm_files(config: Config, verbose: bool = False) -> list[str]:
    """Copy WASM demo files to site directory."""
    copied = []
    wasm_dir = SITE_DIR / "wasm"
    wasm_dir.mkdir(exist_ok=True)

    for module in config.modules:
        if not module.wasm.enabled:
            continue

        script_name = module.wasm.script
        if not script_name:
            continue

        # Handle paths like "wasm/carta-api-demo.js"
        if script_name.startswith("wasm/"):
            script_name = script_name[5:]

        src_path = ROOT / module.id / "wasm" / "build" / script_name
        dst_path = wasm_dir / script_name

        if src_path.exists():
            shutil.copy2(src_path, dst_path)
            copied.append(f"{module.id}/wasm/build/{script_name} -> site/wasm/{script_name}")
            if verbose:
                print(f"  Copied: {src_path.name}")
        else:
            print(f"  Warning: WASM script not found: {src_path}")

    return copied


def load_config() -> Config:
    """Load and validate configuration file."""
    if not CONFIG_FILE.exists():
        raise ConfigError(f"Config file not found: {CONFIG_FILE}")

    try:
        with open(CONFIG_FILE, encoding="utf-8") as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        raise ConfigError(f"Invalid JSON in config file: {e}")

    return Config.from_dict(data)


# =============================================================================
# Main
# =============================================================================

def validate_response_json(endpoints: dict[str, list[ApiEndpoint]], verbose: bool = False) -> list[str]:
    """Validate that all @response_json annotations contain valid JSON."""
    errors = []
    for module_id, apis in endpoints.items():
        for api in apis:
            if api.response_json:
                try:
                    json.loads(api.response_json)
                    if verbose:
                        print(f"    ✓ {api.method} {api.path}: valid JSON")
                except json.JSONDecodeError as e:
                    errors.append(f"{module_id}: {api.method} {api.path}: Invalid JSON - {e}")
    return errors


def main() -> int:
    """Main entry point. Returns exit code."""
    verbose = "--verbose" in sys.argv or "-v" in sys.argv
    check_mode = "--check" in sys.argv
    validate_mode = "--validate" in sys.argv

    try:
        # Load config
        config = load_config()
        print(f"Loaded config with {len(config.modules)} modules")

        # Parse annotations from header files
        parser = AnnotationParser(verbose=verbose)
        all_endpoints: dict[str, list[ApiEndpoint]] = {}
        all_exports: dict[str, list[str]] = {}

        for module in config.modules:
            header_path = ROOT / module.header_file
            if verbose:
                print(f"\n  Parsing {header_path}")

            if header_path.exists():
                apis, exports = parser.parse_file(header_path)
                print(f"  {module.id}: {len(apis)} endpoints, {len(exports)} WASM exports")
                all_endpoints[module.id] = apis
                all_exports[module.id] = exports
            else:
                print(f"  {module.id}: header not found ({header_path})")
                all_endpoints[module.id] = []
                all_exports[module.id] = []

        total_endpoints = sum(len(e) for e in all_endpoints.values())
        total_exports = sum(len(e) for e in all_exports.values())
        print(f"\nTotal: {total_endpoints} endpoints, {total_exports} WASM exports")

        # Validate @response_json annotations
        if validate_mode or verbose:
            print("\nValidating @response_json annotations...")
            json_errors = validate_response_json(all_endpoints, verbose)
            if json_errors:
                print("\n✗ JSON validation errors:")
                for err in json_errors:
                    print(f"  - {err}")
                if validate_mode:
                    return 1
            else:
                print("✓ All @response_json annotations are valid JSON")

        # Generate HTML for endpoints
        html_gen = HtmlGenerator(config)
        endpoints_html: dict[str, str] = {}
        for module_id, apis in all_endpoints.items():
            html_parts = [html_gen.generate_endpoint(api, module_id) for api in apis]
            endpoints_html[module_id] = "\n".join(html_parts)

        # Load and render template
        if not TEMPLATE_FILE.exists():
            raise TemplateError(f"Template file not found: {TEMPLATE_FILE}")

        template = TEMPLATE_FILE.read_text(encoding="utf-8")
        renderer = TemplateRenderer(config, endpoints_html, verbose=verbose)
        output_html = renderer.render(template)

        if check_mode:
            # Compare with existing file
            if OUTPUT_FILE.exists():
                existing = OUTPUT_FILE.read_text(encoding="utf-8")
                if existing == output_html:
                    print(f"\n✓ {OUTPUT_FILE.name} is up-to-date")
                    return 0
                else:
                    print(f"\n✗ {OUTPUT_FILE.name} needs regeneration")
                    print("  Run 'python3 scripts/build-api-docs.py' to update")
                    return 1
            else:
                print(f"\n✗ {OUTPUT_FILE.name} does not exist")
                return 1
        else:
            # Copy WASM files
            copied = copy_wasm_files(config, verbose)
            if copied:
                print("\nCopied WASM files:")
                for c in copied:
                    print(f"  {c}")

            # Write output
            OUTPUT_FILE.write_text(output_html, encoding="utf-8")
            print(f"\n✓ Generated {OUTPUT_FILE.name} ({len(output_html):,} bytes)")
            return 0

    except GenApiError as e:
        print(f"\nError: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"\nUnexpected error: {e}", file=sys.stderr)
        if verbose:
            import traceback
            traceback.print_exc()
        return 1


if __name__ == "__main__":
    sys.exit(main())
