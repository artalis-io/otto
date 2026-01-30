/**
 * Clay WebGL Renderer
 *
 * A reusable WebGL renderer for Clay UI applications.
 *
 * Usage:
 *   import { ClayRenderer, MSDFFont, TextInputOverlay } from './clay-renderer-webgl/index.js';
 *
 *   const renderer = new ClayRenderer(canvas);
 *   const font = new MSDFFont();
 *   await font.load(renderer.gl, 'fonts/ui-font.json', 'fonts/ui-font.png');
 *   renderer.setFont(font);
 *
 *   function render() {
 *       renderer.clear();
 *       const proj = renderer.getProjectionMatrix();
 *       renderer.renderClayCommands(wasm, cmdCount, proj);
 *       requestAnimationFrame(render);
 *   }
 */

export { ClayRenderer } from './renderer.js';
export { MSDFFont } from './font.js';
export { TextInputOverlay } from './text-input-overlay.js';
export { TileCache, MapTileRenderer } from './map-tiles.js';
export { renderTextCursor } from './text-cursor.js';
export { createRenderLoop } from './render-loop.js';
export { loadWasm } from './wasm-loader.js';
export { createTextureFromImage } from './utils.js';
export * from './shaders.js';
