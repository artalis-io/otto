/**
 * Clay Components - Amalgamation File
 *
 * This file includes all component implementations for simple single-file builds.
 * For modular builds, compile cc_common.c, cc_input.c, cc_button.c separately.
 *
 * IMPORTANT: Define CLAY_IMPLEMENTATION and include clay.h BEFORE this file:
 *
 *   #define CLAY_IMPLEMENTATION
 *   #include "clay.h"
 *   #include "cc_immediate.c"  // or compile separately
 */

/* Core and components (no Clay dependency) */
#include "cc_common.c"
#include "cc_input.c"
#include "cc_button.c"
#include "cc_map.c"

/* Clay integration (requires clay.h already included) */
#include "cc_clay.c"
