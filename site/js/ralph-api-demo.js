/**
 * Ralph API Demo - Browser WASM Interface
 *
 * Provides a fetch-like interface to the Ralph LP/MIP solver via WASM.
 * Demonstrates architecture parity between server and browser deployments.
 *
 * Usage:
 *   const ralph = new RalphDemo();
 *   await ralph.init();
 *
 *   // Use like fetch:
 *   const response = await ralph.fetch('/api/v1/health');
 *   const data = await response.json();
 *
 *   // Or use convenience methods:
 *   const result = await ralph.solve('max: 5x + 3y\n...');
 */

class RalphDemo {
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
        // Load WASM module (RalphAPIDemo is the Emscripten factory function)
        // Built with SINGLE_FILE=1, so WASM is embedded as base64 - works offline
        if (typeof RalphAPIDemo === 'undefined') {
            throw new Error('RalphAPIDemo WASM module not loaded. Include wasm/ralph-api-demo.js first.');
        }

        this.module = await RalphAPIDemo();

        // Initialize API
        const result = this.module._ralph_api_init();
        if (result !== 0) {
            throw new Error('Failed to initialize Ralph API');
        }

        this.initialized = true;
        console.log('Ralph API Demo initialized');
    }

    /**
     * Check if the API is ready.
     */
    isReady() {
        return this.initialized && this.module._ralph_wasm_api_ready() === 1;
    }

    /**
     * Get Ralph version string.
     */
    getVersion() {
        if (!this.module) return '0.0.0';
        const versionPtr = this.module._ralph_wasm_api_version();
        return this.module.UTF8ToString(versionPtr);
    }

    /**
     * Fetch-like interface for API requests.
     *
     * Supported paths:
     *   POST /api/v1/solve    - Solve LP/MIP problem
     *   GET  /api/v1/formats  - List supported formats
     *   GET  /api/v1/health   - Health check
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

        // Determine method
        const method = options.method || 'GET';

        // Get body for POST requests
        let body = options.body || null;
        let bodyLen = 0;

        // Allocate strings in WASM memory
        const methodPtr = this.module.allocateUTF8(method);
        const pathPtr = this.module.allocateUTF8(path);
        const queryPtr = query ? this.module.allocateUTF8(query) : 0;
        let bodyPtr = 0;

        if (body) {
            if (typeof body === 'object') {
                body = JSON.stringify(body);
            }
            bodyLen = this.module.lengthBytesUTF8(body);
            bodyPtr = this.module.allocateUTF8(body);
        }

        try {
            // Call WASM handler
            const respPtr = this.module._ralph_wasm_api_handle(methodPtr, pathPtr, queryPtr, bodyPtr, bodyLen);

            if (!respPtr) {
                return new Response('Internal error', {
                    status: 500,
                    headers: { 'Content-Type': 'text/plain' }
                });
            }

            // Read response fields
            const status = this.module._ralph_wasm_response_status(respPtr);
            const contentTypePtr = this.module._ralph_wasm_response_content_type(respPtr);
            const contentType = this.module.UTF8ToString(contentTypePtr);
            const respBodyPtr = this.module._ralph_wasm_response_body(respPtr);
            const respBodyLen = this.module._ralph_wasm_response_body_len(respPtr);

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
            this.module._free(methodPtr);
            this.module._free(pathPtr);
            if (queryPtr) this.module._free(queryPtr);
            if (bodyPtr) this.module._free(bodyPtr);
        }
    }

    /**
     * Convenience method to solve an LP/MIP problem.
     *
     * @param {string} problem - Problem in LP format
     * @param {Object} options - Optional settings
     * @param {string} options.format - Format: 'lp' (default) or 'mps'
     * @param {number} options.timeout_ms - Timeout in ms (default: 5000, max: 30000)
     * @returns {Promise<Object>} - Solution with status, objective, variables
     */
    async solve(problem, options = {}) {
        const format = options.format || 'lp';
        const timeout_ms = options.timeout_ms || 5000;

        const response = await this.fetch('/api/v1/solve', {
            method: 'POST',
            body: {
                format: format,
                problem: problem,
                timeout_ms: timeout_ms
            }
        });

        const data = await response.json();
        if (!response.ok) {
            throw new Error(data.error || `Solve failed (${response.status})`);
        }
        return data;
    }

    /**
     * Convenience method to get supported formats.
     *
     * @returns {Promise<Object>} - List of supported formats
     */
    async getFormats() {
        const response = await this.fetch('/api/v1/formats');
        if (!response.ok) {
            throw new Error(`Formats request failed: ${response.status}`);
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
     * Free resources. Call when done with the API.
     */
    destroy() {
        if (this.module) {
            this.module._ralph_wasm_api_free();
            this.module = null;
            this.initialized = false;
        }
    }
}

// Export for both browser and module environments
if (typeof module !== 'undefined' && module.exports) {
    module.exports = RalphDemo;
}
if (typeof window !== 'undefined') {
    window.RalphDemo = RalphDemo;
}
