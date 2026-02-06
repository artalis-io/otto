/**
 * ClayShards TUI WebGL Renderer
 *
 * Renders ClayShards TUI applications in the browser using WebGL.
 * The C TUI renderer runs in WASM and outputs to a character buffer,
 * which is then rendered via WebGL with optional CRT effects.
 *
 * Usage:
 *   import { TuiRenderer, TerminalFontAtlas, CrtEffects, loadTuiWasm } from './clay-shards-tui-webgl/index.js';
 *
 *   const wasm = await loadTuiWasm('path/to/demo.wasm');
 *   wasm.demo_init(80, 24);
 *
 *   const renderer = new TuiRenderer(canvas);
 *   const fontAtlas = new TerminalFontAtlas(renderer.gl);
 *   fontAtlas.generateProceduralAtlas();
 *   renderer.setFontAtlas(fontAtlas);
 *
 *   const crt = new CrtEffects(renderer.gl);
 *   crt.applyPreset('retro');
 *
 *   function render() {
 *       wasm.demo_frame(0.016);
 *       renderer.clear();
 *       crt.begin();
 *       renderer.renderBatched(wasm);
 *       crt.end(0.016);
 *       requestAnimationFrame(render);
 *   }
 */

export { TuiRenderer } from './tui-renderer.js';
export { TerminalFontAtlas } from './font-atlas.js';
export { CrtEffects, CRT_COLOR_AMBER, CRT_COLOR_GREEN, CRT_COLOR_WHITE, CRT_COLOR_RGB } from './crt-effects.js';
export { loadTuiWasm, setupKeyboardHandlers, setupMouseHandlers, initTuiWasm, resizeTuiWasm } from './wasm-loader.js';
export * from './shaders.js';
