/**
 * Map Viewer Demo - Minimal Application Entry Point
 *
 * This file contains ONLY domain-specific JavaScript:
 *   - WASM loading for this specific module
 *   - Map tile rendering setup
 *   - Application-specific event wiring
 *
 * Generic functionality is imported from clay-renderer-webgl:
 *   - ClayRenderer, MSDFFont (rendering)
 *   - TileCache, MapTileRenderer (map tiles)
 *   - renderTextCursor (text input cursor)
 */

import {
    ClayRenderer,
    MSDFFont,
    TileCache,
    MapTileRenderer,
    renderTextCursor
} from '../clay-renderer-webgl/index.js';

/* ============================================================================
 * Application State
 * ============================================================================ */

let wasm = null;
let renderer = null;
let font = null;
let tileRenderer = null;
let lastFrameTime = 0;
let isDragging = false;

/* ============================================================================
 * WASM Loading
 * ============================================================================ */

async function loadWasm() {
    const response = await fetch('build/map_ui.wasm');
    if (!response.ok) {
        throw new Error(`Failed to load WASM: ${response.status} ${response.statusText}`);
    }
    const { instance } = await WebAssembly.instantiateStreaming(response, {});
    wasm = instance.exports;

    // Validate required exports exist
    const required = [
        'map_init', 'map_frame', 'map_resize',
        'map_get_lat', 'map_get_lon', 'map_get_zoom', 'map_get_layer',
        'map_pointer_down', 'map_pointer_move', 'map_pointer_up', 'map_scroll',
        'cc_set_pending_click', 'cc_focused_id', 'cc_key_down', 'cc_key_char',
        'cc_clay_cmd_type', 'cc_clay_cmd_x', 'cc_clay_cmd_y', 'cc_clay_cmd_w', 'cc_clay_cmd_h'
    ];
    for (const fn of required) {
        if (typeof wasm[fn] !== 'function') {
            throw new Error(`Missing WASM export: ${fn}`);
        }
    }

    return wasm;
}

/* ============================================================================
 * Rendering
 * ============================================================================ */

function render(timestamp) {
    const dt = lastFrameTime ? (timestamp - lastFrameTime) / 1000 : 0.016;
    lastFrameTime = timestamp;

    const projMatrix = renderer.getProjectionMatrix();

    // Clear and render tiles
    renderer.clear();
    tileRenderer.render(
        wasm.map_get_lat(),
        wasm.map_get_lon(),
        wasm.map_get_zoom(),
        wasm.map_get_layer(),
        renderer.width,
        renderer.height,
        projMatrix
    );

    // Render Clay UI
    const count = wasm.map_frame(dt);
    if (!window._debugLogged) {
        console.log('Command count:', count);
        if (count > 0) {
            console.log('First cmd type:', wasm.cc_clay_cmd_type(0));
            console.log('First cmd bounds:', wasm.cc_clay_cmd_x(0), wasm.cc_clay_cmd_y(0), wasm.cc_clay_cmd_w(0), wasm.cc_clay_cmd_h(0));
            console.log('First cmd color:', wasm.cc_clay_cmd_rect_color(0).toString(16));
        }
        window._debugLogged = true;
    }
    renderer.renderClayCommands(wasm, count, projMatrix, 'cc_clay_cmd_');

    // Render text cursor (generic from clay-renderer-webgl)
    const inputBounds = detectSearchInputBounds(count);
    if (inputBounds) {
        renderTextCursor(renderer, wasm, font, projMatrix, inputBounds, {
            fontSize: 12,
            padding: 8
        });
    }

    requestAnimationFrame(render);
}

/**
 * Detect search input bounds from render commands.
 * This is app-specific knowledge of where the input is.
 */
function detectSearchInputBounds(count) {
    for (let i = 0; i < count; i++) {
        const cmdType = wasm.cc_clay_cmd_type(i);
        const x = wasm.cc_clay_cmd_x(i);
        const y = wasm.cc_clay_cmd_y(i);
        const w = wasm.cc_clay_cmd_w(i);
        const h = wasm.cc_clay_cmd_h(i);

        // Search input: rectangle, ~180 wide, ~28 tall, in top-left panel
        if (cmdType === 1 && w > 170 && w < 190 && h > 25 && h < 35 && x < 250 && y > 50) {
            return { x, y, w, h };
        }
    }
    return null;
}

/* ============================================================================
 * Event Handlers
 * ============================================================================ */

function setupEvents(canvas) {
    // Mouse events
    canvas.addEventListener('mousedown', (e) => {
        wasm.cc_set_pending_click();
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
        wasm.map_scroll(e.deltaY > 0 ? -1 : 1, e.clientX, e.clientY);
    }, { passive: false });

    // Touch events - mirror mouse event handling for consistency
    canvas.addEventListener('touchstart', (e) => {
        if (e.touches.length === 1) {
            const t = e.touches[0];
            wasm.cc_set_pending_click();
            if (wasm.map_handle_click(t.clientX, t.clientY)) return;
            isDragging = true;
            wasm.map_pointer_down(t.clientX, t.clientY);
        }
    }, { passive: true });

    canvas.addEventListener('touchmove', (e) => {
        if (e.touches.length === 1 && isDragging) {
            e.preventDefault();
            const t = e.touches[0];
            wasm.map_pointer_move(t.clientX, t.clientY);
        }
    }, { passive: false });

    canvas.addEventListener('touchend', (e) => {
        if (isDragging && e.changedTouches.length > 0) {
            isDragging = false;
            const t = e.changedTouches[0];
            wasm.map_pointer_up(t.clientX, t.clientY);
        }
    });

    // Resize
    window.addEventListener('resize', () => {
        const { width, height } = renderer.resize();
        wasm.map_resize(width, height);
    });

    // Keyboard (route to focused component or handle globally)
    window.addEventListener('keydown', (e) => {
        if (wasm.cc_focused_id() !== 0) {
            // Route to focused input
            if (wasm.cc_key_down(e.keyCode, e.shiftKey ? 1 : 0, e.ctrlKey ? 1 : 0)) {
                e.preventDefault();
                return;
            }
            // Character input
            if (e.key.length === 1 && !e.ctrlKey && !e.metaKey) {
                const code = e.key.charCodeAt(0);
                if (code >= 32 && code <= 126) {
                    wasm.cc_key_char(code);
                    e.preventDefault();
                }
            }
        } else {
            // Global shortcuts
            if (e.key === '+' || e.key === '=') wasm.map_scroll(1, 0, 0);
            if (e.key === '-') wasm.map_scroll(-1, 0, 0);
        }
    });
}

/* ============================================================================
 * Main
 * ============================================================================ */

async function main() {
    const loading = document.getElementById('loading');
    const canvas = document.getElementById('map-canvas');

    try {
        // Initialize renderer
        renderer = new ClayRenderer(canvas);
        const { width, height } = renderer.resize();

        // Initialize font and tiles
        font = new MSDFFont();
        const tileCache = new TileCache(renderer.gl);
        tileRenderer = new MapTileRenderer(renderer, tileCache);

        // Load resources
        await Promise.all([
            loadWasm(),
            font.load(renderer.gl,
                '../clay-renderer-webgl/fonts/ui-font.json',
                '../clay-renderer-webgl/fonts/ui-font.png')
        ]);

        renderer.setFont(font);

        // Initialize app
        wasm.map_init(width, height);
        console.log('After map_init, initialized:', wasm.cc_clay_is_initialized());
        const errPtr = wasm.cc_clay_get_error();
        if (errPtr) {
            const mem = new Uint8Array(wasm.memory.buffer);
            let errStr = '';
            for (let i = 0; i < 256 && mem[errPtr + i]; i++) {
                errStr += String.fromCharCode(mem[errPtr + i]);
            }
            console.log('Clay error:', errStr);
        }
        wasm.map_set_center(47.4979, 19.0402);
        wasm.map_set_zoom(12);

        setupEvents(canvas);
        loading.classList.add('hidden');
        requestAnimationFrame(render);

    } catch (err) {
        loading.querySelector('p').textContent = 'Failed: ' + err.message;
        console.error(err);
    }
}

document.readyState === 'loading'
    ? document.addEventListener('DOMContentLoaded', main)
    : main();
