/*
 * locus.h - Locus Geocoding Library
 *
 * Location Oriented Coordinate Unification System
 * Zero-dependency geocoding library for OSM data
 *
 * Features:
 * - Forward geocoding (text → coordinates)
 * - Reverse geocoding (coordinates → address)
 * - Autocomplete suggestions
 * - Fuzzy text matching
 *
 * Part of the OTTO platform GIS trifecta:
 * - Carta: Map tile generation
 * - Velo: Routing
 * - Locus: Geocoding
 */

#ifndef LOCUS_H
#define LOCUS_H

#include "lc_types.h"
#include "lc_pbf.h"
#include "lc_normalize.h"
#include "lc_trie.h"
#include "lc_ngram.h"
#include "lc_spatial.h"
#include "lc_index.h"

/* ============================================================================
 * Version
 * ============================================================================ */

#define LC_VERSION_MAJOR 0
#define LC_VERSION_MINOR 1
#define LC_VERSION_PATCH 0

const char *lc_version(void);

/* ============================================================================
 * High-Level API
 * ============================================================================ */

/* Load and build index from OSM PBF file */
LCEntityStore *lc_load_pbf(const char *filename, const LCPBFOptions *opts);

/* Get entity count */
uint32_t lc_entity_count(const LCEntityStore *store);

/* Get entity by index */
const LCEntity *lc_get_entity(const LCEntityStore *store, uint32_t index);

#endif /* LOCUS_H */
