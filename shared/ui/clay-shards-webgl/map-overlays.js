/**
 * ClayShards Map Overlays - WebGL Renderer
 *
 * Renders polylines and markers on top of map tiles.
 * Reads overlay data from WASM and projects geo coordinates to screen.
 */

const CS_OVERLAY_NONE = 0;
const CS_OVERLAY_POLYLINE = 1;
const CS_OVERLAY_MARKER = 2;

export class MapOverlayRenderer {
    constructor(renderer) {
        this.renderer = renderer;
        this.gl = renderer.gl;
        this._initShaders();
    }

    _initShaders() {
        const gl = this.gl;

        // Line shader for polylines
        const lineVS = `
            attribute vec2 a_pos;
            uniform mat4 u_proj;
            void main() {
                gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
            }
        `;

        const lineFS = `
            precision mediump float;
            uniform vec4 u_color;
            void main() {
                gl_FragColor = u_color;
            }
        `;

        // Circle shader for markers
        const circleVS = `
            attribute vec2 a_pos;
            uniform mat4 u_proj;
            uniform vec4 u_rect;
            varying vec2 v_uv;
            void main() {
                vec2 pos = u_rect.xy + a_pos * u_rect.zw;
                gl_Position = u_proj * vec4(pos, 0.0, 1.0);
                v_uv = a_pos * 2.0 - 1.0;
            }
        `;

        const circleFS = `
            precision mediump float;
            uniform vec4 u_color;
            uniform vec4 u_borderColor;
            uniform float u_borderWidth;
            uniform float u_radius;
            varying vec2 v_uv;
            void main() {
                float dist = length(v_uv);
                float outerEdge = 1.0;
                float innerEdge = 1.0 - u_borderWidth / u_radius;

                // Anti-aliased edges
                float aa = 2.0 / u_radius;
                float outerAlpha = 1.0 - smoothstep(outerEdge - aa, outerEdge, dist);
                float innerAlpha = 1.0 - smoothstep(innerEdge - aa, innerEdge, dist);

                vec4 fill = u_color * innerAlpha;
                vec4 border = u_borderColor * (outerAlpha - innerAlpha);
                gl_FragColor = fill + border;
            }
        `;

        this.lineShader = this._createProgram(lineVS, lineFS);
        this.circleShader = this._createProgram(circleVS, circleFS);

        // Create buffers
        this.lineBuffer = gl.createBuffer();
        this.quadBuffer = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
            0, 0, 1, 0, 0, 1, 1, 1
        ]), gl.STATIC_DRAW);
    }

    _createProgram(vsSource, fsSource) {
        const gl = this.gl;

        const vs = gl.createShader(gl.VERTEX_SHADER);
        gl.shaderSource(vs, vsSource);
        gl.compileShader(vs);
        if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS)) {
            console.error('Vertex shader error:', gl.getShaderInfoLog(vs));
            return null;
        }

        const fs = gl.createShader(gl.FRAGMENT_SHADER);
        gl.shaderSource(fs, fsSource);
        gl.compileShader(fs);
        if (!gl.getShaderParameter(fs, gl.COMPILE_STATUS)) {
            console.error('Fragment shader error:', gl.getShaderInfoLog(fs));
            return null;
        }

        const program = gl.createProgram();
        gl.attachShader(program, vs);
        gl.attachShader(program, fs);
        gl.linkProgram(program);

        if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
            console.error('Program link error:', gl.getProgramInfoLog(program));
            return null;
        }

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
     * Render all overlays
     * @param {Object} wasm - WASM exports
     * @param {number} lat - Map center latitude
     * @param {number} lon - Map center longitude
     * @param {number} zoom - Visual zoom level
     * @param {number} width - Viewport width
     * @param {number} height - Viewport height
     * @param {Float32Array} projMatrix - Projection matrix
     */
    render(wasm, lat, lon, zoom, width, height, projMatrix) {
        const count = wasm.cs_map_overlay_count();
        if (count === 0) return;

        for (let i = 0; i < count; i++) {
            const type = wasm.cs_map_overlay_type(i);

            if (type === CS_OVERLAY_POLYLINE) {
                this._renderPolyline(wasm, i, lat, lon, zoom, width, height, projMatrix);
            } else if (type === CS_OVERLAY_MARKER) {
                this._renderMarker(wasm, i, lat, lon, zoom, width, height, projMatrix);
            }
        }
    }

    _renderPolyline(wasm, index, lat, lon, zoom, width, height, projMatrix) {
        const gl = this.gl;
        const pointCount = wasm.cs_map_overlay_polyline_count(index);
        if (pointCount < 2) return;

        // Get style
        const color = [
            wasm.cs_map_overlay_polyline_color_r(index),
            wasm.cs_map_overlay_polyline_color_g(index),
            wasm.cs_map_overlay_polyline_color_b(index),
            wasm.cs_map_overlay_polyline_color_a(index)
        ];
        const lineWidth = wasm.cs_map_overlay_polyline_width(index);

        // Project points to screen coordinates
        const screenPoints = [];
        for (let i = 0; i < pointCount; i++) {
            const pLat = wasm.cs_map_overlay_polyline_lat(index, i);
            const pLon = wasm.cs_map_overlay_polyline_lon(index, i);
            const [x, y] = this._geoToScreen(pLat, pLon, lat, lon, zoom, width, height);
            screenPoints.push(x, y);
        }

        // Generate thick line geometry (triangle strip)
        const vertices = this._generateThickLine(screenPoints, lineWidth);
        if (vertices.length === 0) return;

        // Render
        gl.useProgram(this.lineShader.program);
        gl.uniformMatrix4fv(this.lineShader.uniforms.u_proj, false, projMatrix);
        gl.uniform4fv(this.lineShader.uniforms.u_color, color);

        gl.bindBuffer(gl.ARRAY_BUFFER, this.lineBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(vertices), gl.DYNAMIC_DRAW);

        gl.enableVertexAttribArray(this.lineShader.attribs.a_pos);
        gl.vertexAttribPointer(this.lineShader.attribs.a_pos, 2, gl.FLOAT, false, 0, 0);

        gl.drawArrays(gl.TRIANGLE_STRIP, 0, vertices.length / 2);

        gl.disableVertexAttribArray(this.lineShader.attribs.a_pos);
    }

    _renderMarker(wasm, index, lat, lon, zoom, width, height, projMatrix) {
        const gl = this.gl;

        const mLat = wasm.cs_map_overlay_marker_lat(index);
        const mLon = wasm.cs_map_overlay_marker_lon(index);
        const radius = wasm.cs_map_overlay_marker_radius(index);

        const color = [
            wasm.cs_map_overlay_marker_color_r(index),
            wasm.cs_map_overlay_marker_color_g(index),
            wasm.cs_map_overlay_marker_color_b(index),
            wasm.cs_map_overlay_marker_color_a(index)
        ];

        const borderColor = [
            wasm.cs_map_overlay_marker_border_r(index),
            wasm.cs_map_overlay_marker_border_g(index),
            wasm.cs_map_overlay_marker_border_b(index),
            wasm.cs_map_overlay_marker_border_a(index)
        ];

        const borderWidth = wasm.cs_map_overlay_marker_border_width(index);

        // Project to screen
        const [x, y] = this._geoToScreen(mLat, mLon, lat, lon, zoom, width, height);

        // Render circle
        gl.useProgram(this.circleShader.program);
        gl.uniformMatrix4fv(this.circleShader.uniforms.u_proj, false, projMatrix);
        gl.uniform4f(this.circleShader.uniforms.u_rect, x - radius, y - radius, radius * 2, radius * 2);
        gl.uniform4fv(this.circleShader.uniforms.u_color, color);
        gl.uniform4fv(this.circleShader.uniforms.u_borderColor, borderColor);
        gl.uniform1f(this.circleShader.uniforms.u_borderWidth, borderWidth);
        gl.uniform1f(this.circleShader.uniforms.u_radius, radius);

        gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuffer);
        gl.enableVertexAttribArray(this.circleShader.attribs.a_pos);
        gl.vertexAttribPointer(this.circleShader.attribs.a_pos, 2, gl.FLOAT, false, 0, 0);

        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

        gl.disableVertexAttribArray(this.circleShader.attribs.a_pos);
    }

    /**
     * Convert geo coordinates to screen coordinates
     */
    _geoToScreen(pLat, pLon, centerLat, centerLon, zoom, width, height) {
        const scale = Math.pow(2, zoom) * 256;

        // Web Mercator projection
        const centerX = (centerLon + 180) / 360 * scale;
        const centerLatRad = centerLat * Math.PI / 180;
        const centerY = (1 - Math.log(Math.tan(centerLatRad) + 1 / Math.cos(centerLatRad)) / Math.PI) / 2 * scale;

        const pX = (pLon + 180) / 360 * scale;
        const pLatRad = pLat * Math.PI / 180;
        const pY = (1 - Math.log(Math.tan(pLatRad) + 1 / Math.cos(pLatRad)) / Math.PI) / 2 * scale;

        // Convert to screen coordinates (center of viewport is center of map)
        const screenX = width / 2 + (pX - centerX);
        const screenY = height / 2 + (pY - centerY);

        return [screenX, screenY];
    }

    /**
     * Generate thick line geometry as triangle strip with proper miter joins
     */
    _generateThickLine(points, width) {
        const numPoints = points.length / 2;
        if (numPoints < 2) return [];

        const vertices = [];
        const halfWidth = width / 2;
        const miterLimit = 3.0; // Limit miter length to avoid spikes at sharp angles

        for (let i = 0; i < numPoints; i++) {
            const x = points[i * 2];
            const y = points[i * 2 + 1];

            let nx, ny; // Normal (perpendicular) at this vertex

            if (i === 0) {
                // First point: use perpendicular to first segment
                const dx = points[2] - points[0];
                const dy = points[3] - points[1];
                const len = Math.sqrt(dx * dx + dy * dy);
                if (len < 0.001) continue;
                nx = -dy / len;
                ny = dx / len;
            } else if (i === numPoints - 1) {
                // Last point: use perpendicular to last segment
                const dx = points[i * 2] - points[(i - 1) * 2];
                const dy = points[i * 2 + 1] - points[(i - 1) * 2 + 1];
                const len = Math.sqrt(dx * dx + dy * dy);
                if (len < 0.001) continue;
                nx = -dy / len;
                ny = dx / len;
            } else {
                // Middle point: compute miter join
                // Direction of incoming segment
                const dx1 = x - points[(i - 1) * 2];
                const dy1 = y - points[(i - 1) * 2 + 1];
                const len1 = Math.sqrt(dx1 * dx1 + dy1 * dy1);

                // Direction of outgoing segment
                const dx2 = points[(i + 1) * 2] - x;
                const dy2 = points[(i + 1) * 2 + 1] - y;
                const len2 = Math.sqrt(dx2 * dx2 + dy2 * dy2);

                if (len1 < 0.001 || len2 < 0.001) continue;

                // Normalize directions
                const ux1 = dx1 / len1, uy1 = dy1 / len1;
                const ux2 = dx2 / len2, uy2 = dy2 / len2;

                // Perpendiculars (normals) to each segment
                const nx1 = -uy1, ny1 = ux1;
                const nx2 = -uy2, ny2 = ux2;

                // Miter direction: average of the two normals
                let mx = nx1 + nx2;
                let my = ny1 + ny2;
                const mlen = Math.sqrt(mx * mx + my * my);

                if (mlen < 0.001) {
                    // Segments are parallel, use either normal
                    nx = nx1;
                    ny = ny1;
                } else {
                    mx /= mlen;
                    my /= mlen;

                    // Miter length: 1 / dot(miter, normal)
                    // This ensures the line width is correct at the join
                    const dot = mx * nx1 + my * ny1;
                    let miterLen = 1.0 / Math.max(dot, 0.1); // Prevent division by zero

                    // Clamp miter length to avoid spikes at sharp angles
                    miterLen = Math.min(miterLen, miterLimit);

                    nx = mx * miterLen;
                    ny = my * miterLen;
                }
            }

            // Add two vertices: offset in both directions from the line
            vertices.push(x + nx * halfWidth, y + ny * halfWidth);
            vertices.push(x - nx * halfWidth, y - ny * halfWidth);
        }

        return vertices;
    }
}
