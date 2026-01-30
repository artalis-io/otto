/**
 * Clay Map Viewer - JavaScript Runtime
 *
 * Handles:
 * - Emscripten module loading
 * - Map tile fetching and rendering
 * - Clay UI command rendering
 * - User input handling
 */

// Tile server URLs by layer type
const TILE_SERVERS = [
    // OSM Standard
    (z, x, y) => `https://tile.openstreetmap.org/${z}/${x}/${y}.png`,
    // Carto Light
    (z, x, y) => `https://a.basemaps.cartocdn.com/light_all/${z}/${x}/${y}.png`,
    // Stamen Terrain (now hosted by Stadia)
    (z, x, y) => `https://tiles.stadiamaps.com/tiles/stamen_terrain/${z}/${x}/${y}.png`,
];

// Tile cache
const tileCache = new Map();
const TILE_SIZE = 256;
const MAX_CACHE_SIZE = 200;

// Emscripten module
let Module = null;

// Canvas contexts
let mapCanvas, mapCtx;
let uiCanvas, uiCtx;

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

/**
 * Load Emscripten module
 */
async function loadModule() {
    return new Promise((resolve, reject) => {
        const script = document.createElement('script');
        script.src = 'build/map_ui.js';
        script.onload = () => {
            createMapModule().then(module => {
                Module = module;
                resolve(true);
            }).catch(reject);
        };
        script.onerror = () => reject(new Error('Failed to load WASM module'));
        document.head.appendChild(script);
    });
}

/**
 * Initialize canvases
 */
function initCanvases() {
    mapCanvas = document.getElementById('map-canvas');
    uiCanvas = document.getElementById('ui-canvas');

    const dpr = window.devicePixelRatio || 1;
    const width = window.innerWidth;
    const height = window.innerHeight;

    // Map canvas (2D for tiles)
    mapCanvas.width = width * dpr;
    mapCanvas.height = height * dpr;
    mapCanvas.style.width = width + 'px';
    mapCanvas.style.height = height + 'px';
    mapCtx = mapCanvas.getContext('2d');
    mapCtx.scale(dpr, dpr);

    // UI canvas (2D for Clay rendering)
    uiCanvas.width = width * dpr;
    uiCanvas.height = height * dpr;
    uiCanvas.style.width = width + 'px';
    uiCanvas.style.height = height + 'px';
    uiCtx = uiCanvas.getContext('2d');
    uiCtx.scale(dpr, dpr);

    return { width, height };
}

/**
 * Lon/Lat to tile coordinates
 */
function lonToTileX(lon, zoom) {
    return (lon + 180) / 360 * Math.pow(2, zoom);
}

function latToTileY(lat, zoom) {
    const latRad = lat * Math.PI / 180;
    return (1 - Math.log(Math.tan(latRad) + 1 / Math.cos(latRad)) / Math.PI) / 2 * Math.pow(2, zoom);
}

/**
 * Get or load a tile image
 */
function getTile(z, x, y, layerType) {
    const key = `${layerType}/${z}/${x}/${y}`;

    if (tileCache.has(key)) {
        return tileCache.get(key);
    }

    // Start loading
    const img = new Image();
    img.crossOrigin = 'anonymous';

    const tileData = {
        img: img,
        loaded: false,
        error: false,
    };

    img.onload = () => {
        tileData.loaded = true;
    };

    img.onerror = () => {
        tileData.error = true;
    };

    img.src = TILE_SERVERS[layerType](z, x, y);

    // Add to cache
    tileCache.set(key, tileData);

    // Evict old tiles if cache is too large
    if (tileCache.size > MAX_CACHE_SIZE) {
        const firstKey = tileCache.keys().next().value;
        tileCache.delete(firstKey);
    }

    return tileData;
}

/**
 * Render map tiles
 */
function renderTiles() {
    const width = mapCanvas.width / (window.devicePixelRatio || 1);
    const height = mapCanvas.height / (window.devicePixelRatio || 1);

    // Get map state from WASM
    const lat = Module._map_get_lat();
    const lon = Module._map_get_lon();
    const zoom = Module._map_get_zoom();
    const layerType = Module._map_get_layer();

    // Clear canvas
    mapCtx.fillStyle = '#1a1a1a';
    mapCtx.fillRect(0, 0, width, height);

    // Calculate tile coordinates
    const centerTileX = lonToTileX(lon, zoom);
    const centerTileY = latToTileY(lat, zoom);

    // Calculate how many tiles we need
    const tilesX = Math.ceil(width / TILE_SIZE) + 2;
    const tilesY = Math.ceil(height / TILE_SIZE) + 2;

    // Starting tile
    const startTileX = Math.floor(centerTileX - tilesX / 2);
    const startTileY = Math.floor(centerTileY - tilesY / 2);

    // Render tiles
    for (let dy = 0; dy < tilesY; dy++) {
        for (let dx = 0; dx < tilesX; dx++) {
            const tileX = startTileX + dx;
            const tileY = startTileY + dy;

            // Skip invalid tiles
            const maxTile = Math.pow(2, zoom);
            if (tileY < 0 || tileY >= maxTile) continue;

            // Wrap X coordinate
            const wrappedTileX = ((tileX % maxTile) + maxTile) % maxTile;

            // Calculate screen position
            const screenX = width / 2 + (tileX - centerTileX) * TILE_SIZE;
            const screenY = height / 2 + (tileY - centerTileY) * TILE_SIZE;

            // Get tile
            const tile = getTile(zoom, wrappedTileX, tileY, layerType);

            if (tile.loaded) {
                mapCtx.drawImage(tile.img, screenX, screenY, TILE_SIZE, TILE_SIZE);
            } else if (!tile.error) {
                // Draw placeholder
                mapCtx.fillStyle = '#2a2a2a';
                mapCtx.fillRect(screenX, screenY, TILE_SIZE, TILE_SIZE);
                mapCtx.strokeStyle = '#333';
                mapCtx.strokeRect(screenX, screenY, TILE_SIZE, TILE_SIZE);
            }
        }
    }
}

/**
 * Unpack RGBA color from uint32
 */
function unpackColor(packed) {
    const r = (packed >>> 24) & 0xFF;
    const g = (packed >>> 16) & 0xFF;
    const b = (packed >>> 8) & 0xFF;
    const a = packed & 0xFF;
    return `rgba(${r}, ${g}, ${b}, ${a / 255})`;
}

/**
 * Draw rounded rectangle
 */
function roundRect(ctx, x, y, width, height, radius) {
    ctx.beginPath();
    ctx.moveTo(x + radius, y);
    ctx.lineTo(x + width - radius, y);
    ctx.quadraticCurveTo(x + width, y, x + width, y + radius);
    ctx.lineTo(x + width, y + height - radius);
    ctx.quadraticCurveTo(x + width, y + height, x + width - radius, y + height);
    ctx.lineTo(x + radius, y + height);
    ctx.quadraticCurveTo(x, y + height, x, y + height - radius);
    ctx.lineTo(x, y + radius);
    ctx.quadraticCurveTo(x, y, x + radius, y);
    ctx.closePath();
}

/**
 * Render Clay UI commands
 */
function renderUI() {
    const width = uiCanvas.width / (window.devicePixelRatio || 1);
    const height = uiCanvas.height / (window.devicePixelRatio || 1);

    // Clear UI canvas
    uiCtx.clearRect(0, 0, width, height);

    // Run Clay layout and get command count
    const count = Module._map_frame();

    for (let i = 0; i < count; i++) {
        const cmdType = Module._map_cmd_type(i);
        const x = Module._map_cmd_x(i);
        const y = Module._map_cmd_y(i);
        const w = Module._map_cmd_w(i);
        const h = Module._map_cmd_h(i);

        switch (cmdType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                const color = Module._map_cmd_rect_color(i);
                const radius = Module._map_cmd_rect_radius(i);

                uiCtx.fillStyle = unpackColor(color);

                if (radius > 0) {
                    roundRect(uiCtx, x, y, w, h, radius);
                    uiCtx.fill();
                } else {
                    uiCtx.fillRect(x, y, w, h);
                }
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                const strPtr = Module._map_cmd_text_str(i);
                const strLen = Module._map_cmd_text_len(i);
                const color = Module._map_cmd_text_color(i);
                const fontSize = Module._map_cmd_text_size(i);

                // Read string from WASM memory
                let text = '';
                for (let j = 0; j < strLen; j++) {
                    text += String.fromCharCode(Module.HEAPU8[strPtr + j]);
                }

                uiCtx.fillStyle = unpackColor(color);
                uiCtx.font = `${fontSize}px -apple-system, BlinkMacSystemFont, sans-serif`;
                uiCtx.textBaseline = 'top';
                uiCtx.fillText(text, x, y);
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                const color = Module._map_cmd_border_color(i);
                const radius = Module._map_cmd_border_radius(i);
                const borderWidth = Module._map_cmd_border_width(i);

                uiCtx.strokeStyle = unpackColor(color);
                uiCtx.lineWidth = borderWidth || 1;

                if (radius > 0) {
                    roundRect(uiCtx, x, y, w, h, radius);
                    uiCtx.stroke();
                } else {
                    uiCtx.strokeRect(x, y, w, h);
                }
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
                uiCtx.save();
                uiCtx.beginPath();
                uiCtx.rect(x, y, w, h);
                uiCtx.clip();
                break;

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
                uiCtx.restore();
                break;
        }
    }
}

/**
 * Main render loop
 */
function render(timestamp) {
    const dt = timestamp - lastFrameTime;
    lastFrameTime = timestamp;

    renderTiles();
    renderUI();

    animationId = requestAnimationFrame(render);
}

/**
 * Event handlers
 */
function setupEventHandlers() {
    // Mouse events
    mapCanvas.addEventListener('mousedown', (e) => {
        // Check if UI handled the click
        if (Module._map_handle_click(e.clientX, e.clientY)) {
            return;
        }

        isDragging = true;
        mapCanvas.classList.add('dragging');
        Module._map_pointer_down(e.clientX, e.clientY);
    });

    window.addEventListener('mousemove', (e) => {
        Module._map_pointer_move(e.clientX, e.clientY);
    });

    window.addEventListener('mouseup', (e) => {
        if (isDragging) {
            isDragging = false;
            mapCanvas.classList.remove('dragging');
            Module._map_pointer_up(e.clientX, e.clientY);
        }
    });

    // Wheel/scroll for zoom
    mapCanvas.addEventListener('wheel', (e) => {
        e.preventDefault();
        const delta = e.deltaY > 0 ? -1 : 1;
        Module._map_scroll(delta, e.clientX, e.clientY);
    }, { passive: false });

    // Touch events
    mapCanvas.addEventListener('touchstart', (e) => {
        if (e.touches.length === 1) {
            const touch = e.touches[0];
            isDragging = true;
            Module._map_pointer_down(touch.clientX, touch.clientY);
        }
    });

    mapCanvas.addEventListener('touchmove', (e) => {
        if (e.touches.length === 1 && isDragging) {
            e.preventDefault();
            const touch = e.touches[0];
            Module._map_pointer_move(touch.clientX, touch.clientY);
        }
    }, { passive: false });

    mapCanvas.addEventListener('touchend', (e) => {
        if (isDragging) {
            isDragging = false;
            const touch = e.changedTouches[0];
            Module._map_pointer_up(touch.clientX, touch.clientY);
        }
    });

    // Resize handler
    window.addEventListener('resize', () => {
        const { width, height } = initCanvases();
        Module._map_resize(width, height);
    });

    // Keyboard shortcuts
    window.addEventListener('keydown', (e) => {
        switch (e.key) {
            case '+':
            case '=':
                Module._map_scroll(1, 0, 0);
                break;
            case '-':
                Module._map_scroll(-1, 0, 0);
                break;
        }
    });
}

/**
 * Main initialization
 */
async function main() {
    const loadingEl = document.getElementById('loading');

    // Initialize canvases
    const { width, height } = initCanvases();

    try {
        // Load Emscripten module
        await loadModule();
    } catch (err) {
        loadingEl.querySelector('p').textContent = 'Failed to load WASM: ' + err.message;
        return;
    }

    // Initialize map
    Module._map_init(width, height);
    Module._map_set_center(47.4979, 19.0402);  // Budapest
    Module._map_set_zoom(12);

    // Setup event handlers
    setupEventHandlers();

    // Hide loading screen
    loadingEl.classList.add('hidden');

    // Start render loop
    lastFrameTime = performance.now();
    animationId = requestAnimationFrame(render);
}

// Start when DOM is ready
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', main);
} else {
    main();
}
