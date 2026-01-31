/**
 * Clay Components - Immediate Mode API
 *
 * Convenience header that includes all immediate mode components.
 *
 * Usage:
 *   #include "cs_immediate.h"  // Gets everything
 *
 * Or include individual components:
 *   #include "cs_common.h"
 *   #include "cs_input.h"
 *   #include "cs_button.h"
 */

#ifndef CS_IMMEDIATE_H
#define CS_IMMEDIATE_H

/* Core utilities, ID generation, focus management */
#include "cs_common.h"

/* Clay integration (init, lifecycle, render command accessors) */
#include "cs_clay.h"

/* Components */
#include "cs_input.h"
#include "cs_button.h"
#include "cs_map.h"

#endif /* CS_IMMEDIATE_H */
