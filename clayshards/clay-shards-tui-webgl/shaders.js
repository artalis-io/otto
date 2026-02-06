/**
 * ClayShards TUI WebGL - Shader Sources
 *
 * Contains shaders for:
 * - Terminal character grid rendering
 * - CRT post-processing effects
 */

/* ============================================================================
 * Terminal Grid Shaders
 *
 * Renders the character grid using a font atlas.
 * Each cell is a textured quad with fg/bg colors.
 * ============================================================================ */

/**
 * Terminal vertex shader.
 * Positions cell quads in a grid layout.
 *
 * Attributes (per-instance):
 *   a_cellPos   - (col, row) grid position
 *   a_codepoint - Glyph atlas index
 *   a_fgColor   - Foreground color RGB (0-1)
 *   a_bgColor   - Background color RGB (0-1)
 *
 * Varyings:
 *   v_uv      - UV coordinates for font atlas
 *   v_fgColor - Foreground color
 *   v_bgColor - Background color
 */
export const TERM_VS = `
    attribute vec2 a_vertex;      // Quad vertex (0,0), (1,0), (0,1), (1,1)
    attribute vec2 a_cellPos;     // Grid position (col, row) - instanced
    attribute float a_codepoint;  // Glyph codepoint - instanced
    attribute vec3 a_fgColor;     // Foreground RGB - instanced
    attribute vec3 a_bgColor;     // Background RGB - instanced

    uniform mat4 u_proj;
    uniform vec2 u_cellSize;      // Cell size in pixels (width, height)
    uniform vec2 u_atlasGrid;     // Atlas grid size (cols, rows), e.g., (16, 16)

    varying vec2 v_uv;
    varying vec3 v_fgColor;
    varying vec3 v_bgColor;

    void main() {
        // Position in pixels
        vec2 pos = (a_cellPos + a_vertex) * u_cellSize;
        gl_Position = u_proj * vec4(pos, 0.0, 1.0);

        // Calculate UV from codepoint
        // Glyph index in atlas (row-major order)
        float glyphIndex = a_codepoint;
        float col = mod(glyphIndex, u_atlasGrid.x);
        float row = floor(glyphIndex / u_atlasGrid.x);

        // UV for this vertex within the glyph cell
        vec2 glyphUV = vec2(col, row) + a_vertex;
        v_uv = glyphUV / u_atlasGrid;

        v_fgColor = a_fgColor;
        v_bgColor = a_bgColor;
    }
`;

/**
 * Terminal fragment shader.
 * Samples font atlas and composites fg/bg colors.
 */
export const TERM_FS = `
    precision mediump float;

    uniform sampler2D u_fontAtlas;

    varying vec2 v_uv;
    varying vec3 v_fgColor;
    varying vec3 v_bgColor;

    void main() {
        // Sample glyph alpha from atlas (assuming white-on-transparent or alpha channel)
        float alpha = texture2D(u_fontAtlas, v_uv).a;

        // Composite: bg where glyph is transparent, fg where opaque
        vec3 color = mix(v_bgColor, v_fgColor, alpha);
        gl_FragColor = vec4(color, 1.0);
    }
`;

/**
 * Alternative terminal fragment shader using red channel for bitmap fonts.
 * Some font atlases store glyph in R or all RGB channels instead of alpha.
 */
export const TERM_FS_BITMAP = `
    precision mediump float;

    uniform sampler2D u_fontAtlas;

    varying vec2 v_uv;
    varying vec3 v_fgColor;
    varying vec3 v_bgColor;

    void main() {
        // Sample glyph intensity from red channel (for bitmap fonts)
        float alpha = texture2D(u_fontAtlas, v_uv).r;

        // Composite: bg where glyph is transparent, fg where opaque
        vec3 color = mix(v_bgColor, v_fgColor, alpha);
        gl_FragColor = vec4(color, 1.0);
    }
`;

/* ============================================================================
 * CRT Post-Processing Shaders
 *
 * Renders terminal to framebuffer, then applies retro CRT effects.
 * ============================================================================ */

/**
 * CRT vertex shader - simple fullscreen quad.
 */
export const CRT_VS = `
    attribute vec2 a_pos;
    uniform vec2 u_scale;
    uniform vec2 u_offset;
    varying vec2 v_uv;

    void main() {
        vec2 scaled = a_pos * u_scale + u_offset;
        gl_Position = vec4(scaled * 2.0 - 1.0, 0.0, 1.0);
        v_uv = a_pos;
    }
`;

/**
 * CRT fragment shader.
 *
 * Effects:
 * - Scanlines: Horizontal lines with configurable intensity
 * - Curvature: Barrel distortion for CRT shape
 * - Vignette: Darkening at screen edges
 * - Chromatic aberration: RGB color fringing
 * - Flicker: Temporal brightness variation
 * - Color mode: Amber, green, white phosphor, or full RGB
 */
export const CRT_FS = `
    precision mediump float;

    uniform sampler2D u_terminal;
    uniform vec2 u_resolution;
    uniform float u_time;

    // Effect parameters (0 = disabled)
    uniform float u_scanlines;      // Scanline intensity (0.0 - 1.0)
    uniform float u_curvature;      // Barrel distortion (0.0 - 0.1)
    uniform float u_vignette;       // Vignette strength (0.0 - 1.0)
    uniform float u_chromatic;      // Chromatic aberration (0.0 - 0.01)
    uniform float u_flicker;        // Flicker amount (0.0 - 0.1)
    uniform float u_glow;           // Phosphor glow (0.0 - 1.0) - placeholder
    uniform int u_colorMode;        // 0=amber, 1=green, 2=white, 3=rgb
    uniform float u_alpha;          // Overall brightness (for power-off)

    varying vec2 v_uv;

    // Apply barrel distortion for CRT curvature
    vec2 curveUV(vec2 uv) {
        if (u_curvature <= 0.0) return uv;

        vec2 centered = uv * 2.0 - 1.0;
        float r2 = dot(centered, centered);
        centered *= 1.0 + u_curvature * r2;
        return centered * 0.5 + 0.5;
    }

    // Apply phosphor color tint
    vec3 applyColorMode(vec3 color) {
        if (u_colorMode == 3) return color;  // RGB - no tint

        float luma = dot(color, vec3(0.299, 0.587, 0.114));

        if (u_colorMode == 0) {
            // Amber (P3 phosphor)
            return vec3(1.0, 0.7, 0.0) * luma;
        } else if (u_colorMode == 1) {
            // Green (P1 phosphor)
            return vec3(0.2, 1.0, 0.2) * luma;
        } else {
            // White (P4 phosphor - blue-white)
            return vec3(0.9, 0.9, 1.0) * luma;
        }
    }

    void main() {
        vec2 uv = curveUV(v_uv);

        // Discard pixels outside curved screen
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
            gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }

        vec3 color;

        // Chromatic aberration
        if (u_chromatic > 0.0) {
            float r = texture2D(u_terminal, uv + vec2(u_chromatic, 0.0)).r;
            float g = texture2D(u_terminal, uv).g;
            float b = texture2D(u_terminal, uv - vec2(u_chromatic, 0.0)).b;
            color = vec3(r, g, b);
        } else {
            color = texture2D(u_terminal, uv).rgb;
        }

        // Apply color mode (phosphor tint)
        color = applyColorMode(color);

        // Scanlines
        if (u_scanlines > 0.0) {
            float scanY = v_uv.y * u_resolution.y;
            float scanline = sin(scanY * 3.14159) * 0.5 + 0.5;
            color *= 1.0 - u_scanlines * (1.0 - scanline);
        }

        // Vignette
        if (u_vignette > 0.0) {
            float dist = length(v_uv - 0.5);
            float vignette = 1.0 - dist * u_vignette * 2.0;
            color *= clamp(vignette, 0.0, 1.0);
        }

        // Flicker
        if (u_flicker > 0.0) {
            float flicker = 1.0 - u_flicker * 0.5 * sin(u_time * 60.0);
            color *= flicker;
        }

        // Apply alpha (for power-off fade)
        color *= u_alpha;

        gl_FragColor = vec4(color, 1.0);
    }
`;

/* ============================================================================
 * Utility Shaders
 * ============================================================================ */

/**
 * Simple texture blit shader (for framebuffer to screen).
 */
export const BLIT_VS = `
    attribute vec2 a_pos;
    uniform vec2 u_scale;
    uniform vec2 u_offset;
    varying vec2 v_uv;

    void main() {
        vec2 scaled = a_pos * u_scale + u_offset;
        gl_Position = vec4(scaled * 2.0 - 1.0, 0.0, 1.0);
        v_uv = a_pos;
    }
`;

export const BLIT_FS = `
    precision mediump float;
    uniform sampler2D u_texture;
    uniform float u_alpha;
    uniform int u_colorMode;
    varying vec2 v_uv;

    void main() {
        vec4 color = texture2D(u_texture, v_uv);

        // Apply color mode (0=amber, 1=green, 2=white, 3=rgb)
        vec3 rgb = color.rgb;
        if (u_colorMode == 0) {
            float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
            rgb = vec3(1.0, 0.7, 0.0) * luma;
        } else if (u_colorMode == 1) {
            float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
            rgb = vec3(0.2, 1.0, 0.2) * luma;
        } else if (u_colorMode == 2) {
            float luma = dot(rgb, vec3(0.299, 0.587, 0.114));
            rgb = vec3(0.9, 0.9, 1.0) * luma;
        }

        gl_FragColor = vec4(rgb * u_alpha, 1.0);
    }
`;

/* ============================================================================
 * Batched Terminal Shaders (for WebGL1 without instancing)
 *
 * Each vertex includes position, UV, and colors inline.
 * More efficient than per-cell draw calls.
 * ============================================================================ */

/**
 * Batched terminal vertex shader.
 * Position and UV are per-vertex, colors are per-vertex too.
 */
export const TERM_BATCHED_VS = `
    attribute vec2 a_pos;         // Pixel position
    attribute vec2 a_uv;          // Atlas UV
    attribute vec3 a_fgColor;     // Foreground RGB (0-1)
    attribute vec3 a_bgColor;     // Background RGB (0-1)

    uniform mat4 u_proj;

    varying vec2 v_uv;
    varying vec3 v_fgColor;
    varying vec3 v_bgColor;

    void main() {
        gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
        v_uv = a_uv;
        v_fgColor = a_fgColor;
        v_bgColor = a_bgColor;
    }
`;

/**
 * Batched terminal fragment shader (same as regular terminal shader).
 */
export const TERM_BATCHED_FS = `
    precision mediump float;

    uniform sampler2D u_fontAtlas;

    varying vec2 v_uv;
    varying vec3 v_fgColor;
    varying vec3 v_bgColor;

    void main() {
        // Sample glyph intensity from red channel (for bitmap fonts)
        float alpha = texture2D(u_fontAtlas, v_uv).r;

        // Composite: bg where glyph is transparent, fg where opaque
        vec3 color = mix(v_bgColor, v_fgColor, alpha);
        gl_FragColor = vec4(color, 1.0);
    }
`;
