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
 * Bezel fragment shader - renders RobCo-style monitor frame.
 * The screen content is composited in the center.
 */
export const BEZEL_FS = `
    precision mediump float;

    uniform sampler2D u_screen;
    uniform vec2 u_resolution;
    uniform float u_time;
    uniform float u_bezelWidth;    // Bezel thickness (0.08 = 8%)
    uniform float u_enabled;       // 0 or 1

    varying vec2 v_uv;

    // Pseudo-random for texture variation
    float hash(vec2 p) {
        return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
    }

    // Brushed metal noise
    float metalNoise(vec2 uv) {
        vec2 i = floor(uv * 200.0);
        return hash(i) * 0.15;
    }

    void main() {
        if (u_enabled < 0.5) {
            // Bezel disabled - just show screen
            gl_FragColor = texture2D(u_screen, v_uv);
            return;
        }

        float bw = u_bezelWidth;
        float cornerRadius = 0.02;

        // Screen area (inset by bezel width)
        vec2 screenMin = vec2(bw);
        vec2 screenMax = vec2(1.0 - bw);

        // Check if we're in screen area
        if (v_uv.x > screenMin.x && v_uv.x < screenMax.x &&
            v_uv.y > screenMin.y && v_uv.y < screenMax.y) {

            // Map UV to screen content
            vec2 screenUV = (v_uv - screenMin) / (screenMax - screenMin);
            vec3 screen = texture2D(u_screen, screenUV).rgb;

            // Inner bevel shadow
            float innerDist = min(
                min(v_uv.x - screenMin.x, screenMax.x - v_uv.x),
                min(v_uv.y - screenMin.y, screenMax.y - v_uv.y)
            );
            float innerShadow = smoothstep(0.0, 0.015, innerDist);
            screen *= 0.7 + 0.3 * innerShadow;

            gl_FragColor = vec4(screen, 1.0);
            return;
        }

        // We're in bezel area
        vec3 bezelColor;

        // Base metal color with gradient
        float gradY = v_uv.y;
        vec3 metalBase = mix(
            vec3(0.22, 0.21, 0.18),  // Dark olive-gray
            vec3(0.32, 0.31, 0.26),  // Lighter
            gradY * 0.5 + 0.25
        );

        // Add brushed metal texture
        metalBase += metalNoise(v_uv) * vec3(0.08, 0.07, 0.05);

        // Top highlight
        if (v_uv.y > 0.92) {
            metalBase += vec3(0.08) * (v_uv.y - 0.92) / 0.08;
        }

        // Bottom shadow
        if (v_uv.y < 0.08) {
            metalBase -= vec3(0.05) * (0.08 - v_uv.y) / 0.08;
        }

        bezelColor = metalBase;

        // === Nameplate (top center) ===
        vec2 npCenter = vec2(0.5, 0.96);
        vec2 npSize = vec2(0.28, 0.022);
        vec2 npDist = abs(v_uv - npCenter);
        if (npDist.x < npSize.x && npDist.y < npSize.y) {
            // Brass/bronze nameplate
            float npGrad = (v_uv.y - (npCenter.y - npSize.y)) / (npSize.y * 2.0);
            vec3 brass = mix(
                vec3(0.55, 0.45, 0.25),
                vec3(0.75, 0.65, 0.40),
                npGrad
            );
            // Engraved look
            brass *= 0.9 + 0.1 * sin(v_uv.x * 800.0);
            bezelColor = brass;
        }

        // === Vents (top left) ===
        vec2 ventStart = vec2(0.05, 0.955);
        for (int i = 0; i < 4; i++) {
            float vx = ventStart.x + float(i) * 0.025;
            if (v_uv.x > vx && v_uv.x < vx + 0.018 &&
                v_uv.y > 0.95 && v_uv.y < 0.97) {
                bezelColor = vec3(0.08, 0.08, 0.06); // Dark vent slot
            }
        }

        // === Indicator LED (top right) ===
        vec2 ledPos = vec2(0.92, 0.96);
        float ledDist = length(v_uv - ledPos);
        if (ledDist < 0.012) {
            // LED glow
            float pulse = 0.7 + 0.3 * sin(u_time * 2.0);
            vec3 ledColor = vec3(0.2, 0.9, 0.3) * pulse;
            float ledFalloff = smoothstep(0.012, 0.004, ledDist);
            bezelColor = mix(bezelColor, ledColor, ledFalloff);
        }

        // === Screws (bottom corners) ===
        vec2 screwPositions[2];
        screwPositions[0] = vec2(0.06, 0.04);
        screwPositions[1] = vec2(0.94, 0.04);

        for (int i = 0; i < 2; i++) {
            float screwDist = length(v_uv - screwPositions[i]);
            if (screwDist < 0.015) {
                // Screw head
                float screwGrad = screwDist / 0.015;
                vec3 screwColor = mix(vec3(0.4, 0.38, 0.32), vec3(0.25, 0.24, 0.20), screwGrad);
                // Slot
                vec2 toCenter = v_uv - screwPositions[i];
                if (abs(toCenter.x) < 0.008 && abs(toCenter.y) < 0.002) {
                    screwColor = vec3(0.1);
                }
                bezelColor = screwColor;
            }
        }

        // === Model label (bottom center) ===
        vec2 lblCenter = vec2(0.5, 0.035);
        vec2 lblSize = vec2(0.15, 0.012);
        vec2 lblDist = abs(v_uv - lblCenter);
        if (lblDist.x < lblSize.x && lblDist.y < lblSize.y) {
            // Slightly darker inset for label area
            bezelColor *= 0.85;
        }

        // === Outer edge bevel ===
        float edgeDist = min(
            min(v_uv.x, 1.0 - v_uv.x),
            min(v_uv.y, 1.0 - v_uv.y)
        );
        if (edgeDist < 0.008) {
            // Dark outer edge
            bezelColor *= 0.6 + 0.4 * (edgeDist / 0.008);
        }

        // === Inner edge highlight (around screen) ===
        float toScreenX = min(abs(v_uv.x - screenMin.x), abs(v_uv.x - screenMax.x));
        float toScreenY = min(abs(v_uv.y - screenMin.y), abs(v_uv.y - screenMax.y));
        float toScreen = min(toScreenX, toScreenY);

        if (toScreen < 0.012) {
            // Inner bevel - darker toward screen
            bezelColor *= 0.7 + 0.3 * (toScreen / 0.012);
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
