/**
 * Clay Renderer - Keyboard Handling
 *
 * Generic keyboard event routing for Clay UI components.
 * Handles Tab navigation, focus routing, and character input.
 */

/**
 * Sets up keyboard event handling for Clay components.
 *
 * @param {Object} wasm - WASM module with cc_* exports
 * @param {Object} options - Configuration options
 * @param {Function} [options.onGlobalShortcut] - Callback for app-specific shortcuts when unfocused
 * @returns {Function} Cleanup function to remove the event listener
 *
 * Required WASM exports:
 *   cc_key_down(keyCode, shift, ctrl) -> bool
 *   cc_key_char(charCode) -> bool
 *   cc_focused_id() -> uint32
 *
 * @example
 * const cleanup = setupKeyboardHandler(wasm, {
 *     onGlobalShortcut: (e) => {
 *         if (e.key === '+') wasm.map_scroll(1, 0, 0);
 *         if (e.key === '-') wasm.map_scroll(-1, 0, 0);
 *     }
 * });
 */
export function setupKeyboardHandler(wasm, options = {}) {
    const { onGlobalShortcut } = options;

    function handleKeyDown(e) {
        // Tab navigation works globally (even with no focus)
        if (e.key === 'Tab') {
            const beforeId = wasm.cc_focused_id();
            const beforeCursor = wasm.cc_cursor_pos();
            if (wasm.cc_key_down(e.keyCode, e.shiftKey ? 1 : 0, e.ctrlKey ? 1 : 0)) {
                e.preventDefault();
            }
            const afterId = wasm.cc_focused_id();
            const afterCursor = wasm.cc_cursor_pos();
            console.log(`Tab: focus ${beforeId}->${afterId}, cursor ${beforeCursor}->${afterCursor}`);
            return;
        }

        if (wasm.cc_focused_id() !== 0) {
            // Route to focused component
            if (wasm.cc_key_down(e.keyCode, e.shiftKey ? 1 : 0, e.ctrlKey ? 1 : 0)) {
                e.preventDefault();
                return;
            }

            // Character input for text fields
            if (e.key.length === 1 && !e.ctrlKey && !e.metaKey) {
                const code = e.key.charCodeAt(0);
                if (code >= 32 && code <= 126) {
                    wasm.cc_key_char(code);
                    e.preventDefault();
                }
            }
        } else {
            // Global shortcuts (app-specific)
            if (onGlobalShortcut) {
                onGlobalShortcut(e);
            }
        }
    }

    window.addEventListener('keydown', handleKeyDown);

    // Return cleanup function
    return () => window.removeEventListener('keydown', handleKeyDown);
}
