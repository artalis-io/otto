/**
 * Surge Node.js binding — WASM wrapper around the Emscripten API demo build.
 *
 * Usage:
 *   import { loadSurge } from '@artalis/surge';
 *   const surge = await loadSurge();
 *   const result = surge.solve(problem);
 */

let cachedModule = null;

/**
 * Load and initialize the Surge WASM module.
 *
 * @param {string} [wasmPath] - Optional path to surge-api-demo.js.
 *   Defaults to ../build/surge-api-demo.js (relative to this file).
 * @returns {Promise<SurgeAPI>}
 */
export async function loadSurge(wasmPath) {
    if (cachedModule) {
        return createAPI(cachedModule);
    }

    try {
        const modulePath = wasmPath || new URL('../build/surge-api-demo.cjs', import.meta.url).pathname;
        const { default: SurgeAPIDemo } = await import(modulePath);
        cachedModule = await SurgeAPIDemo();
        cachedModule._surge_api_init();
        return createAPI(cachedModule);
    } catch (err) {
        throw new Error(`Failed to load Surge WASM module: ${err.message}`);
    }
}

/**
 * Create the high-level API wrapper around a loaded WASM module.
 * @param {object} module - Emscripten module instance
 * @returns {SurgeAPI}
 */
function createAPI(module) {
    return {
        /**
         * Solve a vehicle routing problem.
         *
         * @param {object} problem - VRP problem definition
         * @returns {object} Solution with status, stats, routes, unassigned
         * @throws {Error} On validation or solver errors
         */
        solve(problem) {
            const jsonStr = JSON.stringify(problem);
            const inputPtr = module.allocateUTF8(jsonStr);

            try {
                module._surge_api_solve(inputPtr, jsonStr.length);

                const status = module._surge_response_status();
                const bodyPtr = module._surge_response_body();
                const bodyLen = module._surge_response_body_len();
                const body = module.UTF8ToString(bodyPtr, bodyLen);
                const result = JSON.parse(body);

                if (status === 400) {
                    throw new Error(result.error || 'Validation error');
                }
                if (status >= 500) {
                    throw new Error(result.error || 'Solver error');
                }

                return result;
            } finally {
                module._free(inputPtr);
            }
        },

        /**
         * Get the Surge solver version string.
         * @returns {string}
         */
        version() {
            const ptr = module._surge_api_version_string();
            return module.UTF8ToString(ptr);
        },

        /**
         * Get the Surge solver health status.
         * @returns {object} Health status object
         */
        health() {
            module._surge_api_handle(
                module.allocateUTF8('/api/v1/health'),
                0,  // no query
                0,  // no body
                0   // no body length
            );
            const bodyPtr = module._surge_response_body();
            const body = module.UTF8ToString(bodyPtr);
            return JSON.parse(body);
        },
    };
}

export default loadSurge;
