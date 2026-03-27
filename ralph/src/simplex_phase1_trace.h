#ifndef SIMPLEX_PHASE1_TRACE_H
#define SIMPLEX_PHASE1_TRACE_H

#include "lp.h"

/* Phase 1 trace recording */
void phase1_trace_record_no_entering(SimplexSolver *solver, int iter, int status_code);
void phase1_trace_record_pivot_failure(SimplexSolver *solver,
                                       const SimplexTableau *tab,
                                       int iter, int repeat_count);
void phase1_trace_emit_summary(SimplexSolver *solver, RalphStatus phase1_status);

/* Stagnation escape */
void phase1_stagnation_window_begin(SimplexSolver *solver,
                                    const SimplexTableau *tab,
                                    int iter);
int phase1_stagnation_escape_should_trigger(SimplexSolver *solver,
                                            const SimplexTableau *tab,
                                            int iter);

/* LU failure → trace reason mapping (used by simplex_pivot in simplex.c) */
int phase1_trace_reason_from_lu_failure(int lu_reason, int forced_refactor_path);

#endif /* SIMPLEX_PHASE1_TRACE_H */
