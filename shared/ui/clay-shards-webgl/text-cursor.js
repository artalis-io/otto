/**
 * text-cursor.js - Text Input Cursor and Selection Rendering
 *
 * Generic text cursor rendering for Clay immediate mode text inputs.
 * Works with any WASM module that exports cs_* functions.
 */

/* Constants - should match CC_MONOSPACE_WIDTH_RATIO in cs_common.h */
const MONOSPACE_WIDTH_RATIO = 0.6;

/**
 * Renders text input cursor and selection
 *
 * @param {Object} renderer - ClayRenderer instance
 * @param {Object} wasm - WASM exports with cs_* functions
 * @param {Object} font - MSDFFont instance (optional, for accurate positioning)
 * @param {Float32Array} projMatrix - Projection matrix
 * @param {Object} inputBounds - { x, y, w, h } of the focused input
 * @param {Object} options - { fontSize, padding, cursorColor, selectionColor }
 */
export function renderTextCursor(renderer, wasm, font, projMatrix, inputBounds, options = {}) {
    const focusedId = wasm.cs_focused_id();
    if (focusedId === 0) return;

    if (!inputBounds) return;

    const {
        fontSize = 12,
        padding = 8,
        cursorColor = [0.2, 0.6, 1.0, 1.0],
        selectionColor = [0.23, 0.51, 0.96, 0.3],
        cursorWidth = 2,
    } = options;

    const { x, y, w, h } = inputBounds;
    const cursor = wasm.cs_cursor_pos();
    const selStart = wasm.cs_selection_start();

    // Get focused text from WASM
    const focusedText = getWasmString(wasm, wasm.cs_focused_text(), wasm.cs_focused_text_len());

    // Calculate cursor X position
    const cursorX = calculateTextOffset(focusedText, cursor, fontSize, padding, x, font);

    const cursorY = y + 4;
    const cursorH = h - 8;

    // Render selection if exists (always visible, doesn't blink)
    if (selStart >= 0 && selStart !== cursor) {
        const start = Math.min(selStart, cursor);
        const end = Math.max(selStart, cursor);

        const selStartX = calculateTextOffset(focusedText, start, fontSize, padding, x, font);
        const selEndX = calculateTextOffset(focusedText, end, fontSize, padding, x, font);

        renderer.renderRect(
            selStartX, cursorY,
            selEndX - selStartX, cursorH,
            selectionColor, 0, 0, null, projMatrix
        );
    }

    // Render cursor (blinks)
    const cursorVisible = wasm.cs_cursor_visible();
    if (cursorVisible) {
        renderer.renderRect(
            cursorX, cursorY,
            cursorWidth, cursorH,
            cursorColor, 0, 0, null, projMatrix
        );
    }
}

/**
 * Extract string from WASM memory
 */
function getWasmString(wasm, ptr, len) {
    if (!ptr || len <= 0) return '';

    const memory = new Uint8Array(wasm.memory.buffer);

    // Bounds check: ensure we don't read past WASM memory
    if (ptr + len > memory.length) return '';

    // Decode UTF-8 text from WASM memory
    const bytes = new Uint8Array(wasm.memory.buffer, ptr, len);
    return new TextDecoder('utf-8').decode(bytes);
}

/**
 * Calculate X offset for a character position in text.
 * Uses font.getCursorX() when available, falls back to monospace approximation.
 */
function calculateTextOffset(text, charIndex, fontSize, padding, baseX, font) {
    let x = baseX + padding;
    if (charIndex <= 0) return x;

    if (font && font.getCursorX) {
        // Use font's glyph metrics
        x += font.getCursorX(text, charIndex, fontSize);
    } else {
        // Fallback to monospace approximation
        const limit = Math.min(charIndex, text.length);
        x += limit * fontSize * MONOSPACE_WIDTH_RATIO;
    }

    return x;
}
