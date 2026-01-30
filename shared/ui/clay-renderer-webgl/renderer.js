/**
 * Clay WebGL Renderer
 *
 * Renders Clay render commands using WebGL.
 */

import { TILE_VS, TILE_FS, RECT_VS, RECT_FS, TEXT_VS, TEXT_FS } from './shaders.js';

// Clay command types
const CLAY_CMD_NONE = 0;
const CLAY_CMD_RECTANGLE = 1;
const CLAY_CMD_BORDER = 2;
const CLAY_CMD_TEXT = 3;
const CLAY_CMD_IMAGE = 4;
const CLAY_CMD_SCISSOR_START = 5;
const CLAY_CMD_SCISSOR_END = 6;
const CLAY_CMD_CUSTOM = 7;

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

function createOrthoMatrix(width, height) {
    return new Float32Array([
        2 / width, 0, 0, 0,
        0, -2 / height, 0, 0,
        0, 0, -1, 0,
        -1, 1, 0, 1
    ]);
}

function unpackColor(packed) {
    const r = ((packed >>> 24) & 0xFF) / 255;
    const g = ((packed >>> 16) & 0xFF) / 255;
    const b = ((packed >>> 8) & 0xFF) / 255;
    const a = (packed & 0xFF) / 255;
    return [r, g, b, a];
}

export class ClayRenderer {
    constructor(canvas) {
        this.canvas = canvas;
        this.gl = canvas.getContext('webgl', { alpha: false, antialias: true });

        if (!this.gl) {
            throw new Error('WebGL not supported');
        }

        // Context loss handling
        this.contextLost = false;
        canvas.addEventListener('webglcontextlost', (e) => {
            e.preventDefault();
            this.contextLost = true;
            console.warn('WebGL context lost');
        });
        canvas.addEventListener('webglcontextrestored', () => {
            this.contextLost = false;
            console.log('WebGL context restored');
            this._initResources();
        });

        this._initResources();

        this.font = null;
        this.width = canvas.width;
        this.height = canvas.height;
    }

    _initResources() {
        const gl = this.gl;

        gl.getExtension('OES_standard_derivatives');

        // Create shaders
        this.tileShader = createProgram(gl, TILE_VS, TILE_FS);
        this.rectShader = createProgram(gl, RECT_VS, RECT_FS);
        this.textShader = createProgram(gl, TEXT_VS, TEXT_FS);

        // Create quad buffer
        this.quadBuffer = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
            0, 0, 0, 0,
            1, 0, 1, 0,
            0, 1, 0, 1,
            1, 1, 1, 1,
        ]), gl.STATIC_DRAW);

        // State
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    }

    isContextLost() {
        return this.contextLost;
    }

    setFont(font) {
        this.font = font;
    }

    resize() {
        const dpr = window.devicePixelRatio || 1;
        const width = this.canvas.clientWidth;
        const height = this.canvas.clientHeight;

        this.canvas.width = width * dpr;
        this.canvas.height = height * dpr;

        this.gl.viewport(0, 0, this.canvas.width, this.canvas.height);

        this.width = width;
        this.height = height;

        return { width, height };
    }

    clear(r = 0.1, g = 0.1, b = 0.1) {
        if (this.contextLost) return;
        this.gl.clearColor(r, g, b, 1.0);
        this.gl.clear(this.gl.COLOR_BUFFER_BIT);
    }

    getProjectionMatrix() {
        return createOrthoMatrix(this.width, this.height);
    }

    /**
     * Render a textured quad (for tiles, images)
     */
    renderTexture(texture, x, y, w, h, projMatrix) {
        if (this.contextLost) return;
        const gl = this.gl;

        gl.useProgram(this.tileShader.program);
        gl.uniformMatrix4fv(this.tileShader.uniforms.u_proj, false, projMatrix);

        gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuffer);
        gl.enableVertexAttribArray(this.tileShader.attribs.a_pos);
        gl.enableVertexAttribArray(this.tileShader.attribs.a_uv);
        gl.vertexAttribPointer(this.tileShader.attribs.a_pos, 2, gl.FLOAT, false, 16, 0);
        gl.vertexAttribPointer(this.tileShader.attribs.a_uv, 2, gl.FLOAT, false, 16, 8);

        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.uniform1i(this.tileShader.uniforms.u_tex, 0);
        gl.uniform4f(this.tileShader.uniforms.u_rect, x, y, w, h);

        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

        gl.disableVertexAttribArray(this.tileShader.attribs.a_pos);
        gl.disableVertexAttribArray(this.tileShader.attribs.a_uv);
    }

    /**
     * Render a rectangle (solid color, optional rounded corners)
     */
    renderRect(x, y, w, h, color, radius = 0, borderWidth = 0, borderColor = null, projMatrix) {
        if (this.contextLost) return;
        const gl = this.gl;

        gl.useProgram(this.rectShader.program);
        gl.uniformMatrix4fv(this.rectShader.uniforms.u_proj, false, projMatrix);

        gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuffer);
        gl.enableVertexAttribArray(this.rectShader.attribs.a_pos);
        gl.vertexAttribPointer(this.rectShader.attribs.a_pos, 2, gl.FLOAT, false, 16, 0);

        gl.uniform4f(this.rectShader.uniforms.u_rect, x, y, w, h);
        gl.uniform4fv(this.rectShader.uniforms.u_color, color);
        gl.uniform1f(this.rectShader.uniforms.u_radius, radius);
        gl.uniform1f(this.rectShader.uniforms.u_borderWidth, borderWidth);
        gl.uniform4fv(this.rectShader.uniforms.u_borderColor, borderColor || [0, 0, 0, 0]);

        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

        gl.disableVertexAttribArray(this.rectShader.attribs.a_pos);
    }

    /**
     * Render text using MSDF font
     */
    renderText(text, x, y, fontSize, color, projMatrix) {
        if (this.contextLost) return;
        if (!this.font || !this.font.texture) return;

        const gl = this.gl;

        gl.useProgram(this.textShader.program);
        gl.uniformMatrix4fv(this.textShader.uniforms.u_proj, false, projMatrix);
        gl.uniform4fv(this.textShader.uniforms.u_color, color);

        const scale = fontSize / this.font.getFontSize();
        const pxRange = this.font.getDistanceRange() * scale;
        gl.uniform1f(this.textShader.uniforms.u_pxRange, pxRange);

        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.font.texture);
        gl.uniform1i(this.textShader.uniforms.u_tex, 0);

        const vertices = [];
        let cursorX = x;

        const atlas = this.font.getAtlasSize();

        for (let i = 0; i < text.length; i++) {
            const charCode = text.charCodeAt(i);
            const glyph = this.font.getGlyph(charCode);

            if (!glyph) {
                cursorX += fontSize * 0.5;
                continue;
            }

            if (glyph.atlasBounds && glyph.planeBounds) {
                const ab = glyph.atlasBounds;
                const pb = glyph.planeBounds;

                const x0 = cursorX + pb.left * fontSize;
                const y0 = y + (1 - pb.top) * fontSize;
                const x1 = cursorX + pb.right * fontSize;
                const y1 = y + (1 - pb.bottom) * fontSize;

                const u0 = ab.left / atlas.width;
                const v0 = 1 - ab.top / atlas.height;
                const u1 = ab.right / atlas.width;
                const v1 = 1 - ab.bottom / atlas.height;

                vertices.push(
                    x0, y0, u0, v0,
                    x1, y0, u1, v0,
                    x0, y1, u0, v1,
                    x1, y0, u1, v0,
                    x0, y1, u0, v1,
                    x1, y1, u1, v1
                );
            }

            cursorX += glyph.advance * fontSize;
        }

        if (vertices.length === 0) return;

        const textBuffer = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, textBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(vertices), gl.STREAM_DRAW);

        gl.enableVertexAttribArray(this.textShader.attribs.a_pos);
        gl.enableVertexAttribArray(this.textShader.attribs.a_uv);
        gl.vertexAttribPointer(this.textShader.attribs.a_pos, 2, gl.FLOAT, false, 16, 0);
        gl.vertexAttribPointer(this.textShader.attribs.a_uv, 2, gl.FLOAT, false, 16, 8);

        gl.drawArrays(gl.TRIANGLES, 0, vertices.length / 4);

        gl.disableVertexAttribArray(this.textShader.attribs.a_pos);
        gl.disableVertexAttribArray(this.textShader.attribs.a_uv);
        gl.deleteBuffer(textBuffer);
    }

    /**
     * Render Clay render commands from WASM
     *
     * @param {Object} wasm - WASM exports
     * @param {number} commandCount - Number of render commands
     * @param {Float32Array} projMatrix - Projection matrix
     * @param {string} prefix - Function prefix (default: 'map_cmd_', alt: 'cc_clay_cmd_')
     */
    renderClayCommands(wasm, commandCount, projMatrix, prefix = 'map_cmd_') {
        if (this.contextLost) return;
        const gl = this.gl;
        const memory = new Uint8Array(wasm.memory.buffer);
        const scissorStack = [];

        // Get accessor functions based on prefix
        const cmd = {
            type: wasm[prefix + 'type'],
            x: wasm[prefix + 'x'],
            y: wasm[prefix + 'y'],
            w: wasm[prefix + 'w'],
            h: wasm[prefix + 'h'],
            rect_color: wasm[prefix + 'rect_color'],
            rect_radius: wasm[prefix + 'rect_radius'],
            text_str: wasm[prefix + 'text_str'],
            text_len: wasm[prefix + 'text_len'],
            text_color: wasm[prefix + 'text_color'],
            text_size: wasm[prefix + 'text_size'],
            border_color: wasm[prefix + 'border_color'],
            border_radius: wasm[prefix + 'border_radius'],
            border_width: wasm[prefix + 'border_width'],
        };

        for (let i = 0; i < commandCount; i++) {
            const cmdType = cmd.type(i);
            const x = cmd.x(i);
            const y = cmd.y(i);
            const w = cmd.w(i);
            const h = cmd.h(i);

            switch (cmdType) {
                case CLAY_CMD_RECTANGLE: {
                    const color = unpackColor(cmd.rect_color(i));
                    const radius = cmd.rect_radius(i);
                    this.renderRect(x, y, w, h, color, radius, 0, null, projMatrix);
                    break;
                }

                case CLAY_CMD_TEXT: {
                    const strPtr = cmd.text_str(i);
                    const strLen = cmd.text_len(i);
                    const color = unpackColor(cmd.text_color(i));
                    const fontSize = cmd.text_size(i);

                    let text = '';
                    for (let j = 0; j < strLen; j++) {
                        text += String.fromCharCode(memory[strPtr + j]);
                    }

                    this.renderText(text, x, y, fontSize, color, projMatrix);
                    break;
                }

                case CLAY_CMD_BORDER: {
                    const color = unpackColor(cmd.border_color(i));
                    const radius = cmd.border_radius(i);
                    const borderWidth = cmd.border_width(i);
                    this.renderRect(x, y, w, h, [0, 0, 0, 0], radius, borderWidth, color, projMatrix);
                    break;
                }

                case CLAY_CMD_SCISSOR_START: {
                    const dpr = window.devicePixelRatio || 1;
                    scissorStack.push({ x, y, w, h });
                    gl.enable(gl.SCISSOR_TEST);
                    gl.scissor(x * dpr, (this.height - y - h) * dpr, w * dpr, h * dpr);
                    break;
                }

                case CLAY_CMD_SCISSOR_END: {
                    if (scissorStack.length === 0) {
                        // Underflow - mismatched scissor commands, just disable
                        gl.disable(gl.SCISSOR_TEST);
                        break;
                    }
                    scissorStack.pop();
                    if (scissorStack.length === 0) {
                        gl.disable(gl.SCISSOR_TEST);
                    } else {
                        const s = scissorStack[scissorStack.length - 1];
                        const dpr = window.devicePixelRatio || 1;
                        gl.scissor(s.x * dpr, (this.height - s.y - s.h) * dpr, s.w * dpr, s.h * dpr);
                    }
                    break;
                }
            }
        }
    }
}
