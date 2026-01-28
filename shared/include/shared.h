/*
 * shared.h - FuelWise Platform Shared Utilities
 *
 * Common utilities used across velo and carta modules:
 * - Geographic calculations (haversine, coordinates)
 * - Bounding box operations
 * - Coordinate projections
 */

#ifndef SHARED_H
#define SHARED_H

#include "sh_geo.h"

/* Library version */
#define SHARED_VERSION_MAJOR 1
#define SHARED_VERSION_MINOR 0
#define SHARED_VERSION_PATCH 0

/* Version string */
const char *sh_version(void);

#endif /* SHARED_H */
