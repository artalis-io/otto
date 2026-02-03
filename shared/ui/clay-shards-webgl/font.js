/**
 * Clay WebGL Renderer - MSDF Font Loader
 */

import { createTextureFromImage } from './utils.js';

export class MSDFFont {
    constructor() {
        this.texture = null;
        this.data = null;
        this.glyphMap = null;
    }

    async load(gl, jsonUrl, pngUrl) {
        // Load font texture
        const img = new Image();
        await new Promise((resolve, reject) => {
            img.onload = resolve;
            img.onerror = () => reject(new Error(`Failed to load font texture: ${pngUrl}`));
            img.src = pngUrl;
        });

        this.texture = createTextureFromImage(gl, img);

        // Load font metrics
        const resp = await fetch(jsonUrl);
        if (!resp.ok) {
            throw new Error(`Failed to load font metrics: ${jsonUrl} (${resp.status})`);
        }
        this.data = await resp.json();

        // Validate font data structure
        if (!this.data) {
            throw new Error('Invalid font data: empty response');
        }
        if (!this.data.glyphs || !Array.isArray(this.data.glyphs)) {
            throw new Error('Invalid font data: missing glyphs array');
        }
        if (!this.data.atlas) {
            throw new Error('Invalid font data: missing atlas info');
        }

        // Build glyph lookup
        this.glyphMap = {};
        for (const glyph of this.data.glyphs) {
            this.glyphMap[glyph.unicode] = glyph;
        }

        return this;
    }

    getGlyph(charCode) {
        return this.glyphMap[charCode] || null;
    }

    getAtlasSize() {
        return {
            width: this.data.atlas.width,
            height: this.data.atlas.height
        };
    }

    getDistanceRange() {
        return this.data.atlas.distanceRange;
    }

    getFontSize() {
        return this.data.atlas.size;
    }

    /**
     * Get cursor/text X position for a given character index.
     * Core text measurement function used by both measureText and cursor positioning.
     */
    getCursorX(text, cursorIndex, fontSize) {
        let x = 0;
        const limit = Math.min(cursorIndex, text.length);
        for (let i = 0; i < limit; i++) {
            const glyph = this.glyphMap[text.charCodeAt(i)];
            x += glyph ? glyph.advance * fontSize : fontSize * 0.5;
        }
        return x;
    }

    /**
     * Measure total text width using font metrics.
     * Convenience wrapper around getCursorX.
     */
    measureText(text, fontSize) {
        return this.getCursorX(text, text.length, fontSize);
    }

    /**
     * Transfer font metrics to WASM for accurate text measurement in Clay layout.
     * Call this after loading font and WASM module.
     *
     * @param {Object} wasm - WASM exports (must have cs_clay_set_glyph_advance)
     * @returns {number} Number of glyphs transferred
     */
    transferMetricsToWasm(wasm) {
        if (!wasm.cs_clay_set_glyph_advance) {
            console.warn('WASM missing cs_clay_set_glyph_advance export');
            return 0;
        }

        let count = 0;
        for (const glyph of this.data.glyphs) {
            if (glyph.unicode < 256 && glyph.advance !== undefined) {
                wasm.cs_clay_set_glyph_advance(glyph.unicode, glyph.advance);
                count++;
            }
        }

        return count;
    }
}
