/**
 * Clay Map - Tile Loading Module
 *
 * Handles fetching and caching map tiles from various providers.
 */

const TILE_SERVERS = [
    // OSM Standard
    (z, x, y) => `https://tile.openstreetmap.org/${z}/${x}/${y}.png`,
    // Carto Light
    (z, x, y) => `https://a.basemaps.cartocdn.com/light_all/${z}/${x}/${y}.png`,
    // Stamen Terrain (Stadia)
    (z, x, y) => `https://tiles.stadiamaps.com/tiles/stamen_terrain/${z}/${x}/${y}.png`,
];

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
        this.tileSize = 256;
    }

    render(lat, lon, zoom, layerType, width, height, projMatrix) {
        const centerTileX = this.lonToTileX(lon, zoom);
        const centerTileY = this.latToTileY(lat, zoom);

        const tilesX = Math.ceil(width / this.tileSize) + 2;
        const tilesY = Math.ceil(height / this.tileSize) + 2;

        const startTileX = Math.floor(centerTileX - tilesX / 2);
        const startTileY = Math.floor(centerTileY - tilesY / 2);

        for (let dy = 0; dy < tilesY; dy++) {
            for (let dx = 0; dx < tilesX; dx++) {
                const tileX = startTileX + dx;
                const tileY = startTileY + dy;

                const maxTile = Math.pow(2, zoom);
                if (tileY < 0 || tileY >= maxTile) continue;

                const wrappedTileX = ((tileX % maxTile) + maxTile) % maxTile;

                const screenX = width / 2 + (tileX - centerTileX) * this.tileSize;
                const screenY = height / 2 + (tileY - centerTileY) * this.tileSize;

                const tile = this.tileCache.getTile(zoom, wrappedTileX, tileY, layerType);

                if (tile.loaded && tile.texture) {
                    this.renderer.renderTexture(
                        tile.texture,
                        screenX, screenY,
                        this.tileSize, this.tileSize,
                        projMatrix
                    );
                }
            }
        }
    }

    lonToTileX(lon, zoom) {
        return (lon + 180) / 360 * Math.pow(2, zoom);
    }

    latToTileY(lat, zoom) {
        const latRad = lat * Math.PI / 180;
        return (1 - Math.log(Math.tan(latRad) + 1 / Math.cos(latRad)) / Math.PI) / 2 * Math.pow(2, zoom);
    }
}
