/**
 * Clay Components - Amalgamation File
 *
 * This file includes all component implementations for simple single-file builds.
 * For modular builds, compile cs_common.c, cs_input.c, cs_button.c separately.
 *
 * IMPORTANT: Define CLAY_IMPLEMENTATION and include clay.h BEFORE this file:
 *
 *   #define CLAY_IMPLEMENTATION
 *   #include "clay.h"
 *   #include "cs_immediate.c"  // or compile separately
 */

/* Core and components (no Clay dependency) */
#include "cs_common.c"
#include "cs_input.c"
#include "cs_button.c"
#include "cs_map.c"

/* Clay integration (requires clay.h already included) */
#include "cs_clay.c"
