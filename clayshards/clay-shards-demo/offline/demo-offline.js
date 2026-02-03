/**
 * ClayShards Offline Demo
 *
 * Demonstrates offline-capable map with integrated Velo routing,
 * Locus geocoding, and Carta tile generation via WASM.
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
} from '../../clay-shards-webgl/index.js';

import { WasmMapProvider, WasmProviderBridge } from '../../clay-shards-webgl/wasm-map-provider.js';

/* Configuration */
const CONFIG = {
    // Default region (change to 'hungary' for full Hungary support)
    region: 'monaco',

    // Map center - Monaco
    defaultCenter: { lat: 43.7384, lon: 7.4246 },
    defaultZoom: 14,

    // For Hungary, use:
    // defaultCenter: { lat: 47.4979, lon: 19.0402 },
    // defaultZoom: 12,

    // Data paths
    dataDir: 'data',

    // WASM module paths (relative to velo/wasm/build, etc.)
    veloWasm: '../../../../velo/wasm/build/velo.js',
    locusWasm: '../../../../locus/wasm/build/locus.js',
    cartaWasm: '../../../../carta/wasm/build/carta.js',
};

/* State */
let wasm = null;
let renderer = null;
let font = null;
let tileRenderer = null;
let overlayRenderer = null;
let wasmProvider = null;
let providerBridge = null;
let isDragging = false;

/* UI Helpers */
function setLoadingStatus(text) {
    document.getElementById('loading-status').textContent = text;
}

function setProgress(percent) {
    document.getElementById('progress-bar').style.width = `${percent}%`;
}

/* Required WASM Exports */
const REQUIRED_EXPORTS = [
    'map_init', 'map_frame', 'map_resize',
    'map_get_lat', 'map_get_lon', 'map_get_zoom', 'map_get_visual_zoom', 'map_get_layer',
    'map_pointer_down', 'map_pointer_move', 'map_pointer_up', 'map_scroll',
    'map_handle_click',
    'cs_set_pending_click', 'cs_focused_id', 'cs_key_down', 'cs_key_char',
    'cs_map_overlay_count', 'cs_map_overlay_type',
    'cs_map_overlay_polyline_count', 'cs_map_overlay_polyline_lat', 'cs_map_overlay_polyline_lon',
    'cs_map_overlay_polyline_color_r', 'cs_map_overlay_polyline_color_g',
    'cs_map_overlay_polyline_color_b', 'cs_map_overlay_polyline_color_a',
    'cs_map_overlay_polyline_width',
    'cs_map_overlay_marker_lat', 'cs_map_overlay_marker_lon', 'cs_map_overlay_marker_radius',
    'cs_map_overlay_marker_color_r', 'cs_map_overlay_marker_color_g',
    'cs_map_overlay_marker_color_b', 'cs_map_overlay_marker_color_a',
    'cs_map_overlay_marker_border_r', 'cs_map_overlay_marker_border_g',
    'cs_map_overlay_marker_border_b', 'cs_map_overlay_marker_border_a',
    'cs_map_overlay_marker_border_width',
    // Provider exports
    'cs_provider_route_is_pending', 'cs_provider_route_mark_fetching',
    'cs_provider_route_from_lat', 'cs_provider_route_from_lon',
    'cs_provider_route_to_lat', 'cs_provider_route_to_lon',
    'cs_provider_on_route_complete', 'cs_provider_on_route_error',
    'malloc', 'free'
];

/* Event Handlers */
function setupEvents(canvas) {
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

    window.addEventListener('resize', () => {
        const { width, height } = renderer.resize();
        wasm.map_resize(width, height);
    });

    setupKeyboardHandler(wasm, {
        onGlobalShortcut: (e) => {
            if (e.key === '+' || e.key === '=') wasm.map_scroll(1, 0, 0);
            if (e.key === '-') wasm.map_scroll(-1, 0, 0);
        }
    });
}

/* Main */
async function main() {
    const loading = document.getElementById('loading');
    const canvas = document.getElementById('map-canvas');

    try {
        // Initialize renderer
        setLoadingStatus('Initializing renderer...');
        setProgress(10);

        renderer = new ClayRenderer(canvas);
        const { width, height } = renderer.resize();

        font = new MSDFFont();
        const tileCache = new TileCache(renderer.gl);
        tileRenderer = new MapTileRenderer(renderer, tileCache);
        overlayRenderer = new MapOverlayRenderer(renderer);

        // Load demo WASM and font
        setLoadingStatus('Loading demo WASM...');
        setProgress(20);

        [wasm] = await Promise.all([
            loadWasm('../build/demo.wasm', REQUIRED_EXPORTS),
            font.load(renderer.gl,
                '../../../fonts/ui-font.json',
                '../../../fonts/ui-font.png')
        ]);

        renderer.setFont(font);
        font.transferMetricsToWasm(wasm);

        // Initialize WASM map provider
        setLoadingStatus('Loading WASM modules (Velo, Locus, Carta)...');
        setProgress(30);

        wasmProvider = new WasmMapProvider();

        // Try to load the provider modules
        try {
            await wasmProvider.initModules({
                veloPath: CONFIG.veloWasm,
                locusPath: CONFIG.locusWasm,
                cartaPath: CONFIG.cartaWasm
            });
            setProgress(50);

            // Load data files
            const vlgFile = `${CONFIG.dataDir}/${CONFIG.region}.vlg`;
            const pbfFile = `${CONFIG.dataDir}/${CONFIG.region}-latest.osm.pbf`;

            setLoadingStatus('Loading Velo graph...');
            await wasmProvider.loadVeloGraph(vlgFile);
            setProgress(70);

            setLoadingStatus('Loading Locus index...');
            await wasmProvider.loadLocusIndex(pbfFile);
            setProgress(85);

            // Note: Carta context loading is optional for offline tiles
            // For now we'll use online tiles as fallback
            // await wasmProvider.loadCartaContext(pbfFile);

            // Create provider bridge for routing
            providerBridge = new WasmProviderBridge(wasm, wasmProvider);
            providerBridge.start();
        } catch (err) {
            // Offline provider not available - fall back to online mode
        }

        setProgress(95);

        // Initialize app
        wasm.map_init(width, height);
        wasm.map_set_center(CONFIG.defaultCenter.lat, CONFIG.defaultCenter.lon);
        wasm.map_set_zoom(CONFIG.defaultZoom);

        setupEvents(canvas);

        setLoadingStatus('Ready!');
        setProgress(100);

        setTimeout(() => loading.classList.add('hidden'), 300);

        // Create render loop
        const render = createRenderLoop(renderer, wasm, font, {
            frameFunction: 'map_frame',
            onRender: (projMatrix) => {
                const lat = wasm.map_get_lat();
                const lon = wasm.map_get_lon();
                const visualZoom = wasm.map_get_visual_zoom();

                // Render tiles
                tileRenderer.render(
                    lat, lon, visualZoom,
                    wasm.map_get_layer(),
                    renderer.width, renderer.height,
                    projMatrix
                );

                // Render overlays (routes, markers)
                overlayRenderer.render(
                    wasm,
                    lat, lon, visualZoom,
                    renderer.width, renderer.height,
                    projMatrix
                );
            },
            cursorFontSize: 12,
            cursorPadding: 8
        });

        requestAnimationFrame(render);

    } catch (err) {
        setLoadingStatus('Failed: ' + err.message);
        console.error(err);
    }
}

document.readyState === 'loading'
    ? document.addEventListener('DOMContentLoaded', main)
    : main();
