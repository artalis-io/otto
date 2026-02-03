# Shared Fonts

MSDF (Multi-channel Signed Distance Field) font assets for the OTTO platform.

## Overview

This directory contains font assets used by:
- **clay-shards-webgl**: WebGL UI renderer
- **carta**: Map tile label rendering (planned)

## Files

| File | Description |
|------|-------------|
| `ui-font.json` | Glyph metrics (advance, bounds, unicode) |
| `ui-font.png` | MSDF atlas texture (grayscale, 3-channel) |

## MSDF Format

MSDF fonts encode the signed distance to glyph edges in RGB channels. This allows:
- **Crisp rendering at any size** - no pixelation
- **Efficient storage** - single atlas for all sizes
- **Hardware acceleration** - simple shader-based rendering

### Atlas Structure

The PNG atlas contains glyphs packed in a grid. Each glyph's position and metrics are defined in the JSON file.

### JSON Structure

```json
{
  "atlas": {
    "type": "msdf",
    "distanceRange": 4,
    "size": 32,
    "width": 512,
    "height": 512
  },
  "glyphs": [
    {
      "unicode": 65,
      "advance": 0.6,
      "planeBounds": { "left": 0.0, "bottom": -0.1, "right": 0.6, "top": 0.7 },
      "atlasBounds": { "left": 0, "bottom": 0, "right": 24, "top": 32 }
    }
  ]
}
```

## Usage

### JavaScript (WebGL)

```javascript
import { MSDFFont } from '../ui/clay-shards-webgl/font.js';

const font = new MSDFFont();
await font.load(gl, 'shared/fonts/ui-font.json', 'shared/fonts/ui-font.png');

// Measure text
const width = font.measureText('Hello', 16);

// Get glyph info
const glyph = font.getGlyph(65); // 'A'
```

### C (Carta - planned)

```c
#include "ct_font.h"

CTMSDFFont font;
ct_font_load(&font, "shared/fonts/ui-font.json", "shared/fonts/ui-font.png");

// Measure text
float width = ct_font_text_width(&font, "Hello", 16.0f);

// Render (software MSDF)
ct_render_text(ctx, &font, "Hello", x, y, 16.0f, color);
```

## Generating New Fonts

To generate MSDF fonts from TTF/OTF files, use [msdf-atlas-gen](https://github.com/Chlumsky/msdf-atlas-gen):

```bash
# Install msdf-atlas-gen
# Then generate atlas:
msdf-atlas-gen \
    -font NotoSans-Regular.ttf \
    -type msdf \
    -size 32 \
    -pxrange 4 \
    -imageout ui-font.png \
    -json ui-font.json \
    -charset ascii
```

## Character Coverage

Current `ui-font` covers:
- ASCII printable characters (32-126)
- Common Latin extended characters
- Currency symbols (€, £, ¥)
- Mathematical symbols (±, ×, ÷)

For CJK or other scripts, additional font atlases would be needed.

## Related

- [clay-shards-webgl](../ui/clay-shards-webgl/) - WebGL renderer using these fonts
- [carta](../../carta/) - Map tile generator (labels planned)
- [CARTA_LABELS_PLAN.md](../../docs/CARTA_LABELS_PLAN.md) - Label implementation plan
