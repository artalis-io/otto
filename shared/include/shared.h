/*
 * shared.h - OTTO Platform Shared Utilities
 *
 * Common utilities used across velo and carta modules:
 * - Geographic calculations (haversine, coordinates)
 * - Bounding box operations
 * - Coordinate projections
 * - Protocol buffer encoding/decoding
 * - Zlib compression/decompression
 * - OSM PBF parsing utilities
 */

#ifndef SHARED_H
#define SHARED_H

#include "sh_geo.h"
#include "sh_protobuf.h"
#include "sh_inflate.h"
#include "sh_pbf.h"

/* Library version */
#define SHARED_VERSION_MAJOR 1
#define SHARED_VERSION_MINOR 0
#define SHARED_VERSION_PATCH 0

/* Version string */
const char *sh_version(void);

#endif /* SHARED_H */
