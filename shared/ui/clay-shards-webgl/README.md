# Clay Renderer - WebGL

WebGL renderer for [Clay](https://github.com/nicbarker/clay) UI applications.

## Features

- Rectangle rendering with rounded corners and borders
- MSDF text (crisp at any size)
- Texture/image rendering
- Scissor clipping
- Slippy map tile rendering

## Quick Start

```javascript
import {
    ClayRenderer, MSDFFont, createRenderLoop, loadWasm
} from './clay-renderer-webgl/index.js';

const renderer = new ClayRenderer(canvas);
renderer.resize();

const font = new MSDFFont();
const [wasm] = await Promise.all([
    loadWasm('build/app.wasm', ['app_init', 'app_frame']),
    font.load(renderer.gl, 'fonts/ui-font.json', 'fonts/ui-font.png')
]);
renderer.setFont(font);
wasm.app_init(width, height);

const render = createRenderLoop(renderer, wasm, font, {
    frameFunction: 'app_frame'
});
requestAnimationFrame(render);
```

## Exports

```javascript
import {
    ClayRenderer,      // Main renderer class
    MSDFFont,          // Font loading
    TileCache,         // Map tile caching
    MapTileRenderer,   // Slippy map rendering
    createRenderLoop,  // Generic render loop with cursor
    loadWasm,          // WASM loading with validation
    renderTextCursor,  // Text cursor rendering
} from './clay-renderer-webgl/index.js';
```

## Files

- `renderer.js` - ClayRenderer class
- `font.js` - MSDF font handling
- `shaders.js` - WebGL shaders
- `map-tiles.js` - Tile cache and renderer
- `render-loop.js` - Generic render loop
- `wasm-loader.js` - WASM loading utility
- `text-cursor.js` - Text cursor rendering
- `fonts/` - Default UI font

## License

MIT - Part of the OTTO platform
