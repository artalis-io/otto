# Carta Feature TODOs

This document outlines planned features for Carta with detailed implementation plans.

## Table of Contents

1. [Level of Detail (LOD)](#1-level-of-detail-lod)
2. [Font/Label Rendering](#2-fontlabel-rendering)
3. [Configurable Styling](#3-configurable-styling)

---

## 1. Level of Detail (LOD)

### Problem

Currently, Carta renders all features at all zoom levels. This causes:
- **Visual clutter** at low zoom (z0-z10): residential roads, small buildings, minor waterways overwhelm the map
- **Performance issues**: unnecessary rendering of details invisible at the current scale
- **Large tile sizes**: encoding features that won't be visible increases MVT/PNG size

### Solution Overview

Implement zoom-dependent feature filtering at two stages:
1. **PBF Query Stage**: Filter features during spatial index queries
2. **Render Stage**: Skip features below minimum size threshold

### Implementation Plan

#### Phase 1: Define LOD Rules (`ct_lod.h`, `ct_lod.c`)

Create a new module for LOD configuration:

```c
// ct_lod.h
typedef struct {
    CTLayer layer;
    int feature_type;       // -1 for all types in layer
    int min_zoom;           // Minimum zoom to show this feature
    int max_zoom;           // Maximum zoom (optional, -1 for no limit)
    float min_area_sqm;     // Minimum area for polygons (at this zoom)
    float min_length_m;     // Minimum length for lines (at this zoom)
} CTLODRule;

typedef struct {
    CTLODRule *rules;
    int num_rules;
} CTLODConfig;

// Predefined configs
void ct_lod_default(CTLODConfig *config);
void ct_lod_detailed(CTLODConfig *config);   // More features at lower zoom
void ct_lod_minimal(CTLODConfig *config);    // Fewer features
```

**Default LOD Rules:**

| Layer | Feature Type | Min Zoom | Notes |
|-------|-------------|----------|-------|
| Roads | Motorway | 5 | Always visible at country level |
| Roads | Trunk | 7 | Visible at regional level |
| Roads | Primary | 9 | Visible at city level |
| Roads | Secondary | 11 | Visible at district level |
| Roads | Tertiary | 13 | Visible at neighborhood level |
| Roads | Residential | 14 | Visible at street level |
| Roads | Service | 16 | Only at close zoom |
| Buildings | All | 14 | Only at street level |
| Water | Large lakes | 5 | > 1km area |
| Water | Rivers | 8 | Major rivers |
| Water | Streams | 12 | Minor waterways |
| Railways | Main | 8 | Intercity rail |
| Railways | Other | 12 | Local rail |
| Landuse | Large areas | 10 | > 10km area |
| Landuse | Small areas | 14 | Parks, gardens |

#### Phase 2: Integrate with PBF Queries (`ct_pbf.c`)

Modify `ct_pbf_get_bbox_features()` to accept zoom level:

```c
// Current signature:
CTStatus ct_pbf_get_bbox_features(const CTPBFContext *ctx, CTBBox bbox,
                                   CTFeature **features, size_t *count);

// New signature:
CTStatus ct_pbf_get_tile_features(const CTPBFContext *ctx, CTTileCoord coord,
                                   const CTLODConfig *lod,
                                   CTFeature **features, size_t *count);
```

**Filtering logic in query:**
1. Check `min_zoom <= coord.z <= max_zoom`
2. For polygons: estimate area, skip if below threshold
3. For lines: estimate length, skip if below threshold
4. Return only features passing LOD filter

#### Phase 3: Geometry Simplification (`ct_simplify.c`)

At lower zoom levels, simplify geometry to reduce points:

```c
// Douglas-Peucker simplification
void ct_simplify_linestring(CTTilePoint *points, int *num_points, float tolerance);
void ct_simplify_polygon(CTTilePoint *points, int *num_points, float tolerance);

// Tolerance based on zoom (pixels at that zoom)
float ct_simplify_tolerance(int zoom);  // e.g., z10 = 4px, z14 = 1px, z18 = 0.25px
```

#### Phase 4: Render-time Filtering (`ct_render.c`)

Add minimum size checks in `ct_render_tile()`:

```c
// Skip features smaller than threshold at render time
float min_pixels = 2.0f;  // Minimum visible size

for (size_t i = 0; i < tile->num_features; i++) {
    const CTFeature *f = &tile->features[i];

    // Calculate bounding box in pixels
    float width = (max_x - min_x) * scale;
    float height = (max_y - min_y) * scale;

    if (f->type == CT_GEOM_LINESTRING && line_length < min_pixels) continue;
    if (f->type == CT_GEOM_POLYGON && width < min_pixels && height < min_pixels) continue;

    // Render feature...
}
```

### TODOs

- [ ] Create `include/ct_lod.h` with LOD rule structures
- [ ] Create `src/ct_lod.c` with default LOD configurations
- [ ] Add `min_zoom` field to `CTOSMWay` during PBF parsing
- [ ] Modify `ct_pbf_get_bbox_features()` to accept zoom parameter
- [ ] Implement area/length estimation for LOD filtering
- [ ] Add Douglas-Peucker simplification in `ct_simplify.c`
- [ ] Integrate LOD config into `ct_generate_png()` and `ct_generate_mvt()`
- [ ] Add render-time size threshold filtering
- [ ] Update tests for LOD behavior
- [ ] Document LOD configuration in README

### Files to Modify/Create

| File | Action |
|------|--------|
| `include/ct_lod.h` | **Create** - LOD rule structures |
| `src/ct_lod.c` | **Create** - LOD configuration functions |
| `src/ct_simplify.c` | **Create** - Geometry simplification |
| `include/ct_simplify.h` | **Create** - Simplification header |
| `src/ct_pbf.c` | **Modify** - Add zoom-aware feature queries |
| `src/ct_render.c` | **Modify** - Add render-time LOD filtering |
| `include/ct_types.h` | **Modify** - Add min_zoom to CTOSMWay |

---

## 2. Font/Label Rendering

### Problem

Map tiles need text labels for:
- Road names
- Place names (cities, towns, villages)
- Points of interest
- Water body names

Currently, `CT_LAYER_LABELS` exists in the enum but has no implementation.

### Solution Options

#### Option A: Embedded Bitmap Font (Zero Dependencies)

**Approach**: Embed a simple bitmap font directly in the code.

**Pros:**
- Zero external dependencies
- Small footprint (~10-20KB for basic ASCII)
- Fast rendering (direct pixel copy)
- WASM-friendly

**Cons:**
- Limited to ASCII or small character sets
- Fixed sizes (need multiple bitmaps for different sizes)
- No anti-aliasing without multiple alpha levels
- Not suitable for international text

**Implementation:**

```c
// ct_font.h
typedef struct {
    uint8_t *bitmap;        // 1-bit or 8-bit alpha bitmap
    int char_width;         // Fixed width (or 0 for variable)
    int char_height;
    int *char_widths;       // For variable-width fonts
    int first_char;         // Usually 32 (space)
    int num_chars;          // Usually 95 (ASCII printable)
} CTBitmapFont;

// Built-in fonts
extern const CTBitmapFont ct_font_small;   // 8px height
extern const CTBitmapFont ct_font_medium;  // 12px height
extern const CTBitmapFont ct_font_large;   // 16px height

void ct_render_text(CTRenderContext *ctx, const char *text,
                    int x, int y, const CTBitmapFont *font, CTColor color);
```

**Data source**: Convert a public domain bitmap font (e.g., IBM PC fonts, GNU Unifont subset) to C arrays.

#### Option B: Vendored Font Library (stb_truetype)

**Approach**: Vendor `stb_truetype.h` (single-header, public domain).

**Pros:**
- TrueType/OpenType font support
- Scalable to any size
- Anti-aliased rendering
- Full Unicode support
- Still zero external dependencies (vendored)

**Cons:**
- Larger codebase (~5000 lines)
- Requires bundled font file
- More complex integration
- CPU-intensive glyph rasterization

**Implementation:**

```c
// ct_font.h (with stb_truetype)
typedef struct {
    stbtt_fontinfo info;
    uint8_t *data;          // Font file data
    float scale;            // Current scale factor
} CTFont;

CTFont *ct_font_load(const uint8_t *ttf_data, size_t size);
void ct_font_free(CTFont *font);

void ct_render_text(CTRenderContext *ctx, const char *text,
                    int x, int y, CTFont *font, float size, CTColor color);
```

**Font bundling**: Embed a permissively-licensed font (e.g., DejaVu Sans, Noto Sans) as a C array.

#### Option C: Hybrid Approach (Recommended)

Use **Option A (bitmap fonts) as default** with **Option B as optional**.

1. Ship with embedded bitmap fonts for basic ASCII labels
2. Allow users to optionally load TTF fonts via `stb_truetype`
3. Configure at compile time with `-DCT_USE_STB_TRUETYPE`

### Label Placement Algorithm

Regardless of font choice, need intelligent label placement:

```c
// ct_label.h
typedef struct {
    char *text;
    double lat, lon;        // Anchor point
    CTLayer layer;          // For priority sorting
    int feature_type;       // Road type, place type, etc.
    int priority;           // Higher = more important
} CTLabel;

typedef struct {
    CTLabel *labels;
    size_t count;
    // Collision detection grid
    uint8_t *occupied;      // Bitmap of occupied regions
    int grid_width;
    int grid_height;
} CTLabelContext;

// Extract labels from PBF data
void ct_extract_labels(const CTPBFContext *pbf, CTTileCoord coord,
                       CTLabel **labels, size_t *count);

// Place labels avoiding collisions
void ct_place_labels(CTLabelContext *ctx, CTLabel *labels, size_t count);

// Render placed labels
void ct_render_labels(CTRenderContext *render_ctx, const CTLabelContext *label_ctx);
```

**Placement rules:**
1. Sort labels by priority (major roads > minor roads > POIs)
2. For each label:
   - Calculate bounding box
   - Check collision grid
   - If no collision, mark grid cells as occupied
   - If collision, try alternate positions (for point labels)
3. Render non-colliding labels

### Label Styling

```c
// Add to CTStyle
typedef struct {
    CTColor text_color;
    CTColor halo_color;     // Outline/glow around text
    float halo_width;
    int font_size;          // In pixels
    int font_weight;        // 0=normal, 1=bold
} CTLabelStyle;

// In CTStyle:
CTLabelStyle road_label_styles[CT_ROAD_TYPE_COUNT];
CTLabelStyle place_label_style;
CTLabelStyle water_label_style;
```

### TODOs

- [ ] Create `include/ct_font.h` with font structures
- [ ] Create `src/ct_font_bitmap.c` with embedded bitmap font
- [ ] Generate font data from public domain font (tool: `tools/gen_font.py`)
- [ ] Implement basic `ct_render_text()` for bitmap fonts
- [ ] Add text halo rendering (outline around text for readability)
- [ ] Create `include/ct_label.h` with label structures
- [ ] Create `src/ct_label.c` with label extraction and placement
- [ ] Implement collision detection grid for label placement
- [ ] Add `name` field extraction in PBF parsing
- [ ] Integrate label rendering into `ct_render_tile()`
- [ ] (Optional) Vendor `stb_truetype.h` for TTF support
- [ ] (Optional) Embed DejaVu Sans font as C array
- [ ] Add label styling to CTStyle
- [ ] Update tests for label rendering

### Files to Modify/Create

| File | Action |
|------|--------|
| `include/ct_font.h` | **Create** - Font structures and API |
| `src/ct_font_bitmap.c` | **Create** - Bitmap font rendering |
| `src/ct_font_data.c` | **Create** - Embedded font bitmap data |
| `include/ct_label.h` | **Create** - Label structures |
| `src/ct_label.c` | **Create** - Label placement algorithm |
| `vendor/stb_truetype.h` | **Optional** - TTF rendering library |
| `src/ct_font_ttf.c` | **Optional** - TTF font support |
| `tools/gen_font.py` | **Create** - Tool to generate font data |
| `src/ct_pbf.c` | **Modify** - Extract name tags |
| `src/ct_render.c` | **Modify** - Add label rendering pass |
| `include/ct_types.h` | **Modify** - Add label styles to CTStyle |

---

## 3. Configurable Styling

### Problem

Currently, styles are hardcoded in `ct_style.c`:
- Colors are fixed (OSM Carto-like palette)
- No way to change styles without recompiling
- No support for different map themes (dark mode, print, etc.)

### Solution Overview

Implement a flexible styling system with:
1. **Runtime-configurable styles** via C API
2. **Style presets** (light, dark, print, satellite overlay)
3. **Optional style file loading** (simple key-value format)

### Implementation Plan

#### Phase 1: Extended Style API (`ct_style.h`, `ct_style.c`)

Expand the existing `CTStyle` structure:

```c
// ct_style.h (extended)

// Layer visibility flags
typedef struct {
    int show_roads;
    int show_buildings;
    int show_water;
    int show_landuse;
    int show_railways;
    int show_labels;
    int show_boundaries;
} CTLayerVisibility;

// Complete style configuration
typedef struct {
    // Existing fields...
    CTColor road_colors[CT_ROAD_TYPE_COUNT];
    CTColor road_outline_colors[CT_ROAD_TYPE_COUNT];
    float road_widths[CT_ROAD_TYPE_COUNT];
    // ...

    // New: Layer visibility
    CTLayerVisibility visibility;

    // New: Opacity per layer (0.0 - 1.0)
    float road_opacity;
    float building_opacity;
    float water_opacity;
    float landuse_opacity;

    // New: Additional colors
    CTColor boundary_color;
    CTColor coastline_color;
    CTColor bridge_color;
    CTColor tunnel_color;

    // New: Line styles
    typedef enum { CT_LINE_SOLID, CT_LINE_DASHED, CT_LINE_DOTTED } CTLineStyle;
    CTLineStyle railway_line_style;
    CTLineStyle boundary_line_style;

    // New: Label styling (if implemented)
    CTLabelStyle label_styles[CT_LAYER_COUNT];

} CTStyle;

// Style presets
void ct_style_default(CTStyle *style);          // Current light theme
void ct_style_dark(CTStyle *style);             // Dark mode
void ct_style_print(CTStyle *style);            // High contrast for printing
void ct_style_satellite_overlay(CTStyle *style); // Transparent for satellite
void ct_style_minimal(CTStyle *style);          // Roads + water only

// Individual setters for common operations
void ct_style_set_road_color(CTStyle *style, CTRoadType type, CTColor color);
void ct_style_set_road_width(CTStyle *style, CTRoadType type, float width);
void ct_style_set_layer_visible(CTStyle *style, CTLayer layer, int visible);
void ct_style_set_layer_opacity(CTStyle *style, CTLayer layer, float opacity);
```

#### Phase 2: Style Presets

**Dark Mode (`ct_style_dark`):**
```c
void ct_style_dark(CTStyle *style)
{
    style->background_color = CT_RGB(30, 30, 30);
    style->land_color = CT_RGB(40, 40, 40);
    style->water_color = CT_RGB(20, 50, 70);

    style->road_colors[CT_ROAD_MOTORWAY] = CT_RGB(180, 100, 120);
    style->road_colors[CT_ROAD_TRUNK] = CT_RGB(180, 130, 100);
    style->road_colors[CT_ROAD_PRIMARY] = CT_RGB(150, 140, 90);
    style->road_colors[CT_ROAD_SECONDARY] = CT_RGB(100, 100, 80);
    style->road_colors[CT_ROAD_TERTIARY] = CT_RGB(80, 80, 80);
    style->road_colors[CT_ROAD_RESIDENTIAL] = CT_RGB(70, 70, 70);

    style->building_color = CT_RGB(50, 50, 50);
    style->building_outline_color = CT_RGB(60, 60, 60);
    // ...
}
```

**Satellite Overlay (`ct_style_satellite_overlay`):**
```c
void ct_style_satellite_overlay(CTStyle *style)
{
    // Transparent background
    style->background_color = CT_RGBA(0, 0, 0, 0);
    style->land_color = CT_RGBA(0, 0, 0, 0);

    // Semi-transparent roads with bright colors
    style->road_colors[CT_ROAD_MOTORWAY] = CT_RGBA(255, 200, 0, 200);
    style->road_colors[CT_ROAD_PRIMARY] = CT_RGBA(255, 255, 100, 180);

    // Hide buildings and landuse
    style->visibility.show_buildings = 0;
    style->visibility.show_landuse = 0;

    // Bright labels with dark halo
    // ...
}
```

#### Phase 3: Style File Loading (Optional)

Simple key-value format for external configuration:

```ini
# carta-style.conf

# General
background_color = #1e1e1e
land_color = #282828

# Roads
road_motorway_color = #e490a1
road_motorway_width = 8.0
road_trunk_color = #fbb289
road_trunk_width = 7.0

# Visibility
show_buildings = true
show_railways = false

# Opacity
water_opacity = 0.8
```

**Parser:**
```c
// ct_style.h
CTStatus ct_style_load(CTStyle *style, const char *filename);
CTStatus ct_style_load_string(CTStyle *style, const char *config_str);
CTStatus ct_style_save(const CTStyle *style, const char *filename);
```

#### Phase 4: Dashed/Dotted Line Support (`ct_render.c`)

For railways, boundaries, and tunnels:

```c
// Line style parameters
typedef struct {
    float dash_length;      // Length of dash in pixels
    float gap_length;       // Length of gap in pixels
    float offset;           // Starting offset (for animation)
} CTDashPattern;

void ct_render_polyline_dashed(CTRenderContext *ctx,
                               const CTTilePoint *points, int num_points,
                               CTColor color, float width,
                               const CTDashPattern *pattern);
```

### TODOs

- [ ] Extend `CTStyle` with visibility flags and opacity per layer
- [ ] Add `CTLayerVisibility` structure
- [ ] Implement `ct_style_dark()` preset
- [ ] Implement `ct_style_print()` preset
- [ ] Implement `ct_style_satellite_overlay()` preset
- [ ] Implement `ct_style_minimal()` preset
- [ ] Add individual style setter functions
- [ ] Implement dashed line rendering in `ct_render.c`
- [ ] Add `CTLineStyle` enum and dash pattern support
- [ ] (Optional) Implement style file parser
- [ ] (Optional) Add style file saving
- [ ] Update `ct_render_tile()` to respect visibility and opacity
- [ ] Add alpha blending for layer opacity
- [ ] Document style API and presets
- [ ] Add style preview examples in tests

### Files to Modify/Create

| File | Action |
|------|--------|
| `include/ct_style.h` | **Create** - Extended style API (split from ct_types.h) |
| `src/ct_style.c` | **Modify** - Add presets and setters |
| `src/ct_style_dark.c` | **Create** - Dark mode preset |
| `src/ct_style_presets.c` | **Create** - All preset implementations |
| `src/ct_style_parser.c` | **Optional** - Config file parser |
| `src/ct_render.c` | **Modify** - Visibility checks, opacity, dashed lines |
| `include/ct_types.h` | **Modify** - Add visibility, line style enums |

---

## Implementation Priority

Recommended order of implementation:

1. **Level of Detail (LOD)** - High priority
   - Immediate visual improvement at low zoom
   - Performance benefits
   - Foundation for other features

2. **Configurable Styling** - Medium priority
   - Quick wins with presets
   - Visibility toggles are simple
   - Dashed lines add polish

3. **Font/Label Rendering** - Lower priority (more complex)
   - Start with bitmap fonts
   - Label placement is algorithmically complex
   - Can be deferred until LOD/styling are stable

## Testing Strategy

For each feature:
1. **Unit tests**: Individual functions (LOD rules, style parsing, font rendering)
2. **Visual regression tests**: Generate reference tiles, compare after changes
3. **Performance benchmarks**: Measure tile generation time before/after

## Resources

- [Mapbox Vector Tile Spec](https://github.com/mapbox/vector-tile-spec)
- [OSM Carto Style](https://github.com/gravitystorm/openstreetmap-carto)
- [stb_truetype](https://github.com/nothings/stb)
- [Douglas-Peucker Algorithm](https://en.wikipedia.org/wiki/Ramer%E2%80%93Douglas%E2%80%93Peucker_algorithm)
- [Map Label Placement](https://www.cs.ubc.ca/~tmm/courses/cpsc533c-04-spr/readings/text-labeling.html)
