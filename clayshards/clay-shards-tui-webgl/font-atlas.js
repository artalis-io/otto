/**
 * ClayShards TUI WebGL - Font Atlas
 *
 * Manages a bitmap font atlas for terminal character rendering.
 * Supports ASCII, box-drawing characters, and block elements.
 */

/**
 * Character mapping for terminal fonts.
 *
 * Atlas layout: 16x16 grid = 256 glyphs
 * First 128: ASCII (0x00-0x7F)
 * Remaining: Box drawing, blocks, common symbols
 */

// ASCII printable range
const ASCII_START = 0x20;  // Space
const ASCII_END = 0x7E;    // Tilde

// Box drawing characters (U+2500-U+257F)
// We map these to indices 128-255 in the atlas
const BOX_DRAWING_MAP = new Map([
    // Horizontal and vertical lines
    [0x2500, 128], // ─ BOX DRAWINGS LIGHT HORIZONTAL
    [0x2501, 129], // ━ BOX DRAWINGS HEAVY HORIZONTAL
    [0x2502, 130], // │ BOX DRAWINGS LIGHT VERTICAL
    [0x2503, 131], // ┃ BOX DRAWINGS HEAVY VERTICAL

    // Light box corners
    [0x250C, 132], // ┌ BOX DRAWINGS LIGHT DOWN AND RIGHT
    [0x2510, 133], // ┐ BOX DRAWINGS LIGHT DOWN AND LEFT
    [0x2514, 134], // └ BOX DRAWINGS LIGHT UP AND RIGHT
    [0x2518, 135], // ┘ BOX DRAWINGS LIGHT UP AND LEFT

    // Heavy box corners
    [0x250F, 136], // ┏ BOX DRAWINGS HEAVY DOWN AND RIGHT
    [0x2513, 137], // ┓ BOX DRAWINGS HEAVY DOWN AND LEFT
    [0x2517, 138], // ┗ BOX DRAWINGS HEAVY UP AND RIGHT
    [0x251B, 139], // ┛ BOX DRAWINGS HEAVY UP AND LEFT

    // Light T-junctions
    [0x251C, 140], // ├ BOX DRAWINGS LIGHT VERTICAL AND RIGHT
    [0x2524, 141], // ┤ BOX DRAWINGS LIGHT VERTICAL AND LEFT
    [0x252C, 142], // ┬ BOX DRAWINGS LIGHT DOWN AND HORIZONTAL
    [0x2534, 143], // ┴ BOX DRAWINGS LIGHT UP AND HORIZONTAL
    [0x253C, 144], // ┼ BOX DRAWINGS LIGHT VERTICAL AND HORIZONTAL

    // Heavy T-junctions
    [0x2523, 145], // ┣ BOX DRAWINGS HEAVY VERTICAL AND RIGHT
    [0x252B, 146], // ┫ BOX DRAWINGS HEAVY VERTICAL AND LEFT
    [0x2533, 147], // ┳ BOX DRAWINGS HEAVY DOWN AND HORIZONTAL
    [0x253B, 148], // ┻ BOX DRAWINGS HEAVY UP AND HORIZONTAL
    [0x254B, 149], // ╋ BOX DRAWINGS HEAVY VERTICAL AND HORIZONTAL

    // Double box characters
    [0x2550, 150], // ═ BOX DRAWINGS DOUBLE HORIZONTAL
    [0x2551, 151], // ║ BOX DRAWINGS DOUBLE VERTICAL
    [0x2554, 152], // ╔ BOX DRAWINGS DOUBLE DOWN AND RIGHT
    [0x2557, 153], // ╗ BOX DRAWINGS DOUBLE DOWN AND LEFT
    [0x255A, 154], // ╚ BOX DRAWINGS DOUBLE UP AND RIGHT
    [0x255D, 155], // ╝ BOX DRAWINGS DOUBLE UP AND LEFT
    [0x2560, 156], // ╠ BOX DRAWINGS DOUBLE VERTICAL AND RIGHT
    [0x2563, 157], // ╣ BOX DRAWINGS DOUBLE VERTICAL AND LEFT
    [0x2566, 158], // ╦ BOX DRAWINGS DOUBLE DOWN AND HORIZONTAL
    [0x2569, 159], // ╩ BOX DRAWINGS DOUBLE UP AND HORIZONTAL
    [0x256C, 160], // ╬ BOX DRAWINGS DOUBLE VERTICAL AND HORIZONTAL

    // Rounded corners
    [0x256D, 161], // ╭ BOX DRAWINGS LIGHT ARC DOWN AND RIGHT
    [0x256E, 162], // ╮ BOX DRAWINGS LIGHT ARC DOWN AND LEFT
    [0x256F, 163], // ╯ BOX DRAWINGS LIGHT ARC UP AND LEFT
    [0x2570, 164], // ╰ BOX DRAWINGS LIGHT ARC UP AND RIGHT
]);

// Block elements (U+2580-U+259F)
const BLOCK_MAP = new Map([
    [0x2580, 176], // ▀ UPPER HALF BLOCK
    [0x2584, 177], // ▄ LOWER HALF BLOCK
    [0x2588, 178], // █ FULL BLOCK
    [0x258C, 179], // ▌ LEFT HALF BLOCK
    [0x2590, 180], // ▐ RIGHT HALF BLOCK
    [0x2591, 181], // ░ LIGHT SHADE
    [0x2592, 182], // ▒ MEDIUM SHADE
    [0x2593, 183], // ▓ DARK SHADE
]);

// Braille patterns (U+2800-U+28FF) - for ASCII art maps
// We can't include all 256, so just include the common ones
const BRAILLE_START = 0x2800;
const BRAILLE_END = 0x28FF;

/**
 * Terminal font atlas manager.
 */
export class TerminalFontAtlas {
    /**
     * @param {WebGLRenderingContext} gl - WebGL context
     */
    constructor(gl) {
        this.gl = gl;
        this.texture = null;
        this.glyphWidth = 8;
        this.glyphHeight = 16;
        this.atlasWidth = 16;   // 16 glyphs per row
        this.atlasHeight = 16;  // 16 rows
        this.loaded = false;

        // Glyph advance (relative to cell size, typically 1.0 for monospace)
        this.advance = 1.0;
    }

    /**
     * Get glyph index in atlas for a Unicode codepoint.
     *
     * @param {number} codepoint - Unicode codepoint
     * @returns {number} Atlas index (0-255), or 0 (space) if not found
     */
    getGlyphIndex(codepoint) {
        // ASCII range maps directly
        if (codepoint >= ASCII_START && codepoint <= ASCII_END) {
            return codepoint - ASCII_START;
        }

        // Space and control characters
        if (codepoint < ASCII_START) {
            return 0;  // Map to space
        }

        // Box drawing
        if (BOX_DRAWING_MAP.has(codepoint)) {
            return BOX_DRAWING_MAP.get(codepoint);
        }

        // Block elements
        if (BLOCK_MAP.has(codepoint)) {
            return BLOCK_MAP.get(codepoint);
        }

        // Braille - map to reserved range 192-255
        if (codepoint >= BRAILLE_START && codepoint <= BRAILLE_END) {
            const offset = codepoint - BRAILLE_START;
            if (offset < 64) {  // Only first 64 braille patterns
                return 192 + offset;
            }
        }

        // Unknown character - return '?' or block
        return '?'.charCodeAt(0) - ASCII_START;
    }

    /**
     * Get UV coordinates for a glyph.
     *
     * @param {number} glyphIndex - Index from getGlyphIndex()
     * @returns {{u0: number, v0: number, u1: number, v1: number}} UV bounds
     */
    getGlyphUV(glyphIndex) {
        const col = glyphIndex % this.atlasWidth;
        const row = Math.floor(glyphIndex / this.atlasWidth);

        const u0 = col / this.atlasWidth;
        const v0 = row / this.atlasHeight;
        const u1 = (col + 1) / this.atlasWidth;
        const v1 = (row + 1) / this.atlasHeight;

        return { u0, v0, u1, v1 };
    }

    /**
     * Load font atlas from an image URL.
     *
     * @param {string} url - URL to font atlas image
     * @returns {Promise<void>}
     */
    async load(url) {
        const gl = this.gl;

        return new Promise((resolve, reject) => {
            const img = new Image();
            img.onload = () => {
                this.texture = gl.createTexture();
                gl.bindTexture(gl.TEXTURE_2D, this.texture);

                // Upload image
                gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);

                // Set filtering (nearest for crisp pixels)
                gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
                gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
                gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
                gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

                // Calculate glyph size from image
                this.glyphWidth = img.width / this.atlasWidth;
                this.glyphHeight = img.height / this.atlasHeight;

                this.loaded = true;
                resolve();
            };
            img.onerror = () => {
                reject(new Error(`Failed to load font atlas: ${url}`));
            };
            img.src = url;
        });
    }

    /**
     * Generate a simple procedural font atlas.
     * This creates a basic ASCII font using canvas 2D.
     *
     * @param {Object} options
     * @param {string} options.fontFamily - CSS font family (default: 'monospace')
     * @param {number} options.fontSize - Font size in pixels (default: 14)
     * @param {string} options.fgColor - Foreground color (default: 'white')
     * @param {string} options.bgColor - Background color (default: 'transparent')
     */
    generateProceduralAtlas(options = {}) {
        const {
            fontFamily = 'monospace',
            fontSize = 14,
            fgColor = 'white',
            bgColor = 'transparent'
        } = options;

        const gl = this.gl;

        // Calculate cell size
        // Create temp canvas to measure character width
        const tempCanvas = document.createElement('canvas');
        const tempCtx = tempCanvas.getContext('2d');
        tempCtx.font = `${fontSize}px ${fontFamily}`;
        const metrics = tempCtx.measureText('M');
        const charWidth = Math.ceil(metrics.width);
        const charHeight = Math.ceil(fontSize * 1.2);

        this.glyphWidth = charWidth;
        this.glyphHeight = charHeight;

        // Create atlas canvas
        const atlasCanvas = document.createElement('canvas');
        atlasCanvas.width = charWidth * this.atlasWidth;
        atlasCanvas.height = charHeight * this.atlasHeight;
        const ctx = atlasCanvas.getContext('2d');

        // Fill background
        if (bgColor !== 'transparent') {
            ctx.fillStyle = bgColor;
            ctx.fillRect(0, 0, atlasCanvas.width, atlasCanvas.height);
        }

        // Set up text rendering
        ctx.font = `${fontSize}px ${fontFamily}`;
        ctx.fillStyle = fgColor;
        ctx.textBaseline = 'top';

        // Render ASCII characters (indices 0-95)
        for (let i = 0; i < 96; i++) {
            const codepoint = ASCII_START + i;
            const char = String.fromCodePoint(codepoint);
            const col = i % this.atlasWidth;
            const row = Math.floor(i / this.atlasWidth);
            const x = col * charWidth;
            const y = row * charHeight;
            ctx.fillText(char, x, y);
        }

        // Render box drawing characters
        for (const [codepoint, index] of BOX_DRAWING_MAP) {
            const char = String.fromCodePoint(codepoint);
            const col = index % this.atlasWidth;
            const row = Math.floor(index / this.atlasWidth);
            const x = col * charWidth;
            const y = row * charHeight;
            ctx.fillText(char, x, y);
        }

        // Render block elements
        for (const [codepoint, index] of BLOCK_MAP) {
            const char = String.fromCodePoint(codepoint);
            const col = index % this.atlasWidth;
            const row = Math.floor(index / this.atlasWidth);
            const x = col * charWidth;
            const y = row * charHeight;
            ctx.fillText(char, x, y);
        }

        // Create texture from canvas
        this.texture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, atlasCanvas);

        // Set filtering
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

        this.loaded = true;
    }

    /**
     * Get atlas dimensions.
     */
    getAtlasSize() {
        return {
            width: this.atlasWidth,
            height: this.atlasHeight,
            glyphWidth: this.glyphWidth,
            glyphHeight: this.glyphHeight
        };
    }

    /**
     * Check if atlas is ready for rendering.
     */
    isReady() {
        return this.loaded && this.texture !== null;
    }

    /**
     * Clean up WebGL resources.
     */
    destroy() {
        if (this.texture) {
            this.gl.deleteTexture(this.texture);
            this.texture = null;
        }
        this.loaded = false;
    }
}
