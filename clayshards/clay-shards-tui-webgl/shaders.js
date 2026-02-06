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
    uniform float u_glassReflection; // Glass reflection intensity (0.0 - 1.0)
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

        // Apply alpha (for power-off fade) - but keep glass reflection
        color *= u_alpha;

        // Glass reflection - simulates ambient light reflecting off curved CRT glass
        // This stays visible even when screen is off (natural light, not phosphor)
        if (u_glassReflection > 0.0) {
            // Main reflection arc in upper-left area
            vec2 reflectCenter = vec2(0.3, 0.25);
            float reflectDist = length(v_uv - reflectCenter);
            float arc = smoothstep(0.35, 0.15, reflectDist) * smoothstep(0.0, 0.2, reflectDist);

            // Secondary smaller highlight
            vec2 highlight = vec2(0.2, 0.15);
            float highlightDist = length(v_uv - highlight);
            float spot = smoothstep(0.12, 0.0, highlightDist);

            // Subtle edge reflection on right side
            float edgeReflect = smoothstep(0.7, 1.0, v_uv.x) * smoothstep(0.8, 0.3, v_uv.y);

            // Combine reflections - stays visible even when alpha=0
            float reflection = (arc * 0.6 + spot * 0.4 + edgeReflect * 0.2) * u_glassReflection;
            color += vec3(reflection * 0.15);
        }

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
 * Monitor Bezel Shader
 *
 * Renders a retro terminal housing around the screen content.
 * ============================================================================ */

export const BEZEL_VS = `
    attribute vec2 a_pos;
    varying vec2 v_uv;

    void main() {
        gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0);
        v_uv = a_pos;
    }
`;

/**
 * Bezel fragment shader - Fallout-style terminal frame.
 * Clean, utilitarian design with rounded screen opening.
 */
export const BEZEL_FS = `
    precision highp float;

    uniform sampler2D u_screen;
    uniform vec2 u_resolution;
    uniform float u_time;
    uniform float u_bezelWidth;
    uniform float u_curvature;
    uniform float u_enabled;

    varying vec2 v_uv;

    float hash(vec2 p) {
        return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
    }

    float noise(vec2 p) {
        vec2 i = floor(p);
        vec2 f = fract(p);
        f = f * f * (3.0 - 2.0 * f);
        return mix(mix(hash(i), hash(i + vec2(1,0)), f.x),
                   mix(hash(i + vec2(0,1)), hash(i + vec2(1,1)), f.x), f.y);
    }

    // Rounded rectangle SDF
    float roundedRect(vec2 p, vec2 size, float radius) {
        vec2 d = abs(p) - size + radius;
        return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
    }

    void main() {
        if (u_enabled < 0.5) {
            gl_FragColor = texture2D(u_screen, v_uv);
            return;
        }

        float bw = u_bezelWidth;
        vec2 center = vec2(0.5);

        // Screen opening with rounded corners
        vec2 screenSize = vec2(0.5 - bw, 0.5 - bw);
        float cornerRadius = 0.03 + u_curvature * 0.3;
        float screenDist = roundedRect(v_uv - center, screenSize, cornerRadius);

        // Inside screen area
        if (screenDist < 0.0) {
            vec2 screenMin = vec2(bw);
            vec2 screenMax = vec2(1.0 - bw);
            vec2 screenUV = (v_uv - screenMin) / (screenMax - screenMin);
            vec3 screen = texture2D(u_screen, screenUV).rgb;

            // Soft shadow at screen edges
            float edgeShadow = smoothstep(0.0, -0.025, screenDist);
            screen *= 0.7 + 0.3 * edgeShadow;

            gl_FragColor = vec4(screen, 1.0);
            return;
        }

        // === BEZEL ===

        // Base color - dark industrial olive/gray
        vec3 bezelColor = vec3(0.16, 0.15, 0.12);

        // Subtle surface texture
        float grain = noise(v_uv * 150.0) * 0.04;
        bezelColor += grain;

        // Simple top-down lighting gradient
        float lightGrad = 0.85 + 0.3 * v_uv.y;
        bezelColor *= lightGrad;

        // Outer frame - thick raised border
        float outerDist = min(min(v_uv.x, 1.0 - v_uv.x), min(v_uv.y, 1.0 - v_uv.y));
        float frameWidth = 0.025;

        if (outerDist < frameWidth) {
            float t = outerDist / frameWidth;
            // Beveled edge - lit on top/left, shadowed on bottom/right
            float bevel = 0.0;
            if (v_uv.y > 0.98 || v_uv.x < 0.02) bevel = 0.15 * (1.0 - t);
            if (v_uv.y < 0.02 || v_uv.x > 0.98) bevel = -0.1 * (1.0 - t);
            bezelColor += bevel;
            bezelColor *= 0.85 + 0.15 * t;
        }

        // Inner lip around screen (recessed edge)
        if (screenDist < 0.02) {
            float t = screenDist / 0.02;
            // Dark recess going into screen
            bezelColor *= 0.6 + 0.4 * t;
            // Lit inner lip
            if (t > 0.5) {
                float lipT = (t - 0.5) / 0.5;
                bezelColor += vec3(0.08) * lipT * (0.5 + 0.5 * v_uv.y);
            }
        }

        // === Simple details ===

        // Small nameplate (top)
        vec2 npPos = vec2(0.5, 0.97);
        vec2 npSize = vec2(0.15, 0.012);
        if (abs(v_uv.x - npPos.x) < npSize.x && abs(v_uv.y - npPos.y) < npSize.y) {
            float npEdge = min(npSize.x - abs(v_uv.x - npPos.x), npSize.y - abs(v_uv.y - npPos.y));
            vec3 plate = vec3(0.5, 0.42, 0.25); // Brass-ish
            if (npEdge < 0.003) plate *= 0.7;
            bezelColor = plate * (0.8 + 0.2 * v_uv.y);
        }

        // Power indicator LED (top right)
        vec2 ledPos = vec2(0.92, 0.97);
        float ledDist = length(v_uv - ledPos);
        if (ledDist < 0.008) {
            float pulse = 0.7 + 0.3 * sin(u_time * 2.0);
            float glow = smoothstep(0.008, 0.002, ledDist);
            bezelColor = mix(bezelColor, vec3(0.2, 0.9, 0.3) * pulse, glow);
        }

        // Speaker grille (bottom) - horizontal lines
        if (v_uv.y < 0.05 && v_uv.x > 0.35 && v_uv.x < 0.65) {
            float lineY = fract(v_uv.y * 60.0);
            if (lineY < 0.4) {
                bezelColor *= 0.3; // Dark slots
            }
        }

        gl_FragColor = vec4(bezelColor, 1.0);
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
