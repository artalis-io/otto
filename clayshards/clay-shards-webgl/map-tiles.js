/**
 * Clay Map - Tile Loading Module
 *
 * Handles fetching and caching map tiles from various providers.
 */

// Default tile server base URL (can be overridden)
let cartaServerUrl = 'http://localhost:8081';

const TILE_SERVERS = [
    // Carta (local tile server)
    (z, x, y) => `${cartaServerUrl}/tiles/${z}/${x}/${y}.png`,
    // OSM Standard
    (z, x, y) => `https://tile.openstreetmap.org/${z}/${x}/${y}.png`,
    // Carto Light
    (z, x, y) => `https://a.basemaps.cartocdn.com/light_all/${z}/${x}/${y}.png`,
    // Stamen Terrain (Stadia)
    (z, x, y) => `https://tiles.stadiamaps.com/tiles/stamen_terrain/${z}/${x}/${y}.png`,
];

/**
 * Configure the Carta tile server URL
 * @param {string} url - Base URL for the Carta tile server
 * @throws {Error} If URL is invalid or uses unsupported protocol
 */
export function setCartaServerUrl(url) {
    // Validate URL format and protocol
    let parsed;
    try {
        parsed = new URL(url);
    } catch {
        throw new Error(`Invalid tile server URL: ${url}`);
    }

    if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
        throw new Error(`Unsupported protocol: ${parsed.protocol} (use http or https)`);
    }

    // Remove trailing slash for consistent URL construction
    cartaServerUrl = url.replace(/\/+$/, '');
}

export class TileCache {
    constructor(gl, maxSize = 200) {
        this.gl = gl;
        this.maxSize = maxSize;
        this.cache = new Map();
    }

    getTile(z, x, y, layerType) {
        const key = `${layerType}/${z}/${x}/${y}`;

        if (this.cache.has(key)) {
            return this.cache.get(key);
        }

        const img = new Image();
        img.crossOrigin = 'anonymous';

        const tileData = {
            img,
            loaded: false,
            error: false,
            texture: null
        };

        img.onload = () => {
            tileData.loaded = true;
            tileData.texture = this.gl.createTexture();
            this.gl.bindTexture(this.gl.TEXTURE_2D, tileData.texture);
            this.gl.texImage2D(this.gl.TEXTURE_2D, 0, this.gl.RGBA, this.gl.RGBA, this.gl.UNSIGNED_BYTE, img);
            this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_MIN_FILTER, this.gl.LINEAR);
            this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_MAG_FILTER, this.gl.LINEAR);
            this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_WRAP_S, this.gl.CLAMP_TO_EDGE);
            this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_WRAP_T, this.gl.CLAMP_TO_EDGE);
        };

        img.onerror = () => {
            tileData.error = true;
        };

        // Validate layerType and fallback to OSM if invalid
        const serverIndex = (layerType >= 0 && layerType < TILE_SERVERS.length) ? layerType : 0;
        img.src = TILE_SERVERS[serverIndex](z, x, y);

        this.cache.set(key, tileData);

        // Evict old tiles
        if (this.cache.size > this.maxSize) {
            const iter = this.cache.keys();
            const first = iter.next();
            if (!first.done && first.value) {
                const firstKey = first.value;
                const old = this.cache.get(firstKey);
                if (old && old.texture) this.gl.deleteTexture(old.texture);
                this.cache.delete(firstKey);
            }
        }

        return tileData;
    }

    clear() {
        for (const tile of this.cache.values()) {
            if (tile.texture) this.gl.deleteTexture(tile.texture);
        }
        this.cache.clear();
    }
}

export class MapTileRenderer {
    constructor(renderer, tileCache) {
        this.renderer = renderer;
        this.tileCache = tileCache;
        this.baseTileSize = 256;

        // Transition state for smooth zoom level changes
        this.lastTileZoom = null;
        this.transitionProgress = 1.0;  // 1.0 = fully transitioned, no crossfade
        this.transitionStartTime = null;
        this.transitionDuration = 400;  // ms for crossfade
    }

    /**
     * Render map tiles with smooth zoom and crossfade transitions
     * @param {number} lat - Center latitude
     * @param {number} lon - Center longitude
     * @param {number} visualZoom - Visual zoom level (can be fractional for smooth animation)
     * @param {number} layerType - Tile layer type
     * @param {number} width - Viewport width
     * @param {number} height - Viewport height
     * @param {Float32Array} projMatrix - Projection matrix
     */
    render(lat, lon, visualZoom, layerType, width, height, projMatrix) {
        const tileZoom = Math.floor(visualZoom);

        // Detect zoom level change - start crossfade transition
        if (this.lastTileZoom !== null && tileZoom !== this.lastTileZoom) {
            this.transitionProgress = 0;
            this.transitionStartTime = performance.now();
        }

        // Update transition progress
        if (this.transitionProgress < 1) {
            const elapsed = performance.now() - this.transitionStartTime;
            this.transitionProgress = Math.min(1, elapsed / this.transitionDuration);
        }

        // During transition: old tiles as backdrop (full opacity), new tiles fade in on top
        if (this.transitionProgress < 1 && this.lastTileZoom !== null) {
            // Old tiles: full opacity backdrop (scaled to current visual zoom)
            const oldScale = Math.pow(2, visualZoom - this.lastTileZoom);
            this.renderTilesAtZoom(
                lat, lon, this.lastTileZoom, oldScale,
                layerType, width, height, projMatrix, 1.0
            );

            // New tiles: fade in on top
            const newAlpha = this.easeOutQuad(this.transitionProgress);
            const newScale = Math.pow(2, visualZoom - tileZoom);
            this.renderTilesAtZoom(
                lat, lon, tileZoom, newScale,
                layerType, width, height, projMatrix, newAlpha
            );
        } else {
            // No transition: just render current zoom level
            const scale = Math.pow(2, visualZoom - tileZoom);
            this.renderTilesAtZoom(
                lat, lon, tileZoom, scale,
                layerType, width, height, projMatrix, 1.0
            );
        }

        // Prefetch tiles at next zoom level when approaching it
        const zoomFraction = visualZoom - tileZoom;
        if (zoomFraction > 0.3) {
            this.prefetchTiles(lat, lon, tileZoom + 1, layerType, width, height);
        }

        // Update last zoom for next frame
        this.lastTileZoom = tileZoom;
    }

    /**
     * Render tiles at a specific zoom level with given scale and alpha
     */
    renderTilesAtZoom(lat, lon, zoom, scale, layerType, width, height, projMatrix, alpha) {
        const tileSize = this.baseTileSize * scale;

        const centerTileX = this.lonToTileX(lon, zoom);
        const centerTileY = this.latToTileY(lat, zoom);

        const tilesX = Math.ceil(width / tileSize) + 2;
        const tilesY = Math.ceil(height / tileSize) + 2;

        const startTileX = Math.floor(centerTileX - tilesX / 2);
        const startTileY = Math.floor(centerTileY - tilesY / 2);

        const maxTile = Math.pow(2, zoom);

        for (let dy = 0; dy < tilesY; dy++) {
            for (let dx = 0; dx < tilesX; dx++) {
                const tileX = startTileX + dx;
                const tileY = startTileY + dy;

                if (tileY < 0 || tileY >= maxTile) continue;

                const wrappedTileX = ((tileX % maxTile) + maxTile) % maxTile;

                const screenX = width / 2 + (tileX - centerTileX) * tileSize;
                const screenY = height / 2 + (tileY - centerTileY) * tileSize;

                const tile = this.tileCache.getTile(zoom, wrappedTileX, tileY, layerType);

                if (tile.loaded && tile.texture) {
                    this.renderer.renderTexture(
                        tile.texture,
                        screenX, screenY,
                        tileSize, tileSize,
                        projMatrix,
                        alpha
                    );
                }
            }
        }
    }

    /**
     * Prefetch tiles at a given zoom level (triggers cache loading without rendering)
     */
    prefetchTiles(lat, lon, zoom, layerType, width, height) {
        const centerTileX = this.lonToTileX(lon, zoom);
        const centerTileY = this.latToTileY(lat, zoom);

        const tilesX = Math.ceil(width / this.baseTileSize) + 2;
        const tilesY = Math.ceil(height / this.baseTileSize) + 2;

        const startTileX = Math.floor(centerTileX - tilesX / 2);
        const startTileY = Math.floor(centerTileY - tilesY / 2);

        const maxTile = Math.pow(2, zoom);

        for (let dy = 0; dy < tilesY; dy++) {
            for (let dx = 0; dx < tilesX; dx++) {
                const tileX = startTileX + dx;
                const tileY = startTileY + dy;

                if (tileY < 0 || tileY >= maxTile) continue;

                const wrappedTileX = ((tileX % maxTile) + maxTile) % maxTile;
                this.tileCache.getTile(zoom, wrappedTileX, tileY, layerType);
            }
        }
    }

    /**
     * Easing function for smooth transitions
     */
    easeOutQuad(t) {
        return t * (2 - t);
    }

    lonToTileX(lon, zoom) {
        return (lon + 180) / 360 * Math.pow(2, zoom);
    }

    latToTileY(lat, zoom) {
        const latRad = lat * Math.PI / 180;
        return (1 - Math.log(Math.tan(latRad) + 1 / Math.cos(latRad)) / Math.PI) / 2 * Math.pow(2, zoom);
    }
}
