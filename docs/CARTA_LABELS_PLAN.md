# Carta Label Implementation Plan

This document outlines the implementation plan for adding label rendering to Carta map tiles.

## Overview

Labels are text annotations placed on map features (roads, places, water bodies, etc.). The implementation needs to support:
- **Point labels**: Cities, POIs, peaks
- **Line labels**: Roads, rivers (text follows the path)
- **Area labels**: Lakes, forests, parks (text centered in polygon)

## Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│                         Label Pipeline                              │
├────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  1. Label Collection     Parse names from OSM features               │
│         ↓                                                            │
│  2. Label Placement      Calculate positions for each label          │
│         ↓                                                            │
│  3. Collision Detection  Remove overlapping labels                   │
│         ↓                                                            │
│  4. Rendering            Draw labels to PNG or encode to MVT         │
│                                                                      │
└────────────────────────────────────────────────────────────────────┘
```

## Phase 1: Data Collection and Storage

### 1.1 Store Names in Features

**File:** `carta/src/ct_pbf.c`

Names are already parsed (`way->name`) but not consistently exposed. Ensure all feature types store names:

```c
/* Already exists - verify it's populated for all features */
typedef struct {
    // ... existing fields ...
    char *name;              /* Feature name (UTF-8) */
} CTOSMWay;
```

### 1.2 Add Place/POI Parsing

**New:** Parse OSM nodes with place/amenity tags for point labels.

```c
/* New structure for labeled points */
typedef struct {
    int64_t id;
    CTCoord coord;
    char *name;
    CTPlaceType type;        /* city, town, village, peak, etc. */
    int population;          /* For prioritization */
    int min_zoom;            /* Label appears at this zoom */
} CTLabeledPoint;

typedef enum {
    CT_PLACE_CITY = 0,       /* z4+ */
    CT_PLACE_TOWN,           /* z8+ */
    CT_PLACE_VILLAGE,        /* z12+ */
    CT_PLACE_HAMLET,         /* z14+ */
    CT_PLACE_SUBURB,         /* z12+ */
    CT_PLACE_PEAK,           /* z12+ */
    CT_PLACE_POI,            /* z15+ */
    CT_PLACE_TYPE_COUNT
} CTPlaceType;
```

**Implementation:**
1. During PBF parsing, extract nodes with `place=*` or `name=*` + relevant tags
2. Store in `CTPBFContext->labeled_points[]` array
3. Build spatial index for efficient tile queries

### 1.3 Label Priority System

Labels compete for space. Priority determines which labels win:

| Feature Type | Priority | Min Zoom |
|-------------|----------|----------|
| Country name | 100 | 2 |
| Capital city | 95 | 4 |
| City (>500k) | 90 | 5 |
| City (>100k) | 85 | 7 |
| Town | 70 | 9 |
| Primary road | 60 | 10 |
| Lake (>10km²) | 55 | 8 |
| Village | 50 | 12 |
| Secondary road | 45 | 12 |
| River | 40 | 10 |
| Residential road | 20 | 15 |

## Phase 2: Font System

### 2.1 MSDF Font Format

Reuse the MSDF (Multi-channel Signed Distance Field) font system from `shared/fonts/`.
MSDF fonts render crisp at any size and scale well.

**Existing assets:**
- `shared/fonts/ui-font.json` - Glyph metrics
- `shared/fonts/ui-font.png` - MSDF atlas texture

**File:** `carta/include/ct_font.h`

```c
/* Glyph metrics (matches MSDF JSON format) */
typedef struct {
    int unicode;             /* Unicode codepoint */
    float advance;           /* Horizontal advance (normalized) */
    struct {
        float left, bottom, right, top;  /* Plane bounds (normalized) */
    } planeBounds;
    struct {
        float left, bottom, right, top;  /* Atlas bounds (pixels) */
    } atlasBounds;
} CTGlyph;

/* MSDF Font definition */
typedef struct {
    CTGlyph *glyphs;         /* Glyph table */
    int glyph_count;
    int atlas_width;         /* Atlas texture dimensions */
    int atlas_height;
    float distance_range;    /* MSDF distance range */
    float em_size;           /* Font size used to generate atlas */
    uint8_t *atlas_data;     /* MSDF atlas (RGBA) */

    /* Quick lookup for ASCII */
    CTGlyph *ascii_table[128];
} CTMSDFFont;

/* Load font from JSON + PNG files */
CTStatus ct_font_load(CTMSDFFont *font, const char *json_path, const char *png_path);

/* Load font from embedded data (for WASM/static builds) */
CTStatus ct_font_load_embedded(CTMSDFFont *font,
                                const char *json_data, size_t json_len,
                                const uint8_t *png_data, size_t png_len);

void ct_font_free(CTMSDFFont *font);
```

### 2.2 Font Data Embedding

For static builds, embed the MSDF font data directly:

```bash
# Convert font assets to C arrays
xxd -i shared/ui/clay-shards-webgl/fonts/ui-font.json > carta/src/ct_font_data.c
xxd -i shared/ui/clay-shards-webgl/fonts/ui-font.png >> carta/src/ct_font_data.c
```

Or use a build script that includes the font JSON/PNG at compile time.

### 2.3 Text Measurement

**File:** `carta/src/ct_font.c`

```c
/* Get glyph for character */
const CTGlyph *ct_font_get_glyph(const CTMSDFFont *font, int unicode);

/* Measure text width at given font size */
float ct_font_text_width(const CTMSDFFont *font, const char *text, float font_size);

/* Measure text with character limit */
float ct_font_text_width_n(const CTMSDFFont *font, const char *text,
                           int max_chars, float font_size);

/* Get line height for font size */
float ct_font_line_height(const CTMSDFFont *font, float font_size);
```

### 2.4 MSDF Rendering

MSDF rendering uses a special shader that samples the distance field:

```c
/* For PNG tiles: software MSDF rendering */
void ct_render_msdf_glyph(CTRenderContext *ctx,
                          const CTMSDFFont *font,
                          const CTGlyph *glyph,
                          int x, int y,
                          float font_size,
                          CTColor color);
```

The MSDF algorithm:
1. Sample R, G, B channels from atlas
2. Take median of the three values
3. Compare to threshold (0.5) for inside/outside
4. Apply anti-aliasing based on distance from edge

## Phase 3: Label Placement

### 3.1 Point Label Placement

**File:** `carta/src/ct_label.c`

For cities, POIs, peaks:

```c
typedef struct {
    int x, y;                /* Anchor point (tile coords) */
    int text_x, text_y;      /* Text position (adjusted for anchor) */
    CTAnchor anchor;         /* N, NE, E, SE, S, SW, W, NW, CENTER */
    int width, height;       /* Bounding box */
} CTLabelPlacement;

typedef enum {
    CT_ANCHOR_N = 0,         /* Text below point */
    CT_ANCHOR_NE,
    CT_ANCHOR_E,             /* Text left of point */
    CT_ANCHOR_SE,
    CT_ANCHOR_S,             /* Text above point */
    CT_ANCHOR_SW,
    CT_ANCHOR_W,             /* Text right of point */
    CT_ANCHOR_NW,
    CT_ANCHOR_CENTER         /* Text centered on point */
} CTAnchor;

/* Try multiple anchor positions to find non-colliding placement */
CTLabelPlacement ct_label_place_point(
    const CTFont *font,
    const char *text,
    int x, int y,
    const CTCollisionIndex *collision
);
```

### 3.2 Line Label Placement

For roads and rivers - text follows the path:

```c
typedef struct {
    CTTilePoint *path;       /* Smoothed path for text to follow */
    int path_len;
    float start_offset;      /* Where along path to start text */
    int is_flipped;          /* 1 if text should be drawn upside-down to read left-to-right */
} CTLineLabelPlacement;

/* Find best position along a line for label */
CTLineLabelPlacement ct_label_place_line(
    const CTFont *font,
    const char *text,
    const CTTilePoint *points,
    int num_points,
    const CTCollisionIndex *collision
);
```

**Line label algorithm:**
1. Measure text width
2. Find segments of the line that are:
   - Long enough for the text
   - Not too curved (max angle per character)
   - Generally horizontal (prefer east-west orientation)
3. Smooth the path using Bezier curves
4. Check collision for text bounding boxes along path
5. Flip text if path goes right-to-left (so text always reads left-to-right)

### 3.3 Area Label Placement

For lakes, parks, forests - text centered in polygon:

```c
/* Find visual center of polygon (pole of inaccessibility) */
CTTilePoint ct_label_polygon_center(
    const CTTilePoint *points,
    int num_points
);

/* Place text at polygon center, possibly rotated */
CTLabelPlacement ct_label_place_area(
    const CTFont *font,
    const char *text,
    const CTTilePoint *polygon,
    int num_points,
    const CTCollisionIndex *collision
);
```

**Area label algorithm:**
1. Calculate pole of inaccessibility (point furthest from edges)
2. Determine if text fits within polygon at that point
3. For elongated shapes, rotate text to match orientation

## Phase 4: Collision Detection

### 4.1 Collision Index

**File:** `carta/src/ct_collision.c`

```c
/* Grid-based collision index for fast overlap detection */
typedef struct {
    uint8_t *grid;           /* Occupied cells bitmap */
    int grid_width;          /* Grid cells in X */
    int grid_height;         /* Grid cells in Y */
    int cell_size;           /* Pixels per cell (e.g., 8) */
    int tile_width;          /* Tile width in pixels */
    int tile_height;         /* Tile height in pixels */
} CTCollisionIndex;

/* Initialize collision index for a tile */
CTCollisionIndex *ct_collision_create(int tile_width, int tile_height);
void ct_collision_free(CTCollisionIndex *idx);

/* Check if rectangle would collide */
int ct_collision_test(const CTCollisionIndex *idx,
                      int x, int y, int width, int height);

/* Mark rectangle as occupied */
void ct_collision_mark(CTCollisionIndex *idx,
                       int x, int y, int width, int height);

/* Check and mark in one operation (atomic) */
int ct_collision_place(CTCollisionIndex *idx,
                       int x, int y, int width, int height);
```

### 4.2 Label Padding

Add padding around labels to prevent visual crowding:

```c
#define LABEL_PADDING_X 4    /* Horizontal padding (pixels) */
#define LABEL_PADDING_Y 2    /* Vertical padding (pixels) */
#define LABEL_LINE_GAP 20    /* Min gap between line labels on same feature */
```

## Phase 5: PNG Rendering

### 5.1 Text Drawing

**File:** `carta/src/ct_render.c`

```c
/* Draw horizontal text */
void ct_render_text(CTRenderContext *ctx,
                    const CTFont *font,
                    const char *text,
                    int x, int y,
                    CTColor color);

/* Draw text with halo (outline) for readability */
void ct_render_text_halo(CTRenderContext *ctx,
                         const CTFont *font,
                         const char *text,
                         int x, int y,
                         CTColor text_color,
                         CTColor halo_color,
                         int halo_width);

/* Draw text along a path (for roads/rivers) */
void ct_render_text_path(CTRenderContext *ctx,
                         const CTFont *font,
                         const char *text,
                         const CTTilePoint *path,
                         int path_len,
                         float start_offset,
                         int is_flipped,
                         CTColor text_color,
                         CTColor halo_color);
```

### 5.2 Rendering Order

Update `ct_render_tile()` to render labels last:

```c
void ct_render_tile(CTRenderContext *ctx, CTTile *tile, int zoom)
{
    /* 1. Clear to background */
    ct_render_clear(ctx);

    /* 2. Render layers in order */
    /* landuse → water → buildings → roads → railways → boundaries */

    /* 3. Render labels (always last, on top of everything) */
    ct_render_labels(ctx, tile, zoom);
}
```

## Phase 6: MVT Encoding

### 6.1 Labels Layer

Add a `labels` layer to MVT output with point geometry and text properties:

```c
/* MVT labels layer encoding */
void ct_mvt_encode_labels(CTMVTEncoder *enc,
                          const CTTile *tile,
                          int zoom);
```

Each label feature has:
- **geometry**: Point (placement anchor)
- **properties**:
  - `name`: Text to display
  - `class`: Feature class (city, road, water, etc.)
  - `rank`: Priority rank for filtering
  - `anchor`: Preferred anchor direction

### 6.2 Client-Side Rendering

For MVT tiles, labels are rendered client-side (MapLibre/Mapbox GL):

```json
{
  "id": "place-city",
  "type": "symbol",
  "source": "carta",
  "source-layer": "labels",
  "filter": ["==", "class", "city"],
  "layout": {
    "text-field": "{name}",
    "text-size": 14,
    "text-anchor": "bottom"
  },
  "paint": {
    "text-color": "#333",
    "text-halo-color": "#fff",
    "text-halo-width": 2
  }
}
```

## Phase 7: Performance Optimization

### 7.1 Label Caching

For server-side rendering, cache label placements per tile:

```c
typedef struct {
    CTTileCoord coord;
    CTLabelPlacement *placements;
    int num_placements;
} CTLabelCache;
```

### 7.2 Early Filtering

Skip label calculation for:
- Features without names
- Features too small at current zoom
- Features outside tile bounds (with buffer)

### 7.3 Parallel Placement

Label placement can be parallelized per tile since each tile has independent collision detection.

## Implementation Order

### Milestone 1: Basic Point Labels (MVP)
1. Parse place nodes from PBF
2. Implement bitmap font system
3. Basic point label placement (single anchor)
4. Text rendering with halo
5. Simple collision detection

**Deliverable:** City/town names appear on PNG tiles

### Milestone 2: Road Labels
1. Line label placement algorithm
2. Text-along-path rendering
3. Road name extraction
4. Label repetition along long roads

**Deliverable:** Road names on PNG tiles

### Milestone 3: Area Labels
1. Pole of inaccessibility algorithm
2. Area label placement
3. Water body names
4. Forest/park names

**Deliverable:** Lake and park names on PNG tiles

### Milestone 4: MVT Labels
1. Labels layer in MVT output
2. Properties encoding
3. MapLibre style configuration

**Deliverable:** Labels in vector tiles for client-side rendering

### Milestone 5: Polish
1. Multi-anchor point placement
2. Label priority tuning
3. Zoom-dependent font sizes
4. Performance optimization
5. Unicode/UTF-8 support for international names

## File Structure

```
carta/
├── include/
│   ├── ct_font.h          # MSDF font structures and API
│   ├── ct_label.h         # Label placement API
│   └── ct_collision.h     # Collision detection API
├── src/
│   ├── ct_font.c          # MSDF font loading and measurement
│   ├── ct_font_data.c     # Embedded font data (generated)
│   ├── ct_font_msdf.c     # MSDF rendering algorithm
│   ├── ct_label.c         # Label placement algorithms
│   ├── ct_label_point.c   # Point label placement
│   ├── ct_label_line.c    # Line label placement
│   ├── ct_label_area.c    # Area label placement
│   └── ct_collision.c     # Collision detection
shared/
├── fonts/
│   ├── CLAUDE.md          # Font documentation
│   ├── ui-font.json       # MSDF glyph metrics
│   └── ui-font.png        # MSDF atlas texture
└── ui/clay-shards-webgl/
    └── font.js            # Reference MSDF implementation (JS)
```

## Memory Budget

Per tile (256x256):
- Collision grid: 32x32 cells = 128 bytes
- Label placements: ~50 labels × 32 bytes = 1.6 KB
- Font data: ~20 KB (shared across all tiles)

Total: < 25 KB per tile (negligible)

## Testing Plan

1. **Unit tests:**
   - Font measurement
   - Collision detection
   - Point placement
   - Line placement (curve detection, flip logic)
   - Area center calculation

2. **Visual tests:**
   - Render sample tiles with labels
   - Compare to reference images

3. **Performance tests:**
   - Label placement throughput
   - Collision detection speed
   - Memory usage

## Dependencies

- **MSDF Font:** Reuses `shared/ui/clay-shards-webgl/fonts/ui-font.*`
- **PNG decoding:** Uses existing `shared/` library (for loading atlas texture)
- **JSON parsing:** Simple parser for font metrics (or embed as C struct)
- Uses existing carta infrastructure (tile coords, rendering, MVT encoding)
- Font atlas is embedded at build time for WASM/static builds

## Questions to Resolve

1. **Font coverage:** Current ui-font covers Latin + common symbols. Need CJK/Arabic?
2. **Multiple fonts:** Need bold/italic variants? Different sizes?
3. **Abbreviations:** Should "Street" → "St", "Avenue" → "Ave"?
4. **Localization:** Support `name:en`, `name:de` variants?
5. **Font generation:** Use msdf-atlas-gen or similar if new fonts needed
