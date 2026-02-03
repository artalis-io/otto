/*
 * Ralph - MPS File Reader
 *
 * Reads LP/MIP problems in MPS format (fixed and free format).
 * MPS is the standard format for linear programming problems.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include "lp.h"
#include "ralph.h"

#define MAX_LINE 1024
#define MAX_NAME 256

/* MPS sections */
typedef enum {
    SECTION_NONE = 0,
    SECTION_NAME,
    SECTION_OBJSENSE,
    SECTION_ROWS,
    SECTION_COLUMNS,
    SECTION_RHS,
    SECTION_RANGES,
    SECTION_BOUNDS,
    SECTION_END
} MPSSection;

/* Row type from MPS */
typedef struct {
    char name[MAX_NAME];
    char type;  /* N=objective, L=<=, G=>=, E== */
    int index;
} MPSRow;

/* Column/variable info */
typedef struct {
    char name[MAX_NAME];
    int index;
    char type;  /* C=continuous, I=integer, B=binary */
} MPSColumn;

/* MPS parser state */
typedef struct {
    FILE *file;
    char line[MAX_LINE];
    int line_num;

    /* Problem data */
    char name[MAX_NAME];
    int obj_sense;  /* 1=min, -1=max */

    /* Rows */
    MPSRow *rows;
    int num_rows;
    int row_capacity;
    int obj_row;  /* Index of objective row */

    /* Columns */
    MPSColumn *columns;
    int num_cols;
    int col_capacity;

    /* RHS values */
    double *rhs;

    /* Bounds */
    double *lb;
    double *ub;

    /* Matrix in triplet form */
    SparseTriplets *matrix;

    /* Objective coefficients */
    double *obj;

} MPSParser;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static char* trim(char *str) {
    while (isspace(*str)) str++;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) *end-- = '\0';
    return str;
}

static int find_row(MPSParser *parser, const char *name) {
    for (int i = 0; i < parser->num_rows; i++) {
        if (strcmp(parser->rows[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_or_add_column(MPSParser *parser, const char *name) {
    for (int i = 0; i < parser->num_cols; i++) {
        if (strcmp(parser->columns[i].name, name) == 0) {
            return i;
        }
    }

    /* Add new column */
    if (parser->num_cols >= parser->col_capacity) {
        int new_cap = parser->col_capacity * 2;
        MPSColumn *new_cols = (MPSColumn*)realloc(parser->columns,
                                                   new_cap * sizeof(MPSColumn));
        if (!new_cols) return -1;
        parser->columns = new_cols;
        parser->col_capacity = new_cap;

        /* Expand bounds and obj arrays */
        double *new_lb = (double*)realloc(parser->lb, new_cap * sizeof(double));
        if (!new_lb) return -1;
        parser->lb = new_lb;

        double *new_ub = (double*)realloc(parser->ub, new_cap * sizeof(double));
        if (!new_ub) return -1;
        parser->ub = new_ub;

        double *new_obj = (double*)realloc(parser->obj, new_cap * sizeof(double));
        if (!new_obj) return -1;
        parser->obj = new_obj;
    }

    int idx = parser->num_cols;
    strncpy(parser->columns[idx].name, name, MAX_NAME - 1);
    parser->columns[idx].name[MAX_NAME - 1] = '\0';
    parser->columns[idx].index = idx;
    parser->columns[idx].type = 'C';

    /* Default bounds */
    parser->lb[idx] = 0.0;
    parser->ub[idx] = RALPH_INFINITY;
    parser->obj[idx] = 0.0;

    parser->num_cols++;
    return idx;
}

/* ============================================================================
 * Section Parsers
 * ============================================================================ */

static int parse_name(MPSParser *parser) {
    char *name = trim(parser->line + 4);  /* Skip "NAME" */
    strncpy(parser->name, name, MAX_NAME - 1);
    parser->name[MAX_NAME - 1] = '\0';  /* Ensure null termination */
    return 0;
}

static int parse_objsense(MPSParser *parser, const char *line) {
    char *sense = trim((char*)line);
    if (strncasecmp(sense, "MIN", 3) == 0) {
        parser->obj_sense = 1;
    } else if (strncasecmp(sense, "MAX", 3) == 0) {
        parser->obj_sense = -1;
    }
    return 0;
}

static int parse_rows_line(MPSParser *parser, const char *line) {
    /* Format: TYPE  NAME */
    char type;
    char name[MAX_NAME];

    if (sscanf(line, " %c %255s", &type, name) < 2) {
        return -1;
    }

    type = toupper(type);

    /* Expand if needed */
    if (parser->num_rows >= parser->row_capacity) {
        int new_cap = parser->row_capacity * 2;
        MPSRow *new_rows = (MPSRow*)realloc(parser->rows, new_cap * sizeof(MPSRow));
        if (!new_rows) return -1;
        parser->rows = new_rows;

        double *new_rhs = (double*)realloc(parser->rhs, new_cap * sizeof(double));
        if (!new_rhs) return -1;
        parser->rhs = new_rhs;

        parser->row_capacity = new_cap;
    }

    int idx = parser->num_rows;
    snprintf(parser->rows[idx].name, MAX_NAME, "%s", name);
    parser->rows[idx].type = type;
    parser->rows[idx].index = idx;
    parser->rhs[idx] = 0.0;

    if (type == 'N') {
        parser->obj_row = idx;
    }

    parser->num_rows++;
    return 0;
}

static int parse_columns_line(MPSParser *parser, const char *line) {
    /* Format: COLNAME  ROWNAME  VALUE  [ROWNAME  VALUE] */
    char col_name[MAX_NAME];
    char row_name1[MAX_NAME], row_name2[MAX_NAME];
    double val1, val2;

    int n = sscanf(line, " %255s %255s %lf %255s %lf",
                   col_name, row_name1, &val1, row_name2, &val2);

    if (n < 3) return -1;

    /* Check for MARKER for integer variables */
    if (strcmp(row_name1, "'MARKER'") == 0) {
        if (strstr(line, "'INTORG'")) {
            /* Start of integer section - mark subsequent columns as integer */
            /* This is handled by tracking state */
        } else if (strstr(line, "'INTEND'")) {
            /* End of integer section */
        }
        return 0;
    }

    int col_idx = find_or_add_column(parser, col_name);
    if (col_idx < 0) return -1;

    /* First coefficient */
    int row_idx = find_row(parser, row_name1);
    if (row_idx >= 0) {
        if (row_idx == parser->obj_row) {
            parser->obj[col_idx] = val1;
        } else {
            triplets_add(parser->matrix, row_idx, col_idx, val1);
        }
    }

    /* Optional second coefficient */
    if (n >= 5) {
        row_idx = find_row(parser, row_name2);
        if (row_idx >= 0) {
            if (row_idx == parser->obj_row) {
                parser->obj[col_idx] = val2;
            } else {
                triplets_add(parser->matrix, row_idx, col_idx, val2);
            }
        }
    }

    return 0;
}

static int parse_rhs_line(MPSParser *parser, const char *line) {
    /* Format: RHSNAME  ROWNAME  VALUE  [ROWNAME  VALUE] */
    char rhs_name[MAX_NAME];
    char row_name1[MAX_NAME], row_name2[MAX_NAME];
    double val1, val2;

    int n = sscanf(line, " %255s %255s %lf %255s %lf",
                   rhs_name, row_name1, &val1, row_name2, &val2);

    if (n < 3) return -1;

    int row_idx = find_row(parser, row_name1);
    if (row_idx >= 0 && row_idx != parser->obj_row) {
        parser->rhs[row_idx] = val1;
    }

    if (n >= 5) {
        row_idx = find_row(parser, row_name2);
        if (row_idx >= 0 && row_idx != parser->obj_row) {
            parser->rhs[row_idx] = val2;
        }
    }

    return 0;
}

static int parse_bounds_line(MPSParser *parser, const char *line) {
    /* Format: TYPE  BNDNAME  COLNAME  VALUE */
    char type[8];
    char bnd_name[MAX_NAME];
    char col_name[MAX_NAME];
    double val = 0.0;

    int n = sscanf(line, " %7s %255s %255s %lf", type, bnd_name, col_name, &val);

    if (n < 3) return -1;

    int col_idx = find_or_add_column(parser, col_name);
    if (col_idx < 0) return -1;

    /* Process bound type */
    if (strcmp(type, "LO") == 0) {
        parser->lb[col_idx] = val;
    } else if (strcmp(type, "UP") == 0) {
        parser->ub[col_idx] = val;
    } else if (strcmp(type, "FX") == 0) {
        parser->lb[col_idx] = val;
        parser->ub[col_idx] = val;
    } else if (strcmp(type, "FR") == 0) {
        parser->lb[col_idx] = -RALPH_INFINITY;
        parser->ub[col_idx] = RALPH_INFINITY;
    } else if (strcmp(type, "MI") == 0) {
        parser->lb[col_idx] = -RALPH_INFINITY;
    } else if (strcmp(type, "PL") == 0) {
        parser->ub[col_idx] = RALPH_INFINITY;
    } else if (strcmp(type, "BV") == 0) {
        /* Binary variable */
        parser->lb[col_idx] = 0.0;
        parser->ub[col_idx] = 1.0;
        parser->columns[col_idx].type = 'B';
    } else if (strcmp(type, "LI") == 0) {
        parser->lb[col_idx] = val;
        parser->columns[col_idx].type = 'I';
    } else if (strcmp(type, "UI") == 0) {
        parser->ub[col_idx] = val;
        parser->columns[col_idx].type = 'I';
    }

    return 0;
}

/* ============================================================================
 * Main Parser
 * ============================================================================ */

static MPSParser* mps_parser_create(void) {
    MPSParser *parser = (MPSParser*)calloc(1, sizeof(MPSParser));
    if (!parser) return NULL;

    parser->obj_sense = 1;  /* Minimize by default */
    parser->obj_row = -1;

    /* Initial allocations */
    parser->row_capacity = 128;
    parser->col_capacity = 128;

    parser->rows = (MPSRow*)malloc(parser->row_capacity * sizeof(MPSRow));
    parser->columns = (MPSColumn*)malloc(parser->col_capacity * sizeof(MPSColumn));
    parser->rhs = (double*)calloc(parser->row_capacity, sizeof(double));
    parser->lb = (double*)malloc(parser->col_capacity * sizeof(double));
    parser->ub = (double*)malloc(parser->col_capacity * sizeof(double));
    parser->obj = (double*)calloc(parser->col_capacity, sizeof(double));
    parser->matrix = triplets_create(parser->row_capacity, parser->col_capacity, 1024);

    if (!parser->rows || !parser->columns || !parser->rhs ||
        !parser->lb || !parser->ub || !parser->obj || !parser->matrix) {
        /* Cleanup on failure */
        free(parser->rows);
        free(parser->columns);
        free(parser->rhs);
        free(parser->lb);
        free(parser->ub);
        free(parser->obj);
        triplets_free(parser->matrix);
        free(parser);
        return NULL;
    }

    return parser;
}

static void mps_parser_free(MPSParser *parser) {
    if (!parser) return;

    if (parser->file) fclose(parser->file);
    free(parser->rows);
    free(parser->columns);
    free(parser->rhs);
    free(parser->lb);
    free(parser->ub);
    free(parser->obj);
    triplets_free(parser->matrix);
    free(parser);
}

/* ============================================================================
 * Public Interface
 * ============================================================================ */

int ralph_read_mps(RalphModel *model, const char *filename) {
    if (!model || !filename) return -1;

    MPSParser *parser = mps_parser_create();
    if (!parser) return -1;

    parser->file = fopen(filename, "r");
    if (!parser->file) {
        mps_parser_free(parser);
        return -1;
    }

    MPSSection section = SECTION_NONE;
    int in_integer = 0;

    while (fgets(parser->line, MAX_LINE, parser->file)) {
        parser->line_num++;

        /* Remove newline */
        char *nl = strchr(parser->line, '\n');
        if (nl) *nl = '\0';
        nl = strchr(parser->line, '\r');
        if (nl) *nl = '\0';

        /* Skip empty lines and comments */
        char *trimmed = trim(parser->line);
        if (trimmed[0] == '\0' || trimmed[0] == '*') continue;

        /* Check for section headers */
        if (strncmp(trimmed, "NAME", 4) == 0) {
            section = SECTION_NAME;
            parse_name(parser);
            continue;
        } else if (strncmp(trimmed, "OBJSENSE", 8) == 0) {
            section = SECTION_OBJSENSE;
            continue;
        } else if (strncmp(trimmed, "ROWS", 4) == 0) {
            section = SECTION_ROWS;
            continue;
        } else if (strncmp(trimmed, "COLUMNS", 7) == 0) {
            section = SECTION_COLUMNS;
            continue;
        } else if (strncmp(trimmed, "RHS", 3) == 0) {
            section = SECTION_RHS;
            continue;
        } else if (strncmp(trimmed, "RANGES", 6) == 0) {
            section = SECTION_RANGES;
            continue;
        } else if (strncmp(trimmed, "BOUNDS", 6) == 0) {
            section = SECTION_BOUNDS;
            continue;
        } else if (strncmp(trimmed, "ENDATA", 6) == 0) {
            section = SECTION_END;
            break;
        }

        /* Process line based on current section */
        switch (section) {
            case SECTION_OBJSENSE:
                parse_objsense(parser, trimmed);
                break;
            case SECTION_ROWS:
                parse_rows_line(parser, trimmed);
                break;
            case SECTION_COLUMNS:
                /* Check for integer marker */
                if (strstr(trimmed, "'MARKER'") && strstr(trimmed, "'INTORG'")) {
                    in_integer = 1;
                } else if (strstr(trimmed, "'MARKER'") && strstr(trimmed, "'INTEND'")) {
                    in_integer = 0;
                } else {
                    parse_columns_line(parser, trimmed);
                    /* Mark column as integer if in integer section */
                    if (in_integer && parser->num_cols > 0) {
                        parser->columns[parser->num_cols - 1].type = 'I';
                    }
                }
                break;
            case SECTION_RHS:
                parse_rhs_line(parser, trimmed);
                break;
            case SECTION_BOUNDS:
                parse_bounds_line(parser, trimmed);
                break;
            default:
                break;
        }
    }

    /* Build model using public API */
    /* Set objective sense */
    ralph_set_obj_sense(model, parser->obj_sense == 1 ? RALPH_MINIMIZE : RALPH_MAXIMIZE);

    /* Add variables */
    for (int j = 0; j < parser->num_cols; j++) {
        RalphVarType type = RALPH_CONTINUOUS;
        if (parser->columns[j].type == 'I') type = RALPH_INTEGER;
        if (parser->columns[j].type == 'B') type = RALPH_BINARY;

        ralph_add_var(model, parser->lb[j], parser->ub[j], parser->obj[j], type);
    }

    /* Build constraint matrix and add constraints */
    SparseMatrix *A = triplets_to_csc(parser->matrix);
    if (A) {
        /* Add each constraint */
        double *row_coefs = (double*)calloc(parser->num_cols, sizeof(double));
        int *row_indices = (int*)malloc(parser->num_cols * sizeof(int));

        for (int i = 0; i < parser->num_rows; i++) {
            if (i == parser->obj_row) continue;  /* Skip objective row */

            /* Get row coefficients */
            int nnz = 0;
            for (int j = 0; j < parser->num_cols; j++) {
                double val = sparse_get_element(A, i, j);
                if (fabs(val) > 1e-15) {
                    row_indices[nnz] = j;
                    row_coefs[nnz] = val;
                    nnz++;
                }
            }

            /* Determine constraint sense */
            RalphSense sense;
            char type = parser->rows[i].type;
            if (type == 'L') sense = RALPH_LESS_EQUAL;
            else if (type == 'G') sense = RALPH_GREATER_EQUAL;
            else sense = RALPH_EQUAL;

            ralph_add_constraint(model, nnz, row_indices, row_coefs,
                               sense, parser->rhs[i]);
        }

        free(row_coefs);
        free(row_indices);
        sparse_free(A);
    }

    mps_parser_free(parser);
    return 0;
}
