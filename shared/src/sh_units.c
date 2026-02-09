/*
 * sh_units.c - Unit System Conversions
 *
 * Implementation of metric/imperial unit conversions.
 */

#include "sh_units.h"
#include <string.h>
#include <ctype.h>
#include <stddef.h>

/* ============================================================================
 * Distance Conversions
 * ============================================================================ */

double sh_km_to_miles(double km)
{
    return km * SH_MILES_PER_KM;
}

double sh_miles_to_km(double miles)
{
    return miles * SH_KM_PER_MILE;
}

double sh_m_to_km(double m)
{
    return m / 1000.0;
}

double sh_km_to_m(double km)
{
    return km * 1000.0;
}

double sh_m_to_miles(double m)
{
    return m / 1609.344;
}

double sh_miles_to_m(double miles)
{
    return miles * 1609.344;
}

/* Medium distances */

double sh_m_to_ft(double m)
{
    return m * SH_FT_PER_M;
}

double sh_ft_to_m(double ft)
{
    return ft * SH_M_PER_FT;
}

double sh_m_to_yards(double m)
{
    return m * SH_YARD_PER_M;
}

double sh_yards_to_m(double yards)
{
    return yards * SH_M_PER_YARD;
}

/* Small distances */

double sh_cm_to_in(double cm)
{
    return cm * SH_IN_PER_CM;
}

double sh_in_to_cm(double in)
{
    return in * SH_CM_PER_IN;
}

double sh_mm_to_in(double mm)
{
    return mm / 10.0 * SH_IN_PER_CM;
}

double sh_in_to_mm(double in)
{
    return in * SH_CM_PER_IN * 10.0;
}

/* ============================================================================
 * Volume Conversions
 * ============================================================================ */

double sh_liters_to_gallons(double liters)
{
    return liters * SH_GALLONS_PER_LITER;
}

double sh_gallons_to_liters(double gallons)
{
    return gallons * SH_LITERS_PER_GALLON;
}

double sh_ml_to_floz(double ml)
{
    return ml * SH_FL_OZ_PER_ML;
}

double sh_floz_to_ml(double floz)
{
    return floz * SH_ML_PER_FL_OZ;
}

/* ============================================================================
 * Weight Conversions
 * ============================================================================ */

double sh_kg_to_lbs(double kg)
{
    return kg * SH_LBS_PER_KG;
}

double sh_lbs_to_kg(double lbs)
{
    return lbs * SH_KG_PER_LB;
}

double sh_g_to_oz(double g)
{
    return g * SH_OZ_PER_G;
}

double sh_oz_to_g(double oz)
{
    return oz * SH_G_PER_OZ;
}

double sh_kg_to_tons_us(double kg)
{
    return kg / SH_KG_PER_TON_US;
}

double sh_tons_us_to_kg(double tons)
{
    return tons * SH_KG_PER_TON_US;
}

double sh_kg_to_tonnes(double kg)
{
    return kg / SH_KG_PER_TONNE;
}

double sh_tonnes_to_kg(double tonnes)
{
    return tonnes * SH_KG_PER_TONNE;
}

/* ============================================================================
 * Fuel Efficiency Conversions (inverse relationship!)
 * ============================================================================ */

double sh_mpg_to_l100km(double mpg)
{
    if (mpg <= 0) return 0;  /* avoid division by zero */
    return SH_MPG_L100KM_FACTOR / mpg;
}

double sh_l100km_to_mpg(double l100km)
{
    if (l100km <= 0) return 0;  /* avoid division by zero */
    return SH_MPG_L100KM_FACTOR / l100km;
}

/* ============================================================================
 * Price Conversions
 * ============================================================================ */

double sh_price_per_gallon_to_liter(double price_per_gallon)
{
    return price_per_gallon * SH_GALLONS_PER_LITER;
}

double sh_price_per_liter_to_gallon(double price_per_liter)
{
    return price_per_liter * SH_LITERS_PER_GALLON;
}

/* ============================================================================
 * Parsing
 * ============================================================================ */

SHUnitSystem sh_parse_units(const char *str)
{
    if (!str) return SH_UNITS_METRIC;

    /* Skip whitespace and quotes */
    while (*str && (isspace((unsigned char)*str) || *str == '"')) str++;

    /* Case-insensitive comparison for "imperial" */
    if (strncasecmp(str, "imperial", 8) == 0) {
        return SH_UNITS_IMPERIAL;
    }

    return SH_UNITS_METRIC;  /* default */
}

const char *sh_units_string(SHUnitSystem units)
{
    return units == SH_UNITS_IMPERIAL ? "imperial" : "metric";
}
