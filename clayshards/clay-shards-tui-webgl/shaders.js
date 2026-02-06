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
 * Bezel fragment shader - renders RobCo-style monitor frame with 3D depth.
 * Features: normal mapping, industrial coating texture, curved screen cutout.
 */
export const BEZEL_FS = `
    precision highp float;

    uniform sampler2D u_screen;
    uniform vec2 u_resolution;
    uniform float u_time;
    uniform float u_bezelWidth;    // Bezel thickness (0.08 = 8%)
    uniform float u_curvature;     // Screen curvature to match CRT
    uniform float u_enabled;       // 0 or 1

    varying vec2 v_uv;

    // Light direction (from top-left-front)
    const vec3 lightDir = normalize(vec3(-0.4, 0.5, 1.0));
    const vec3 lightColor = vec3(1.0, 0.98, 0.95);
    const float ambient = 0.35;

    // Hash functions for procedural textures
    float hash(vec2 p) {
        return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
    }

    float hash3(vec3 p) {
        return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
    }

    // Smooth noise
    float noise(vec2 p) {
        vec2 i = floor(p);
        vec2 f = fract(p);
        f = f * f * (3.0 - 2.0 * f);

        float a = hash(i);
        float b = hash(i + vec2(1.0, 0.0));
        float c = hash(i + vec2(0.0, 1.0));
        float d = hash(i + vec2(1.0, 1.0));

        return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    }

    // FBM for industrial coating texture
    float fbm(vec2 p) {
        float value = 0.0;
        float amplitude = 0.5;
        for (int i = 0; i < 4; i++) {
            value += amplitude * noise(p);
            p *= 2.0;
            amplitude *= 0.5;
        }
        return value;
    }

    // Compute normal from height field (for bump mapping)
    vec3 computeNormal(vec2 uv, float scale) {
        float eps = 0.002;
        float h = fbm(uv * scale);
        float hx = fbm((uv + vec2(eps, 0.0)) * scale);
        float hy = fbm((uv + vec2(0.0, eps)) * scale);

        vec3 normal = normalize(vec3(
            (h - hx) / eps * 0.15,
            (h - hy) / eps * 0.15,
            1.0
        ));
        return normal;
    }

    // Apply barrel distortion (same as CRT shader) for screen edge
    vec2 curveUV(vec2 uv, float curvature) {
        vec2 centered = uv * 2.0 - 1.0;
        float r2 = dot(centered, centered);
        centered *= 1.0 + curvature * r2;
        return centered * 0.5 + 0.5;
    }

    // Check if point is inside curved screen area
    bool isInScreen(vec2 uv, float bw, float curvature) {
        vec2 screenMin = vec2(bw);
        vec2 screenMax = vec2(1.0 - bw);

        // First check rectangular bounds
        if (uv.x < screenMin.x || uv.x > screenMax.x ||
            uv.y < screenMin.y || uv.y > screenMax.y) {
            return false;
        }

        // Map to screen space and check curvature
        vec2 screenUV = (uv - screenMin) / (screenMax - screenMin);
        vec2 curved = curveUV(screenUV, curvature);

        // If curved UV is outside 0-1, we're in the curved corner area
        return curved.x >= 0.0 && curved.x <= 1.0 &&
               curved.y >= 0.0 && curved.y <= 1.0;
    }

    // Distance to curved screen edge (for bevels)
    float distToScreenEdge(vec2 uv, float bw, float curvature) {
        vec2 screenMin = vec2(bw);
        vec2 screenMax = vec2(1.0 - bw);
        vec2 screenUV = (uv - screenMin) / (screenMax - screenMin);

        // Distance to rectangular edge
        float rectDist = min(
            min(uv.x - screenMin.x, screenMax.x - uv.x),
            min(uv.y - screenMin.y, screenMax.y - uv.y)
        );

        // Curved corner adjustment
        vec2 centered = screenUV * 2.0 - 1.0;
        float cornerDist = length(centered) - 1.0;
        float curveAdjust = curvature * cornerDist * 0.5;

        return rectDist - curveAdjust * 0.1;
    }

    void main() {
        if (u_enabled < 0.5) {
            gl_FragColor = texture2D(u_screen, v_uv);
            return;
        }

        float bw = u_bezelWidth;
        float curvature = u_curvature;

        // Screen area check with curvature
        vec2 screenMin = vec2(bw);
        vec2 screenMax = vec2(1.0 - bw);

        if (isInScreen(v_uv, bw, curvature * 0.5)) {
            // Inside screen - show CRT content
            vec2 screenUV = (v_uv - screenMin) / (screenMax - screenMin);
            vec3 screen = texture2D(u_screen, screenUV).rgb;

            // Inner shadow/bevel around screen edge
            float edgeDist = distToScreenEdge(v_uv, bw, curvature);
            float innerShadow = smoothstep(0.0, 0.02, edgeDist);

            // Deep inset shadow
            screen *= 0.6 + 0.4 * innerShadow;

            // Subtle screen glass reflection
            float glassReflect = pow(1.0 - screenUV.y, 3.0) * 0.08;
            screen += vec3(glassReflect);

            gl_FragColor = vec4(screen, 1.0);
            return;
        }

        // === BEZEL AREA ===

        // Industrial coating base color (dark olive/gray like old equipment)
        vec3 baseColor = vec3(0.18, 0.17, 0.14);

        // Surface texture - industrial powder coating with slight orange peel
        float coatingNoise = fbm(v_uv * 80.0) * 0.08;
        float fineGrain = noise(v_uv * 400.0) * 0.03;
        baseColor += vec3(coatingNoise + fineGrain) * vec3(1.0, 0.95, 0.85);

        // Compute surface normal for lighting (bump mapping)
        vec3 normal = computeNormal(v_uv, 60.0);

        // Add larger surface undulations (casting imperfections)
        float largeWave = fbm(v_uv * 15.0) * 0.3;
        normal = normalize(normal + vec3(
            sin(v_uv.x * 30.0 + largeWave) * 0.05,
            sin(v_uv.y * 25.0 + largeWave) * 0.05,
            0.0
        ));

        // Lighting calculation
        float NdotL = max(dot(normal, lightDir), 0.0);
        float diffuse = NdotL * 0.6;

        // Specular (subtle, matte surface)
        vec3 viewDir = vec3(0.0, 0.0, 1.0);
        vec3 halfDir = normalize(lightDir + viewDir);
        float spec = pow(max(dot(normal, halfDir), 0.0), 20.0) * 0.15;

        // Combine lighting
        vec3 bezelColor = baseColor * (ambient + diffuse) * lightColor + vec3(spec);

        // === Depth bevels ===

        // Outer frame bevel (raised edge)
        float outerDist = min(
            min(v_uv.x, 1.0 - v_uv.x),
            min(v_uv.y, 1.0 - v_uv.y)
        );

        if (outerDist < 0.015) {
            float t = outerDist / 0.015;
            // Outer lit edge
            float edgeLight = (1.0 - t) * 0.3;
            if (v_uv.y > 0.5) edgeLight *= 1.5; // Top edge catches more light
            if (v_uv.x < 0.5) edgeLight *= 1.2; // Left edge catches light
            bezelColor += vec3(edgeLight);
            // Dark inner part of bevel
            bezelColor *= 0.7 + 0.3 * t;
        }

        // Inner bevel (around screen - recessed)
        float innerDist = distToScreenEdge(v_uv, bw, curvature);
        if (innerDist > 0.0 && innerDist < 0.025) {
            float t = innerDist / 0.025;
            // Dark shadow going into screen recess
            bezelColor *= 0.5 + 0.5 * (1.0 - pow(1.0 - t, 2.0));
            // Highlight on outer lip of recess
            if (t > 0.7) {
                float lipLight = (t - 0.7) / 0.3;
                if (v_uv.y < 0.5 + bw) bezelColor += vec3(lipLight * 0.15); // Bottom lip lit
                if (v_uv.x > 0.5 - bw) bezelColor += vec3(lipLight * 0.1);  // Right lip lit
            }
        }

        // === DETAILS ===

        // Nameplate (embossed brass plate)
        vec2 npCenter = vec2(0.5, 0.965);
        vec2 npSize = vec2(0.22, 0.018);
        vec2 npDist = abs(v_uv - npCenter);
        if (npDist.x < npSize.x && npDist.y < npSize.y) {
            float npEdge = min(npSize.x - npDist.x, npSize.y - npDist.y);
            vec3 brass = vec3(0.7, 0.55, 0.3);

            // Brushed brass texture
            brass += noise(vec2(v_uv.x * 500.0, v_uv.y * 50.0)) * 0.1;

            // Embossed effect
            if (npEdge < 0.004) {
                float bevelT = npEdge / 0.004;
                brass *= 0.6 + 0.4 * bevelT;
                brass += vec3(0.2) * (1.0 - bevelT) * float(v_uv.y > npCenter.y);
            }

            // Engraved text suggestion (subtle horizontal lines)
            float textLine = sin(v_uv.x * 300.0) * 0.5 + 0.5;
            brass *= 0.95 + 0.05 * textLine;

            bezelColor = brass;
        }

        // Ventilation slots (top left) - recessed
        for (int i = 0; i < 5; i++) {
            vec2 ventCenter = vec2(0.06 + float(i) * 0.022, 0.962);
            vec2 ventSize = vec2(0.008, 0.012);
            vec2 vd = abs(v_uv - ventCenter);
            if (vd.x < ventSize.x && vd.y < ventSize.y) {
                float ventDepth = min(ventSize.x - vd.x, ventSize.y - vd.y);
                if (ventDepth < 0.003) {
                    // Vent edge bevel
                    bezelColor *= 0.4 + 0.6 * (ventDepth / 0.003);
                } else {
                    // Deep dark interior
                    bezelColor = vec3(0.02, 0.02, 0.015);
                }
            }
        }

        // Power LED (top right) - recessed housing with glowing LED
        vec2 ledPos = vec2(0.93, 0.962);
        float ledDist = length((v_uv - ledPos) * vec2(1.0, 1.5)); // Slightly oval
        if (ledDist < 0.015) {
            if (ledDist > 0.01) {
                // LED housing rim
                bezelColor = vec3(0.08) * (1.0 + (ledDist - 0.01) / 0.005);
            } else {
                // LED glow
                float pulse = 0.6 + 0.4 * sin(u_time * 1.5);
                float intensity = (1.0 - ledDist / 0.01);
                vec3 ledColor = vec3(0.1, 1.0, 0.2) * pulse * intensity;
                // Bloom
                ledColor += vec3(0.05, 0.3, 0.1) * pow(intensity, 0.5);
                bezelColor = ledColor;
            }
        }

        // Screws (4 corners) - Phillips head, recessed
        vec2 screwPos[4];
        screwPos[0] = vec2(0.035, 0.035);
        screwPos[1] = vec2(0.965, 0.035);
        screwPos[2] = vec2(0.035, 0.965);
        screwPos[3] = vec2(0.965, 0.965);

        for (int i = 0; i < 4; i++) {
            float sd = length(v_uv - screwPos[i]);
            if (sd < 0.018) {
                if (sd > 0.014) {
                    // Countersink recess
                    float t = (sd - 0.014) / 0.004;
                    bezelColor *= 0.5 + 0.5 * t;
                } else if (sd > 0.012) {
                    // Screw head edge
                    bezelColor = vec3(0.35, 0.33, 0.28);
                } else {
                    // Screw head surface
                    vec3 screwColor = vec3(0.45, 0.43, 0.38);
                    // Dome shape lighting
                    float dome = 1.0 - sd / 0.012;
                    screwColor *= 0.8 + 0.4 * dome;

                    // Phillips cross
                    vec2 sc = v_uv - screwPos[i];
                    float cross = min(abs(sc.x), abs(sc.y));
                    if (cross < 0.002 && sd < 0.008) {
                        screwColor *= 0.3; // Dark cross slot
                    }
                    bezelColor = screwColor;
                }
            }
        }

        // Model number label (bottom center) - recessed plate
        vec2 lblCenter = vec2(0.5, 0.032);
        vec2 lblSize = vec2(0.12, 0.012);
        vec2 lblDist = abs(v_uv - lblCenter);
        if (lblDist.x < lblSize.x && lblDist.y < lblSize.y) {
            float lblEdge = min(lblSize.x - lblDist.x, lblSize.y - lblDist.y);
            if (lblEdge < 0.003) {
                bezelColor *= 0.6 + 0.4 * (lblEdge / 0.003);
            } else {
                // Slightly lighter recessed area for label
                bezelColor = vec3(0.22, 0.21, 0.18);
            }
        }

        // Wear and age effects
        float wear = noise(v_uv * 20.0);
        if (wear > 0.85) {
            // Subtle scratches/wear marks
            bezelColor *= 0.9 + 0.1 * noise(v_uv * 200.0);
        }

        // Edge wear (corners and edges show more use)
        float cornerWear = (1.0 - outerDist * 20.0) * 0.1;
        bezelColor += vec3(cornerWear * wear);

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
