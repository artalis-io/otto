/**
 * WASM Loading Utility
 */

/**
 * Load a WASM module and validate required exports.
 *
 * @param {string} path - Path to the WASM file
 * @param {string[]} requiredExports - List of function names that must be exported
 * @returns {Promise<WebAssembly.Exports>} The WASM module exports
 */
export async function loadWasm(path, requiredExports = []) {
    const response = await fetch(path);
    if (!response.ok) {
        throw new Error(`Failed to load WASM: ${response.status} ${response.statusText}`);
    }
    const { instance } = await WebAssembly.instantiateStreaming(response, {});
    const wasm = instance.exports;

    for (const fn of requiredExports) {
        if (typeof wasm[fn] !== 'function') {
            throw new Error(`Missing WASM export: ${fn}`);
        }
    }

    return wasm;
}
