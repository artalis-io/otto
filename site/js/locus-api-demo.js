/**
 * Locus API Demo - Browser WASM Interface
 *
 * Provides a fetch-like interface to the Locus geocoding engine via WASM.
 * Demonstrates architecture parity between server and browser deployments.
 *
 * The Monaco geocoding index is embedded directly in the WASM module.
 *
 * Usage:
 *   const locus = new LocusDemo();
 *   await locus.init();
 *
 *   // Search for a place:
 *   const results = await locus.search('Monte Carlo');
 *
 *   // Autocomplete:
 *   const suggestions = await locus.autocomplete('Mon');
 *
 *   // Reverse geocode:
 *   const location = await locus.reverse(43.7384, 7.4246);
 */

class LocusDemo {
    constructor() {
        this.module = null;
        this.initialized = false;
        this.initializing = null;
    }

    /**
     * Initialize the WASM module and load embedded Monaco index.
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
        if (typeof LocusAPIDemo === 'undefined') {
            throw new Error('LocusAPIDemo WASM module not loaded. Include wasm/locus-api-demo.js first.');
        }

        this.module = await LocusAPIDemo();

        const result = this.module._locus_api_init();
        if (result !== 0) {
            throw new Error('Failed to initialize Locus API');
        }

        this.initialized = true;
        console.log(`Locus API Demo initialized (Index: ${(this.getIndexSize() / 1024).toFixed(0)} KB, ${this.getEntityCount()} entities)`);
    }

    /**
     * Check if the API is ready.
     */
    isReady() {
        return this.initialized && this.module._locus_api_ready() === 1;
    }

    /**
     * Get size of embedded index data.
     */
    getIndexSize() {
        return this.module ? this.module._locus_api_index_size() : 0;
    }

    /**
     * Get entity count.
     */
    getEntityCount() {
        return this.module ? this.module._locus_api_entity_count() : 0;
    }

    /**
     * Get Locus version string.
     */
    getVersion() {
        if (!this.module) return '0.0.0';
        const versionPtr = this.module._locus_api_version();
        return this.module.UTF8ToString(versionPtr);
    }

    /**
     * Fetch-like interface for API requests.
     *
     * Supported paths:
     *   /api/v1/search      - Forward geocoding (requires q param)
     *   /api/v1/autocomplete - Autocomplete suggestions (requires q param)
     *   /api/v1/reverse     - Reverse geocoding (requires lat, lon params)
     *   /api/v1/health      - Health check
     *   /api/v1/stats       - Statistics
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
            this.module._locus_api_handle(pathPtr, queryPtr);

            const status = this.module._locus_response_status();
            const contentTypePtr = this.module._locus_response_content_type();
            const contentType = this.module.UTF8ToString(contentTypePtr);
            const bodyPtr = this.module._locus_response_body();
            const bodyLen = this.module._locus_response_body_len();

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
     * Search for places by name.
     *
     * @param {string} query - Search query
     * @param {Object} options - Optional {limit: number}
     * @returns {Promise<Object>} - Search results with matches
     */
    async search(query, options = {}) {
        const params = new URLSearchParams({ q: query });
        if (options.limit) params.set('limit', options.limit.toString());

        const response = await this.fetch(`/api/v1/search?${params}`);
        const data = await response.json();

        if (!response.ok) {
            throw new Error(data.error || `Search failed: ${response.status}`);
        }

        return data;
    }

    /**
     * Get autocomplete suggestions.
     *
     * @param {string} prefix - Search prefix
     * @param {Object} options - Optional {limit: number}
     * @returns {Promise<string[]>} - Array of suggested names
     */
    async autocomplete(prefix, options = {}) {
        const params = new URLSearchParams({ q: prefix });
        if (options.limit) params.set('limit', options.limit.toString());

        const response = await this.fetch(`/api/v1/autocomplete?${params}`);
        const data = await response.json();

        if (!response.ok) {
            throw new Error(data.error || `Autocomplete failed: ${response.status}`);
        }

        return data;
    }

    /**
     * Reverse geocode a coordinate.
     *
     * @param {number} lat - Latitude
     * @param {number} lon - Longitude
     * @returns {Promise<Object>} - Location info with display_name, distance_m
     */
    async reverse(lat, lon) {
        const params = new URLSearchParams({
            lat: lat.toString(),
            lon: lon.toString()
        });

        const response = await this.fetch(`/api/v1/reverse?${params}`);
        const data = await response.json();

        if (!response.ok) {
            throw new Error(data.error || `Reverse failed: ${response.status}`);
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
     * Get index statistics.
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
            this.module._locus_api_free();
            this.module = null;
            this.initialized = false;
        }
    }
}

// Export for both browser and module environments
if (typeof module !== 'undefined' && module.exports) {
    module.exports = LocusDemo;
}
if (typeof window !== 'undefined') {
    window.LocusDemo = LocusDemo;
}
