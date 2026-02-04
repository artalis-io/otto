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
    setupKeyboardHandler,
    setCartaServerUrl
} from '../clay-shards-webgl/index.js';

import { MapProvider } from '../clay-shards-webgl/map-provider.js';

/* ============================================================================
 * Server Configuration
 * ============================================================================
 *
 * Servers can be configured via URL parameters:
 *   ?carta=8081         - Use port 8081 on localhost
 *   ?velo=8082          - Use port 8082 on localhost
 *   ?locus=8083         - Use port 8083 on localhost
 *   ?carta=http://...   - Use full URL
 *
 * Example: demo.html?carta=8081&velo=8082&locus=8083
 */

function getServerConfig() {
    const params = new URLSearchParams(window.location.search);

    const parseServer = (key, defaultPort) => {
        const value = params.get(key);
        if (!value) return `http://localhost:${defaultPort}`;

        // If it's just a number, treat as localhost port
        if (/^\d+$/.test(value)) {
            return `http://localhost:${value}`;
        }

        // Otherwise use as full URL
        return value;
    };

    return {
        carta: parseServer('carta', 8081),
        velo: parseServer('velo', 8082),
        locus: parseServer('locus', 8083)
    };
}

const SERVER_CONFIG = getServerConfig();

/**
 * Allocate a string in WASM memory
 */
function _allocString(wasm, str) {
    const bytes = new TextEncoder().encode(str + '\0');
    const ptr = wasm.malloc(bytes.length);
    const memory = new Uint8Array(wasm.memory.buffer);
    memory.set(bytes, ptr);
    return ptr;
}

/* ============================================================================
 * Application State
 * ============================================================================ */

let wasm = null;
let renderer = null;
let font = null;
let tileRenderer = null;
let overlayRenderer = null;
let mapProvider = null;
let isDragging = false;
let isDraggingScrollbar = false;

/* ============================================================================
 * Required WASM Exports
 * ============================================================================ */

const REQUIRED_EXPORTS = [
    'map_init', 'map_frame', 'map_resize',
    'map_get_lat', 'map_get_lon', 'map_get_zoom', 'map_get_visual_zoom', 'map_get_layer',
    'map_get_component_id', 'map_get_width', 'map_get_height', 'map_get_show_tile_info', 'map_get_attribution_clicked',
    'map_pointer_down', 'map_pointer_move', 'map_pointer_up', 'map_scroll',
    'map_handle_click',
    'cs_set_pending_click', 'cs_focused_id', 'cs_key_down', 'cs_key_char',
    'cs_set_scroll_delta', 'cs_scroll_container_hovered', 'cs_scroll_set_position',
    'cs_clay_pointer_over',
    'scrollbar_start_drag', 'scrollbar_move', 'scrollbar_end_drag', 'scrollbar_is_dragging',
    'scrollbar_hit_test_xy', 'scrollbar_content_height', 'scrollbar_view_height', 'scrollbar_track_height', 'scrollbar_scroll_id',
    'scrollbar_debug_visible', 'scrollbar_debug_x', 'scrollbar_debug_y', 'scrollbar_debug_w', 'scrollbar_debug_h',
    'cs_focused_x', 'cs_focused_y', 'cs_focused_w', 'cs_focused_h',
    'cs_clay_cmd_type', 'cs_clay_cmd_x', 'cs_clay_cmd_y', 'cs_clay_cmd_w', 'cs_clay_cmd_h',
    // Overlay accessors (multi-instance: take map_id as first parameter)
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
    'cs_map_overlay_marker_border_width', 'cs_map_overlay_marker_draggable',
    // Hit testing and interaction state
    'cs_map_hit_test', 'cs_map_set_hovered_overlay', 'cs_map_get_hovered_overlay',
    'cs_map_get_dragging_overlay',
    // Provider exports
    'cs_provider_set_route_profile', 'cs_provider_get_route_profile',
    'cs_provider_set_route_mode', 'cs_provider_get_route_mode',
    'cs_provider_route_is_pending', 'cs_provider_route_mark_fetching',
    'cs_provider_route_from_lat', 'cs_provider_route_from_lon',
    'cs_provider_route_to_lat', 'cs_provider_route_to_lon',
    'cs_provider_route_server', 'cs_provider_on_route_complete', 'cs_provider_on_route_error',
    'cs_provider_search_is_pending', 'cs_provider_search_mark_fetching',
    'cs_provider_search_query', 'cs_provider_search_has_bias',
    'cs_provider_search_bias_lat', 'cs_provider_search_bias_lon',
    'cs_provider_geocode_server', 'cs_provider_set_search_result',
    'cs_provider_on_search_complete', 'cs_provider_on_search_error',
    'cs_provider_reverse_is_pending', 'cs_provider_reverse_mark_fetching',
    'cs_provider_reverse_lat', 'cs_provider_reverse_lon',
    'cs_provider_on_reverse_complete', 'cs_provider_on_reverse_error',
    'malloc', 'free'
];

/* ============================================================================
 * Event Handlers
 * ============================================================================ */

function setupEvents(canvas) {
    // Mouse events
    canvas.addEventListener('mousedown', (e) => {
        const rect = canvas.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const y = e.clientY - rect.top;

        // Check if clicking on scrollbar track
        if (wasm.scrollbar_hit_test_xy(x, y)) {
            isDraggingScrollbar = true;
            wasm.scrollbar_start_drag(
                wasm.scrollbar_scroll_id(), y,
                wasm.scrollbar_track_height(),
                wasm.scrollbar_content_height(),
                wasm.scrollbar_view_height()
            );
            canvas.style.cursor = 'grabbing';
            return;  // Don't propagate to map
        }

        wasm.cs_set_pending_click();
        if (wasm.map_handle_click(x, y)) return;
        isDragging = true;
        canvas.classList.add('dragging');
        wasm.map_pointer_down(x, y);
    });

    window.addEventListener('mousemove', (e) => {
        const rect = canvas.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const y = e.clientY - rect.top;

        // Handle scrollbar drag
        if (isDraggingScrollbar) {
            wasm.scrollbar_move(y);
            return;
        }

        wasm.map_pointer_move(x, y);

        // Hit testing for cursor feedback (only when not dragging)
        // Use visual_zoom (not integer zoom) for consistency with overlay rendering
        if (!isDragging) {
            const mapId = wasm.map_get_component_id();
            const hit = wasm.cs_map_hit_test(
                mapId, x, y,
                wasm.map_get_lat(), wasm.map_get_lon(), wasm.map_get_visual_zoom(),
                wasm.map_get_width(), wasm.map_get_height()
            );
            wasm.cs_map_set_hovered_overlay(mapId, hit);

            // Update cursor based on hit state - check scrollbar first
            if (wasm.scrollbar_hit_test_xy(x, y)) {
                canvas.style.cursor = 'pointer';
            } else if (hit !== 0) {
                canvas.style.cursor = 'pointer';
            } else {
                canvas.style.cursor = 'grab';
            }
        } else {
            // While dragging, show grabbing cursor
            canvas.style.cursor = 'grabbing';
        }
    });

    window.addEventListener('mouseup', (e) => {
        // Handle scrollbar drag end
        if (isDraggingScrollbar) {
            isDraggingScrollbar = false;
            wasm.scrollbar_end_drag();
            canvas.style.cursor = 'grab';
            return;
        }

        if (isDragging) {
            const rect = canvas.getBoundingClientRect();
            const x = e.clientX - rect.left;
            const y = e.clientY - rect.top;
            isDragging = false;
            canvas.classList.remove('dragging');
            wasm.map_pointer_up(x, y);
            canvas.style.cursor = 'grab';
        }
    });

    canvas.addEventListener('wheel', (e) => {
        e.preventDefault();
        const rect = canvas.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const y = e.clientY - rect.top;

        // Route to scroll containers if one is hovered, otherwise zoom the map
        if (wasm.cs_scroll_container_hovered()) {
            wasm.cs_set_scroll_delta(e.deltaY);
        } else {
            wasm.map_scroll(e.deltaY > 0 ? -1 : 1, x, y);
        }
    }, { passive: false });

    // Touch events - mirror mouse event handling for consistency
    canvas.addEventListener('touchstart', (e) => {
        if (e.touches.length === 1) {
            const t = e.touches[0];
            const rect = canvas.getBoundingClientRect();
            const x = t.clientX - rect.left;
            const y = t.clientY - rect.top;
            wasm.cs_set_pending_click();
            if (wasm.map_handle_click(x, y)) return;
            isDragging = true;
            wasm.map_pointer_down(x, y);
        }
    }, { passive: true });

    canvas.addEventListener('touchmove', (e) => {
        if (e.touches.length === 1 && isDragging) {
            e.preventDefault();
            const t = e.touches[0];
            const rect = canvas.getBoundingClientRect();
            const x = t.clientX - rect.left;
            const y = t.clientY - rect.top;
            wasm.map_pointer_move(x, y);
        }
    }, { passive: false });

    canvas.addEventListener('touchend', (e) => {
        if (isDragging && e.changedTouches.length > 0) {
            isDragging = false;
            const t = e.changedTouches[0];
            const rect = canvas.getBoundingClientRect();
            const x = t.clientX - rect.left;
            const y = t.clientY - rect.top;
            wasm.map_pointer_up(x, y);
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
                '../fonts/ui-font.json',
                '../fonts/ui-font.png')
        ]);

        renderer.setFont(font);

        // Transfer font metrics to WASM for accurate text layout
        font.transferMetricsToWasm(wasm);

        // Configure servers
        setCartaServerUrl(SERVER_CONFIG.carta);

        // Initialize app
        wasm.map_init(width, height);
        // Budapest center (Hungary data)
        wasm.map_set_center(47.4979, 19.0402);
        wasm.map_set_zoom(12);

        // Configure provider servers (configurable via URL params)
        wasm.cs_provider_set_route_server(_allocString(wasm, SERVER_CONFIG.velo));
        wasm.cs_provider_set_geocode_server(_allocString(wasm, SERVER_CONFIG.locus));
        console.log('Server config:', SERVER_CONFIG);

        // Initialize provider for API integration (routing, geocoding)
        mapProvider = new MapProvider(wasm);
        mapProvider.start();

        setupEvents(canvas);
        loading.classList.add('hidden');

        // Create and start render loop
        const render = createRenderLoop(renderer, wasm, font, {
            frameFunction: 'map_frame',
            onRender: (projMatrix) => {
                const lat = wasm.map_get_lat();
                const lon = wasm.map_get_lon();
                const visualZoom = wasm.map_get_visual_zoom();
                const mapId = wasm.map_get_component_id();

                // Render tiles first
                tileRenderer.render(
                    lat, lon, visualZoom,
                    wasm.map_get_layer(),
                    renderer.width,
                    renderer.height,
                    projMatrix
                );

                // Render overlays on top of tiles (pass mapId for multi-instance support)
                overlayRenderer.render(
                    wasm,
                    mapId,
                    lat, lon, visualZoom,
                    renderer.width,
                    renderer.height,
                    projMatrix
                );

                // Check if attribution link was clicked
                if (wasm.map_get_attribution_clicked()) {
                    window.open('https://www.openstreetmap.org/copyright', '_blank');
                }
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
