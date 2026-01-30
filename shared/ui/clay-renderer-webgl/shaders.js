/**
 * Clay WebGL Renderer - Shader Sources
 */

export const TILE_VS = `
    attribute vec2 a_pos;
    attribute vec2 a_uv;
    uniform mat4 u_proj;
    uniform vec4 u_rect;
    varying vec2 v_uv;
    void main() {
        vec2 pos = u_rect.xy + a_pos * u_rect.zw;
        gl_Position = u_proj * vec4(pos, 0.0, 1.0);
        v_uv = a_uv;
    }
`;

export const TILE_FS = `
    precision mediump float;
    uniform sampler2D u_tex;
    varying vec2 v_uv;
    void main() {
        gl_FragColor = texture2D(u_tex, v_uv);
    }
`;

export const RECT_VS = `
    attribute vec2 a_pos;
    uniform mat4 u_proj;
    uniform vec4 u_rect;
    varying vec2 v_localPos;
    varying vec2 v_size;
    void main() {
        vec2 pos = u_rect.xy + a_pos * u_rect.zw;
        gl_Position = u_proj * vec4(pos, 0.0, 1.0);
        v_localPos = a_pos * u_rect.zw;
        v_size = u_rect.zw;
    }
`;

export const RECT_FS = `
    precision mediump float;
    uniform vec4 u_color;
    uniform float u_radius;
    uniform float u_borderWidth;
    uniform vec4 u_borderColor;
    varying vec2 v_localPos;
    varying vec2 v_size;

    float roundedBoxSDF(vec2 p, vec2 b, float r) {
        vec2 q = abs(p) - b + r;
        return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    }

    void main() {
        vec2 center = v_size * 0.5;
        vec2 p = v_localPos - center;
        float d = roundedBoxSDF(p, center, u_radius);

        float aa = 1.0;
        float alpha = 1.0 - smoothstep(-aa, aa, d);

        if (u_borderWidth > 0.0) {
            float innerD = roundedBoxSDF(p, center - u_borderWidth, max(0.0, u_radius - u_borderWidth));
            float innerAlpha = 1.0 - smoothstep(-aa, aa, innerD);
            vec4 fill = u_color * innerAlpha;
            vec4 border = u_borderColor * (alpha - innerAlpha);
            gl_FragColor = fill + border;
        } else {
            gl_FragColor = u_color * alpha;
        }
    }
`;

export const TEXT_VS = `
    attribute vec2 a_pos;
    attribute vec2 a_uv;
    uniform mat4 u_proj;
    varying vec2 v_uv;
    void main() {
        gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
        v_uv = a_uv;
    }
`;

export const TEXT_FS = `
    #extension GL_OES_standard_derivatives : enable
    precision mediump float;
    uniform sampler2D u_tex;
    uniform vec4 u_color;
    uniform float u_pxRange;
    varying vec2 v_uv;

    float median(float r, float g, float b) {
        return max(min(r, g), min(max(r, g), b));
    }

    void main() {
        vec3 msd = texture2D(u_tex, v_uv).rgb;
        float sd = median(msd.r, msd.g, msd.b);
        float screenPxDistance = u_pxRange * (sd - 0.5);
        float opacity = clamp(screenPxDistance + 0.5, 0.0, 1.0);
        gl_FragColor = vec4(u_color.rgb, u_color.a * opacity);
    }
`;
