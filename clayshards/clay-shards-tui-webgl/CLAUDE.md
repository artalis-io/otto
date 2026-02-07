# ClayShards TUI WebGL

WebGL renderer for ClayShards TUI applications in the browser with retro CRT effects.

## Overview

Renders the TUI character buffer (from WASM) to WebGL:
- Character grid rendering via font atlas
- Box-drawing characters and block elements
- CRT post-processing: scanlines, curvature, vignette, chromatic aberration
- Color modes: amber, green, white, RGB

## Architecture

```
+---------------------------------------------------+
|  Application (C/WASM)                             |
|  - Clay layout + ClayShards components            |
|  - cs_tui_* rendering to character buffer         |
+---------------------------------------------------+
             |
             v (cell buffer in WASM memory)
+---------------------------------------------------+
|  tui-renderer.js                                  |
|  - Reads 16-byte cells from WASM memory           |
|  - Builds vertex buffer (batched quads)           |
|  - Renders via WebGL                              |
+---------------------------------------------------+
             |
             v (framebuffer)
+---------------------------------------------------+
|  crt-effects.js                                   |
|  - Scanlines, curvature, vignette                 |
|  - Chromatic aberration, flicker                  |
|  - Color mode (amber/green/white/RGB)             |
+---------------------------------------------------+
             |
             v (screen)
```

## Files

| File | Purpose |
|------|---------|
| `index.js` | Module exports |
| `tui-renderer.js` | Core WebGL character grid renderer |
| `shaders.js` | GLSL shaders (terminal + CRT) |
| `font-atlas.js` | Bitmap font atlas management |
| `crt-effects.js` | Post-processing pipeline |
| `wasm-loader.js` | WASM loading and input handling |

## Usage

```javascript
import { TuiRenderer, TerminalFontAtlas, CrtEffects, loadTuiWasm } from './index.js';

// Load WASM
const wasm = await loadTuiWasm('path/to/demo.wasm');
wasm.demo_init(80, 24);

// Setup renderer
const renderer = new TuiRenderer(canvas, { cellWidth: 9, cellHeight: 18 });

// Setup font atlas
const fontAtlas = new TerminalFontAtlas(renderer.gl);
fontAtlas.generateProceduralAtlas({ fontFamily: 'monospace', fontSize: 16 });
renderer.setFontAtlas(fontAtlas);

// Setup CRT effects
const crt = new CrtEffects(renderer.gl);
crt.resize(canvas.width, canvas.height);
crt.applyPreset('retro');  // 'off', 'subtle', 'retro', 'arcade', 'amber', 'green'

// Render loop
function render(timestamp) {
    const dt = /* delta time */;
    wasm.demo_frame(dt);

    renderer.clear(0.1, 0.1, 0.1);

    if (crt.isEnabled()) {
        crt.begin();
        renderer.renderBatched(wasm);
        crt.end(dt);
    } else {
        renderer.renderBatched(wasm);
    }

    requestAnimationFrame(render);
}
```

## Cell Buffer Format

Each cell is 16 bytes (packed struct):
```
Offset  Size  Field
0       4     codepoint (uint32)
4       1     fg_r
5       1     fg_g
6       1     fg_b
7       1     bg_r
8       1     bg_g
9       1     bg_b
10      1     flags
11      1     padding
12      2     z_index (int16)
14      2     padding
```

## CRT Presets

| Preset | Scanlines | Curvature | Vignette | Chromatic | Color |
|--------|-----------|-----------|----------|-----------|-------|
| off | - | - | - | - | - |
| subtle | 0.1 | 0.01 | 0.1 | 0.0005 | RGB |
| retro | 0.3 | 0.03 | 0.2 | 0.001 | RGB |
| arcade | 0.5 | 0.05 | 0.3 | 0.002 | RGB |
| amber | 0.3 | 0.03 | 0.2 | 0 | Amber |
| green | 0.3 | 0.03 | 0.2 | 0 | Green |

## Building the Demo

```bash
# From project root
make tui-demo-serve
# Opens http://localhost:8000/clayshards/clay-shards-tui/wasm/demo.html
```

Demo lives in `../clay-shards-tui/wasm/demo.html` (co-located with WASM source).

## Dependencies

- WebGL 1.0 (WebGL 2 not required)
- Emscripten SDK (for WASM builds)

## Related

- [clay-shards-tui](../clay-shards-tui/) - Terminal TUI renderer
- [clay-shards-webgl](../clay-shards-webgl/) - Regular WebGL renderer
- [Clay Library](../../vendor/clay/)
