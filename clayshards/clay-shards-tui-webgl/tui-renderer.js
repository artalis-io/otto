/**
 * ClayShards TUI WebGL Renderer
 *
 * Renders the TUI character grid to WebGL using instanced rendering.
 * Reads cell buffer from WASM memory and builds vertex data for the GPU.
 */

import { TERM_VS, TERM_FS, TERM_FS_BITMAP, TERM_BATCHED_VS, TERM_BATCHED_FS } from './shaders.js';

/**
 * Cell buffer layout (16 bytes per cell):
 * Offset 0:  codepoint (uint32)
 * Offset 4:  fg_r (uint8)
 * Offset 5:  fg_g (uint8)
 * Offset 6:  fg_b (uint8)
 * Offset 7:  bg_r (uint8)
 * Offset 8:  bg_g (uint8)
 * Offset 9:  bg_b (uint8)
 * Offset 10: flags (uint8)
 * Offset 11: padding (uint8)
 * Offset 12: z_index (int16)
 * Offset 14: padding (uint16)
 */
const CELL_SIZE = 16;

/**
 * Create a shader program.
 */
function createShader(gl, type, source) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);

    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        console.error('Shader compile error:', gl.getShaderInfoLog(shader));
        gl.deleteShader(shader);
        return null;
    }
    return shader;
}

function createProgram(gl, vsSource, fsSource) {
    const vs = createShader(gl, gl.VERTEX_SHADER, vsSource);
    const fs = createShader(gl, gl.FRAGMENT_SHADER, fsSource);
    if (!vs || !fs) return null;

    const program = gl.createProgram();
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.linkProgram(program);

    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        console.error('Program link error:', gl.getProgramInfoLog(program));
        gl.deleteProgram(program);
        return null;
    }

    // Cache attribute and uniform locations
    const wrapper = { program, attribs: {}, uniforms: {} };

    const numAttribs = gl.getProgramParameter(program, gl.ACTIVE_ATTRIBUTES);
    for (let i = 0; i < numAttribs; i++) {
        const info = gl.getActiveAttrib(program, i);
        wrapper.attribs[info.name] = gl.getAttribLocation(program, info.name);
    }

    const numUniforms = gl.getProgramParameter(program, gl.ACTIVE_UNIFORMS);
    for (let i = 0; i < numUniforms; i++) {
        const info = gl.getActiveUniform(program, i);
        wrapper.uniforms[info.name] = gl.getUniformLocation(program, info.name);
    }

    return wrapper;
}

/**
 * Create orthographic projection matrix.
 */
function createOrthoMatrix(width, height) {
    return new Float32Array([
        2 / width, 0, 0, 0,
        0, -2 / height, 0, 0,
        0, 0, -1, 0,
        -1, 1, 0, 1
    ]);
}

/**
 * TUI WebGL Renderer.
 *
 * Renders terminal character grid from WASM memory to WebGL canvas.
 */
export class TuiRenderer {
    /**
     * @param {HTMLCanvasElement} canvas - Target canvas
     * @param {Object} options - Configuration options
     */
    constructor(canvas, options = {}) {
        this.canvas = canvas;
        this.gl = canvas.getContext('webgl', {
            alpha: false,
            antialias: false,  // Crisp pixels for terminal
            premultipliedAlpha: false
        });

        if (!this.gl) {
            throw new Error('WebGL not supported');
        }

        this.options = {
            cellWidth: options.cellWidth || 8,
            cellHeight: options.cellHeight || 16,
            useBitmapShader: options.useBitmapShader !== false,  // Default true for bitmap fonts
            ...options
        };

        // Terminal dimensions in cells
        this.cols = 80;
        this.rows = 24;

        // Font atlas
        this.fontAtlas = null;

        // Context loss handling
        this.contextLost = false;
        canvas.addEventListener('webglcontextlost', (e) => {
            e.preventDefault();
            this.contextLost = true;
            console.warn('WebGL context lost');
        });
        canvas.addEventListener('webglcontextrestored', () => {
            this.contextLost = false;
            this._initResources();
            console.log('WebGL context restored');
        });

        this._initResources();
    }

    /**
     * Initialize WebGL resources.
     */
    _initResources() {
        const gl = this.gl;

        // Create shader programs
        const fsSource = this.options.useBitmapShader ? TERM_FS_BITMAP : TERM_FS;
        this.shader = createProgram(gl, TERM_VS, fsSource);
        this.batchedShader = createProgram(gl, TERM_BATCHED_VS, TERM_BATCHED_FS);

        if (!this.batchedShader) {
            console.error('Failed to create batched shader');
        } else {
            console.log('Batched shader created:', this.batchedShader);
        }

        // Create quad vertex buffer (4 vertices: 0,0 1,0 0,1 1,1)
        this.quadBuffer = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
            0, 0,
            1, 0,
            0, 1,
            1, 1,
        ]), gl.STATIC_DRAW);

        // Instance data buffer (will be filled each frame)
        this.instanceBuffer = gl.createBuffer();
        this.instanceData = null;

        // Enable blending
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    }

    /**
     * Set the font atlas for rendering.
     * @param {TerminalFontAtlas} fontAtlas
     */
    setFontAtlas(fontAtlas) {
        this.fontAtlas = fontAtlas;

        // Update cell dimensions from atlas
        if (fontAtlas.isReady()) {
            const size = fontAtlas.getAtlasSize();
            this.options.cellWidth = size.glyphWidth;
            this.options.cellHeight = size.glyphHeight;
        }
    }

    /**
     * Resize the renderer to match terminal dimensions.
     * @param {number} cols - Number of columns
     * @param {number} rows - Number of rows
     */
    resize(cols, rows) {
        this.cols = cols;
        this.rows = rows;

        // Update canvas size
        const width = cols * this.options.cellWidth;
        const height = rows * this.options.cellHeight;

        const dpr = window.devicePixelRatio || 1;
        this.canvas.width = width * dpr;
        this.canvas.height = height * dpr;
        this.canvas.style.width = `${width}px`;
        this.canvas.style.height = `${height}px`;

        this.gl.viewport(0, 0, this.canvas.width, this.canvas.height);

        // Allocate instance data buffer
        // Per cell: cellPos (2), codepoint (1), fgColor (3), bgColor (3) = 9 floats
        this.instanceData = new Float32Array(cols * rows * 9);
    }

    /**
     * Clear the screen with a background color.
     * @param {number} r - Red (0-1)
     * @param {number} g - Green (0-1)
     * @param {number} b - Blue (0-1)
     */
    clear(r = 0.1, g = 0.1, b = 0.1) {
        if (this.contextLost) return;
        this.gl.clearColor(r, g, b, 1.0);
        this.gl.clear(this.gl.COLOR_BUFFER_BIT);
    }

    /**
     * Render the terminal from WASM cell buffer.
     *
     * @param {WebAssembly.Exports} wasm - WASM module exports
     */
    render(wasm) {
        if (this.contextLost) return;
        if (!this.fontAtlas || !this.fontAtlas.isReady()) return;

        const gl = this.gl;

        // Get buffer info from WASM
        const bufferPtr = wasm.wasm_tui_get_buffer();
        const width = wasm.wasm_tui_get_width();
        const height = wasm.wasm_tui_get_height();

        if (!bufferPtr || width <= 0 || height <= 0) return;

        // Update dimensions if changed
        if (width !== this.cols || height !== this.rows) {
            this.resize(width, height);
        }

        // Read cell buffer from WASM memory
        const memory = new DataView(wasm.memory.buffer);

        // Build instance data from cell buffer
        let instanceIdx = 0;
        for (let row = 0; row < height; row++) {
            for (let col = 0; col < width; col++) {
                const cellOffset = bufferPtr + (row * width + col) * CELL_SIZE;

                // Read cell data
                const codepoint = memory.getUint32(cellOffset, true);
                const fg_r = memory.getUint8(cellOffset + 4);
                const fg_g = memory.getUint8(cellOffset + 5);
                const fg_b = memory.getUint8(cellOffset + 6);
                const bg_r = memory.getUint8(cellOffset + 7);
                const bg_g = memory.getUint8(cellOffset + 8);
                const bg_b = memory.getUint8(cellOffset + 9);

                // Convert codepoint to atlas index
                const glyphIndex = this.fontAtlas.getGlyphIndex(codepoint);

                // Store instance data
                this.instanceData[instanceIdx++] = col;                  // cellPos.x
                this.instanceData[instanceIdx++] = row;                  // cellPos.y
                this.instanceData[instanceIdx++] = glyphIndex;           // codepoint
                this.instanceData[instanceIdx++] = fg_r / 255;           // fgColor.r
                this.instanceData[instanceIdx++] = fg_g / 255;           // fgColor.g
                this.instanceData[instanceIdx++] = fg_b / 255;           // fgColor.b
                this.instanceData[instanceIdx++] = bg_r / 255;           // bgColor.r
                this.instanceData[instanceIdx++] = bg_g / 255;           // bgColor.g
                this.instanceData[instanceIdx++] = bg_b / 255;           // bgColor.b
            }
        }

        // Upload instance data
        gl.bindBuffer(gl.ARRAY_BUFFER, this.instanceBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, this.instanceData, gl.DYNAMIC_DRAW);

        // Use shader
        gl.useProgram(this.shader.program);

        // Set uniforms
        const projMatrix = createOrthoMatrix(
            width * this.options.cellWidth,
            height * this.options.cellHeight
        );
        gl.uniformMatrix4fv(this.shader.uniforms.u_proj, false, projMatrix);
        gl.uniform2f(this.shader.uniforms.u_cellSize,
            this.options.cellWidth, this.options.cellHeight);
        gl.uniform2f(this.shader.uniforms.u_atlasGrid,
            this.fontAtlas.atlasWidth, this.fontAtlas.atlasHeight);

        // Bind font atlas texture
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.fontAtlas.texture);
        gl.uniform1i(this.shader.uniforms.u_fontAtlas, 0);

        // Draw each cell (without instancing for WebGL1 compatibility)
        // For each cell, we render a quad
        const FLOATS_PER_INSTANCE = 9;

        for (let i = 0; i < width * height; i++) {
            const offset = i * FLOATS_PER_INSTANCE;

            // Bind quad vertices
            gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuffer);
            gl.enableVertexAttribArray(this.shader.attribs.a_vertex);
            gl.vertexAttribPointer(this.shader.attribs.a_vertex, 2, gl.FLOAT, false, 0, 0);

            // Set per-instance attributes as uniforms (fallback for WebGL1)
            // We're using a_cellPos etc as attributes, but without ANGLE_instanced_arrays
            // we need to set them differently

            // Actually, for WebGL1 without instancing, we need a different approach
            // Let's set them as uniforms for the simple case
            const cellX = this.instanceData[offset + 0];
            const cellY = this.instanceData[offset + 1];
            const codepoint = this.instanceData[offset + 2];
            const fgR = this.instanceData[offset + 3];
            const fgG = this.instanceData[offset + 4];
            const fgB = this.instanceData[offset + 5];
            const bgR = this.instanceData[offset + 6];
            const bgG = this.instanceData[offset + 7];
            const bgB = this.instanceData[offset + 8];

            // We need a shader that takes these as uniforms
            // Let's use vertexAttrib* for non-array attributes
            gl.vertexAttrib2f(this.shader.attribs.a_cellPos, cellX, cellY);
            gl.vertexAttrib1f(this.shader.attribs.a_codepoint, codepoint);
            gl.vertexAttrib3f(this.shader.attribs.a_fgColor, fgR, fgG, fgB);
            gl.vertexAttrib3f(this.shader.attribs.a_bgColor, bgR, bgG, bgB);

            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }

        gl.disableVertexAttribArray(this.shader.attribs.a_vertex);
    }

    /**
     * Render using batched quads (more efficient for WebGL1).
     * Builds a single vertex buffer with all cells.
     *
     * @param {WebAssembly.Exports} wasm - WASM module exports
     * @param {Object} options - Optional function name overrides
     * @param {string} options.getBuffer - Buffer function name (default: 'wasm_tui_get_buffer')
     * @param {string} options.getWidth - Width function name (default: 'wasm_tui_get_width')
     * @param {string} options.getHeight - Height function name (default: 'wasm_tui_get_height')
     */
    renderBatched(wasm, options = {}) {
        if (this.contextLost) return;
        if (!this.fontAtlas || !this.fontAtlas.isReady()) return;

        const gl = this.gl;

        // Get function names (allow custom prefixes)
        const getBuffer = options.getBuffer || 'wasm_tui_get_buffer';
        const getWidth = options.getWidth || 'wasm_tui_get_width';
        const getHeight = options.getHeight || 'wasm_tui_get_height';

        // Get buffer info from WASM
        const bufferPtr = wasm[getBuffer]();
        const width = wasm[getWidth]();
        const height = wasm[getHeight]();

        if (!bufferPtr || width <= 0 || height <= 0) {
            console.warn('TuiRenderer.renderBatched: invalid buffer', { bufferPtr, width, height });
            return;
        }

        if (!this.batchedShader) {
            console.error('TuiRenderer.renderBatched: batchedShader not initialized');
            return;
        }

        // Update dimensions if changed
        if (width !== this.cols || height !== this.rows) {
            this.resize(width, height);
        }

        // Read cell buffer from WASM memory
        const memory = new DataView(wasm.memory.buffer);

        // Build vertex data (6 vertices per cell, each with: pos, uv, fg, bg)
        // pos (2) + uv (2) + fg (3) + bg (3) = 10 floats per vertex
        // 6 vertices per cell (2 triangles)
        const floatsPerVertex = 10;
        const verticesPerCell = 6;
        const vertexData = new Float32Array(width * height * verticesPerCell * floatsPerVertex);

        let vertIdx = 0;
        const cellW = this.options.cellWidth;
        const cellH = this.options.cellHeight;
        const atlasW = this.fontAtlas.atlasWidth;
        const atlasH = this.fontAtlas.atlasHeight;

        for (let row = 0; row < height; row++) {
            for (let col = 0; col < width; col++) {
                const cellOffset = bufferPtr + (row * width + col) * CELL_SIZE;

                // Read cell data
                const codepoint = memory.getUint32(cellOffset, true);
                const fg_r = memory.getUint8(cellOffset + 4) / 255;
                const fg_g = memory.getUint8(cellOffset + 5) / 255;
                const fg_b = memory.getUint8(cellOffset + 6) / 255;
                const bg_r = memory.getUint8(cellOffset + 7) / 255;
                const bg_g = memory.getUint8(cellOffset + 8) / 255;
                const bg_b = memory.getUint8(cellOffset + 9) / 255;

                // Convert codepoint to atlas UV
                const glyphIndex = this.fontAtlas.getGlyphIndex(codepoint);
                const glyphCol = glyphIndex % atlasW;
                const glyphRow = Math.floor(glyphIndex / atlasW);
                const u0 = glyphCol / atlasW;
                const v0 = glyphRow / atlasH;
                const u1 = (glyphCol + 1) / atlasW;
                const v1 = (glyphRow + 1) / atlasH;

                // Pixel positions
                const x0 = col * cellW;
                const y0 = row * cellH;
                const x1 = x0 + cellW;
                const y1 = y0 + cellH;

                // Triangle 1: top-left, top-right, bottom-left
                // Vertex 0: top-left
                vertexData[vertIdx++] = x0; vertexData[vertIdx++] = y0;
                vertexData[vertIdx++] = u0; vertexData[vertIdx++] = v0;
                vertexData[vertIdx++] = fg_r; vertexData[vertIdx++] = fg_g; vertexData[vertIdx++] = fg_b;
                vertexData[vertIdx++] = bg_r; vertexData[vertIdx++] = bg_g; vertexData[vertIdx++] = bg_b;

                // Vertex 1: top-right
                vertexData[vertIdx++] = x1; vertexData[vertIdx++] = y0;
                vertexData[vertIdx++] = u1; vertexData[vertIdx++] = v0;
                vertexData[vertIdx++] = fg_r; vertexData[vertIdx++] = fg_g; vertexData[vertIdx++] = fg_b;
                vertexData[vertIdx++] = bg_r; vertexData[vertIdx++] = bg_g; vertexData[vertIdx++] = bg_b;

                // Vertex 2: bottom-left
                vertexData[vertIdx++] = x0; vertexData[vertIdx++] = y1;
                vertexData[vertIdx++] = u0; vertexData[vertIdx++] = v1;
                vertexData[vertIdx++] = fg_r; vertexData[vertIdx++] = fg_g; vertexData[vertIdx++] = fg_b;
                vertexData[vertIdx++] = bg_r; vertexData[vertIdx++] = bg_g; vertexData[vertIdx++] = bg_b;

                // Triangle 2: top-right, bottom-right, bottom-left
                // Vertex 3: top-right
                vertexData[vertIdx++] = x1; vertexData[vertIdx++] = y0;
                vertexData[vertIdx++] = u1; vertexData[vertIdx++] = v0;
                vertexData[vertIdx++] = fg_r; vertexData[vertIdx++] = fg_g; vertexData[vertIdx++] = fg_b;
                vertexData[vertIdx++] = bg_r; vertexData[vertIdx++] = bg_g; vertexData[vertIdx++] = bg_b;

                // Vertex 4: bottom-right
                vertexData[vertIdx++] = x1; vertexData[vertIdx++] = y1;
                vertexData[vertIdx++] = u1; vertexData[vertIdx++] = v1;
                vertexData[vertIdx++] = fg_r; vertexData[vertIdx++] = fg_g; vertexData[vertIdx++] = fg_b;
                vertexData[vertIdx++] = bg_r; vertexData[vertIdx++] = bg_g; vertexData[vertIdx++] = bg_b;

                // Vertex 5: bottom-left
                vertexData[vertIdx++] = x0; vertexData[vertIdx++] = y1;
                vertexData[vertIdx++] = u0; vertexData[vertIdx++] = v1;
                vertexData[vertIdx++] = fg_r; vertexData[vertIdx++] = fg_g; vertexData[vertIdx++] = fg_b;
                vertexData[vertIdx++] = bg_r; vertexData[vertIdx++] = bg_g; vertexData[vertIdx++] = bg_b;
            }
        }

        // Upload and render
        gl.bindBuffer(gl.ARRAY_BUFFER, this.instanceBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, vertexData, gl.DYNAMIC_DRAW);

        // Use batched shader
        gl.useProgram(this.batchedShader.program);

        const projMatrix = createOrthoMatrix(
            width * this.options.cellWidth,
            height * this.options.cellHeight
        );
        gl.uniformMatrix4fv(this.batchedShader.uniforms.u_proj, false, projMatrix);

        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.fontAtlas.texture);
        gl.uniform1i(this.batchedShader.uniforms.u_fontAtlas, 0);

        // Set up vertex attributes
        const stride = floatsPerVertex * 4;  // bytes
        gl.enableVertexAttribArray(this.batchedShader.attribs.a_pos);
        gl.enableVertexAttribArray(this.batchedShader.attribs.a_uv);
        gl.enableVertexAttribArray(this.batchedShader.attribs.a_fgColor);
        gl.enableVertexAttribArray(this.batchedShader.attribs.a_bgColor);

        gl.vertexAttribPointer(this.batchedShader.attribs.a_pos, 2, gl.FLOAT, false, stride, 0);
        gl.vertexAttribPointer(this.batchedShader.attribs.a_uv, 2, gl.FLOAT, false, stride, 8);
        gl.vertexAttribPointer(this.batchedShader.attribs.a_fgColor, 3, gl.FLOAT, false, stride, 16);
        gl.vertexAttribPointer(this.batchedShader.attribs.a_bgColor, 3, gl.FLOAT, false, stride, 28);

        gl.drawArrays(gl.TRIANGLES, 0, width * height * 6);

        gl.disableVertexAttribArray(this.batchedShader.attribs.a_pos);
        gl.disableVertexAttribArray(this.batchedShader.attribs.a_uv);
        gl.disableVertexAttribArray(this.batchedShader.attribs.a_fgColor);
        gl.disableVertexAttribArray(this.batchedShader.attribs.a_bgColor);
    }

    /**
     * Check if context is lost.
     */
    isContextLost() {
        return this.contextLost;
    }

    /**
     * Get canvas dimensions in pixels.
     */
    getSize() {
        return {
            width: this.cols * this.options.cellWidth,
            height: this.rows * this.options.cellHeight
        };
    }

    /**
     * Clean up WebGL resources.
     */
    destroy() {
        const gl = this.gl;
        if (this.shader) {
            gl.deleteProgram(this.shader.program);
        }
        if (this.batchedShader) {
            gl.deleteProgram(this.batchedShader.program);
        }
        if (this.quadBuffer) {
            gl.deleteBuffer(this.quadBuffer);
        }
        if (this.instanceBuffer) {
            gl.deleteBuffer(this.instanceBuffer);
        }
    }
}
