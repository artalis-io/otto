/*
 * FuelWise - Truck Refueling Optimization Library
 * Geospatial Utilities Implementation
 *
 * NOTE: Most functions have been moved to shared library (sh_polyline.c, sh_geo.c).
 * This file is now empty as all functionality is provided by the shared library.
 *
 * Copyright (c) 2024. All rights reserved.
 */

#include "fw_geo.h"

/* All functions are now provided by the shared library via macros in fw_geo.h:
 * - fw_haversine_distance() -> sh_haversine()
 * - fw_point_to_segment_distance() -> sh_point_to_segment_distance()
 * - fw_latlon_to_local() -> sh_latlon_to_local()
 * - fw_local_to_latlon() -> sh_local_to_latlon()
 * - fw_polyline_length() -> sh_polyline_length()
 * - fw_find_closest_on_polyline() -> sh_find_closest_on_polyline()
 * - fw_distance_along_polyline() -> sh_distance_along_polyline()
 * - fw_subsample_polyline() -> sh_subsample_polyline()
 */
