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

### 2.1 MSDF Font Library (shared/)

**The MSDF font library is already implemented in `shared/`.** Carta will link against this
library for text measurement and MSDF sampling.

**Existing implementation:**
- `shared/include/sh_font.h` - Font API
- `shared/src/sh_font.c` - Implementation (UTF-8, glyph lookup, text measurement, MSDF)
- `shared/src/sh_font_data.c` - Embedded font data (163 glyphs, 332x332 atlas)
- `shared/fonts/generate_font_data.py` - Build-time font generator

**Font assets:**
- `shared/fonts/ui-font.json` - Glyph metrics
- `shared/fonts/ui-font.png` - MSDF atlas texture

### 2.2 Using the Shared Font Library

Carta links against `libshared.a` which includes the font library:

```c
#include "sh_font.h"

/* Get the embedded font */
const SHFont *font = sh_font_get_default();

/* Get glyph for character */
const SHGlyph *g = sh_font_get_glyph(font, 'A');

/* Measure text width at given font size */
float width = sh_font_text_width(font, "Hello", 16.0f);

/* Measure text with character limit */
float w5 = sh_font_text_width_n(font, "Hello World", 5, 16.0f);

/* Get line height for font size */
float lh = sh_font_line_height(font, 16.0f);

/* UTF-8 decoding */
uint32_t codepoint;
int bytes = sh_utf8_decode("é", &codepoint);  /* Returns 2, codepoint=0x00E9 */
```

### 2.3 MSDF Sampling for Software Rendering

The shared library provides MSDF sampling for software rasterization:

```c
/* Sample MSDF atlas at pixel coordinates */
uint8_t dist = sh_font_sample_msdf(font, x, y);

/* Check if point is inside glyph */
int inside = sh_font_msdf_inside(font, glyph, local_x, local_y);

/* Get anti-aliased coverage [0.0, 1.0] */
float coverage = sh_font_msdf_coverage(font, glyph, local_x, local_y, font_size);
```

The MSDF algorithm (implemented in `sh_font.c`):
1. Sample R, G, B channels from atlas
2. Take median of the three values
3. Convert to signed distance [-1, 1]
4. Apply smoothstep for anti-aliasing based on font size

### 2.4 Regenerating Font Data

To regenerate the embedded font data from new assets:

```bash
cd shared && make generate-font-data
```

This runs `fonts/generate_font_data.py` which converts JSON + PNG to `src/sh_font_data.c`.

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

### Milestone 1: Basic Point Labels (MVP) ✓ COMPLETE

| Task | Status | Files |
|------|--------|-------|
| Parse place nodes from PBF | ✓ | `ct_pbf.c` - `parse_dense_nodes()`, `classify_place()` |
| Collision detection system | ✓ | `ct_collision.h`, `ct_collision.c` |
| Label placement with multi-anchor | ✓ | `ct_label.h`, `ct_label.c` |
| Text rendering with halo | ✓ | `ct_render.c` - `ct_render_text_halo()` |
| MSDF font library | ✓ | `shared/` - bilinear sampling, threshold coverage |

**Implementation details:**
- Grid-based collision (1 bit per 8x8 cell, 128 bytes for 256x256 tile)
- 9 anchor positions tried in order: right, corners, cardinal, center
- Priority sorting by place type, population, stable ID
- Font size scaled by place type (country 1.4x, city 1.2x, hamlet 0.9x)
- Bilinear MSDF sampling with smoothstep anti-aliasing
- Threshold-based coverage for halo expansion

**Tests:** 95 carta tests, 23 font tests

**Deliverable:** City/town names appear on PNG tiles ✓

---

### Milestone 2: Road Labels (NEXT)

**Goal:** Road names rendered along the path geometry

| Task | Status | Description |
|------|--------|-------------|
| Line label placement | TODO | Find suitable segments, check curvature |
| Text-along-path rendering | TODO | `ct_render_text_path()` with per-glyph rotation |
| Road name extraction | PARTIAL | Names parsed, need consistent tile access |
| Label repetition | TODO | Repeat every ~200px on long roads |

**Key algorithm - Line label placement:**
1. Measure text width at target font size
2. Walk the path finding candidate segments:
   - Length >= text width + padding
   - Max angle between adjacent segments < 30°
   - Prefer segments closer to horizontal
3. Score candidates by: length, straightness, centrality
4. Check collision for text bounding boxes along path
5. Flip text if path goes right-to-left (always read left-to-right)

**Key algorithm - Text-along-path rendering:**
1. For each character in text:
   - Calculate cumulative advance along path
   - Find position on path at that distance
   - Calculate tangent angle at that point
   - Render glyph rotated to match tangent
2. Apply halo by rendering expanded glyphs first

**Data structure:**
```c
typedef struct {
    CTTilePoint *path;       /* Smoothed path for text to follow */
    int path_len;
    float start_offset;      /* Where along path to start text */
    int is_flipped;          /* 1 if flipped to read left-to-right */
} CTLineLabelPlacement;

/* Find best position along a line for label */
CTLineLabelPlacement *ct_label_place_line(
    CTLabelPlacer *placer,
    const char *text,
    const CTTilePoint *points,
    int num_points,
    const SHFont *font,
    float font_size
);

/* Draw text along a curved path */
void ct_render_text_path(CTRenderContext *ctx,
                         const char *text,
                         const CTTilePoint *path,
                         int path_len,
                         float start_offset,
                         int is_flipped,
                         const SHFont *font,
                         float font_size,
                         CTColor fill_color,
                         CTColor halo_color,
                         float halo_width);
```

**Constants:**
```c
#define LINE_LABEL_MIN_LENGTH   50.0f   /* Min segment length in pixels */
#define LINE_LABEL_MAX_ANGLE    30.0f   /* Max angle between segments (degrees) */
#define LINE_LABEL_REPEAT_GAP   200.0f  /* Gap between repeated labels */
#define LINE_LABEL_PADDING      10.0f   /* Padding at segment ends */
```

**Deliverable:** Road names on PNG tiles

---

### Milestone 3: Area Labels

### Milestone 3: Area Labels

**Goal:** Lake, park, and forest names centered in polygons

| Task | Status | Description |
|------|--------|-------------|
| Pole of inaccessibility | TODO | Find point furthest from polygon edges |
| Area label placement | TODO | Place text at visual center |
| Water body names | TODO | Lakes, reservoirs |
| Forest/park names | TODO | Natural areas |

**Algorithm - Pole of inaccessibility:**
- Iterative grid search to find point maximally distant from all edges
- Faster than true geometric solution, good enough for labels

**Deliverable:** Lake and park names on PNG tiles

---

### Milestone 4: MVT Labels

**Goal:** Labels in vector tiles for client-side rendering

| Task | Status | Description |
|------|--------|-------------|
| Labels layer encoding | TODO | Add `labels` layer to MVT output |
| Properties encoding | TODO | name, class, rank, anchor |
| MapLibre style | TODO | Example style configuration |

**MVT label properties:**
- `name`: Display text
- `class`: Feature class (city, road, water, etc.)
- `rank`: Priority for client-side filtering
- `anchor`: Preferred anchor direction

**Deliverable:** Labels in vector tiles for client-side rendering

---

### Milestone 5: Polish

**Goal:** Production-quality labels

| Task | Status | Description |
|------|--------|-------------|
| Multi-anchor point placement | ✓ | Done in Milestone 1 (9 anchors) |
| Label priority tuning | PARTIAL | Basic priority, needs refinement |
| Zoom-dependent font sizes | ✓ | Done in Milestone 1 |
| Performance optimization | TODO | Caching, early filtering |
| International names | TODO | Full Unicode support, RTL text |

**Deliverable:** Production-ready label system

## File Structure

```
carta/
├── include/
│   ├── ct_label.h         # Label placement API
│   └── ct_collision.h     # Collision detection API
├── src/
│   ├── ct_label.c         # Label placement algorithms
│   ├── ct_label_point.c   # Point label placement
│   ├── ct_label_line.c    # Line label placement
│   ├── ct_label_area.c    # Area label placement
│   └── ct_collision.c     # Collision detection
shared/
├── include/
│   └── sh_font.h          # MSDF font API (used by carta)
├── src/
│   ├── sh_font.c          # Font implementation (UTF-8, measurement, MSDF)
│   └── sh_font_data.c     # Embedded font data (generated)
├── fonts/
│   ├── CLAUDE.md          # Font documentation
│   ├── ui-font.json       # MSDF glyph metrics
│   ├── ui-font.png        # MSDF atlas texture
│   └── generate_font_data.py  # Build-time font generator
├── tests/
│   └── test_font.c        # 20 font unit tests
└── ui/clay-shards-webgl/
    └── font.js            # WebGL MSDF renderer (JS)
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

- **MSDF Font Library:** `shared/` library (`sh_font.h`, `sh_font.c`) - already implemented
  - 163 glyphs, 332x332 atlas (440KB embedded)
  - UTF-8 decoding with error handling
  - O(1) ASCII lookup, binary search for Unicode
  - Text measurement and MSDF sampling
  - 20 unit tests
- **Font Assets:** `shared/fonts/ui-font.json` + `ui-font.png`
- Uses existing carta infrastructure (tile coords, rendering, MVT encoding)
- No runtime file loading - font atlas embedded at compile time

## Questions to Resolve

1. **Font coverage:** Current ui-font covers Latin + common symbols. Need CJK/Arabic?
2. **Multiple fonts:** Need bold/italic variants? Different sizes?
3. **Abbreviations:** Should "Street" → "St", "Avenue" → "Ave"?
4. **Localization:** Support `name:en`, `name:de` variants?
5. **Font generation:** Use msdf-atlas-gen or similar if new fonts needed
