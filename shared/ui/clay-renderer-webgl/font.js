/**
 * Clay WebGL Renderer - MSDF Font Loader
 */

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
            img.onerror = reject;
            img.src = pngUrl;
        });

        this.texture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

        // Load font metrics
        const resp = await fetch(jsonUrl);
        this.data = await resp.json();

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
     * Measure text width using font metrics
     */
    measureText(text, fontSize) {
        let width = 0;
        for (let i = 0; i < text.length; i++) {
            const glyph = this.glyphMap[text.charCodeAt(i)];
            if (glyph) {
                width += glyph.advance * fontSize;
            } else {
                width += fontSize * 0.5;  // fallback
            }
        }
        return width;
    }

    /**
     * Get cursor X position for a given character index
     */
    getCursorX(text, cursorIndex, fontSize) {
        let x = 0;
        for (let i = 0; i < cursorIndex && i < text.length; i++) {
            const glyph = this.glyphMap[text.charCodeAt(i)];
            if (glyph) {
                x += glyph.advance * fontSize;
            } else {
                x += fontSize * 0.5;
            }
        }
        return x;
    }
}
