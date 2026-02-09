/*
 * FuelWise Problem Generator
 *
 * Generates satisfiable refueling problems for benchmark testing.
 * Uses sh_dist.h for random number generation.
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#include "fw_bench.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * Preset Configurations
 * ============================================================================ */

FWBenchConfig fw_bench_config_short_urban(void)
{
    FWBenchConfig cfg = {0};
    cfg.route_length_m = 200000;        /* 200 km */
    cfg.mean_station_gap_m = 15000;     /* 15 km average */
    cfg.gap_shape = 2.5;
    cfg.tank_capacity_l = 150;
    cfg.tare_weight_kg = 8000;
    cfg.max_gvw_kg = 12000;
    cfg.curve = NULL;                   /* Use light truck */
    cfg.num_weight_events = 2;
    cfg.cargo_weight_mean_kg = 1500;
    cfg.cargo_weight_stddev_kg = 300;
    cfg.min_fuel_l = 15;
    cfg.start_fuel_fraction = 0.5;
    cfg.base_price_per_l = 1.60;
    cfg.price_stddev = 0.15;
    cfg.price_correlation = 0.5;
    cfg.rng_type = SH_RNG_XORSHIFT128;
    cfg.seed = 0;                       /* Will be set by caller */
    return cfg;
}

FWBenchConfig fw_bench_config_highway(void)
{
    FWBenchConfig cfg = {0};
    cfg.route_length_m = 800000;        /* 800 km */
    cfg.mean_station_gap_m = 40000;     /* 40 km average */
    cfg.gap_shape = 2.5;
    cfg.tank_capacity_l = 500;
    cfg.tare_weight_kg = 15000;
    cfg.max_gvw_kg = 40000;
    cfg.curve = NULL;                   /* Use EU standard */
    cfg.num_weight_events = 4;
    cfg.cargo_weight_mean_kg = 5000;
    cfg.cargo_weight_stddev_kg = 1500;
    cfg.min_fuel_l = 50;
    cfg.start_fuel_fraction = 0.6;
    cfg.base_price_per_l = 1.50;
    cfg.price_stddev = 0.12;
    cfg.price_correlation = 0.4;
    cfg.rng_type = SH_RNG_XORSHIFT128;
    cfg.seed = 0;
    return cfg;
}

FWBenchConfig fw_bench_config_long_haul(void)
{
    FWBenchConfig cfg = {0};
    cfg.route_length_m = 2000000;       /* 2000 km */
    cfg.mean_station_gap_m = 60000;     /* 60 km average */
    cfg.gap_shape = 2.0;                /* More variance */
    cfg.tank_capacity_l = 800;
    cfg.tare_weight_kg = 15000;
    cfg.max_gvw_kg = 40000;
    cfg.curve = NULL;
    cfg.num_weight_events = 6;
    cfg.cargo_weight_mean_kg = 6000;
    cfg.cargo_weight_stddev_kg = 2000;
    cfg.min_fuel_l = 80;
    cfg.start_fuel_fraction = 0.7;
    cfg.base_price_per_l = 1.45;
    cfg.price_stddev = 0.20;
    cfg.price_correlation = 0.3;
    cfg.rng_type = SH_RNG_XORSHIFT128;
    cfg.seed = 0;
    return cfg;
}

FWBenchConfig fw_bench_config_tight_margins(void)
{
    FWBenchConfig cfg = {0};
    cfg.route_length_m = 500000;        /* 500 km */
    cfg.mean_station_gap_m = 50000;     /* 50 km - sparse */
    cfg.gap_shape = 3.0;                /* Less variance */
    cfg.tank_capacity_l = 300;          /* Smaller tank */
    cfg.tare_weight_kg = 15000;
    cfg.max_gvw_kg = 40000;
    cfg.curve = NULL;
    cfg.num_weight_events = 3;
    cfg.cargo_weight_mean_kg = 8000;    /* Heavy cargo */
    cfg.cargo_weight_stddev_kg = 1000;
    cfg.min_fuel_l = 40;
    cfg.start_fuel_fraction = 0.4;      /* Low starting fuel */
    cfg.base_price_per_l = 1.55;
    cfg.price_stddev = 0.10;
    cfg.price_correlation = 0.6;
    cfg.rng_type = SH_RNG_XORSHIFT128;
    cfg.seed = 0;
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
    out->problem.min_purchase = 0;
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
