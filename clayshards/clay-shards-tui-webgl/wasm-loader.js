/**
 * ClayShards TUI WebGL - WASM Loader
 *
 * Loads the TUI WASM module and provides input handling utilities.
 */

/**
 * Load the TUI WASM module.
 *
 * @param {string} path - Path to the WASM file
 * @returns {Promise<WebAssembly.Exports>} WASM module exports
 */
export async function loadTuiWasm(path) {
    const response = await fetch(path);
    if (!response.ok) {
        throw new Error(`Failed to load TUI WASM: ${response.status} ${response.statusText}`);
    }

    // Emscripten standalone WASM imports
    const imports = {
        env: {
            emscripten_notify_memory_growth: () => {}
        }
    };

    const { instance } = await WebAssembly.instantiateStreaming(response, imports);
    const wasm = instance.exports;

    // Validate required exports
    const required = [
        'wasm_tui_init',
        'wasm_tui_get_buffer',
        'wasm_tui_get_width',
        'wasm_tui_get_height',
        'wasm_tui_frame_begin',
        'wasm_tui_frame_end',
        'memory'
    ];

    for (const fn of required) {
        if (!wasm[fn]) {
            throw new Error(`Missing TUI WASM export: ${fn}`);
        }
    }

    return wasm;
}

/**
 * Setup keyboard event handlers.
 *
 * @param {HTMLElement} target - Element to attach listeners to
 * @param {WebAssembly.Exports} wasm - WASM module exports
 */
export function setupKeyboardHandlers(target, wasm) {
    target.addEventListener('keydown', (e) => {
        // Prevent default for special keys
        if (e.key === 'Tab' || e.key === 'Escape' || e.key === 'Backspace') {
            e.preventDefault();
        }

        const mods = (e.ctrlKey ? 1 : 0) | (e.shiftKey ? 2 : 0) | (e.altKey ? 4 : 0);
        wasm.wasm_tui_key_event(e.keyCode, mods, 1);
    });

    target.addEventListener('keyup', (e) => {
        const mods = (e.ctrlKey ? 1 : 0) | (e.shiftKey ? 2 : 0) | (e.altKey ? 4 : 0);
        wasm.wasm_tui_key_event(e.keyCode, mods, 0);
    });

    // Character input for printable keys
    target.addEventListener('keypress', (e) => {
        if (e.charCode > 0 && !e.ctrlKey && !e.altKey) {
            wasm.wasm_tui_char_event(e.charCode);
        }
    });
}

/**
 * Setup mouse event handlers.
 *
 * @param {HTMLCanvasElement} canvas - Canvas element
 * @param {WebAssembly.Exports} wasm - WASM module exports
 * @param {Object} options - Configuration
 * @param {number} options.cellWidth - Cell width in pixels
 * @param {number} options.cellHeight - Cell height in pixels
 */
export function setupMouseHandlers(canvas, wasm, options) {
    const cellWidth = options.cellWidth || 8;
    const cellHeight = options.cellHeight || 16;

    function getCellPos(e) {
        const rect = canvas.getBoundingClientRect();
        const scaleX = canvas.width / rect.width;
        const scaleY = canvas.height / rect.height;
        const x = Math.floor((e.clientX - rect.left) * scaleX / cellWidth);
        const y = Math.floor((e.clientY - rect.top) * scaleY / cellHeight);
        return { x, y };
    }

    canvas.addEventListener('mousemove', (e) => {
        const { x, y } = getCellPos(e);
        wasm.wasm_tui_mouse_move(x, y);
    });

    canvas.addEventListener('mousedown', (e) => {
        const { x, y } = getCellPos(e);
        wasm.wasm_tui_mouse_event(x, y, e.button, 1);
    });

    canvas.addEventListener('mouseup', (e) => {
        const { x, y } = getCellPos(e);
        wasm.wasm_tui_mouse_event(x, y, e.button, 0);
    });

    canvas.addEventListener('wheel', (e) => {
        e.preventDefault();
        wasm.wasm_tui_scroll(e.deltaY);
    }, { passive: false });

    // Prevent context menu on right-click
    canvas.addEventListener('contextmenu', (e) => {
        e.preventDefault();
    });
}

/**
 * Initialize TUI WASM module with given dimensions.
 *
 * @param {WebAssembly.Exports} wasm - WASM module exports
 * @param {number} cols - Number of columns
 * @param {number} rows - Number of rows
 * @returns {boolean} True if initialization succeeded
 */
export function initTuiWasm(wasm, cols, rows) {
    return wasm.wasm_tui_init(cols, rows) !== 0;
}

/**
 * Resize the TUI WASM module.
 *
 * @param {WebAssembly.Exports} wasm - WASM module exports
 * @param {number} cols - Number of columns
 * @param {number} rows - Number of rows
 */
export function resizeTuiWasm(wasm, cols, rows) {
    wasm.wasm_tui_resize(cols, rows);
}
