/**
 * ClayShards TUI WebGL - CRT Effects
 *
 * Post-processing pipeline for retro CRT monitor effects.
 * Renders the terminal to a framebuffer, then applies effects to the screen.
 */

import { CRT_VS, CRT_FS, BLIT_VS, BLIT_FS } from './shaders.js';

/**
 * Color mode constants.
 */
export const CRT_COLOR_AMBER = 0;
export const CRT_COLOR_GREEN = 1;
export const CRT_COLOR_WHITE = 2;
export const CRT_COLOR_RGB = 3;

/**
 * Create a shader program.
 */
function createShader(gl, type, source) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);

    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        console.error('CRT shader compile error:', gl.getShaderInfoLog(shader));
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
        console.error('CRT program link error:', gl.getProgramInfoLog(program));
        gl.deleteProgram(program);
        return null;
    }

    // Cache uniform locations
    const wrapper = { program, uniforms: {} };
    const numUniforms = gl.getProgramParameter(program, gl.ACTIVE_UNIFORMS);
    for (let i = 0; i < numUniforms; i++) {
        const info = gl.getActiveUniform(program, i);
        wrapper.uniforms[info.name] = gl.getUniformLocation(program, info.name);
    }

    return wrapper;
}

/**
 * CRT post-processing effect renderer.
 */
export class CrtEffects {
    /**
     * @param {WebGLRenderingContext} gl - WebGL context
     */
    constructor(gl) {
        this.gl = gl;
        this.enabled = false;
        this.time = 0;

        // Effect parameters
        this.params = {
            scanlines: 0.3,      // Scanline intensity (0-1)
            curvature: 0.03,     // Barrel distortion (0-0.1)
            vignette: 0.2,       // Edge darkening (0-1)
            chromatic: 0.001,    // RGB split (0-0.01)
            flicker: 0.02,       // Brightness variation (0-0.1)
            glow: 0.1,           // Phosphor bloom (0-1, placeholder)
            colorMode: CRT_COLOR_RGB  // Phosphor color
        };

        // Glass reflection is separate from CRT effects (ambient light on glass)
        this.glassEnabled = false;

        this._initResources();
    }

    /**
     * Initialize WebGL resources.
     */
    _initResources() {
        const gl = this.gl;

        // Create shaders
        this.crtShader = createProgram(gl, CRT_VS, CRT_FS);
        this.blitShader = createProgram(gl, BLIT_VS, BLIT_FS);

        // Create fullscreen quad
        this.quadBuffer = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
            0, 0,
            1, 0,
            0, 1,
            1, 0,
            1, 1,
            0, 1
        ]), gl.STATIC_DRAW);

        // Framebuffer for rendering terminal
        this.framebuffer = gl.createFramebuffer();
        this.texture = null;
        this.width = 0;
        this.height = 0;
    }

    /**
     * Resize the framebuffer.
     * @param {number} width - Width in pixels
     * @param {number} height - Height in pixels
     */
    resize(width, height) {
        if (width === this.width && height === this.height) return;

        const gl = this.gl;

        // Delete old texture
        if (this.texture) {
            gl.deleteTexture(this.texture);
        }

        // Create new texture
        this.texture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

        // Attach to framebuffer
        gl.bindFramebuffer(gl.FRAMEBUFFER, this.framebuffer);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.texture, 0);

        // Check completeness
        const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
        if (status !== gl.FRAMEBUFFER_COMPLETE) {
            console.error('CRT framebuffer incomplete:', status);
        }

        gl.bindFramebuffer(gl.FRAMEBUFFER, null);

        this.width = width;
        this.height = height;
    }

    /**
     * Begin rendering to the CRT framebuffer.
     * Call this before rendering the terminal.
     */
    begin() {
        if (!this.enabled) return;

        const gl = this.gl;
        gl.bindFramebuffer(gl.FRAMEBUFFER, this.framebuffer);
        gl.viewport(0, 0, this.width, this.height);
    }

    /**
     * End rendering and apply CRT effects to screen.
     * Call this after rendering the terminal.
     *
     * @param {number} dt - Delta time in seconds
     */
    end(dt) {
        if (!this.enabled) return;

        const gl = this.gl;

        this.time += dt;

        // Bind default framebuffer (screen)
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, this.width, this.height);

        // Use CRT shader
        gl.useProgram(this.crtShader.program);

        // Set uniforms
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        gl.uniform1i(this.crtShader.uniforms.u_terminal, 0);
        gl.uniform2f(this.crtShader.uniforms.u_resolution, this.width, this.height);
        gl.uniform1f(this.crtShader.uniforms.u_time, this.time);

        // Effect parameters
        gl.uniform1f(this.crtShader.uniforms.u_scanlines, this.params.scanlines);
        gl.uniform1f(this.crtShader.uniforms.u_curvature, this.params.curvature);
        gl.uniform1f(this.crtShader.uniforms.u_vignette, this.params.vignette);
        gl.uniform1f(this.crtShader.uniforms.u_chromatic, this.params.chromatic);
        gl.uniform1f(this.crtShader.uniforms.u_flicker, this.params.flicker);
        gl.uniform1f(this.crtShader.uniforms.u_glow, this.params.glow);
        gl.uniform1f(this.crtShader.uniforms.u_glassReflection, this.glassEnabled ? 1.0 : 0.0);
        gl.uniform1i(this.crtShader.uniforms.u_colorMode, this.params.colorMode);

        // Default transform (fullscreen, full brightness)
        gl.uniform2f(this.crtShader.uniforms.u_scale, 1.0, 1.0);
        gl.uniform2f(this.crtShader.uniforms.u_offset, 0.0, 0.0);
        gl.uniform1f(this.crtShader.uniforms.u_alpha, 1.0);

        // Draw fullscreen quad
        gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuffer);
        const posLoc = gl.getAttribLocation(this.crtShader.program, 'a_pos');
        gl.enableVertexAttribArray(posLoc);
        gl.vertexAttribPointer(posLoc, 2, gl.FLOAT, false, 0, 0);

        gl.drawArrays(gl.TRIANGLES, 0, 6);

        gl.disableVertexAttribArray(posLoc);
    }

    /**
     * Render glass reflection overlay only (when CRT effects are off).
     * Call this after rendering the terminal directly when this.enabled is false.
     */
    renderGlassOnly() {
        if (!this.glassEnabled) return;

        const gl = this.gl;

        // Enable blending to overlay glass on top of terminal
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.ONE, gl.ONE); // Additive blending

        gl.useProgram(this.crtShader.program);

        // Create a black texture to use as terminal (we only want glass)
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        gl.uniform1i(this.crtShader.uniforms.u_terminal, 0);
        gl.uniform2f(this.crtShader.uniforms.u_resolution, this.width, this.height);
        gl.uniform1f(this.crtShader.uniforms.u_time, this.time);

        // Disable all CRT effects, only glass
        gl.uniform1f(this.crtShader.uniforms.u_scanlines, 0);
        gl.uniform1f(this.crtShader.uniforms.u_curvature, 0);
        gl.uniform1f(this.crtShader.uniforms.u_vignette, 0);
        gl.uniform1f(this.crtShader.uniforms.u_chromatic, 0);
        gl.uniform1f(this.crtShader.uniforms.u_flicker, 0);
        gl.uniform1f(this.crtShader.uniforms.u_glow, 0);
        gl.uniform1f(this.crtShader.uniforms.u_glassReflection, 1.0);
        gl.uniform1i(this.crtShader.uniforms.u_colorMode, CRT_COLOR_RGB);

        gl.uniform2f(this.crtShader.uniforms.u_scale, 1.0, 1.0);
        gl.uniform2f(this.crtShader.uniforms.u_offset, 0.0, 0.0);
        gl.uniform1f(this.crtShader.uniforms.u_alpha, 0.0); // No terminal content, just glass

        gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuffer);
        const posLoc = gl.getAttribLocation(this.crtShader.program, 'a_pos');
        gl.enableVertexAttribArray(posLoc);
        gl.vertexAttribPointer(posLoc, 2, gl.FLOAT, false, 0, 0);
        gl.drawArrays(gl.TRIANGLES, 0, 6);
        gl.disableVertexAttribArray(posLoc);

        gl.disable(gl.BLEND);
    }

    /**
     * Set effect parameters.
     * @param {Object} params - Effect parameters to update
     */
    setParams(params) {
        Object.assign(this.params, params);
    }

    /**
     * Enable or disable CRT effects.
     * @param {boolean} enabled
     */
    setEnabled(enabled) {
        this.enabled = enabled;
    }

    /**
     * Check if effects are enabled.
     */
    isEnabled() {
        return this.enabled;
    }

    /**
     * Set color mode.
     * @param {number} mode - CRT_COLOR_* constant
     */
    setColorMode(mode) {
        this.params.colorMode = mode;
    }

    /**
     * Enable/disable glass reflection effect.
     * Glass is independent of CRT effects - it's ambient light on the screen.
     * @param {boolean} enabled
     */
    setGlass(enabled) {
        this.glassEnabled = enabled;
    }

    /**
     * Check if glass reflection is enabled.
     */
    isGlassEnabled() {
        return this.glassEnabled;
    }

    /**
     * Apply a preset.
     * @param {string} preset - 'off', 'subtle', 'retro', 'arcade'
     */
    applyPreset(preset) {
        switch (preset) {
            case 'off':
                this.enabled = false;
                break;
            case 'subtle':
                this.enabled = true;
                this.params = {
                    scanlines: 0.1,
                    curvature: 0.01,
                    vignette: 0.1,
                    chromatic: 0.0005,
                    flicker: 0.01,
                    glow: 0.05,
                    colorMode: CRT_COLOR_RGB
                };
                break;
            case 'retro':
                this.enabled = true;
                this.params = {
                    scanlines: 0.3,
                    curvature: 0.03,
                    vignette: 0.2,
                    chromatic: 0.001,
                    flicker: 0.02,
                    glow: 0.1,
                    colorMode: CRT_COLOR_RGB
                };
                break;
            case 'arcade':
                this.enabled = true;
                this.params = {
                    scanlines: 0.5,
                    curvature: 0.05,
                    vignette: 0.3,
                    chromatic: 0.002,
                    flicker: 0.03,
                    glow: 0.2,
                    colorMode: CRT_COLOR_RGB
                };
                break;
            case 'amber':
                this.enabled = true;
                this.params = {
                    scanlines: 0.3,
                    curvature: 0.03,
                    vignette: 0.2,
                    chromatic: 0,
                    flicker: 0.02,
                    glow: 0.15,
                    colorMode: CRT_COLOR_AMBER
                };
                break;
            case 'green':
                this.enabled = true;
                this.params = {
                    scanlines: 0.3,
                    curvature: 0.03,
                    vignette: 0.2,
                    chromatic: 0,
                    flicker: 0.02,
                    glow: 0.15,
                    colorMode: CRT_COLOR_GREEN
                };
                break;
            default:
                console.warn(`Unknown CRT preset: ${preset}`);
        }
    }

    /**
     * Draw CRT power-off effect (shrinking to horizontal line then fade).
     * @param {number} progress - Animation progress (0-1), where 1 is fully off
     */
    drawPowerOff(progress) {
        const gl = this.gl;

        // Clear to black
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, this.width, this.height);
        gl.clearColor(0, 0, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);

        if (progress >= 1) return; // Fully off

        // Phase 1 (0-0.35): Vertical collapse to horizontal line
        // Phase 2 (0.35-0.6): Horizontal collapse to dot in center
        // Phase 3 (0.6-1): Dot fades out
        const phase1End = 0.35;
        const phase2End = 0.6;

        // Use CRT shader to maintain scanlines/chromatic effects
        gl.useProgram(this.crtShader.program);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        gl.uniform1i(this.crtShader.uniforms.u_terminal, 0);
        gl.uniform2f(this.crtShader.uniforms.u_resolution, this.width, this.height);
        gl.uniform1f(this.crtShader.uniforms.u_time, this.time);

        // Apply CRT effect parameters
        gl.uniform1f(this.crtShader.uniforms.u_scanlines, this.params.scanlines);
        gl.uniform1f(this.crtShader.uniforms.u_curvature, this.params.curvature);
        gl.uniform1f(this.crtShader.uniforms.u_vignette, this.params.vignette);
        gl.uniform1f(this.crtShader.uniforms.u_chromatic, this.params.chromatic);
        gl.uniform1f(this.crtShader.uniforms.u_flicker, this.params.flicker);
        gl.uniform1f(this.crtShader.uniforms.u_glow, this.params.glow);
        gl.uniform1f(this.crtShader.uniforms.u_glassReflection, this.glassEnabled ? 1.0 : 0.0);
        gl.uniform1i(this.crtShader.uniforms.u_colorMode, this.params.colorMode);

        gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuffer);
        const posLoc = gl.getAttribLocation(this.crtShader.program, 'a_pos');
        gl.enableVertexAttribArray(posLoc);
        gl.vertexAttribPointer(posLoc, 2, gl.FLOAT, false, 0, 0);

        let scaleX = 1.0, scaleY = 1.0, alpha = 1.0;

        if (progress < phase1End) {
            // Phase 1: Vertical collapse to horizontal line
            const t = progress / phase1End;
            scaleY = 1 - t * 0.97; // Shrink to 3% height
        } else if (progress < phase2End) {
            // Phase 2: Horizontal collapse to dot
            const t = (progress - phase1End) / (phase2End - phase1End);
            scaleY = 0.03; // Stay at 3% height
            scaleX = 1 - t * 0.95; // Shrink to 5% width
        } else {
            // Phase 3: Dot fades out
            const t = (progress - phase2End) / (1 - phase2End);
            scaleX = 0.05;
            scaleY = 0.03;
            alpha = 1 - t;
        }

        gl.uniform2f(this.crtShader.uniforms.u_scale, scaleX, scaleY);
        gl.uniform2f(this.crtShader.uniforms.u_offset, (1 - scaleX) / 2, (1 - scaleY) / 2);
        gl.uniform1f(this.crtShader.uniforms.u_alpha, alpha);

        gl.drawArrays(gl.TRIANGLES, 0, 6);
        gl.disableVertexAttribArray(posLoc);
    }

    /**
     * Clean up WebGL resources.
     */
    destroy() {
        const gl = this.gl;
        if (this.crtShader) {
            gl.deleteProgram(this.crtShader.program);
        }
        if (this.blitShader) {
            gl.deleteProgram(this.blitShader.program);
        }
        if (this.quadBuffer) {
            gl.deleteBuffer(this.quadBuffer);
        }
        if (this.framebuffer) {
            gl.deleteFramebuffer(this.framebuffer);
        }
        if (this.texture) {
            gl.deleteTexture(this.texture);
        }
    }
}
