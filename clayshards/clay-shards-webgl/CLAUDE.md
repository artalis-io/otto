# ClayShards WebGL

WebGL renderer for ClayShards and Clay UI applications in the browser.

## Overview

Renders Clay's render commands using WebGL:
- Rectangles with rounded corners and borders
- MSDF text rendering (crisp at any size)
- Textures/images
- Scissor clipping
- Slippy map tiles

## Architecture

```
┌─────────────────────────────────────┐
│          Application                │
│   renderer.renderClayCommands()     │
├─────────────────────────────────────┤
│        ClayRenderer                 │
│  • renderRect()                     │
│  • renderText()                     │
│  • renderTexture()                  │
├─────────────────────────────────────┤
│          WebGL                      │
│  • Shaders (rect, text, tile)       │
│  • MSDF font texture                │
└─────────────────────────────────────┘
```

## Files

| File | Purpose |
|------|---------|
| `renderer.js` | ClayRenderer class - main rendering API |
| `font.js` | MSDFFont class - font loading and metrics |
| `shaders.js` | GLSL shader sources |
| `map-tiles.js` | TileCache, MapTileRenderer for slippy maps |
| `render-loop.js` | Generic render loop with cursor rendering |
| `wasm-loader.js` | WASM loading with export validation |
| `keyboard.js` | Keyboard event routing for Clay components |
| `text-cursor.js` | Text cursor rendering for focused inputs |
| `index.js` | Module exports |
| `../../fonts/` | MSDF font assets (shared location) |

## Usage

```javascript
import {
    ClayRenderer, MSDFFont, createRenderLoop, loadWasm
} from './clay-shards-webgl/index.js';

// Initialize renderer
const renderer = new ClayRenderer(canvas);
renderer.resize();

// Load resources
const font = new MSDFFont();
const [wasm] = await Promise.all([
    loadWasm('build/app.wasm', ['app_init', 'app_frame', ...]),
    font.load(renderer.gl, '../../fonts/ui-font.json', '../../fonts/ui-font.png')
]);
renderer.setFont(font);

// Initialize app
wasm.app_init(width, height);

// Create and start render loop
const render = createRenderLoop(renderer, wasm, font, {
    frameFunction: 'app_frame',
    onRender: (proj) => {
        // Domain-specific rendering (e.g., map tiles)
    }
});
requestAnimationFrame(render);
```

## API

### ClayRenderer

```javascript
const renderer = new ClayRenderer(canvas);

renderer.resize();                    // Handle window resize
renderer.clear(r, g, b);              // Clear with color
renderer.getProjectionMatrix();       // Get ortho projection

renderer.renderRect(x, y, w, h, color, radius, borderWidth, borderColor, proj);
renderer.renderText(text, x, y, fontSize, color, proj);
renderer.renderTexture(texture, x, y, w, h, proj);
renderer.renderClayCommands(wasm, count, proj);
```

### MSDFFont

```javascript
const font = new MSDFFont();
await font.load(gl, jsonPath, pngPath);

font.getGlyph(charCode);   // Get glyph metrics
font.getFontSize();        // Atlas font size
font.getDistanceRange();   // MSDF distance range
font.getAtlasSize();       // { width, height }
```

### TileCache & MapTileRenderer

```javascript
const cache = new TileCache(gl, maxSize);
const tileRenderer = new MapTileRenderer(renderer, cache);

tileRenderer.render(lat, lon, zoom, layerType, width, height, proj);
```

### setupKeyboardHandler

Sets up keyboard event routing for Clay components. Handles Tab navigation,
focus routing, and character input automatically.

```javascript
import { setupKeyboardHandler } from './clay-shards-webgl/index.js';

const cleanup = setupKeyboardHandler(wasm, {
    onGlobalShortcut: (e) => {
        // App-specific shortcuts when nothing is focused
        if (e.key === '+') wasm.map_scroll(1, 0, 0);
        if (e.key === '-') wasm.map_scroll(-1, 0, 0);
    }
});

// Call cleanup() to remove the event listener
```

Required WASM exports: `cs_key_down`, `cs_key_char`, `cs_focused_id`

### createRenderLoop

Creates a generic render loop that handles Clay UI rendering and text cursor.

```javascript
const render = createRenderLoop(renderer, wasm, font, {
    frameFunction: 'app_frame',    // WASM frame function name
    cmdPrefix: 'cs_clay_cmd_',     // Command accessor prefix
    onRender: (proj, dt) => {},    // Domain-specific rendering callback
    cursorFontSize: 12,            // Font size for cursor positioning
    cursorPadding: 8               // Input padding for cursor positioning
});
requestAnimationFrame(render);
```

### loadWasm

Loads a WASM module with export validation.

```javascript
const wasm = await loadWasm('build/app.wasm', [
    'app_init', 'app_frame', 'app_resize',
    'cs_focused_id', 'cs_key_down', 'cs_key_char'
]);
```

## Shaders

### Rectangle Shader
- Rounded corners via SDF
- Border support
- Anti-aliased edges

### Text Shader
- MSDF (Multi-channel Signed Distance Field)
- Crisp text at any size
- Alpha blending

### Tile Shader
- Simple textured quad
- Used for map tiles and images

## Adding a New Renderer

To create a renderer for another platform (SDL, raylib, etc.):

1. Implement rectangle drawing with rounded corners
2. Implement text rendering (with font metrics callback to Clay)
3. Implement texture/image rendering
4. Implement scissor/clipping
5. Process Clay's render command types:
   - `RECTANGLE` - Solid/rounded rect with optional border
   - `TEXT` - String at position with font size and color
   - `BORDER` - Border-only rectangle
   - `IMAGE` - Textured quad
   - `SCISSOR_START/END` - Clipping regions

## Future Renderers

Planned additional backends:

```
clay-renderer-sdl/       # SDL2 for desktop/mobile
clay-renderer-raylib/    # raylib for games
clay-renderer-sokol/     # Sokol for minimal deps
clay-renderer-terminal/  # TUI rendering
```

## Related

- [clay-shards](../clay-shards/) - Immediate mode components
- [clay-shards-demo](../clay-shards-demo/) - Example application
- [Clay Library](../../../vendor/clay/)
- [Architecture Guide](../../../.claude/skills/clay-ui-architecture.md)
