/**
 * ClayShards Demo - Application Entry Point
 *
 * Demonstrates ClayShards immediate-mode UI components with a map viewer:
 *   - cs_map: Pan/zoom map interaction
 *   - cs_input: Text input with cursor/selection
 *   - cs_button: Clickable buttons with variants
 *
 * Generic functionality is imported from clay-shards-webgl:
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
    MapOverlayRenderer,
    createRenderLoop,
    loadWasm,
    setupKeyboardHandler
} from '../clay-shards-webgl/index.js';

/* ============================================================================
 * Application State
 * ============================================================================ */

let wasm = null;
let renderer = null;
let font = null;
let tileRenderer = null;
let overlayRenderer = null;
let isDragging = false;

/* ============================================================================
 * Required WASM Exports
 * ============================================================================ */

const REQUIRED_EXPORTS = [
    'map_init', 'map_frame', 'map_resize',
    'map_get_lat', 'map_get_lon', 'map_get_zoom', 'map_get_visual_zoom', 'map_get_layer',
    'map_pointer_down', 'map_pointer_move', 'map_pointer_up', 'map_scroll',
    'map_handle_click',
    'cs_set_pending_click', 'cs_focused_id', 'cs_key_down', 'cs_key_char',
    'cs_focused_x', 'cs_focused_y', 'cs_focused_w', 'cs_focused_h',
    'cs_clay_cmd_type', 'cs_clay_cmd_x', 'cs_clay_cmd_y', 'cs_clay_cmd_w', 'cs_clay_cmd_h',
    // Overlay accessors
    'cs_map_overlay_count', 'cs_map_overlay_type', 'cs_map_overlay_id',
    'cs_map_overlay_polyline_count', 'cs_map_overlay_polyline_lat', 'cs_map_overlay_polyline_lon',
    'cs_map_overlay_polyline_color_r', 'cs_map_overlay_polyline_color_g',
    'cs_map_overlay_polyline_color_b', 'cs_map_overlay_polyline_color_a',
    'cs_map_overlay_polyline_width',
    'cs_map_overlay_marker_lat', 'cs_map_overlay_marker_lon', 'cs_map_overlay_marker_radius',
    'cs_map_overlay_marker_color_r', 'cs_map_overlay_marker_color_g',
    'cs_map_overlay_marker_color_b', 'cs_map_overlay_marker_color_a',
    'cs_map_overlay_marker_border_r', 'cs_map_overlay_marker_border_g',
    'cs_map_overlay_marker_border_b', 'cs_map_overlay_marker_border_a',
    'cs_map_overlay_marker_border_width'
];

/* ============================================================================
 * Event Handlers
 * ============================================================================ */

function setupEvents(canvas) {
    // Mouse events
    canvas.addEventListener('mousedown', (e) => {
        wasm.cs_set_pending_click();
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
            wasm.cs_set_pending_click();
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

    // Keyboard handling (generic routing + app-specific shortcuts)
    setupKeyboardHandler(wasm, {
        onGlobalShortcut: (e) => {
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

        // Initialize font, tiles, and overlays
        font = new MSDFFont();
        const tileCache = new TileCache(renderer.gl);
        tileRenderer = new MapTileRenderer(renderer, tileCache);
        overlayRenderer = new MapOverlayRenderer(renderer);

        // Load resources
        [wasm] = await Promise.all([
            loadWasm('build/demo.wasm', REQUIRED_EXPORTS),
            font.load(renderer.gl,
                '../clay-shards-webgl/fonts/ui-font.json',
                '../clay-shards-webgl/fonts/ui-font.png')
        ]);

        renderer.setFont(font);

        // Transfer font metrics to WASM for accurate text layout
        font.transferMetricsToWasm(wasm);

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
                const lat = wasm.map_get_lat();
                const lon = wasm.map_get_lon();
                const visualZoom = wasm.map_get_visual_zoom();

                // Render tiles first
                tileRenderer.render(
                    lat, lon, visualZoom,
                    wasm.map_get_layer(),
                    renderer.width,
                    renderer.height,
                    projMatrix
                );

                // Render overlays on top of tiles
                overlayRenderer.render(
                    wasm,
                    lat, lon, visualZoom,
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
