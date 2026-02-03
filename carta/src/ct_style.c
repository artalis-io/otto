/*
 * ct_style.c - Map styling configuration
 */

#include "ct_types.h"
#include "ct_render.h"
#include <math.h>

void ct_default_style(CTStyle *style)
{
    /* Road colors (similar to OSM Carto) */
    style->road_colors[CT_ROAD_MOTORWAY]    = CT_RGB(233, 144, 161);  /* Pink-ish */
    style->road_colors[CT_ROAD_TRUNK]       = CT_RGB(251, 178, 137);  /* Orange */
    style->road_colors[CT_ROAD_PRIMARY]     = CT_RGB(253, 214, 164);  /* Yellow */
    style->road_colors[CT_ROAD_SECONDARY]   = CT_RGB(247, 250, 191);  /* Light yellow */
    style->road_colors[CT_ROAD_TERTIARY]    = CT_RGB(255, 255, 255);  /* White */
    style->road_colors[CT_ROAD_RESIDENTIAL] = CT_RGB(255, 255, 255);  /* White */
    style->road_colors[CT_ROAD_SERVICE]     = CT_RGB(255, 255, 255);  /* White */
    style->road_colors[CT_ROAD_OTHER]       = CT_RGB(200, 200, 200);  /* Gray */

    /* Road outline colors */
    style->road_outline_colors[CT_ROAD_MOTORWAY]    = CT_RGB(200, 100, 120);
    style->road_outline_colors[CT_ROAD_TRUNK]       = CT_RGB(200, 140, 100);
    style->road_outline_colors[CT_ROAD_PRIMARY]     = CT_RGB(200, 170, 100);
    style->road_outline_colors[CT_ROAD_SECONDARY]   = CT_RGB(200, 200, 140);
    style->road_outline_colors[CT_ROAD_TERTIARY]    = CT_RGB(180, 180, 180);
    style->road_outline_colors[CT_ROAD_RESIDENTIAL] = CT_RGB(180, 180, 180);
    style->road_outline_colors[CT_ROAD_SERVICE]     = CT_RGB(180, 180, 180);
    style->road_outline_colors[CT_ROAD_OTHER]       = CT_RGB(150, 150, 150);

    /* Road widths at key zoom levels (OSM Carto-style)
     * Format: { z10, z14, z18 }
     * Widths are linearly interpolated between these points.
     */
    style->road_widths[CT_ROAD_MOTORWAY]    = (CTRoadWidth){ 2.0f, 4.0f, 8.0f };
    style->road_widths[CT_ROAD_TRUNK]       = (CTRoadWidth){ 1.5f, 3.5f, 7.0f };
    style->road_widths[CT_ROAD_PRIMARY]     = (CTRoadWidth){ 1.0f, 3.0f, 6.0f };
    style->road_widths[CT_ROAD_SECONDARY]   = (CTRoadWidth){ 0.8f, 2.5f, 5.0f };
    style->road_widths[CT_ROAD_TERTIARY]    = (CTRoadWidth){ 0.6f, 2.0f, 4.0f };
    style->road_widths[CT_ROAD_RESIDENTIAL] = (CTRoadWidth){ 0.4f, 1.5f, 3.0f };
    style->road_widths[CT_ROAD_SERVICE]     = (CTRoadWidth){ 0.3f, 1.0f, 2.0f };
    style->road_widths[CT_ROAD_OTHER]       = (CTRoadWidth){ 0.3f, 1.0f, 2.0f };

    /* Waterway widths by type (data-driven, not zoom-dependent)
     *
     * NOTE: Major rivers (Danube, Rhine) are typically mapped as POLYGONS
     * using natural=water or waterway=riverbank - those render as filled areas.
     * The waterway=river tag is just the centerline reference, rendered thin.
     *
     * Only small waterways (streams, canals, ditches) are rendered as lines.
     */
    style->waterway_widths[CT_WATERWAY_RIVER]  = 1.5f;   /* Centerline only - actual shape is polygon */
    style->waterway_widths[CT_WATERWAY_CANAL]  = 2.5f;   /* Navigable canals */
    style->waterway_widths[CT_WATERWAY_STREAM] = 1.2f;   /* Small streams */
    style->waterway_widths[CT_WATERWAY_DRAIN]  = 0.8f;   /* Drainage */
    style->waterway_widths[CT_WATERWAY_DITCH]  = 0.6f;   /* Ditches */
    style->waterway_widths[CT_WATERWAY_OTHER]  = 1.0f;   /* Default */

    /* Area colors */
    style->water_color           = CT_RGB(170, 211, 223);  /* Light blue */
    style->land_color            = CT_RGB(242, 239, 233);  /* Beige */
    style->building_color        = CT_RGB(217, 208, 201);  /* Light brown */
    style->building_outline_color = CT_RGB(180, 167, 158);
    style->forest_color          = CT_RGB(173, 209, 158);  /* Green */
    style->grass_color           = CT_RGB(205, 235, 176);  /* Light green */
    style->sand_color            = CT_RGB(245, 233, 186);  /* Beige/yellow */

    /* Railway */
    style->railway_color = CT_RGB(120, 120, 120);
    style->railway_width = 2.0f;

    /* Background */
    style->background_color = CT_RGB(242, 239, 233);  /* Same as land */

    /* Reference zoom */
    style->reference_zoom = 14;
}

float ct_scale_width(float base_width, int zoom, int ref_zoom)
{
    /* Scale width exponentially with zoom difference */
    float scale = powf(2.0f, (float)(zoom - ref_zoom));

    /* Apply some minimum */
    float width = base_width * scale;
    if (width < 1.0f) width = 1.0f;

    return width;
}

/*
 * Get road width at a specific zoom level.
 * Uses linear interpolation between z10, z14, and z18 reference points.
 */
float ct_road_width_at_zoom(const CTRoadWidth *rw, int zoom)
{
    if (zoom <= 10) return rw->z10;
    if (zoom >= 18) return rw->z18;

    /* Linear interpolation between key points */
    if (zoom <= 14) {
        /* Interpolate between z10 and z14 */
        float t = (float)(zoom - 10) / 4.0f;
        return rw->z10 + t * (rw->z14 - rw->z10);
    } else {
        /* Interpolate between z14 and z18 */
        float t = (float)(zoom - 14) / 4.0f;
        return rw->z14 + t * (rw->z18 - rw->z14);
    }
}

/*
 * Get width for a road type at a specific zoom level.
 */
float ct_style_road_width(const CTStyle *style, CTRoadType road_type, int zoom)
{
    if (road_type < 0 || road_type >= CT_ROAD_TYPE_COUNT) {
        return 1.0f;  /* Fallback */
    }

    float width = ct_road_width_at_zoom(&style->road_widths[road_type], zoom);

    /* Ensure minimum visibility */
    if (width < 0.5f) width = 0.5f;

    return width;
}

/*
 * Get width for a waterway type.
 * Widths are data-driven based on waterway class, not zoom-dependent.
 */
float ct_style_waterway_width(const CTStyle *style, CTWaterwayType waterway_type)
{
    if (waterway_type < 0 || waterway_type >= CT_WATERWAY_TYPE_COUNT) {
        return 1.5f;  /* Fallback to stream width */
    }

    float width = style->waterway_widths[waterway_type];

    /* Ensure minimum visibility */
    if (width < 0.5f) width = 0.5f;

    return width;
}
