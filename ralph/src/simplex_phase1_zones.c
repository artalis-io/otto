/*
 * simplex_phase1_zones.c - Phase 1 crisis zone handler implementations.
 *
 * Each function corresponds to one crisis zone in the Phase 1 main loop.
 * The main loop in simplex.c calls these sequentially per iteration.
 *
 * Zero behavior change — pure code motion from simplex.c.
 */

#include "simplex_phase1_zones.h"
#include "simplex_internal.h"
#include "simplex_pricing.h"
#include "simplex_ratio.h"
#include "simplex_perturb.h"
#include "lp_refactor_policy.h"
#include "lp_glpk_strict.h"
#include "lp_basis_governor.h"
#include "lp_log.h"

#include <math.h>
#include <limits.h>
