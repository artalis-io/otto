/**
 * Carta API Demo - Browser WASM Interface
 *
 * Provides a fetch-like interface to the Carta tile generator via WASM.
 * Demonstrates architecture parity between server and browser deployments.
 *
 * The Monaco map data is embedded directly in the WASM module.
 *
 * Usage:
 *   const carta = new CartaDemo();
 *   await carta.init();
 *
 *   // Now use like fetch:
 *   const response = await carta.fetch('/tiles/14/8529/5974.png');
 *   const blob = await response.blob();
 */

class CartaDemo {
    constructor() {
        this.module = null;
        this.initialized = false;
        this.initializing = null;  // Promise for concurrent init calls
    }

    /**
     * Initialize the WASM module and load embedded Monaco data.
     * Safe to call multiple times - will only initialize once.
     */
    async init() {
        if (this.initialized) return;

        // Handle concurrent init calls
        if (this.initializing) {
            return this.initializing;
        }

        this.initializing = this._doInit();
        await this.initializing;
        this.initializing = null;
    }

    async _doInit() {
        // Load WASM module (CartaAPIDemo is the Emscripten factory function)
        // Built with SINGLE_FILE=1, so WASM is embedded as base64 - works offline
        if (typeof CartaAPIDemo === 'undefined') {
            throw new Error('CartaAPIDemo WASM module not loaded. Include wasm/carta-api-demo.js first.');
        }

        this.module = await CartaAPIDemo();

        // Initialize API context with embedded Monaco PBF
        const result = this.module._carta_api_init();
        if (result !== 0) {
            throw new Error('Failed to initialize Carta API');
        }

        this.initialized = true;
        console.log(`Carta API Demo initialized (Monaco PBF: ${this.getPBFSize()} bytes)`);
    }

    /**
     * Check if the API is ready.
     */
    isReady() {
        return this.initialized && this.module._carta_api_ready() === 1;
    }

    /**
     * Get size of embedded PBF data.
     */
    getPBFSize() {
        return this.module ? this.module._carta_api_pbf_size() : 0;
    }

    /**
     * Get Carta version string.
     */
    getVersion() {
        if (!this.module) return '0.0.0';
        const versionPtr = this.module._carta_api_version();
        return this.module.UTF8ToString(versionPtr);
    }

    /**
     * Fetch-like interface for API requests.
     *
     * Supported paths:
     *   /tiles/{z}/{x}/{y}.png  - PNG tile
     *   /tiles/{z}/{x}/{y}.mvt  - MVT tile
     *   /tiles.json             - TileJSON metadata
     *   /api/v1/health          - Health check
     *   /api/v1/stats           - Statistics
     *
     * @param {string} url - API path or full URL
     * @returns {Promise<Response>} - Standard Response object
     */
    async fetch(url) {
        if (!this.initialized) await this.init();

        // Parse URL to get path and query
        const parsed = new URL(url, 'http://wasm.demo');
        const path = parsed.pathname;
        const query = parsed.search.slice(1) || null;

        // Allocate strings in WASM memory
        const pathPtr = this.module.allocateUTF8(path);
        const queryPtr = query ? this.module.allocateUTF8(query) : 0;

        try {
            // Call WASM handler
            const respPtr = this.module._carta_api_handle(pathPtr, queryPtr);

            if (!respPtr) {
                return new Response('Internal error', {
                    status: 500,
                    headers: { 'Content-Type': 'text/plain' }
                });
            }

            // Read response fields
            const status = this.module._carta_response_status(respPtr);
            const contentTypePtr = this.module._carta_response_content_type(respPtr);
            const contentType = this.module.UTF8ToString(contentTypePtr);
            const bodyPtr = this.module._carta_response_body(respPtr);
            const bodyLen = this.module._carta_response_body_len(respPtr);

            // Copy body to JavaScript
            let body;
            if (bodyPtr && bodyLen > 0) {
                body = new Uint8Array(bodyLen);
                body.set(this.module.HEAPU8.subarray(bodyPtr, bodyPtr + bodyLen));
            } else {
                body = new Uint8Array(0);
            }

            // Return fetch-like Response
            return new Response(body, {
                status: status,
                headers: { 'Content-Type': contentType }
            });
        } finally {
            // Free allocated strings
            this.module._free(pathPtr);
            if (queryPtr) this.module._free(queryPtr);
        }
    }

    /**
     * Convenience method to get a PNG tile as a data URL.
     *
     * @param {number} z - Zoom level
     * @param {number} x - Tile X coordinate
     * @param {number} y - Tile Y coordinate
     * @returns {Promise<string>} - Data URL for the tile image
     */
    async getTileDataURL(z, x, y) {
        const response = await this.fetch(`/tiles/${z}/${x}/${y}.png`);
        if (!response.ok) {
            throw new Error(`Tile generation failed: ${response.status}`);
        }
        const blob = await response.blob();
        return URL.createObjectURL(blob);
    }

    /**
     * Convenience method to get TileJSON metadata.
     *
     * @returns {Promise<Object>} - TileJSON object
     */
    async getTileJSON() {
        const response = await this.fetch('/tiles.json');
        if (!response.ok) {
            throw new Error(`TileJSON failed: ${response.status}`);
        }
        return response.json();
    }

    /**
     * Convenience method to get API stats.
     *
     * @returns {Promise<Object>} - Stats object
     */
    async getStats() {
        const response = await this.fetch('/api/v1/stats');
        if (!response.ok) {
            throw new Error(`Stats failed: ${response.status}`);
        }
        return response.json();
    }

    /**
     * Free resources. Call when done with the API.
     */
    destroy() {
        if (this.module) {
            this.module._carta_api_free();
            this.module = null;
            this.initialized = false;
        }
    }
}

// Export for both browser and module environments
if (typeof module !== 'undefined' && module.exports) {
    module.exports = CartaDemo;
}
if (typeof window !== 'undefined') {
    window.CartaDemo = CartaDemo;
}
