import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

// The Carta tile server to proxy MVT + vector TileJSON from in dev, so the
// viewer is same-origin (style uses relative /tiles.vector.json, /tiles/...).
// Override with VITE_CARTA_ORIGIN. No public CDNs are used at runtime: tiles are
// proxied from Carta and fonts are served locally from public/fonts.
const CARTA = process.env.VITE_CARTA_ORIGIN || 'http://localhost:8097';

// MapLibre requests one glyph PBF per 256-codepoint range on demand. For a range
// the font does not cover, the file genuinely does not exist and the correct
// answer is 404 -- MapLibre then omits those glyphs. Vite's dev server otherwise
// falls through to the SPA index.html with a 200, which MapLibre tries to parse
// as a protobuf ("Unimplemented type: 4"). This guard keeps dev faithful to how
// a static host serves the built site. Production needs no equivalent: a missing
// file 404s on its own.
function glyph404() {
  const fontsDir = fileURLToPath(new URL('./public/fonts/', import.meta.url));
  return {
    name: 'glyph-404',
    configureServer(server: any) {
      server.middlewares.use((req: any, res: any, next: any) => {
        const url = decodeURIComponent((req.url || '').split('?')[0]);
        const m = url.match(/^\/fonts\/(.+\.pbf)$/);
        if (m && !existsSync(fontsDir + m[1])) { res.statusCode = 404; res.end(); return; }
        next();
      });
    },
  };
}

export default defineConfig({
  plugins: [react(), glyph404()],
  server: {
    port: 5178,
    proxy: {
      '/tiles.vector.json': { target: CARTA, changeOrigin: true },
      '/tiles': { target: CARTA, changeOrigin: true },
    },
  },
});
