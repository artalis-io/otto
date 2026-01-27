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

    /* Road widths at z=14 */
    style->road_widths[CT_ROAD_MOTORWAY]    = 8.0f;
    style->road_widths[CT_ROAD_TRUNK]       = 7.0f;
    style->road_widths[CT_ROAD_PRIMARY]     = 6.0f;
    style->road_widths[CT_ROAD_SECONDARY]   = 5.0f;
    style->road_widths[CT_ROAD_TERTIARY]    = 4.0f;
    style->road_widths[CT_ROAD_RESIDENTIAL] = 3.0f;
    style->road_widths[CT_ROAD_SERVICE]     = 2.0f;
    style->road_widths[CT_ROAD_OTHER]       = 1.5f;

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
