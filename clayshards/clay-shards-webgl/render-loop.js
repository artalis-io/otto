/**
 * Clay Render Loop - Generic render loop for Clay UI applications
 */

import { renderTextCursor } from './text-cursor.js';

/**
 * Create a render loop for a Clay UI application.
 *
 * @param {ClayRenderer} renderer - The Clay renderer instance
 * @param {Object} wasm - WASM module exports
 * @param {MSDFFont} font - Font for text cursor rendering
 * @param {Object} options - Configuration options
 * @param {Function} options.onRender - Domain-specific rendering callback (proj, dt) => void
 * @param {string} options.frameFunction - WASM frame function name (default: 'frame')
 * @param {string} options.cmdPrefix - Command accessor prefix (default: 'cs_clay_cmd_')
 * @param {number} options.cursorFontSize - Font size for cursor (default: 12)
 * @param {number} options.cursorPadding - Padding for cursor (default: 8)
 * @returns {Function} The render function to pass to requestAnimationFrame
 */
export function createRenderLoop(renderer, wasm, font, options = {}) {
    const {
        onRender = null,
        frameFunction = 'frame',
        cmdPrefix = 'cs_clay_cmd_',
        cursorFontSize = 12,
        cursorPadding = 8,
    } = options;

    let lastFrameTime = 0;

    return function render(timestamp) {
        const dt = lastFrameTime ? (timestamp - lastFrameTime) / 1000 : 0.016;
        lastFrameTime = timestamp;

        const projMatrix = renderer.getProjectionMatrix();

        // Clear
        renderer.clear();

        // Domain-specific rendering (e.g., map tiles)
        if (onRender) {
            onRender(projMatrix, dt);
        }

        // Render Clay UI
        const count = wasm[frameFunction](dt);
        if (count > 0) {
            renderer.renderClayCommands(wasm, count, projMatrix, cmdPrefix);
        }

        // Render text cursor for focused text input (not buttons)
        if (wasm.cs_focused_id && wasm.cs_focused_id() !== 0) {
            // Only render cursor if there's an active text buffer (text input, not button)
            const textLen = wasm.cs_focused_text_len ? wasm.cs_focused_text_len() : 0;
            const hasActiveText = textLen >= 0 && wasm.cs_focused_text && wasm.cs_focused_w() > 0;

            if (hasActiveText) {
                const inputBounds = {
                    x: wasm.cs_focused_x(),
                    y: wasm.cs_focused_y(),
                    w: wasm.cs_focused_w(),
                    h: wasm.cs_focused_h()
                };
                renderTextCursor(renderer, wasm, font, projMatrix, inputBounds, {
                    fontSize: cursorFontSize,
                    padding: cursorPadding
                });
            }
        }

        requestAnimationFrame(render);
    };
}
