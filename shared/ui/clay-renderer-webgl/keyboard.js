/**
 * Clay Renderer - Keyboard Handling
 *
 * Generic keyboard event routing for Clay UI components.
 * Handles Tab navigation, focus routing, clipboard, and character input.
 */

/**
 * Get selected text from WASM text input
 */
function getSelectedText(wasm) {
    const cursor = wasm.cc_cursor_pos();
    const selStart = wasm.cc_selection_start();
    if (selStart < 0 || selStart === cursor) return null;

    const start = Math.min(cursor, selStart);
    const end = Math.max(cursor, selStart);

    const ptr = wasm.cc_focused_text();
    const len = wasm.cc_focused_text_len();
    if (!ptr || len <= 0) return null;

    const memory = new Uint8Array(wasm.memory.buffer);
    let text = '';
    for (let i = start; i < end && i < len; i++) {
        text += String.fromCharCode(memory[ptr + i]);
    }
    return text;
}

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
            if (wasm.cc_key_down(e.keyCode, e.shiftKey ? 1 : 0, e.ctrlKey ? 1 : 0)) {
                e.preventDefault();
            }
            return;
        }

        if (wasm.cc_focused_id() !== 0) {
            // Clipboard operations (Ctrl+C, Ctrl+V, Ctrl+X)
            if ((e.ctrlKey || e.metaKey) && (e.key === 'c' || e.key === 'v' || e.key === 'x')) {
                handleClipboard(e, wasm);
                return;
            }

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

    async function handleClipboard(e, wasm) {
        e.preventDefault();

        if (e.key === 'c' || e.key === 'x') {
            // Copy or Cut
            const selectedText = getSelectedText(wasm);
            if (selectedText) {
                try {
                    await navigator.clipboard.writeText(selectedText);
                } catch (err) {
                    console.warn('Clipboard write failed:', err);
                }

                // For cut, delete the selection
                if (e.key === 'x') {
                    wasm.cc_key_down(8, 0, 0); // Backspace deletes selection
                }
            }
        } else if (e.key === 'v') {
            // Paste
            try {
                const text = await navigator.clipboard.readText();
                for (const char of text) {
                    const code = char.charCodeAt(0);
                    if (code >= 32 && code <= 126) {
                        wasm.cc_key_char(code);
                    }
                }
            } catch (err) {
                console.warn('Clipboard read failed:', err);
            }
        }
    }

    window.addEventListener('keydown', handleKeyDown);

    // Return cleanup function
    return () => window.removeEventListener('keydown', handleKeyDown);
}
