#ifndef SIMPLEX_CRASH_H
#define SIMPLEX_CRASH_H

#include "lp.h"

/* Triangular crash basis (Maros LTSF).
 * Replace slack variables in the initial basis with structural columns
 * that have good pivot elements. Returns number of columns placed. */
int crash_triangular(SimplexTableau *tab, int verbose);

#endif /* SIMPLEX_CRASH_H */
