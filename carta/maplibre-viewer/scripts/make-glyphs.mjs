/*
 * Generate MapLibre SDF glyph PBFs locally from IBM Plex Sans (OFL, bundled via
 * @fontsource). No network at runtime: the resulting public/fonts/<stack>/<range>.pbf
 * are served by Vite. Basic Latin comes from the "latin" subset; Hungarian o-double
 * -acute / u-double-acute (U+0150..U+0171) come from the "latin-ext" subset; the two
 * are composited per 256-codepoint range so one fontstack covers both.
 */
import fontnik from 'fontnik';
import { decompress } from 'wawoff2';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import { promisify } from 'node:util';
import Pbf from 'pbf';

const range = promisify(fontnik.range);
const composite = promisify(fontnik.composite);
const STACK = 'IBM Plex Sans Regular';
const BASE = 'node_modules/@fontsource/ibm-plex-sans/files';
const OUT = `public/fonts/${STACK}`;

async function ttf(subset) {
  const woff2 = await readFile(`${BASE}/ibm-plex-sans-${subset}-400-normal.woff2`);
  return Buffer.from(await decompress(woff2));
}

// Count the glyphs in a fontnik/SDF glyph PBF: top message `glyphs` has field 1
// (stacks, repeated fontstack); each fontstack has field 3 (glyphs, repeated).
// A range the font has no coverage for still returns a valid PBF with an empty
// stack -- we skip writing those so MapLibre gets a clean 404 and simply omits
// the glyph, rather than being handed an empty file to treat as present.
function glyphCount(buf) {
  const pbf = new Pbf(buf);
  let n = 0;
  pbf.readFields((tag, _, p) => {
    if (tag !== 1) { p.skip(p.type); return; }
    const end = p.readVarint() + p.pos;
    while (p.pos < end) { const t = p.readVarint(); if ((t >> 3) === 3) { n++; p.skip(2); } else p.skip(t & 7); }
  }, null);
  return n;
}

const latin = await ttf('latin');
const latinExt = await ttf('latin-ext');
await mkdir(OUT, { recursive: true });

// Emit every 256-codepoint range the composited font actually covers (Basic
// Latin through punctuation/currency and beyond). Place names carry more than
// ASCII + Hungarian: en dashes and curly apostrophes live in General
// Punctuation (U+2000-206F, the 8192-8447 range), so a fixed 0-1023 window left
// MapLibre requesting a range that did not exist. Covering the font's full
// glyph set removes the guesswork; ranges with no glyphs are skipped.
let written = 0;
for (let start = 0; start <= 0xffff; start += 256) {
  const end = start + 255;
  const a = await range({ font: latin, start, end });
  const b = await range({ font: latinExt, start, end });
  const merged = await composite([a, b]);
  if (glyphCount(merged) === 0) continue;
  await writeFile(`${OUT}/${start}-${end}.pbf`, merged);
  written++;
  console.log(`wrote ${OUT}/${start}-${end}.pbf (${merged.length} bytes)`);
}
console.log(`glyphs done: ${written} ranges`);
