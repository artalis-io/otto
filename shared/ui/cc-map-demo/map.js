/**
 * Map Viewer Demo - Minimal Application Entry Point
 *
 * This file contains ONLY domain-specific code:
 *   - Required WASM exports list
 *   - Map tile rendering callback
 *   - Application-specific event wiring
 *
 * Generic functionality is imported from clay-renderer-webgl:
 *   - ClayRenderer, MSDFFont (rendering)
 *   - TileCache, MapTileRenderer (map tiles)
 *   - createRenderLoop (generic render loop with cursor rendering)
 *   - loadWasm (WASM loading with export validation)
 */

import {
    ClayRenderer,
    MSDFFont,
    TileCache,
    MapTileRenderer,
    createRenderLoop,
    loadWasm
} from '../clay-renderer-webgl/index.js';

/* ============================================================================
 * Application State
 * ============================================================================ */

let wasm = null;
let renderer = null;
let font = null;
let tileRenderer = null;
let isDragging = false;

/* ============================================================================
 * Required WASM Exports
 * ============================================================================ */

const REQUIRED_EXPORTS = [
    'map_init', 'map_frame', 'map_resize',
    'map_get_lat', 'map_get_lon', 'map_get_zoom', 'map_get_layer',
    'map_pointer_down', 'map_pointer_move', 'map_pointer_up', 'map_scroll',
    'map_handle_click',
    'cc_set_pending_click', 'cc_focused_id', 'cc_key_down', 'cc_key_char',
    'cc_focused_x', 'cc_focused_y', 'cc_focused_w', 'cc_focused_h',
    'cc_clay_cmd_type', 'cc_clay_cmd_x', 'cc_clay_cmd_y', 'cc_clay_cmd_w', 'cc_clay_cmd_h'
];

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
        [wasm] = await Promise.all([
            loadWasm('build/map_ui.wasm', REQUIRED_EXPORTS),
            font.load(renderer.gl,
                '../clay-renderer-webgl/fonts/ui-font.json',
                '../clay-renderer-webgl/fonts/ui-font.png')
        ]);

        renderer.setFont(font);

        // Initialize app
        wasm.map_init(width, height);
        wasm.map_set_center(47.4979, 19.0402);
        wasm.map_set_zoom(12);

        setupEvents(canvas);
        loading.classList.add('hidden');

        // Create and start render loop
        const render = createRenderLoop(renderer, wasm, font, {
            frameFunction: 'map_frame',
            onRender: (projMatrix) => {
                tileRenderer.render(
                    wasm.map_get_lat(),
                    wasm.map_get_lon(),
                    wasm.map_get_zoom(),
                    wasm.map_get_layer(),
                    renderer.width,
                    renderer.height,
                    projMatrix
                );
            },
            cursorFontSize: 12,
            cursorPadding: 8
        });
        requestAnimationFrame(render);

    } catch (err) {
        loading.querySelector('p').textContent = 'Failed: ' + err.message;
        console.error(err);
    }
}

document.readyState === 'loading'
    ? document.addEventListener('DOMContentLoaded', main)
    : main();
