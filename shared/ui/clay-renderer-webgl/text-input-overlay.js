/**
 * Clay WebGL Renderer - Text Input Overlay
 *
 * Renders cursor and selection for text inputs.
 */

export class TextInputOverlay {
    constructor(renderer) {
        this.renderer = renderer;
    }

    /**
     * Render cursor and selection for a text input
     *
     * @param {Object} state - Text input state from WASM
     * @param {number} state.x - Input element X position
     * @param {number} state.y - Input element Y position
     * @param {number} state.width - Input element width
     * @param {number} state.height - Input element height
     * @param {number} state.cursor - Cursor position (character index)
     * @param {number} state.selectionStart - Selection start (-1 if no selection)
     * @param {boolean} state.cursorVisible - Whether cursor should be visible (blink state)
     * @param {string} state.text - Input text content
     * @param {number} state.fontSize - Font size in pixels
     * @param {number} state.padding - Input padding in pixels
     * @param {Float32Array} projMatrix - Projection matrix
     */
    render(state, projMatrix) {
        const {
            x, y, width, height,
            cursor, selectionStart, cursorVisible,
            text, fontSize, padding
        } = state;

        const font = this.renderer.font;
        if (!font) return;

        // Calculate cursor X position using font metrics
        let cursorX = x + padding;
        if (text) {
            cursorX += font.getCursorX(text, cursor, fontSize);
        }

        const cursorY = y + 4;
        const cursorH = height - 8;

        // Render selection first (behind cursor)
        if (selectionStart >= 0 && selectionStart !== cursor) {
            const start = Math.min(selectionStart, cursor);
            const end = Math.max(selectionStart, cursor);

            const selX = x + padding + font.getCursorX(text || '', start, fontSize);
            const selEndX = x + padding + font.getCursorX(text || '', end, fontSize);
            const selW = selEndX - selX;

            // Selection highlight (semi-transparent blue)
            this.renderer.renderRect(
                selX, cursorY, selW, cursorH,
                [0.23, 0.51, 0.96, 0.3],
                0, 0, null,
                projMatrix
            );
        }

        // Render cursor (if visible)
        if (cursorVisible) {
            this.renderer.renderRect(
                cursorX, cursorY, 2, cursorH,
                [0.2, 0.6, 1.0, 1.0],
                0, 0, null,
                projMatrix
            );
        }
    }

    /**
     * Helper to get text input state from WASM exports
     */
    static getStateFromWasm(wasm, memory) {
        const textPtr = wasm.map_search_get_text();
        const textLen = wasm.map_search_get_length();

        let text = '';
        const memView = new Uint8Array(memory.buffer);
        for (let i = 0; i < textLen; i++) {
            text += String.fromCharCode(memView[textPtr + i]);
        }

        return {
            x: wasm.map_search_get_x(),
            y: wasm.map_search_get_y(),
            width: wasm.map_search_get_width(),
            height: wasm.map_search_get_height(),
            cursor: wasm.map_search_get_cursor(),
            selectionStart: wasm.map_search_get_selection(),
            cursorVisible: wasm.map_search_cursor_visible() === 1,
            focused: wasm.map_search_is_focused() === 1,
            text,
            fontSize: 12,  // Should match the C code
            padding: 8,    // Should match the C code
        };
    }
}
