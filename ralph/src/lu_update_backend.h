#ifndef LU_UPDATE_BACKEND_H
#define LU_UPDATE_BACKEND_H

#include "lp.h"

int lu_update_backend_is_ft(const LUFactorization *lu);
int lu_update_backend_storage_capacity(const LUFactorization *lu);
int lu_update_backend_has_updates(const LUFactorization *lu);
void lu_update_backend_reset(LUFactorization *lu);
int lu_update_backend_apply_forward(const LUFactorization *lu, double *x);
void lu_update_backend_apply_backward(const LUFactorization *lu, double *x);
int lu_update_backend_store(LUFactorization *lu,
                            int step_pos,
                            const double *base_spike,
                            const double *spike,
                            int off_diag_nnz);

#endif
