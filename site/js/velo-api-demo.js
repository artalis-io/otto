/**
 * Velo API Demo - Browser WASM Interface
 *
 * Provides a fetch-like interface to the Velo routing engine via WASM.
 * Demonstrates architecture parity between server and browser deployments.
 *
 * The Monaco routing graph is embedded directly in the WASM module.
 *
 * Usage:
 *   const velo = new VeloDemo();
 *   await velo.init();
 *
 *   // Calculate a route:
 *   const route = await velo.route(
 *     {lat: 43.7384, lon: 7.4246},  // from (Monaco Casino)
 *     {lat: 43.7311, lon: 7.4197}   // to (Port)
 *   );
 */

class VeloDemo {
    constructor() {
        this.module = null;
        this.initialized = false;
        this.initializing = null;
    }

    /**
     * Initialize the WASM module and load embedded Monaco graph.
     * Safe to call multiple times - will only initialize once.
     */
    async init() {
        if (this.initialized) return;

        if (this.initializing) {
            return this.initializing;
        }

        this.initializing = this._doInit();
        await this.initializing;
        this.initializing = null;
    }

    async _doInit() {
        if (typeof VeloAPIDemo === 'undefined') {
            throw new Error('VeloAPIDemo WASM module not loaded. Include wasm/velo-api-demo.js first.');
        }

        this.module = await VeloAPIDemo();

        const result = this.module._velo_api_init();
        if (result !== 0) {
            throw new Error('Failed to initialize Velo API');
        }

        this.initialized = true;
        console.log(`Velo API Demo initialized (Graph: ${(this.getGraphSize() / 1024).toFixed(0)} KB, ${this.getNodeCount()} nodes)`);
    }

    /**
     * Check if the API is ready.
     */
    isReady() {
        return this.initialized && this.module._velo_api_ready() === 1;
    }

    /**
     * Get size of embedded graph data.
     */
    getGraphSize() {
        return this.module ? this.module._velo_api_graph_size() : 0;
    }

    /**
     * Get node count.
     */
    getNodeCount() {
        return this.module ? this.module._velo_api_node_count() : 0;
    }

    /**
     * Get edge count.
     */
    getEdgeCount() {
        return this.module ? this.module._velo_api_edge_count() : 0;
    }

    /**
     * Get Velo version string.
     */
    getVersion() {
        if (!this.module) return '0.0.0';
        const versionPtr = this.module._velo_api_version();
        return this.module.UTF8ToString(versionPtr);
    }

    /**
     * Fetch-like interface for API requests.
     *
     * Supported paths:
     *   /api/v1/route   - Calculate route (requires query params)
     *   /api/v1/health  - Health check
     *   /api/v1/stats   - Statistics
     *
     * @param {string} url - API path or full URL
     * @returns {Promise<Response>} - Standard Response object
     */
    async fetch(url) {
        if (!this.initialized) await this.init();

        const parsed = new URL(url, 'http://wasm.demo');
        const path = parsed.pathname;
        const query = parsed.search.slice(1) || null;

        const pathPtr = this.module.allocateUTF8(path);
        const queryPtr = query ? this.module.allocateUTF8(query) : 0;

        try {
            this.module._velo_api_handle(pathPtr, queryPtr);

            const status = this.module._velo_response_status();
            const contentTypePtr = this.module._velo_response_content_type();
            const contentType = this.module.UTF8ToString(contentTypePtr);
            const bodyPtr = this.module._velo_response_body();
            const bodyLen = this.module._velo_response_body_len();

            let body;
            if (bodyPtr && bodyLen > 0) {
                body = this.module.UTF8ToString(bodyPtr);
            } else {
                body = '';
            }

            return new Response(body, {
                status: status,
                headers: { 'Content-Type': contentType }
            });
        } finally {
            this.module._free(pathPtr);
            if (queryPtr) this.module._free(queryPtr);
        }
    }

    /**
     * Calculate a route between two coordinates.
     *
     * @param {Object} from - Origin {lat, lon}
     * @param {Object} to - Destination {lat, lon}
     * @param {Object} options - Optional {profile: 'car'|'truck'|'bike'|'foot', mode: 'fastest'|'shortest'}
     * @returns {Promise<Object>} - Route object with distance, duration, geometry
     */
    async route(from, to, options = {}) {
        const params = new URLSearchParams({
            from_lat: from.lat.toString(),
            from_lon: from.lon.toString(),
            to_lat: to.lat.toString(),
            to_lon: to.lon.toString()
        });

        if (options.profile) params.set('profile', options.profile);
        if (options.mode) params.set('mode', options.mode);
        if (options.geometry !== undefined) params.set('geometry', options.geometry.toString());

        const response = await this.fetch(`/api/v1/route?${params}`);
        const data = await response.json();

        if (!response.ok) {
            throw new Error(data.error || `Route failed: ${response.status}`);
        }

        return data;
    }

    /**
     * Get health status.
     */
    async getHealth() {
        const response = await this.fetch('/api/v1/health');
        return response.json();
    }

    /**
     * Get graph statistics.
     */
    async getStats() {
        const response = await this.fetch('/api/v1/stats');
        return response.json();
    }

    /**
     * Free resources.
     */
    destroy() {
        if (this.module) {
            this.module._velo_api_free();
            this.module = null;
            this.initialized = false;
        }
    }
}

// Export for both browser and module environments
if (typeof module !== 'undefined' && module.exports) {
    module.exports = VeloDemo;
}
if (typeof window !== 'undefined') {
    window.VeloDemo = VeloDemo;
}
