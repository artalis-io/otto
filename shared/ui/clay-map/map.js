/**
 * Clay Map Viewer - WebGL Runtime with MSDF Text
 *
 * Handles:
 * - Direct WASM module loading (no Emscripten JS glue)
 * - WebGL rendering for tiles and UI
 * - MSDF font rendering for crisp text at any size
 * - User input handling
 */

// Tile server URLs by layer type
const TILE_SERVERS = [
    (z, x, y) => `https://tile.openstreetmap.org/${z}/${x}/${y}.png`,
    (z, x, y) => `https://a.basemaps.cartocdn.com/light_all/${z}/${x}/${y}.png`,
    (z, x, y) => `https://tiles.stadiamaps.com/tiles/stamen_terrain/${z}/${x}/${y}.png`,
];

// Tile cache
const tileCache = new Map();
const TILE_SIZE = 256;
const MAX_CACHE_SIZE = 200;

// WASM module and memory
let wasm = null;
let memory = null;
let HEAPU8 = null;

// WebGL state
let gl = null;
let canvas = null;

// Shaders
let tileShader = null;
let rectShader = null;
let textShader = null;

// Buffers
let quadBuffer = null;

// MSDF Font
let fontTexture = null;
let fontData = null;

// Tile textures
const tileTextures = new Map();

// Animation state
let animationId = null;
let lastFrameTime = 0;

// Input state
let isDragging = false;

// Clay command types
const CLAY_RENDER_COMMAND_TYPE_NONE = 0;
const CLAY_RENDER_COMMAND_TYPE_RECTANGLE = 1;
const CLAY_RENDER_COMMAND_TYPE_BORDER = 2;
const CLAY_RENDER_COMMAND_TYPE_TEXT = 3;
const CLAY_RENDER_COMMAND_TYPE_IMAGE = 4;
const CLAY_RENDER_COMMAND_TYPE_SCISSOR_START = 5;
const CLAY_RENDER_COMMAND_TYPE_SCISSOR_END = 6;
const CLAY_RENDER_COMMAND_TYPE_CUSTOM = 7;

// ============================================================================
// Shader Sources
// ============================================================================

const TILE_VS = `
    attribute vec2 a_pos;
    attribute vec2 a_uv;
    uniform mat4 u_proj;
    uniform vec4 u_rect;  // x, y, w, h
    varying vec2 v_uv;
    void main() {
        vec2 pos = u_rect.xy + a_pos * u_rect.zw;
        gl_Position = u_proj * vec4(pos, 0.0, 1.0);
        v_uv = a_uv;
    }
`;

const TILE_FS = `
    precision mediump float;
    uniform sampler2D u_tex;
    varying vec2 v_uv;
    void main() {
        gl_FragColor = texture2D(u_tex, v_uv);
    }
`;

const RECT_VS = `
    attribute vec2 a_pos;
    uniform mat4 u_proj;
    uniform vec4 u_rect;  // x, y, w, h
    varying vec2 v_localPos;
    varying vec2 v_size;
    void main() {
        vec2 pos = u_rect.xy + a_pos * u_rect.zw;
        gl_Position = u_proj * vec4(pos, 0.0, 1.0);
        v_localPos = a_pos * u_rect.zw;
        v_size = u_rect.zw;
    }
`;

const RECT_FS = `
    precision mediump float;
    uniform vec4 u_color;
    uniform float u_radius;
    uniform float u_borderWidth;
    uniform vec4 u_borderColor;
    varying vec2 v_localPos;
    varying vec2 v_size;

    float roundedBoxSDF(vec2 p, vec2 b, float r) {
        vec2 q = abs(p) - b + r;
        return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    }

    void main() {
        vec2 center = v_size * 0.5;
        vec2 p = v_localPos - center;
        float d = roundedBoxSDF(p, center, u_radius);

        float aa = 1.0;  // anti-aliasing width
        float alpha = 1.0 - smoothstep(-aa, aa, d);

        if (u_borderWidth > 0.0) {
            float innerD = roundedBoxSDF(p, center - u_borderWidth, max(0.0, u_radius - u_borderWidth));
            float innerAlpha = 1.0 - smoothstep(-aa, aa, innerD);
            vec4 fill = u_color * innerAlpha;
            vec4 border = u_borderColor * (alpha - innerAlpha);
            gl_FragColor = fill + border;
        } else {
            gl_FragColor = u_color * alpha;
        }
    }
`;

const TEXT_VS = `
    attribute vec2 a_pos;
    attribute vec2 a_uv;
    uniform mat4 u_proj;
    varying vec2 v_uv;
    void main() {
        gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
        v_uv = a_uv;
    }
`;

const TEXT_FS = `
    #extension GL_OES_standard_derivatives : enable
    precision mediump float;
    uniform sampler2D u_tex;
    uniform vec4 u_color;
    uniform float u_pxRange;
    varying vec2 v_uv;

    float median(float r, float g, float b) {
        return max(min(r, g), min(max(r, g), b));
    }

    void main() {
        vec3 msd = texture2D(u_tex, v_uv).rgb;
        float sd = median(msd.r, msd.g, msd.b);
        float screenPxDistance = u_pxRange * (sd - 0.5);
        float opacity = clamp(screenPxDistance + 0.5, 0.0, 1.0);
        gl_FragColor = vec4(u_color.rgb, u_color.a * opacity);
    }
`;

// ============================================================================
// WebGL Utilities
// ============================================================================

function createShader(gl, type, source) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        console.error('Shader compile error:', gl.getShaderInfoLog(shader));
        gl.deleteShader(shader);
        return null;
    }
    return shader;
}

function createProgram(gl, vsSource, fsSource) {
    const vs = createShader(gl, gl.VERTEX_SHADER, vsSource);
    const fs = createShader(gl, gl.FRAGMENT_SHADER, fsSource);
    if (!vs || !fs) return null;

    const program = gl.createProgram();
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.linkProgram(program);

    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        console.error('Program link error:', gl.getProgramInfoLog(program));
        gl.deleteProgram(program);
        return null;
    }

    // Cache attribute and uniform locations
    const wrapper = { program, attribs: {}, uniforms: {} };

    const numAttribs = gl.getProgramParameter(program, gl.ACTIVE_ATTRIBUTES);
    for (let i = 0; i < numAttribs; i++) {
        const info = gl.getActiveAttrib(program, i);
        wrapper.attribs[info.name] = gl.getAttribLocation(program, info.name);
    }

    const numUniforms = gl.getProgramParameter(program, gl.ACTIVE_UNIFORMS);
    for (let i = 0; i < numUniforms; i++) {
        const info = gl.getActiveUniform(program, i);
        wrapper.uniforms[info.name] = gl.getUniformLocation(program, info.name);
    }

    return wrapper;
}

function createOrthoMatrix(width, height) {
    return new Float32Array([
        2 / width, 0, 0, 0,
        0, -2 / height, 0, 0,
        0, 0, -1, 0,
        -1, 1, 0, 1
    ]);
}

// ============================================================================
// Initialization
// ============================================================================

async function loadModule() {
    const response = await fetch('build/map_ui.wasm');
    const { instance } = await WebAssembly.instantiateStreaming(response, {});
    wasm = instance.exports;
    memory = wasm.memory;
    HEAPU8 = new Uint8Array(memory.buffer);
    if (wasm._initialize) wasm._initialize();
    return true;
}

async function loadFont() {
    // Load font texture
    const img = new Image();
    await new Promise((resolve, reject) => {
        img.onload = resolve;
        img.onerror = reject;
        img.src = 'fonts/ui-font.png';
    });

    fontTexture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, fontTexture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

    // Load font metrics
    const resp = await fetch('fonts/ui-font.json');
    fontData = await resp.json();

    // Build glyph lookup
    fontData.glyphMap = {};
    for (const glyph of fontData.glyphs) {
        fontData.glyphMap[glyph.unicode] = glyph;
    }
}

function initWebGL() {
    canvas = document.getElementById('map-canvas');
    gl = canvas.getContext('webgl', { alpha: false, antialias: true });

    if (!gl) {
        throw new Error('WebGL not supported');
    }

    // Enable extensions for MSDF
    gl.getExtension('OES_standard_derivatives');

    // Create shaders
    tileShader = createProgram(gl, TILE_VS, TILE_FS);
    rectShader = createProgram(gl, RECT_VS, RECT_FS);
    textShader = createProgram(gl, TEXT_VS, TEXT_FS);

    // Create quad buffer (positions + UVs)
    quadBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
        // pos       uv
        0, 0,       0, 0,
        1, 0,       1, 0,
        0, 1,       0, 1,
        1, 1,       1, 1,
    ]), gl.STATIC_DRAW);

    // Set up WebGL state
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
}

function resizeCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const width = window.innerWidth;
    const height = window.innerHeight;

    canvas.width = width * dpr;
    canvas.height = height * dpr;
    canvas.style.width = width + 'px';
    canvas.style.height = height + 'px';

    gl.viewport(0, 0, canvas.width, canvas.height);

    return { width, height };
}

// ============================================================================
// Tile Rendering
// ============================================================================

function lonToTileX(lon, zoom) {
    return (lon + 180) / 360 * Math.pow(2, zoom);
}

function latToTileY(lat, zoom) {
    const latRad = lat * Math.PI / 180;
    return (1 - Math.log(Math.tan(latRad) + 1 / Math.cos(latRad)) / Math.PI) / 2 * Math.pow(2, zoom);
}

function getTile(z, x, y, layerType) {
    const key = `${layerType}/${z}/${x}/${y}`;

    if (tileCache.has(key)) {
        return tileCache.get(key);
    }

    const img = new Image();
    img.crossOrigin = 'anonymous';

    const tileData = { img, loaded: false, error: false, texture: null };

    img.onload = () => {
        tileData.loaded = true;
        // Create texture
        tileData.texture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tileData.texture);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    };

    img.onerror = () => { tileData.error = true; };
    img.src = TILE_SERVERS[layerType](z, x, y);

    tileCache.set(key, tileData);

    if (tileCache.size > MAX_CACHE_SIZE) {
        const firstKey = tileCache.keys().next().value;
        const old = tileCache.get(firstKey);
        if (old.texture) gl.deleteTexture(old.texture);
        tileCache.delete(firstKey);
    }

    return tileData;
}

function renderTiles(width, height, projMatrix) {
    const lat = wasm.map_get_lat();
    const lon = wasm.map_get_lon();
    const zoom = wasm.map_get_zoom();
    const layerType = wasm.map_get_layer();

    const centerTileX = lonToTileX(lon, zoom);
    const centerTileY = latToTileY(lat, zoom);

    const tilesX = Math.ceil(width / TILE_SIZE) + 2;
    const tilesY = Math.ceil(height / TILE_SIZE) + 2;

    const startTileX = Math.floor(centerTileX - tilesX / 2);
    const startTileY = Math.floor(centerTileY - tilesY / 2);

    gl.useProgram(tileShader.program);
    gl.uniformMatrix4fv(tileShader.uniforms.u_proj, false, projMatrix);

    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuffer);
    gl.enableVertexAttribArray(tileShader.attribs.a_pos);
    gl.enableVertexAttribArray(tileShader.attribs.a_uv);
    gl.vertexAttribPointer(tileShader.attribs.a_pos, 2, gl.FLOAT, false, 16, 0);
    gl.vertexAttribPointer(tileShader.attribs.a_uv, 2, gl.FLOAT, false, 16, 8);

    gl.activeTexture(gl.TEXTURE0);
    gl.uniform1i(tileShader.uniforms.u_tex, 0);

    for (let dy = 0; dy < tilesY; dy++) {
        for (let dx = 0; dx < tilesX; dx++) {
            const tileX = startTileX + dx;
            const tileY = startTileY + dy;

            const maxTile = Math.pow(2, zoom);
            if (tileY < 0 || tileY >= maxTile) continue;

            const wrappedTileX = ((tileX % maxTile) + maxTile) % maxTile;

            const screenX = width / 2 + (tileX - centerTileX) * TILE_SIZE;
            const screenY = height / 2 + (tileY - centerTileY) * TILE_SIZE;

            const tile = getTile(zoom, wrappedTileX, tileY, layerType);

            if (tile.loaded && tile.texture) {
                gl.bindTexture(gl.TEXTURE_2D, tile.texture);
                gl.uniform4f(tileShader.uniforms.u_rect, screenX, screenY, TILE_SIZE, TILE_SIZE);
                gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
            }
        }
    }
}

// ============================================================================
// UI Rendering
// ============================================================================

function unpackColor(packed) {
    const r = ((packed >>> 24) & 0xFF) / 255;
    const g = ((packed >>> 16) & 0xFF) / 255;
    const b = ((packed >>> 8) & 0xFF) / 255;
    const a = (packed & 0xFF) / 255;
    return [r, g, b, a];
}

function renderRect(x, y, w, h, color, radius, borderWidth, borderColor, projMatrix) {
    gl.useProgram(rectShader.program);
    gl.uniformMatrix4fv(rectShader.uniforms.u_proj, false, projMatrix);

    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuffer);

    // Enable only the position attribute for rect shader
    gl.enableVertexAttribArray(rectShader.attribs.a_pos);
    gl.vertexAttribPointer(rectShader.attribs.a_pos, 2, gl.FLOAT, false, 16, 0);

    // Disable any other attributes that might have been enabled
    gl.disableVertexAttribArray(1);  // Typically a_uv
    gl.disableVertexAttribArray(2);

    gl.uniform4f(rectShader.uniforms.u_rect, x, y, w, h);
    gl.uniform4fv(rectShader.uniforms.u_color, color);
    gl.uniform1f(rectShader.uniforms.u_radius, radius);
    gl.uniform1f(rectShader.uniforms.u_borderWidth, borderWidth);
    gl.uniform4fv(rectShader.uniforms.u_borderColor, borderColor || [0, 0, 0, 0]);

    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
}

function renderText(text, x, y, fontSize, color, projMatrix) {
    if (!fontData || !fontTexture) return;

    gl.useProgram(textShader.program);
    gl.uniformMatrix4fv(textShader.uniforms.u_proj, false, projMatrix);
    gl.uniform4fv(textShader.uniforms.u_color, color);

    // Calculate pixel range for current font size
    const scale = fontSize / fontData.atlas.size;
    const pxRange = fontData.atlas.distanceRange * scale;
    gl.uniform1f(textShader.uniforms.u_pxRange, pxRange);

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, fontTexture);
    gl.uniform1i(textShader.uniforms.u_tex, 0);

    // Build vertex data for all glyphs
    const vertices = [];
    let cursorX = x;

    const atlasW = fontData.atlas.width;
    const atlasH = fontData.atlas.height;

    for (let i = 0; i < text.length; i++) {
        const charCode = text.charCodeAt(i);
        const glyph = fontData.glyphMap[charCode];

        if (!glyph) {
            cursorX += fontSize * 0.5;  // space for unknown
            continue;
        }

        if (glyph.atlasBounds && glyph.planeBounds) {
            const ab = glyph.atlasBounds;
            const pb = glyph.planeBounds;

            // Screen coordinates
            const x0 = cursorX + pb.left * fontSize;
            const y0 = y + (1 - pb.top) * fontSize;  // flip Y
            const x1 = cursorX + pb.right * fontSize;
            const y1 = y + (1 - pb.bottom) * fontSize;

            // UV coordinates (flip Y for bottom origin)
            const u0 = ab.left / atlasW;
            const v0 = 1 - ab.top / atlasH;
            const u1 = ab.right / atlasW;
            const v1 = 1 - ab.bottom / atlasH;

            vertices.push(
                x0, y0, u0, v0,
                x1, y0, u1, v0,
                x0, y1, u0, v1,
                x1, y0, u1, v0,
                x0, y1, u0, v1,
                x1, y1, u1, v1
            );
        }

        cursorX += glyph.advance * fontSize;
    }

    if (vertices.length === 0) return;

    // Upload and draw
    const textBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, textBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(vertices), gl.STREAM_DRAW);

    gl.enableVertexAttribArray(textShader.attribs.a_pos);
    gl.enableVertexAttribArray(textShader.attribs.a_uv);
    gl.vertexAttribPointer(textShader.attribs.a_pos, 2, gl.FLOAT, false, 16, 0);
    gl.vertexAttribPointer(textShader.attribs.a_uv, 2, gl.FLOAT, false, 16, 8);

    gl.drawArrays(gl.TRIANGLES, 0, vertices.length / 4);

    gl.deleteBuffer(textBuffer);
}

function renderUI(width, height, projMatrix, dt) {
    if (memory.buffer.byteLength !== HEAPU8.buffer.byteLength) {
        HEAPU8 = new Uint8Array(memory.buffer);
    }

    const count = wasm.map_frame(dt);

    // Find focused input bounds while iterating
    for (let i = 0; i < count; i++) {
        const cmdType = wasm.map_cmd_type(i);
        const x = wasm.map_cmd_x(i);
        const y = wasm.map_cmd_y(i);
        const w = wasm.map_cmd_w(i);
        const h = wasm.map_cmd_h(i);

        // Detect search input by its characteristics
        if (cmdType === 1 && w > 170 && w < 190 && h > 25 && h < 35 && x < 250 && y > 50) {
            focusedInputBounds = { x, y, w, h };
        }
    }

    // Scissor stack for clipping
    const scissorStack = [];

    for (let i = 0; i < count; i++) {
        const cmdType = wasm.map_cmd_type(i);
        const x = wasm.map_cmd_x(i);
        const y = wasm.map_cmd_y(i);
        const w = wasm.map_cmd_w(i);
        const h = wasm.map_cmd_h(i);

        switch (cmdType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                const color = unpackColor(wasm.map_cmd_rect_color(i));
                const radius = wasm.map_cmd_rect_radius(i);
                renderRect(x, y, w, h, color, radius, 0, null, projMatrix);
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                const strPtr = wasm.map_cmd_text_str(i);
                const strLen = wasm.map_cmd_text_len(i);
                const color = unpackColor(wasm.map_cmd_text_color(i));
                const fontSize = wasm.map_cmd_text_size(i);

                let text = '';
                for (let j = 0; j < strLen; j++) {
                    text += String.fromCharCode(HEAPU8[strPtr + j]);
                }

                renderText(text, x, y, fontSize, color, projMatrix);
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                const color = unpackColor(wasm.map_cmd_border_color(i));
                const radius = wasm.map_cmd_border_radius(i);
                const borderWidth = wasm.map_cmd_border_width(i);
                renderRect(x, y, w, h, [0, 0, 0, 0], radius, borderWidth, color, projMatrix);
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
                const dpr = window.devicePixelRatio || 1;
                scissorStack.push({ x, y, w, h });
                gl.enable(gl.SCISSOR_TEST);
                gl.scissor(x * dpr, (height - y - h) * dpr, w * dpr, h * dpr);
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: {
                scissorStack.pop();
                if (scissorStack.length === 0) {
                    gl.disable(gl.SCISSOR_TEST);
                } else {
                    const s = scissorStack[scissorStack.length - 1];
                    const dpr = window.devicePixelRatio || 1;
                    gl.scissor(s.x * dpr, (height - s.y - s.h) * dpr, s.w * dpr, s.h * dpr);
                }
                break;
            }
        }
    }
}

// ============================================================================
// Main Render Loop
// ============================================================================

// Track focused input bounds (set during UI render by scanning commands)
let focusedInputBounds = null;

function findFocusedInputBounds(width, height) {
    // The focused input bounds are tracked by the component system
    // We need to find the input element in the render commands
    // For now, we'll use a simple heuristic: look for the search input position
    // based on it being in the info panel (top-left area)

    // Actually, we can compute this by looking for specific patterns in render commands
    // But the cleaner approach is to have WASM tell us via cc_get_focused_bounds

    // For simplicity, scan for a text input-like element (small height, has border)
    // This is a temporary solution - ideally WASM would export bounds

    const count = wasm.map_frame(0); // Get command count without time update
    for (let i = 0; i < count; i++) {
        const cmdType = wasm.map_cmd_type(i);
        const x = wasm.map_cmd_x(i);
        const y = wasm.map_cmd_y(i);
        const w = wasm.map_cmd_w(i);
        const h = wasm.map_cmd_h(i);

        // Look for search input: ~180px wide, ~28px tall, in top-left area
        if (cmdType === 1 && w > 170 && w < 190 && h > 25 && h < 35 && x < 250 && y < 200) {
            return { x, y, w, h };
        }
    }
    return null;
}

function renderTextInputCursor(projMatrix) {
    const focusedId = wasm.cc_get_focused_id();
    if (focusedId === 0) return;

    const cursorVis = wasm.cc_is_cursor_visible();
    if (!cursorVis) return;

    // Find the focused input bounds by scanning render commands
    // This is needed because we can't easily pass float array from WASM
    if (!focusedInputBounds) {
        focusedInputBounds = findFocusedInputBounds();
    }

    if (!focusedInputBounds) return;

    const { x, y, w, h } = focusedInputBounds;

    const cursor = wasm.cc_get_cursor_pos();
    const fontSize = 12;
    const padding = 8;

    // Get actual text from WASM to measure properly
    const textPtr = wasm.cc_get_focused_text();
    const textLen = wasm.cc_get_focused_text_len();

    // Read text from WASM memory
    let focusedText = '';
    if (textPtr && textLen > 0) {
        if (memory.buffer.byteLength !== HEAPU8.buffer.byteLength) {
            HEAPU8 = new Uint8Array(memory.buffer);
        }
        for (let j = 0; j < textLen; j++) {
            focusedText += String.fromCharCode(HEAPU8[textPtr + j]);
        }
    }

    // Calculate cursor X position using actual font metrics
    let cursorX = x + padding;

    if (fontData && fontData.glyphMap && cursor > 0) {
        // Measure width of text up to cursor position
        for (let i = 0; i < Math.min(cursor, focusedText.length); i++) {
            const charCode = focusedText.charCodeAt(i);
            const glyph = fontData.glyphMap[charCode];
            if (glyph) {
                cursorX += glyph.advance * fontSize;
            } else {
                cursorX += fontSize * 0.5;  // Unknown char fallback
            }
        }
    } else if (cursor > 0) {
        // Fallback without font data
        cursorX += cursor * fontSize * 0.6;
    }

    const cursorY = y + 4;
    const cursorH = h - 8;

    // Render selection
    const selStart = wasm.cc_get_selection_start();
    if (selStart >= 0 && selStart !== cursor) {
        const start = Math.min(selStart, cursor);
        const end = Math.max(selStart, cursor);

        // Calculate selection bounds using actual font metrics
        let selX = x + padding;
        let selEndX = x + padding;

        if (fontData && fontData.glyphMap) {
            for (let i = 0; i < Math.min(end, focusedText.length); i++) {
                const charCode = focusedText.charCodeAt(i);
                const glyph = fontData.glyphMap[charCode];
                const advance = glyph ? glyph.advance * fontSize : fontSize * 0.5;
                if (i < start) {
                    selX += advance;
                }
                selEndX += advance;
            }
        } else {
            selX += start * fontSize * 0.6;
            selEndX += end * fontSize * 0.6;
        }

        renderRect(selX, cursorY, selEndX - selX, cursorH, [0.23, 0.51, 0.96, 0.3], 0, 0, null, projMatrix);
    }

    // Render cursor
    renderRect(cursorX, cursorY, 2, cursorH, [0.2, 0.6, 1.0, 1.0], 0, 0, null, projMatrix);
}

function render(timestamp) {
    // Calculate delta time
    const dt = lastFrameTime ? (timestamp - lastFrameTime) / 1000 : 0.016;
    lastFrameTime = timestamp;

    // Reset focused bounds each frame (will be found during render)
    focusedInputBounds = null;

    const dpr = window.devicePixelRatio || 1;
    const width = canvas.width / dpr;
    const height = canvas.height / dpr;

    const projMatrix = createOrthoMatrix(width, height);

    // Clear
    gl.clearColor(0.1, 0.1, 0.1, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT);

    // Render tiles
    renderTiles(width, height, projMatrix);

    // Render UI (pass dt for cursor blink)
    renderUI(width, height, projMatrix, dt);

    // Render text input cursor on top
    renderTextInputCursor(projMatrix);

    animationId = requestAnimationFrame(render);
}

// ============================================================================
// Event Handlers
// ============================================================================

function setupEventHandlers() {
    canvas.addEventListener('mousedown', (e) => {
        // Set pending click for immediate mode components
        wasm.cc_set_click();

        if (wasm.map_handle_click(e.clientX, e.clientY)) return;
        isDragging = true;
        canvas.classList.add('dragging');
        wasm.map_pointer_down(e.clientX, e.clientY);
    });

    window.addEventListener('mousemove', (e) => {
        wasm.map_pointer_move(e.clientX, e.clientY);
    });

    window.addEventListener('mouseup', (e) => {
        if (isDragging) {
            isDragging = false;
            canvas.classList.remove('dragging');
            wasm.map_pointer_up(e.clientX, e.clientY);
        }
    });

    canvas.addEventListener('wheel', (e) => {
        e.preventDefault();
        const delta = e.deltaY > 0 ? -1 : 1;
        wasm.map_scroll(delta, e.clientX, e.clientY);
    }, { passive: false });

    canvas.addEventListener('touchstart', (e) => {
        if (e.touches.length === 1) {
            const touch = e.touches[0];
            isDragging = true;
            wasm.map_pointer_down(touch.clientX, touch.clientY);
        }
    });

    canvas.addEventListener('touchmove', (e) => {
        if (e.touches.length === 1 && isDragging) {
            e.preventDefault();
            const touch = e.touches[0];
            wasm.map_pointer_move(touch.clientX, touch.clientY);
        }
    }, { passive: false });

    canvas.addEventListener('touchend', (e) => {
        if (isDragging) {
            isDragging = false;
            const touch = e.changedTouches[0];
            wasm.map_pointer_up(touch.clientX, touch.clientY);
        }
    });

    window.addEventListener('resize', () => {
        const { width, height } = resizeCanvas();
        wasm.map_resize(width, height);
    });

    window.addEventListener('keydown', (e) => {
        // Check if any input is focused (generic - not per-component!)
        const focusedId = wasm.cc_get_focused_id();
        if (focusedId !== 0) {
            // Handle special keys first
            const handled = wasm.cc_handle_key_down(e.keyCode, e.shiftKey ? 1 : 0, e.ctrlKey ? 1 : 0);
            if (handled) {
                e.preventDefault();
                return;
            }

            // Handle printable characters
            if (e.key.length === 1 && !e.ctrlKey && !e.metaKey) {
                const charCode = e.key.charCodeAt(0);
                if (charCode >= 32 && charCode <= 126) {
                    wasm.cc_handle_key_char(charCode);
                    e.preventDefault();
                }
                return;
            }
        } else {
            // Map keyboard shortcuts when no input focused
            switch (e.key) {
                case '+':
                case '=':
                    wasm.map_scroll(1, 0, 0);
                    break;
                case '-':
                    wasm.map_scroll(-1, 0, 0);
                    break;
            }
        }
    });
}

// ============================================================================
// Main
// ============================================================================

async function main() {
    const loadingEl = document.getElementById('loading');

    try {
        // Initialize WebGL
        initWebGL();
        const { width, height } = resizeCanvas();

        // Load resources in parallel
        await Promise.all([
            loadModule(),
            loadFont()
        ]);

        // Initialize map
        wasm.map_init(width, height);
        wasm.map_set_center(47.4979, 19.0402);  // Budapest
        wasm.map_set_zoom(12);

        // Setup events
        setupEventHandlers();

        // Hide loading
        loadingEl.classList.add('hidden');

        // Start render loop
        animationId = requestAnimationFrame(render);

    } catch (err) {
        loadingEl.querySelector('p').textContent = 'Failed to load: ' + err.message;
        console.error('Init error:', err);
    }
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', main);
} else {
    main();
}
