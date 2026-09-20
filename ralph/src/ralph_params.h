/*
 * ralph_params.h - the parameter subsystem, and the few model setters it
 * shares with ralph.c.
 *
 * Private to ralph/src. The public surface is ralph_core.h; this header
 * exists only so ralph_params.c and ralph.c can see each other.
 *
 * The five setters below stay in ralph.c rather than moving with the
 * parameter code that is their only caller, because each one calls the
 * GLPK policy marshallers, and those are used by the optimize path too.
 * Moving them would have split a genuinely shared concern to keep an
 * extraction tidy.
 */
#ifndef RALPH_PARAMS_H
#define RALPH_PARAMS_H

#include "ralph_core.h"

int ralph_set_requested_lp_algorithm_internal(RalphModel *model, int value);
int ralph_set_requested_barrier_crossover_internal(RalphModel *model, int value);
int ralph_set_requested_lp_external_provider_internal(RalphModel *model, int value);
int ralph_set_glpk_policy_int_param(RalphModel *model, RalphParamId param,
                                    int value);
int ralph_set_glpk_policy_double_param(RalphModel *model, RalphParamId param,
                                       double value);

#endif /* RALPH_PARAMS_H */
