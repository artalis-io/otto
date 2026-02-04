/**
 * WASM Map Provider - Offline-capable map services
 *
 * Loads Velo, Locus, and Carta WASM modules and provides
 * routing, geocoding, and tile generation without needing API servers.
 *
 * Data files:
 *   - hungary.vlg: Pre-compiled Velo graph
 *   - hungary.osm.pbf: OSM data for Locus and Carta
 */

export class WasmMapProvider {
    constructor() {
        this.velo = null;
        this.locus = null;
        this.carta = null;

        this.veloGraph = null;
        this.locusIndex = null;
        this.cartaContext = null;

        this.ready = false;
        this.loadError = null;
    }

    /**
     * Initialize WASM modules
     * @param {Object} config - Configuration
     * @param {string} config.veloPath - Path to velo.js
     * @param {string} config.locusPath - Path to locus.js
     * @param {string} config.cartaPath - Path to carta.js
     */
    async initModules(config) {
        const { veloPath, locusPath, cartaPath } = config;

        try {
            // Load WASM modules in parallel
            const [Velo, Locus, Carta] = await Promise.all([
                import(veloPath).then(m => m.default()),
                import(locusPath).then(m => m.default()),
                import(cartaPath).then(m => m.default())
            ]);

            this.velo = Velo;
            this.locus = Locus;
            this.carta = Carta;

            return true;
        } catch (err) {
            this.loadError = err.message;
            return false;
        }
    }

    /**
     * Load Velo graph from URL
     * @param {string} url - URL to .vlg file
     */
    async loadVeloGraph(url) {
        if (!this.velo) throw new Error('Velo module not initialized');

        const response = await fetch(url);
        if (!response.ok) throw new Error(`HTTP ${response.status}`);

        const buffer = await response.arrayBuffer();
        const data = new Uint8Array(buffer);

        const ptr = this.velo._wasm_malloc(data.length);
        this.velo.HEAPU8.set(data, ptr);

        this.veloGraph = this.velo._wasm_load_graph_memory(ptr, data.length);
        this.velo._wasm_free(ptr);

        if (!this.veloGraph) {
            throw new Error('Failed to load Velo graph');
        }

        return true;
    }

    /**
     * Load Locus index from PBF URL
     * @param {string} url - URL to .osm.pbf file
     */
    async loadLocusIndex(url) {
        if (!this.locus) throw new Error('Locus module not initialized');

        const response = await fetch(url);
        if (!response.ok) throw new Error(`HTTP ${response.status}`);

        const buffer = await response.arrayBuffer();
        const data = new Uint8Array(buffer);

        this.locusIndex = this.locus._wasm_index_create();
        if (!this.locusIndex) {
            throw new Error('Failed to create Locus index');
        }

        const ptr = this.locus._wasm_malloc(data.length);
        this.locus.HEAPU8.set(data, ptr);

        const status = this.locus._wasm_index_load_pbf_memory(this.locusIndex, ptr, data.length);
        this.locus._wasm_free(ptr);

        if (status !== 0) {
            throw new Error(`Failed to load Locus index: status ${status}`);
        }

        return true;
    }

    /**
     * Load Carta context from PBF URL
     * @param {string} url - URL to .osm.pbf file
     */
    async loadCartaContext(url) {
        if (!this.carta) throw new Error('Carta module not initialized');

        const response = await fetch(url);
        if (!response.ok) throw new Error(`HTTP ${response.status}`);

        const buffer = await response.arrayBuffer();
        const data = new Uint8Array(buffer);

        const ptr = this.carta._wasm_malloc(data.length);
        this.carta.HEAPU8.set(data, ptr);

        this.cartaContext = this.carta._wasm_load_pbf_memory(ptr, data.length);
        this.carta._wasm_free(ptr);

        if (!this.cartaContext) {
            throw new Error('Failed to load Carta context');
        }

        return true;
    }

    /**
     * Calculate route between two points
     * @param {Object} from - {lat, lon}
     * @param {Object} to - {lat, lon}
     * @param {number} profile - 0=car, 1=truck, 2=bike, 3=foot
     * @param {number} mode - 0=fastest, 1=shortest
     * @returns {Object|null} Route result or null
     */
    route(from, to, profile = 0, mode = 0) {
        if (!this.veloGraph) return null;

        const routePtr = this.velo._wasm_route(
            this.veloGraph,
            from.lat, from.lon,
            to.lat, to.lon,
            profile, mode
        );

        if (!routePtr) return null;

        const distance = this.velo._wasm_route_distance(routePtr);
        const duration = this.velo._wasm_route_duration(routePtr);
        const nodeCount = this.velo._wasm_route_node_count(routePtr);

        // Get coordinates
        const coordsPtr = this.velo._wasm_malloc(nodeCount * 2 * 8);
        this.velo._wasm_route_get_coords(routePtr, coordsPtr);

        const coords = [];
        for (let i = 0; i < nodeCount; i++) {
            coords.push({
                lat: this.velo.HEAPF64[(coordsPtr >> 3) + i * 2],
                lon: this.velo.HEAPF64[(coordsPtr >> 3) + i * 2 + 1]
            });
        }

        this.velo._wasm_free(coordsPtr);
        this.velo._wasm_route_free(routePtr);

        return { distance, duration, geometry: coords };
    }

    /**
     * Forward geocoding search
     * @param {string} query - Search text
     * @param {number} limit - Max results
     * @returns {Array} Search results
     */
    search(query, limit = 10) {
        if (!this.locusIndex) return [];

        const queryPtr = this._allocString(this.locus, query);
        const resultPtr = this.locus._wasm_search(this.locusIndex, queryPtr, limit, 1);
        this.locus._wasm_free(queryPtr);

        if (!resultPtr) return [];

        return this._extractLocusResults(resultPtr);
    }

    /**
     * Autocomplete search
     * @param {string} prefix - Search prefix
     * @param {number} limit - Max results
     * @returns {Array} Autocomplete results
     */
    autocomplete(prefix, limit = 10) {
        if (!this.locusIndex) return [];

        const prefixPtr = this._allocString(this.locus, prefix);
        const resultPtr = this.locus._wasm_autocomplete(this.locusIndex, prefixPtr, limit);
        this.locus._wasm_free(prefixPtr);

        if (!resultPtr) return [];

        return this._extractLocusResults(resultPtr);
    }

    /**
     * Reverse geocoding
     * @param {number} lat - Latitude
     * @param {number} lon - Longitude
     * @param {number} radius - Search radius in meters
     * @returns {Array} Nearby places
     */
    reverse(lat, lon, radius = 100) {
        if (!this.locusIndex) return [];

        const resultPtr = this.locus._wasm_reverse(this.locusIndex, lat, lon, radius);
        if (!resultPtr) return [];

        return this._extractLocusResults(resultPtr);
    }

    /**
     * Generate PNG tile
     * @param {number} z - Zoom level
     * @param {number} x - Tile X
     * @param {number} y - Tile Y
     * @param {number} size - Tile size (default 256)
     * @returns {Blob|null} PNG blob or null
     */
    generateTilePNG(z, x, y, size = 256) {
        if (!this.cartaContext) return null;

        const bufferSize = size * size * 4 + 4096;
        const bufferPtr = this.carta._wasm_malloc(bufferSize);

        const pngSize = this.carta._wasm_generate_png(
            this.cartaContext, z, x, y, size, bufferPtr, bufferSize
        );

        if (pngSize === 0) {
            this.carta._wasm_free(bufferPtr);
            return null;
        }

        const pngData = new Uint8Array(pngSize);
        pngData.set(this.carta.HEAPU8.subarray(bufferPtr, bufferPtr + pngSize));
        this.carta._wasm_free(bufferPtr);

        return new Blob([pngData], { type: 'image/png' });
    }

    /**
     * Generate MVT tile
     * @param {number} z - Zoom level
     * @param {number} x - Tile X
     * @param {number} y - Tile Y
     * @returns {ArrayBuffer|null} MVT data or null
     */
    generateTileMVT(z, x, y) {
        if (!this.cartaContext) return null;

        const bufferSize = 1024 * 1024;
        const bufferPtr = this.carta._wasm_malloc(bufferSize);

        const mvtSize = this.carta._wasm_generate_mvt(
            this.cartaContext, z, x, y, bufferPtr, bufferSize
        );

        if (mvtSize === 0) {
            this.carta._wasm_free(bufferPtr);
            return null;
        }

        const mvtData = new ArrayBuffer(mvtSize);
        new Uint8Array(mvtData).set(
            this.carta.HEAPU8.subarray(bufferPtr, bufferPtr + mvtSize)
        );
        this.carta._wasm_free(bufferPtr);

        return mvtData;
    }

    /**
     * Helper: allocate string in WASM memory
     */
    _allocString(module, str) {
        const len = module.lengthBytesUTF8(str) + 1;
        const ptr = module._wasm_malloc(len);
        module.stringToUTF8(str, ptr, len);
        return ptr;
    }

    /**
     * Helper: extract Locus search results
     */
    _extractLocusResults(resultPtr) {
        const results = [];
        const count = this.locus._wasm_result_count(resultPtr);

        for (let i = 0; i < count; i++) {
            const namePtr = this.locus._wasm_result_get_name(resultPtr, i);
            results.push({
                id: this.locus._wasm_result_get_id(resultPtr, i),
                name: namePtr ? this.locus.UTF8ToString(namePtr) : '',
                lat: this.locus._wasm_result_get_lat(resultPtr, i),
                lon: this.locus._wasm_result_get_lon(resultPtr, i),
                score: this.locus._wasm_result_get_score(resultPtr, i)
            });
        }

        this.locus._wasm_result_free(resultPtr);
        return results;
    }

    /**
     * Clean up resources
     */
    destroy() {
        if (this.veloGraph && this.velo) {
            this.velo._wasm_graph_free(this.veloGraph);
            this.veloGraph = null;
        }
        if (this.locusIndex && this.locus) {
            this.locus._wasm_index_free(this.locusIndex);
            this.locusIndex = null;
        }
        if (this.cartaContext && this.carta) {
            this.carta._wasm_context_free(this.cartaContext);
            this.cartaContext = null;
        }
    }
}

/**
 * Provider bridge that connects WasmMapProvider to ClayShards demo
 *
 * Polls the WASM app for pending route requests and uses WasmMapProvider
 * to compute them directly (no HTTP needed).
 */
export class WasmProviderBridge {
    constructor(wasm, wasmProvider) {
        this.wasm = wasm;
        this.provider = wasmProvider;
        this.polling = false;
        this.pollInterval = null;
    }

    start(intervalMs = 50) {
        if (this.polling) return;
        this.polling = true;
        this.pollInterval = setInterval(() => this._poll(), intervalMs);
    }

    stop() {
        this.polling = false;
        if (this.pollInterval) {
            clearInterval(this.pollInterval);
            this.pollInterval = null;
        }
    }

    _poll() {
        if (!this.wasm || !this.provider.veloGraph) return;

        // Check for pending route request
        if (this.wasm.cs_provider_route_is_pending()) {
            this._handleRoute();
        }
    }

    _handleRoute() {
        const wasm = this.wasm;

        // Mark as being handled
        wasm.cs_provider_route_mark_fetching();

        const from = {
            lat: wasm.cs_provider_route_from_lat(),
            lon: wasm.cs_provider_route_from_lon()
        };
        const to = {
            lat: wasm.cs_provider_route_to_lat(),
            lon: wasm.cs_provider_route_to_lon()
        };

        // Compute route directly via WASM
        const result = this.provider.route(from, to);

        if (!result || result.geometry.length === 0) {
            const msgPtr = this._allocString('No route found');
            wasm.cs_provider_on_route_error(msgPtr);
            wasm.free(msgPtr);
            return;
        }

        // Pass result to WASM
        const count = result.geometry.length;
        // Allocate both arrays first (either malloc may trigger memory growth)
        const latsPtr = wasm.malloc(count * 8);
        const lonsPtr = wasm.malloc(count * 8);

        // Create views AFTER all mallocs to ensure buffer is stable
        const lats = new Float64Array(wasm.memory.buffer, latsPtr, count);
        const lons = new Float64Array(wasm.memory.buffer, lonsPtr, count);

        for (let i = 0; i < count; i++) {
            lats[i] = result.geometry[i].lat;
            lons[i] = result.geometry[i].lon;
        }

        wasm.cs_provider_on_route_complete(
            latsPtr, lonsPtr, count,
            result.distance,
            result.duration
        );

        wasm.free(latsPtr);
        wasm.free(lonsPtr);
    }

    _allocString(str) {
        const bytes = new TextEncoder().encode(str + '\0');
        const ptr = this.wasm.malloc(bytes.length);
        new Uint8Array(this.wasm.memory.buffer).set(bytes, ptr);
        return ptr;
    }
}
