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
     * Updated to better match OSM Carto visual appearance.
     */
    style->road_widths[CT_ROAD_MOTORWAY]    = (CTRoadWidth){ 2.5f, 5.0f, 10.0f };
    style->road_widths[CT_ROAD_TRUNK]       = (CTRoadWidth){ 2.0f, 4.5f, 9.0f };
    style->road_widths[CT_ROAD_PRIMARY]     = (CTRoadWidth){ 1.5f, 4.0f, 8.0f };
    style->road_widths[CT_ROAD_SECONDARY]   = (CTRoadWidth){ 1.2f, 3.5f, 7.0f };
    style->road_widths[CT_ROAD_TERTIARY]    = (CTRoadWidth){ 1.0f, 3.0f, 6.0f };
    style->road_widths[CT_ROAD_RESIDENTIAL] = (CTRoadWidth){ 0.8f, 2.5f, 5.0f };
    style->road_widths[CT_ROAD_SERVICE]     = (CTRoadWidth){ 0.5f, 1.5f, 3.0f };
    style->road_widths[CT_ROAD_OTHER]       = (CTRoadWidth){ 0.5f, 1.5f, 3.0f };

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

    /* Area colors (OSM Carto-matched)
     * Reference: https://github.com/gravitystorm/openstreetmap-carto
     */
    style->water_color           = CT_RGB(170, 211, 223);  /* #aad3df - OSM water */
    style->land_color            = CT_RGB(242, 239, 233);  /* #f2efe9 - OSM land */
    style->building_color        = CT_RGB(217, 208, 201);  /* #d9d0c9 - OSM building fill */
    style->building_outline_color = CT_RGB(196, 182, 171); /* #c4b6ab - darker for visibility */
    style->forest_color          = CT_RGB(173, 209, 158);  /* #add19e - muted forest green */
    style->grass_color           = CT_RGB(205, 235, 176);  /* #cdebb0 - OSM grass */
    style->park_color            = CT_RGB(198, 236, 199);  /* #c6ecc7 - muted park green */
    style->sand_color            = CT_RGB(245, 233, 186);  /* Beach/sand */

    /* Railway colors by type (OSM Carto-inspired) */
    style->railway_colors[CT_RAILWAY_RAIL]         = CT_RGB(112, 112, 112);  /* Main rail - dark gray */
    style->railway_colors[CT_RAILWAY_SUBWAY]       = CT_RGB(100, 100, 180);  /* Subway - bluish */
    style->railway_colors[CT_RAILWAY_TRAM]         = CT_RGB(68, 68, 68);     /* Tram - darker gray */
    style->railway_colors[CT_RAILWAY_NARROW_GAUGE] = CT_RGB(112, 112, 112);  /* Narrow gauge */
    style->railway_colors[CT_RAILWAY_PRESERVED]    = CT_RGB(140, 100, 80);   /* Heritage - brownish */
    style->railway_colors[CT_RAILWAY_DISUSED]      = CT_RGB(180, 180, 180);  /* Disused - light gray */
    style->railway_colors[CT_RAILWAY_OTHER]        = CT_RGB(140, 140, 140);  /* Other */

    /* Railway outline colors (for crosshatch/dash effect) */
    style->railway_outline_colors[CT_RAILWAY_RAIL]         = CT_RGB(255, 255, 255);
    style->railway_outline_colors[CT_RAILWAY_SUBWAY]       = CT_RGB(255, 255, 255);
    style->railway_outline_colors[CT_RAILWAY_TRAM]         = CT_RGB(200, 200, 200);
    style->railway_outline_colors[CT_RAILWAY_NARROW_GAUGE] = CT_RGB(255, 255, 255);
    style->railway_outline_colors[CT_RAILWAY_PRESERVED]    = CT_RGB(220, 200, 180);
    style->railway_outline_colors[CT_RAILWAY_DISUSED]      = CT_RGB(220, 220, 220);
    style->railway_outline_colors[CT_RAILWAY_OTHER]        = CT_RGB(200, 200, 200);

    /* Railway widths by type */
    style->railway_widths[CT_RAILWAY_RAIL]         = 2.0f;
    style->railway_widths[CT_RAILWAY_SUBWAY]       = 2.0f;
    style->railway_widths[CT_RAILWAY_TRAM]         = 1.5f;
    style->railway_widths[CT_RAILWAY_NARROW_GAUGE] = 1.5f;
    style->railway_widths[CT_RAILWAY_PRESERVED]    = 1.5f;
    style->railway_widths[CT_RAILWAY_DISUSED]      = 1.0f;
    style->railway_widths[CT_RAILWAY_OTHER]        = 1.5f;

    /* Bridge styling (stronger outline for elevation effect) */
    style->bridge_outline_color = CT_RGB(100, 100, 100);  /* Dark outline */
    style->bridge_outline_width = 1.5f;                    /* Extra outline width */

    /* Boundaries (admin borders) - subtle purple like OSM Carto */
    style->boundary_color = CT_RGBA(170, 80, 170, 140);  /* More transparent purple */
    style->boundary_width = 1.0f;  /* Thinner for less clutter */

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

/*
 * Get width for a railway type at a specific zoom level.
 * Base width scales with zoom similar to roads.
 */
float ct_style_railway_width(const CTStyle *style, CTRailwayType railway_type, int zoom)
{
    if (railway_type < 0 || railway_type >= CT_RAILWAY_TYPE_COUNT) {
        return 1.5f;  /* Fallback */
    }

    float base_width = style->railway_widths[railway_type];

    /* Scale with zoom (railways are thinner at low zoom) */
    float scale = 1.0f;
    if (zoom <= 10) {
        scale = 0.5f;
    } else if (zoom <= 12) {
        scale = 0.75f;
    } else if (zoom >= 16) {
        scale = 1.5f;
    }

    float width = base_width * scale;

    /* Ensure minimum visibility */
    if (width < 0.5f) width = 0.5f;

    return width;
}

/*
 * Get styling for a boundary based on type and admin level.
 */
void ct_style_boundary(const CTStyle *style, CTBoundaryType boundary_type,
                       int admin_level, int zoom,
                       CTColor *color_out, float *width_out,
                       float *dash_out, float *gap_out)
{
    (void)style;  /* For future custom styling */

    /* Default style */
    CTColor color = CT_RGBA(170, 80, 170, 140);
    float width = 1.0f;
    float dash = 8.0f;
    float gap = 4.0f;

    if (boundary_type == CT_BOUNDARY_TYPE_PROTECTED) {
        /* Protected areas (national parks) - green dashed */
        color = CT_RGBA(85, 170, 85, 160);  /* Semi-transparent green */
        width = 1.5f;
        dash = 6.0f;
        gap = 3.0f;
    } else {
        /* Administrative boundaries - purple by admin level */
        switch (admin_level) {
            case CT_BOUNDARY_COUNTRY:  /* admin_level=2 */
                color = CT_RGBA(140, 60, 140, 180);  /* Stronger purple */
                width = 2.0f;
                dash = 10.0f;
                gap = 5.0f;
                break;
            case CT_BOUNDARY_STATE:  /* admin_level=4 */
                color = CT_RGBA(160, 80, 160, 160);
                width = 1.5f;
                dash = 8.0f;
                gap = 4.0f;
                break;
            case CT_BOUNDARY_COUNTY:  /* admin_level=6 */
                color = CT_RGBA(180, 100, 180, 140);
                width = 1.0f;
                dash = 6.0f;
                gap = 3.0f;
                break;
            default:  /* admin_level=8+ */
                color = CT_RGBA(200, 140, 200, 120);  /* Fainter */
                width = 0.75f;
                dash = 4.0f;
                gap = 2.0f;
                break;
        }
    }

    /* Scale width with zoom */
    if (zoom <= 8) {
        width *= 0.5f;
    } else if (zoom <= 10) {
        width *= 0.75f;
    } else if (zoom >= 14) {
        width *= 1.25f;
    }

    if (width < 0.5f) width = 0.5f;

    *color_out = color;
    *width_out = width;
    *dash_out = dash;
    *gap_out = gap;
}
