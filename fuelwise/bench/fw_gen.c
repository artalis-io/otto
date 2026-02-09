/*
 * FuelWise Problem Generator
 *
 * Generates satisfiable refueling problems for benchmark testing.
 * Uses sh_dist.h for random number generation.
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#include "fw_bench.h"
#include "sh_units.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * Preset Configurations
 * ============================================================================ */

/*
 * Preset Configurations
 *
 * Realistic trucking scenarios:
 * - Drivers cover 500-700 km/day (EU driving time limits)
 * - Week-long routes: 3000-5000 km
 * - Fuel consumption: 25-35 L/100km depending on load
 * - Tank sizes: 400-1000L (400L typical, 800-1000L for long-haul)
 * - Fuel stops: Every 500-1000 km (1-2x per day)
 */

/* 3-day regional route: ~1500 km, Central Europe hub-and-spoke */
FWBenchConfig fw_bench_config_short_urban(void)
{
    FWBenchConfig cfg = {0};
    cfg.route_length_m = 1500000;       /* 1500 km (3 days) */
    cfg.mean_station_gap_m = 30000;     /* 30 km average (dense coverage) */
    cfg.gap_shape = 2.5;
    cfg.tank_capacity_l = 400;          /* Standard single tank */
    cfg.tare_weight_kg = 15000;
    cfg.max_gvw_kg = 40000;
    cfg.curve = NULL;                   /* EU standard truck */
    cfg.num_weight_events = 4;          /* Pickups/deliveries */
    cfg.cargo_weight_mean_kg = 6000;
    cfg.cargo_weight_stddev_kg = 2000;
    cfg.min_fuel_l = 50;
    cfg.min_purchase_l = 0;             /* 0 = LP solver (fast) */
    cfg.start_fuel_fraction = 0.5;      /* Half tank at start */
    cfg.base_price_per_l = 1.55;        /* EUR/L */
    cfg.price_stddev = 0.15;
    cfg.price_correlation = 0.5;
    cfg.rng_type = SH_RNG_XORSHIFT128;
    cfg.seed = 0;
    return cfg;
}

/* 5-day cross-country: ~3000 km, typical Europe corridor */
FWBenchConfig fw_bench_config_highway(void)
{
    FWBenchConfig cfg = {0};
    cfg.route_length_m = 3000000;       /* 3000 km (5 days) */
    cfg.mean_station_gap_m = 40000;     /* 40 km average */
    cfg.gap_shape = 2.5;
    cfg.tank_capacity_l = 500;          /* Dual tanks common for this range */
    cfg.tare_weight_kg = 15000;
    cfg.max_gvw_kg = 40000;
    cfg.curve = NULL;
    cfg.num_weight_events = 6;          /* Multiple stops */
    cfg.cargo_weight_mean_kg = 8000;
    cfg.cargo_weight_stddev_kg = 2500;
    cfg.min_fuel_l = 60;
    cfg.min_purchase_l = 0;             /* 0 = LP solver (fast) */
    cfg.start_fuel_fraction = 0.6;
    cfg.base_price_per_l = 1.50;
    cfg.price_stddev = 0.18;            /* Higher variance across countries */
    cfg.price_correlation = 0.3;        /* Less correlated (border effects) */
    cfg.rng_type = SH_RNG_XORSHIFT128;
    cfg.seed = 0;
    return cfg;
}

/* 7+ day transcontinental: ~5000 km, Spain to Poland type route */
FWBenchConfig fw_bench_config_long_haul(void)
{
    FWBenchConfig cfg = {0};
    cfg.route_length_m = 5000000;       /* 5000 km (full week+) */
    cfg.mean_station_gap_m = 50000;     /* 50 km average */
    cfg.gap_shape = 2.0;                /* More variance (rural sections) */
    cfg.tank_capacity_l = 800;          /* Long-haul configuration */
    cfg.tare_weight_kg = 15000;
    cfg.max_gvw_kg = 40000;
    cfg.curve = NULL;
    cfg.num_weight_events = 8;          /* Multiple pickups/drops */
    cfg.cargo_weight_mean_kg = 10000;
    cfg.cargo_weight_stddev_kg = 3000;
    cfg.min_fuel_l = 80;
    cfg.min_purchase_l = 0;             /* 0 = LP solver (fast) */
    cfg.start_fuel_fraction = 0.7;
    cfg.base_price_per_l = 1.45;        /* Varies by country */
    cfg.price_stddev = 0.25;            /* Large variance (Spain vs Poland) */
    cfg.price_correlation = 0.2;        /* Low correlation (different markets) */
    cfg.rng_type = SH_RNG_XORSHIFT128;
    cfg.seed = 0;
    return cfg;
}

/* Tight margins: Eastern Europe, older equipment, sparse infrastructure */
FWBenchConfig fw_bench_config_tight_margins(void)
{
    FWBenchConfig cfg = {0};
    cfg.route_length_m = 2500000;       /* 2500 km (4-5 days) */
    cfg.mean_station_gap_m = 70000;     /* 70 km - sparser coverage */
    cfg.gap_shape = 2.0;
    cfg.tank_capacity_l = 350;          /* Smaller/older tank */
    cfg.tare_weight_kg = 15000;
    cfg.max_gvw_kg = 40000;
    cfg.curve = NULL;
    cfg.num_weight_events = 5;
    cfg.cargo_weight_mean_kg = 12000;   /* Heavy loads */
    cfg.cargo_weight_stddev_kg = 2000;
    cfg.min_fuel_l = 40;
    cfg.min_purchase_l = 0;             /* 0 = LP solver (fast) */
    cfg.start_fuel_fraction = 0.3;      /* Low starting fuel */
    cfg.base_price_per_l = 1.35;        /* Lower base (Eastern Europe) */
    cfg.price_stddev = 0.20;
    cfg.price_correlation = 0.4;
    cfg.rng_type = SH_RNG_XORSHIFT128;
    cfg.seed = 0;
    return cfg;
}

/* US Interstate: ~2000 miles (3200 km), typical Class 8 truck */
FWBenchConfig fw_bench_config_us_interstate(void)
{
    FWBenchConfig cfg = {0};
    /* Config in imperial, stored as metric internally */
    cfg.route_length_m = sh_miles_to_m(2000);           /* 2000 miles */
    cfg.mean_station_gap_m = sh_miles_to_m(50);         /* 50 miles average */
    cfg.gap_shape = 2.5;
    cfg.tank_capacity_l = sh_gallons_to_liters(300);    /* 300 gallon dual tanks */
    cfg.tare_weight_kg = sh_lbs_to_kg(35000);           /* 35,000 lbs tare */
    cfg.max_gvw_kg = sh_lbs_to_kg(80000);               /* 80,000 lbs GVW limit */
    cfg.curve = NULL;                                    /* US Class 8 default */
    cfg.num_weight_events = 4;
    cfg.cargo_weight_mean_kg = sh_lbs_to_kg(30000);     /* 30,000 lbs average load */
    cfg.cargo_weight_stddev_kg = sh_lbs_to_kg(10000);
    cfg.min_fuel_l = sh_gallons_to_liters(50);          /* 50 gallon minimum */
    cfg.min_purchase_l = 0;                              /* 0 = LP solver (fast) */
    cfg.start_fuel_fraction = 0.5;
    cfg.base_price_per_l = sh_price_per_gallon_to_liter(3.50);  /* $3.50/gal */
    cfg.price_stddev = sh_price_per_gallon_to_liter(0.40);      /* $0.40 stddev */
    cfg.price_correlation = 0.4;
    cfg.rng_type = SH_RNG_XORSHIFT128;
    cfg.seed = 0;
    cfg.units = SH_UNITS_IMPERIAL;                       /* Report in imperial */
    return cfg;
}

/* ============================================================================
 * Problem Generation
 * ============================================================================ */

/*
 * Calculate maximum safe gap between stations.
 *
 * max_gap = (tank_capacity - min_fuel) / max_consumption * 100 km
 */
static double max_safe_gap(
    double tank_capacity_l,
    double min_fuel_l,
    double base_consumption)
{
    double available_fuel = tank_capacity_l - min_fuel_l;
    double max_consumption = base_consumption;

    if (max_consumption <= 0) {
        max_consumption = 35.0;  /* Default worst-case L/100km */
    }

    /* available_fuel / (max_consumption / 100) = range in km */
    double range_km = available_fuel / (max_consumption / 100.0);
    return range_km * 1000.0 * 0.9;  /* 90% safety margin, convert to meters */
}

int fw_bench_generate(const FWBenchConfig *config, FWBenchInstance *out)
{
    if (!config || !out) return -1;

    memset(out, 0, sizeof(FWBenchInstance));

    /* Create RNG */
    SHRng *rng = sh_rng_create(config->rng_type);
    if (!rng) return -1;

    if (config->seed != 0) {
        sh_rng_seed(rng, config->seed);
    }

    /* Get or create consumption curve */
    FWConsumptionCurve *curve = config->curve;
    int own_curve = 0;
    if (curve == NULL) {
        if (config->max_gvw_kg <= 15000) {
            curve = fw_curve_light_truck();
        } else if (config->tare_weight_kg >= 13000) {
            curve = fw_curve_eu_standard();
        } else {
            curve = fw_curve_us_class8();
        }
        own_curve = 1;
    }

    /* Calculate base consumption from curve at tare weight */
    double base_consumption = 30.0;
    if (curve) {
        base_consumption = fw_consumption_at_weight(curve, config->tare_weight_kg);
    }

    /* Calculate max safe gap */
    double max_gap = max_safe_gap(
        config->tank_capacity_l,
        config->min_fuel_l,
        base_consumption
    );

    /* Generate station positions using gamma distribution */
    double gap_scale = config->mean_station_gap_m / config->gap_shape;
    int max_stations = (int)(config->route_length_m / config->mean_station_gap_m * 3) + 10;

    FWSnappedStation *stations = calloc(max_stations, sizeof(FWSnappedStation));
    if (!stations) {
        if (own_curve) fw_consumption_curve_free(curve);
        sh_rng_free(rng);
        return -1;
    }

    int num_stations = 0;
    double pos = 0;

    while (pos < config->route_length_m && num_stations < max_stations) {
        /* Generate gap */
        double gap = sh_rng_gamma(rng, config->gap_shape, gap_scale);

        /* Clamp gap to be safe */
        if (gap > max_gap) {
            gap = max_gap;
        }
        if (gap < 1000) {
            gap = 1000;  /* Minimum 1 km between stations */
        }

        pos += gap;

        /* Stop if past route end */
        if (pos >= config->route_length_m - 1000) {
            break;
        }

        /* Check if we need to insert extra stations due to large gap */
        if (num_stations > 0) {
            double prev_pos = stations[num_stations - 1].distance_from_start;
            double actual_gap = pos - prev_pos;
            if (actual_gap > max_gap) {
                /* Insert intermediate station */
                double mid_pos = prev_pos + max_gap * 0.9;
                if (mid_pos < pos) {
                    stations[num_stations].station_id = num_stations;
                    stations[num_stations].distance_from_start = mid_pos;
                    stations[num_stations].perpendicular_distance = 0;
                    /* Price will be set below */
                    num_stations++;
                }
            }
        }

        stations[num_stations].station_id = num_stations;
        stations[num_stations].distance_from_start = pos;
        stations[num_stations].perpendicular_distance = 0;
        num_stations++;
    }

    /* Ensure at least one station */
    if (num_stations == 0) {
        stations[0].station_id = 0;
        stations[0].distance_from_start = config->route_length_m * 0.5;
        stations[0].perpendicular_distance = 0;
        num_stations = 1;
    }

    /* Generate prices with spatial correlation (AR(1) model) */
    double prev_price = config->base_price_per_l;
    double noise_scale = config->price_stddev *
        sqrt(1.0 - config->price_correlation * config->price_correlation);

    for (int i = 0; i < num_stations; i++) {
        double noise = sh_rng_normal(rng, 0, noise_scale);
        double price = config->base_price_per_l +
            config->price_correlation * (prev_price - config->base_price_per_l) +
            noise;

        /* Clamp to reasonable range */
        if (price < config->base_price_per_l * 0.5) {
            price = config->base_price_per_l * 0.5;
        }
        if (price > config->base_price_per_l * 2.0) {
            price = config->base_price_per_l * 2.0;
        }

        stations[i].price = price;
        prev_price = price;
    }

    /* Build the problem (no weight profile for simplicity in initial version) */
    out->stations = stations;
    out->num_stations = num_stations;
    out->curve = own_curve ? curve : NULL;  /* Only store if we own it */
    out->weight_profile = NULL;  /* Disabled for now - focus on basic benchmark */

    out->problem.total_distance = config->route_length_m;
    out->problem.num_segments = 0;
    out->problem.segments = NULL;
    out->problem.base_consumption = base_consumption;
    out->problem.tank_capacity = config->tank_capacity_l;
    out->problem.current_fuel = config->tank_capacity_l * config->start_fuel_fraction;
    out->problem.minimum_fuel = config->min_fuel_l;
    out->problem.minimum_fuel_at_end = config->min_fuel_l;
    out->problem.num_stations = num_stations;
    out->problem.stations = stations;
    out->problem.min_purchase = config->min_purchase_l;
    out->problem.stop_cost = 0;
    out->problem.remaining_fuel_value = 0;

    /* Calculate total fuel required (for reference) */
    double distance_km = config->route_length_m / 1000.0;
    out->total_fuel_required = (base_consumption / 100.0) * distance_km;

    sh_rng_free(rng);
    return 0;
}

void fw_bench_free_instance(FWBenchInstance *instance)
{
    if (!instance) return;

    if (instance->stations) {
        free(instance->stations);
        instance->stations = NULL;
    }

    if (instance->curve) {
        fw_consumption_curve_free(instance->curve);
        instance->curve = NULL;
    }

    if (instance->weight_profile) {
        fw_weight_profile_free(instance->weight_profile);
        instance->weight_profile = NULL;
    }

    memset(instance, 0, sizeof(FWBenchInstance));
}
