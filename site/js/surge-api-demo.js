/**
 * Surge API Demo - Browser WASM Interface
 *
 * Provides a fetch-like interface to the Surge VRP solver via WASM.
 * Demonstrates architecture parity between server and browser deployments.
 *
 * Usage:
 *   const surge = new SurgeDemo();
 *   await surge.init();
 *
 *   // Now use like fetch:
 *   const response = await surge.fetch('/api/v1/health');
 *   const data = await response.json();
 *
 *   // Or use convenience methods:
 *   const result = await surge.solve({ ... });
 */

class SurgeDemo {
    constructor() {
        this.module = null;
        this.initialized = false;
        this.initializing = null;
    }

    /**
     * Initialize the WASM module.
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
        if (typeof SurgeAPIDemo === 'undefined') {
            throw new Error('SurgeAPIDemo WASM module not loaded. Include wasm/surge-api-demo.js first.');
        }

        this.module = await SurgeAPIDemo();

        const result = this.module._surge_api_init();
        if (result !== 0) {
            throw new Error('Failed to initialize Surge API');
        }

        this.initialized = true;
        console.log('Surge API Demo initialized');
    }

    /**
     * Check if the API is ready.
     */
    isReady() {
        return this.initialized && this.module._surge_api_ready() === 1;
    }

    /**
     * Get Surge version string.
     */
    getVersion() {
        if (!this.module) return '0.0.0';
        const versionPtr = this.module._surge_api_version_string();
        return this.module.UTF8ToString(versionPtr);
    }

    /**
     * Fetch-like interface for API requests.
     *
     * Supported paths:
     *   POST /api/v1/solve    - Solve VRP problem
     *   GET  /api/v1/health   - Health check
     *   GET  /api/v1/version  - Version info
     *
     * @param {string} url - API path or full URL
     * @param {Object} options - Fetch-like options (method, body)
     * @returns {Promise<Response>} - Standard Response object
     */
    async fetch(url, options = {}) {
        if (!this.initialized) await this.init();

        const parsed = new URL(url, 'http://wasm.demo');
        const path = parsed.pathname;
        const query = parsed.search.slice(1) || null;

        let body = options.body || null;
        let bodyLen = 0;

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
            this.module._surge_api_handle(pathPtr, queryPtr, bodyPtr, bodyLen);

            const status = this.module._surge_response_status();
            const contentTypePtr = this.module._surge_response_content_type();
            const contentType = this.module.UTF8ToString(contentTypePtr);
            const respBodyPtr = this.module._surge_response_body();
            const respBodyLen = this.module._surge_response_body_len();

            let responseBody;
            if (respBodyPtr && respBodyLen > 0) {
                responseBody = new Uint8Array(respBodyLen);
                responseBody.set(this.module.HEAPU8.subarray(respBodyPtr, respBodyPtr + respBodyLen));
            } else {
                responseBody = new Uint8Array(0);
            }

            return new Response(responseBody, {
                status: status,
                headers: { 'Content-Type': contentType }
            });
        } finally {
            this.module._free(pathPtr);
            if (queryPtr) this.module._free(queryPtr);
            if (bodyPtr) this.module._free(bodyPtr);
        }
    }

    /**
     * Convenience method to solve a VRP problem.
     *
     * @param {Object} problem - VRP problem definition
     * @returns {Promise<Object>} - Solution with routes and stats
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
     * Free resources. Call when done with the API.
     */
    destroy() {
        if (this.module) {
            this.module._surge_api_free();
            this.module = null;
            this.initialized = false;
        }
    }
}

if (typeof module !== 'undefined' && module.exports) {
    module.exports = SurgeDemo;
}
if (typeof window !== 'undefined') {
    window.SurgeDemo = SurgeDemo;
}
