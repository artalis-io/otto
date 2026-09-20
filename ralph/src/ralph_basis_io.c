/*
 * ralph_basis_io.c - reading and writing basis and MIP-start files.
 *
 * Text-format parsing and serialisation, moved verbatim out of ralph.c.
 * It sat in the composition root while lp_reader.c, lp_writer.c and
 * mps_reader.c next door already owned exactly this concern -- file
 * formats in, file formats out.
 *
 * Four public entry points: ralph_core_{write,read}_basis_file and
 * ralph_core_{write,read}_mip_start_file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ralph_core.h"
#include "ralph_internal.h"

int ralph_core_write_basis_file(const RalphBasis *basis, const char *filename) {
    if (!basis || !filename || !basis->basis || !basis->var_status) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "basis or filename is null");
    }
    RALPH_CLEAR_API_ERROR(NULL);
    if (basis->m < 0 || basis->n < 0) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       basis->m,
                       basis->n,
                       "basis dimensions are invalid");
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_OPEN_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to open basis file for write");
    }

    if (fprintf(fp, "RALPH_BASIS_V1 %d %d\n", basis->m, basis->n) < 0) {
        fclose(fp);
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_WRITE_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to write basis header");
    }

    for (int i = 0; i < basis->m; i++) {
        if (fprintf(fp, "%d%c", basis->basis[i], (i + 1 == basis->m) ? '\n' : ' ') < 0) {
            fclose(fp);
            RALPH_FAIL_API(NULL,
                           RALPH_ERROR_DOMAIN_IO,
                           RALPH_ERROR_CODE_IO_WRITE_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           i,
                           basis->m,
                           "failed to write basis indices");
        }
    }
    if (basis->m == 0 && fprintf(fp, "\n") < 0) {
        fclose(fp);
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_WRITE_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to write empty basis row");
    }

    for (int j = 0; j < basis->n; j++) {
        if (fprintf(fp, "%d%c", (int)basis->var_status[j], (j + 1 == basis->n) ? '\n' : ' ') < 0) {
            fclose(fp);
            RALPH_FAIL_API(NULL,
                           RALPH_ERROR_DOMAIN_IO,
                           RALPH_ERROR_CODE_IO_WRITE_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           j,
                           basis->n,
                           "failed to write basis status");
        }
    }
    if (basis->n == 0 && fprintf(fp, "\n") < 0) {
        fclose(fp);
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_WRITE_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to write empty basis status row");
    }

    if (fclose(fp) != 0) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_WRITE_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to close basis file");
    }
    return 0;
}

RalphBasis* ralph_core_read_basis_file(const char *filename) {
    if (!filename) {
        RALPH_FAIL_API_PTR(NULL,
                           RALPH_ERROR_DOMAIN_ARGUMENT,
                           RALPH_ERROR_CODE_NULL_POINTER,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           0,
                           0,
                           "filename is null");
    }
    RALPH_CLEAR_API_ERROR(NULL);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        RALPH_FAIL_API_PTR(NULL,
                           RALPH_ERROR_DOMAIN_IO,
                           RALPH_ERROR_CODE_IO_OPEN_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           0,
                           0,
                           "failed to open basis file for read");
    }

    char magic[32] = {0};
    int m = 0, n = 0;
    if (fscanf(fp, "%31s %d %d", magic, &m, &n) != 3) {
        fclose(fp);
        RALPH_FAIL_API_PTR(NULL,
                           RALPH_ERROR_DOMAIN_PARSE,
                           RALPH_ERROR_CODE_PARSE_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           0,
                           0,
                           "failed to parse basis header");
    }
    if (strcmp(magic, "RALPH_BASIS_V1") != 0 || m < 0 || n < 0) {
        fclose(fp);
        RALPH_FAIL_API_PTR(NULL,
                           RALPH_ERROR_DOMAIN_PARSE,
                           RALPH_ERROR_CODE_PARSE_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           m,
                           n,
                           "invalid basis header");
    }

    RalphBasis *basis = (RalphBasis*)calloc(1, sizeof(RalphBasis));
    if (!basis) {
        fclose(fp);
        RALPH_FAIL_API_PTR(NULL,
                           RALPH_ERROR_DOMAIN_MEMORY,
                           RALPH_ERROR_CODE_ALLOCATION_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           0,
                           0,
                           "failed to allocate basis object");
    }
    basis->m = m;
    basis->n = n;

    if (m > 0) {
        basis->basis = (int*)calloc((size_t)m, sizeof(int));
        if (!basis->basis) {
            ralph_core_free_basis(basis);
            fclose(fp);
            RALPH_FAIL_API_PTR(NULL,
                               RALPH_ERROR_DOMAIN_MEMORY,
                               RALPH_ERROR_CODE_ALLOCATION_FAILED,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_IO,
                               m,
                               0,
                               "failed to allocate basis indices");
        }
    }
    if (n > 0) {
        basis->var_status = (VarStatus*)calloc((size_t)n, sizeof(VarStatus));
        if (!basis->var_status) {
            ralph_core_free_basis(basis);
            fclose(fp);
            RALPH_FAIL_API_PTR(NULL,
                               RALPH_ERROR_DOMAIN_MEMORY,
                               RALPH_ERROR_CODE_ALLOCATION_FAILED,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_IO,
                               n,
                               0,
                               "failed to allocate basis status");
        }
    }

    for (int i = 0; i < m; i++) {
        if (fscanf(fp, "%d", &basis->basis[i]) != 1) {
            ralph_core_free_basis(basis);
            fclose(fp);
            RALPH_FAIL_API_PTR(NULL,
                               RALPH_ERROR_DOMAIN_PARSE,
                               RALPH_ERROR_CODE_PARSE_FAILED,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_IO,
                               i,
                               m,
                               "failed to parse basis index");
        }
    }
    for (int j = 0; j < n; j++) {
        int v = 0;
        if (fscanf(fp, "%d", &v) != 1) {
            ralph_core_free_basis(basis);
            fclose(fp);
            RALPH_FAIL_API_PTR(NULL,
                               RALPH_ERROR_DOMAIN_PARSE,
                               RALPH_ERROR_CODE_PARSE_FAILED,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_IO,
                               j,
                               n,
                               "failed to parse basis status");
        }
        if (v < (int)RALPH_BASIC || v > (int)RALPH_FIXED) {
            ralph_core_free_basis(basis);
            fclose(fp);
            RALPH_FAIL_API_PTR(NULL,
                               RALPH_ERROR_DOMAIN_PARSE,
                               RALPH_ERROR_CODE_PARSE_FAILED,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_IO,
                               v,
                               j,
                               "basis status value out of range");
        }
        basis->var_status[j] = (VarStatus)v;
    }

    fclose(fp);
    return basis;
}

int ralph_core_write_mip_start_file(const RalphModel *model, const char *filename) {
    if (!model || !model->lp_model || !filename) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "model or filename is null");
    }
    RALPH_CLEAR_API_ERROR(model);

    int n = model->lp_model->num_vars;
    if (n <= 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_IO,
                       n,
                       0,
                       "no variables available for MIP start serialization");
    }

    const double *start = NULL;
    const int *mask = NULL;
    int nnz = 0;

    if (model->mip_start && model->mip_start_n == n) {
        start = model->mip_start;
        mask = model->mip_start_mask;
        nnz = model->mip_start_nnz;
    } else if (model->solution && ralph_core_is_mip(model)) {
        start = model->solution;
    }

    if (!start) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "no MIP start or incumbent solution available");
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_OPEN_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to open MIP start file for write");
    }

    if (fprintf(fp, "RALPH_MIPSTART_V1 %d %d %d\n", n,
                (int)model->mip_start_repair_mode, nnz) < 0) {
        fclose(fp);
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_WRITE_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to write MIP start header");
    }

    for (int j = 0; j < n; j++) {
        if (fprintf(fp, "%.17g%c", start[j], (j + 1 == n) ? '\n' : ' ') < 0) {
            fclose(fp);
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_IO,
                           RALPH_ERROR_CODE_IO_WRITE_FAILED,
                           model->status,
                           RALPH_ERROR_API_IO,
                           j,
                           n,
                           "failed to write MIP start values");
        }
    }

    for (int j = 0; j < n; j++) {
        int bit = mask ? (mask[j] ? 1 : 0) : 1;
        if (fprintf(fp, "%d%c", bit, (j + 1 == n) ? '\n' : ' ') < 0) {
            fclose(fp);
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_IO,
                           RALPH_ERROR_CODE_IO_WRITE_FAILED,
                           model->status,
                           RALPH_ERROR_API_IO,
                           j,
                           n,
                           "failed to write MIP start mask");
        }
    }

    if (fclose(fp) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_WRITE_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to close MIP start file");
    }
    return 0;
}

int ralph_core_read_mip_start_file(RalphModel *model, const char *filename) {
    if (!model || !model->lp_model || !filename) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "model or filename is null");
    }
    RALPH_CLEAR_API_ERROR(model);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_OPEN_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to open MIP start file for read");
    }

    char magic[32] = {0};
    int n = 0;
    int repair = 0;
    int nnz = 0;
    if (fscanf(fp, "%31s %d %d %d", magic, &n, &repair, &nnz) != 4) {
        fclose(fp);
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARSE,
                       RALPH_ERROR_CODE_PARSE_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to parse MIP start header");
    }
    if (strcmp(magic, "RALPH_MIPSTART_V1") != 0 || n <= 0 ||
        n != model->lp_model->num_vars) {
        fclose(fp);
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARSE,
                       RALPH_ERROR_CODE_PARSE_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       n,
                       model->lp_model->num_vars,
                       "invalid MIP start header");
    }

    double *start = (double*)malloc((size_t)n * sizeof(double));
    int *mask = (int*)calloc((size_t)n, sizeof(int));
    if (!start || !mask) {
        free(start);
        free(mask);
        fclose(fp);
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_MEMORY,
                       RALPH_ERROR_CODE_ALLOCATION_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       n,
                       0,
                       "failed to allocate MIP start buffers");
    }

    for (int j = 0; j < n; j++) {
        if (fscanf(fp, "%lf", &start[j]) != 1) {
            free(start);
            free(mask);
            fclose(fp);
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_PARSE,
                           RALPH_ERROR_CODE_PARSE_FAILED,
                           model->status,
                           RALPH_ERROR_API_IO,
                           j,
                           n,
                           "failed to parse MIP start value");
        }
    }
    int counted = 0;
    for (int j = 0; j < n; j++) {
        int bit = 0;
        if (fscanf(fp, "%d", &bit) != 1) {
            free(start);
            free(mask);
            fclose(fp);
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_PARSE,
                           RALPH_ERROR_CODE_PARSE_FAILED,
                           model->status,
                           RALPH_ERROR_API_IO,
                           j,
                           n,
                           "failed to parse MIP start mask");
        }
        mask[j] = bit ? 1 : 0;
        counted += mask[j];
    }

    fclose(fp);

    if (ralph_set_mip_start_copy(model, start, mask, n) != 0) {
        free(start);
        free(mask);
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_MEMORY,
                       RALPH_ERROR_CODE_ALLOCATION_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       n,
                       0,
                       "failed to stage MIP start");
    }
    model->mip_start_nnz = counted;

    if (repair >= (int)RALPH_MIP_START_REPAIR_STRICT &&
        repair <= (int)RALPH_MIP_START_REPAIR_PROJECT_AND_ROUND) {
        model->mip_start_repair_mode = (RalphMIPStartRepairMode)repair;
    } else {
        model->mip_start_repair_mode = RALPH_MIP_START_REPAIR_STRICT;
    }

    free(start);
    free(mask);
    return 0;
}
