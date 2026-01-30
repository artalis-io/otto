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
import { ClayRenderer, MSDFFont } from './clay-renderer-webgl/index.js';

const renderer = new ClayRenderer(canvas);
renderer.resize();

const font = new MSDFFont();
await font.load(renderer.gl, 'fonts/ui-font.json', 'fonts/ui-font.png');
renderer.setFont(font);

function render() {
    renderer.clear();
    const proj = renderer.getProjectionMatrix();
    const count = wasm.render_frame(dt);
    renderer.renderClayCommands(wasm, count, proj);
    requestAnimationFrame(render);
}
```

## Exports

```javascript
import {
    ClayRenderer,      // Main renderer class
    MSDFFont,          // Font loading
    TileCache,         // Map tile caching
    MapTileRenderer,   // Slippy map rendering
} from './clay-renderer-webgl/index.js';
```

## Files

- `renderer.js` - ClayRenderer class
- `font.js` - MSDF font handling
- `shaders.js` - WebGL shaders
- `map-tiles.js` - Tile cache and renderer
- `fonts/` - Default UI font

## License

MIT - Part of the OTTO platform
