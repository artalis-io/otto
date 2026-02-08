/**
 * FuelWise API Demo - Browser WASM Interface
 *
 * Provides a fetch-like interface to the FuelWise optimizer via WASM.
 * Demonstrates architecture parity between server and browser deployments.
 *
 * Usage:
 *   const fuelwise = new FuelWiseDemo();
 *   await fuelwise.init();
 *
 *   // Now use like fetch:
 *   const response = await fuelwise.fetch('/api/v1/health');
 *   const data = await response.json();
 *
 *   // Or use convenience methods:
 *   const result = await fuelwise.solve({ ... });
 */

class FuelWiseDemo {
    constructor() {
        this.module = null;
        this.initialized = false;
        this.initializing = null;  // Promise for concurrent init calls
    }

    /**
     * Initialize the WASM module.
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
        // Load WASM module (FuelWiseAPIDemo is the Emscripten factory function)
        // Built with SINGLE_FILE=1, so WASM is embedded as base64 - works offline
        if (typeof FuelWiseAPIDemo === 'undefined') {
            throw new Error('FuelWiseAPIDemo WASM module not loaded. Include wasm/fuelwise-api-demo.js first.');
        }

        this.module = await FuelWiseAPIDemo();

        // Initialize API (no-op for FuelWise since it's stateless)
        const result = this.module._fuelwise_api_init();
        if (result !== 0) {
            throw new Error('Failed to initialize FuelWise API');
        }

        this.initialized = true;
        console.log('FuelWise API Demo initialized');
    }

    /**
     * Check if the API is ready.
     */
    isReady() {
        return this.initialized && this.module._fuelwise_api_ready() === 1;
    }

    /**
     * Get FuelWise version string.
     */
    getVersion() {
        if (!this.module) return '0.0.0';
        const versionPtr = this.module._fuelwise_api_version();
        return this.module.UTF8ToString(versionPtr);
    }

    /**
     * Fetch-like interface for API requests.
     *
     * Supported paths:
     *   POST /api/v1/solve    - Solve refueling problem
     *   POST /api/v1/filter   - Filter stations to route
     *   POST /api/v1/optimize - Full optimization pipeline
     *   GET  /api/v1/health   - Health check
     *   GET  /api/v1/stats    - Statistics
     *
     * @param {string} url - API path or full URL
     * @param {Object} options - Fetch-like options (method, body)
     * @returns {Promise<Response>} - Standard Response object
     */
    async fetch(url, options = {}) {
        if (!this.initialized) await this.init();

        // Parse URL to get path and query
        const parsed = new URL(url, 'http://wasm.demo');
        const path = parsed.pathname;
        const query = parsed.search.slice(1) || null;

        // Get body for POST requests
        let body = options.body || null;
        let bodyLen = 0;

        // Allocate strings in WASM memory
        const pathPtr = this.module.allocateUTF8(path);
        const queryPtr = query ? this.module.allocateUTF8(query) : 0;
        let bodyPtr = 0;

        if (body) {
            if (typeof body === 'object') {
                body = JSON.stringify(body);
            }
            bodyLen = body.length;
            bodyPtr = this.module.allocateUTF8(body);
        }

        try {
            // Call WASM handler
            const respPtr = this.module._fuelwise_api_handle(pathPtr, queryPtr, bodyPtr, bodyLen);

            if (!respPtr) {
                return new Response('Internal error', {
                    status: 500,
                    headers: { 'Content-Type': 'text/plain' }
                });
            }

            // Read response fields
            const status = this.module._fuelwise_response_status(respPtr);
            const contentTypePtr = this.module._fuelwise_response_content_type(respPtr);
            const contentType = this.module.UTF8ToString(contentTypePtr);
            const respBodyPtr = this.module._fuelwise_response_body(respPtr);
            const respBodyLen = this.module._fuelwise_response_body_len(respPtr);

            // Copy body to JavaScript
            let responseBody;
            if (respBodyPtr && respBodyLen > 0) {
                responseBody = new Uint8Array(respBodyLen);
                responseBody.set(this.module.HEAPU8.subarray(respBodyPtr, respBodyPtr + respBodyLen));
            } else {
                responseBody = new Uint8Array(0);
            }

            // Return fetch-like Response
            return new Response(responseBody, {
                status: status,
                headers: { 'Content-Type': contentType }
            });
        } finally {
            // Free allocated strings
            this.module._free(pathPtr);
            if (queryPtr) this.module._free(queryPtr);
            if (bodyPtr) this.module._free(bodyPtr);
        }
    }

    /**
     * Convenience method to solve a refueling problem.
     *
     * @param {Object} problem - Refueling problem
     * @param {number} problem.total_distance - Total route distance in miles
     * @param {number} problem.tank_capacity - Tank capacity in gallons
     * @param {number} problem.current_fuel - Current fuel level in gallons
     * @param {number} problem.consumption_mpg - Fuel consumption in miles per gallon
     * @param {number} problem.minimum_fuel - Minimum fuel level to maintain
     * @param {Array} problem.stations - Array of {id, distance, price}
     * @returns {Promise<Object>} - Solution with stops and costs
     */
    async solve(problem) {
        const response = await this.fetch('/api/v1/solve', {
            method: 'POST',
            body: problem
        });
        if (!response.ok) {
            const text = await response.text();
            throw new Error(`Solve failed (${response.status}): ${text}`);
        }
        return response.json();
    }

    /**
     * Convenience method to filter stations to a route.
     *
     * @param {Object} request - Filter request
     * @param {Array} request.stations - Array of {lat, lon, price}
     * @param {Array} request.route - Route polyline [[lat, lon], ...]
     * @param {number} request.max_distance - Maximum perpendicular distance in miles
     * @returns {Promise<Object>} - Filtered stations
     */
    async filter(request) {
        const response = await this.fetch('/api/v1/filter', {
            method: 'POST',
            body: request
        });
        if (!response.ok) {
            const text = await response.text();
            throw new Error(`Filter failed (${response.status}): ${text}`);
        }
        return response.json();
    }

    /**
     * Convenience method for full optimization (filter + solve).
     *
     * @param {Object} request - Optimization request
     * @returns {Promise<Object>} - Complete optimization result
     */
    async optimize(request) {
        const response = await this.fetch('/api/v1/optimize', {
            method: 'POST',
            body: request
        });
        if (!response.ok) {
            const text = await response.text();
            throw new Error(`Optimize failed (${response.status}): ${text}`);
        }
        return response.json();
    }

    /**
     * Convenience method to get API health status.
     *
     * @returns {Promise<Object>} - Health status
     */
    async getHealth() {
        const response = await this.fetch('/api/v1/health');
        if (!response.ok) {
            throw new Error(`Health check failed: ${response.status}`);
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
            this.module._fuelwise_api_free();
            this.module = null;
            this.initialized = false;
        }
    }
}

// Export for both browser and module environments
if (typeof module !== 'undefined' && module.exports) {
    module.exports = FuelWiseDemo;
}
if (typeof window !== 'undefined') {
    window.FuelWiseDemo = FuelWiseDemo;
}
