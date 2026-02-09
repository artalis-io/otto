/*
 * sh_units.h - Unit System Conversions
 *
 * Provides conversions between metric and imperial units.
 * Used by FuelWise and other modules that need unit-agnostic APIs.
 */

#ifndef SH_UNITS_H
#define SH_UNITS_H

/* ============================================================================
 * Unit System Enum
 * ============================================================================ */

typedef enum {
    SH_UNITS_METRIC = 0,    /* DEFAULT: km, liters, L/100km, kg */
    SH_UNITS_IMPERIAL = 1   /* miles, gallons, MPG, lbs */
} SHUnitSystem;

/* ============================================================================
 * Conversion Constants
 * ============================================================================ */

/* Exact conversion factors - inverses are computed to ensure round-trip precision */

/* Distance */
#define SH_KM_PER_MILE       1.609344
#define SH_MILES_PER_KM      (1.0 / 1.609344)
#define SH_M_PER_FT          0.3048
#define SH_FT_PER_M          (1.0 / 0.3048)
#define SH_CM_PER_IN         2.54
#define SH_IN_PER_CM         (1.0 / 2.54)
#define SH_FT_PER_MILE       5280.0
#define SH_M_PER_YARD        0.9144
#define SH_YARD_PER_M        (1.0 / 0.9144)

/* Volume */
#define SH_LITERS_PER_GALLON 3.785411784
#define SH_GALLONS_PER_LITER (1.0 / 3.785411784)
#define SH_ML_PER_FL_OZ      29.5735295625
#define SH_FL_OZ_PER_ML      (1.0 / 29.5735295625)

/* Weight/Mass */
#define SH_KG_PER_LB         0.45359237
#define SH_LBS_PER_KG        (1.0 / 0.45359237)
#define SH_G_PER_OZ          28.349523125
#define SH_OZ_PER_G          (1.0 / 28.349523125)
#define SH_KG_PER_TON_US     907.18474    /* US short ton */
#define SH_KG_PER_TONNE      1000.0       /* Metric tonne */

/* Fuel Efficiency:
 * MPG to L/100km: L/100km = 100 * liters_per_gallon / (mpg * km_per_mile)
 * So: L/100km = (100 * LITERS / KM_PER_MILE) / mpg = FACTOR / mpg
 * Factor = 100 * LITERS_PER_GALLON / KM_PER_MILE */
#define SH_MPG_L100KM_FACTOR (100.0 * SH_LITERS_PER_GALLON / SH_KM_PER_MILE)

/* ============================================================================
 * Distance Conversions
 * ============================================================================ */

/* Large distances */
double sh_km_to_miles(double km);
double sh_miles_to_km(double miles);
double sh_m_to_km(double m);
double sh_km_to_m(double km);
double sh_m_to_miles(double m);
double sh_miles_to_m(double miles);

/* Medium distances */
double sh_m_to_ft(double m);
double sh_ft_to_m(double ft);
double sh_m_to_yards(double m);
double sh_yards_to_m(double yards);

/* Small distances */
double sh_cm_to_in(double cm);
double sh_in_to_cm(double in);
double sh_mm_to_in(double mm);
double sh_in_to_mm(double in);

/* ============================================================================
 * Volume Conversions
 * ============================================================================ */

double sh_liters_to_gallons(double liters);
double sh_gallons_to_liters(double gallons);
double sh_ml_to_floz(double ml);
double sh_floz_to_ml(double floz);

/* ============================================================================
 * Weight Conversions
 * ============================================================================ */

double sh_kg_to_lbs(double kg);
double sh_lbs_to_kg(double lbs);
double sh_g_to_oz(double g);
double sh_oz_to_g(double oz);
double sh_kg_to_tons_us(double kg);    /* US short tons */
double sh_tons_us_to_kg(double tons);
double sh_kg_to_tonnes(double kg);     /* Metric tonnes */
double sh_tonnes_to_kg(double tonnes);

/* ============================================================================
 * Fuel Efficiency Conversions (inverse relationship!)
 *
 * MPG = miles per gallon (higher = better)
 * L/100km = liters per 100 kilometers (lower = better)
 * ============================================================================ */

double sh_mpg_to_l100km(double mpg);
double sh_l100km_to_mpg(double l100km);

/* ============================================================================
 * Price Conversions (volume-based)
 * ============================================================================ */

double sh_price_per_gallon_to_liter(double price_per_gallon);
double sh_price_per_liter_to_gallon(double price_per_liter);

/* ============================================================================
 * Parsing
 * ============================================================================ */

/*
 * Parse "metric" or "imperial" string (case-insensitive).
 * Returns SH_UNITS_METRIC for unknown/NULL input.
 */
SHUnitSystem sh_parse_units(const char *str);

/*
 * Get string representation of unit system.
 */
const char *sh_units_string(SHUnitSystem units);

#endif /* SH_UNITS_H */
