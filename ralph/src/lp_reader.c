/*
 * Ralph - CPLEX LP File Reader
 *
 * Reads LP/MIP problems in CPLEX LP format.
 * Supports: MINIMIZE/MAXIMIZE, SUBJECT TO, BOUNDS, GENERAL, BINARY, END
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include "lp.h"
#include "ralph_lp.h"

#define LP_MAX_LINE 560
#define LP_MAX_NAME 256
#define LP_MAX_ERROR 512
#define LP_MAX_TERMS 10000

/* LP sections */
typedef enum {
    LP_SECTION_NONE = 0,
    LP_SECTION_OBJECTIVE,
    LP_SECTION_CONSTRAINTS,
    LP_SECTION_BOUNDS,
    LP_SECTION_GENERAL,
    LP_SECTION_BINARY,
    LP_SECTION_END
} LPSection;

/* Token types */
typedef enum {
    TOK_EOF = 0,
    TOK_NAME,
    TOK_NUMBER,
    TOK_PLUS,
    TOK_MINUS,
    TOK_COLON,
    TOK_LE,
    TOK_GE,
    TOK_EQ,
    TOK_FREE,
    TOK_KEYWORD
} LPTokenType;

/* Token */
typedef struct {
    LPTokenType type;
    char text[LP_MAX_NAME];
    double value;
    LPSection keyword_section;
    int is_max;  /* 1 if MAXIMIZE keyword */
} LPToken;

/* Variable info */
typedef struct {
    char name[LP_MAX_NAME];
    double lb;
    double ub;
    char type;  /* 'C', 'I', 'B' */
    double obj;
} LPVariable;

/* Constraint info */
typedef struct {
    char name[LP_MAX_NAME];
    int *indices;
    double *coeffs;
    int nnz;
    int capacity;
    char sense;
    double rhs;
} LPConstraint;

/* LP parser state */
typedef struct {
    FILE *file;
    char line[LP_MAX_LINE];
    char *pos;
    int line_num;
    int eof;

    /* Pushback token */
    LPToken pushback;
    int has_pushback;

    /* Problem data */
    int obj_sense;  /* 1=min, -1=max */
    char obj_name[LP_MAX_NAME];

    /* Variables */
    LPVariable *vars;
    int num_vars;
    int var_capacity;

    /* Constraints */
    LPConstraint *cons;
    int num_cons;
    int con_capacity;

    /* Section tracking */
    int has_objective;
    int has_constraints;
    int has_end;

    /* Error reporting */
    char error[LP_MAX_ERROR];
    int error_line;
} LPParser;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static void set_error(LPParser *p, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(p->error, LP_MAX_ERROR, fmt, args);
    va_end(args);
    p->error_line = p->line_num;
}

static int read_line(LPParser *p) {
    if (p->eof) return 0;

    if (!fgets(p->line, LP_MAX_LINE, p->file)) {
        if (feof(p->file)) {
            p->eof = 1;
            return 0;
        }
        set_error(p, "read error");
        return -1;
    }
    p->line_num++;
    p->pos = p->line;

    /* Check for line truncation */
    size_t len = strnlen(p->line, LP_MAX_LINE);
    if (len > 0 && p->line[len - 1] != '\n' && len == LP_MAX_LINE - 1) {
        int ch;
        while ((ch = fgetc(p->file)) != EOF && ch != '\n')
            ;
        set_error(p, "line %d exceeds maximum length (%d chars)",
                  p->line_num, LP_MAX_LINE - 1);
        return -1;
    }

    return 1;
}

static void skip_whitespace(LPParser *p) {
    while (p->pos && isspace((unsigned char)*p->pos) && *p->pos != '\n') {
        p->pos++;
    }
}

static int skip_to_next_content(LPParser *p) {
    while (1) {
        skip_whitespace(p);

        /* Check for comment */
        if (p->pos && *p->pos == '\\') {
            /* Skip rest of line */
            if (read_line(p) <= 0) return 0;
            continue;
        }

        /* Check for end of line */
        if (!p->pos || *p->pos == '\0' || *p->pos == '\n') {
            if (read_line(p) <= 0) return 0;
            continue;
        }

        return 1;
    }
}

/* Check if string matches a section keyword */
static LPSection check_keyword(const char *s, int *is_max) {
    *is_max = 0;

    /* Objective */
    if (strncasecmp(s, "MINIMIZE", 8) == 0 ||
        strncasecmp(s, "MINIMUM", 7) == 0 ||
        strncasecmp(s, "MIN", 3) == 0) {
        return LP_SECTION_OBJECTIVE;
    }
    if (strncasecmp(s, "MAXIMIZE", 8) == 0 ||
        strncasecmp(s, "MAXIMUM", 7) == 0 ||
        strncasecmp(s, "MAX", 3) == 0) {
        *is_max = 1;
        return LP_SECTION_OBJECTIVE;
    }

    /* Constraints
     * Note: The tokenizer splits on whitespace, so "SUBJECT TO" comes as two tokens.
     * We recognize "SUBJECT", "SUCH", "S.T.", "ST.", "ST" as constraint section starters.
     * The caller must consume the optional "TO" or "THAT" after these.
     */
    if (strncasecmp(s, "SUBJECT", 7) == 0 ||
        strncasecmp(s, "SUCH", 4) == 0 ||
        strncasecmp(s, "S.T.", 4) == 0 ||
        strncasecmp(s, "ST.", 3) == 0 ||
        (strncasecmp(s, "ST", 2) == 0 && strlen(s) == 2)) {
        return LP_SECTION_CONSTRAINTS;
    }

    /* Bounds */
    if (strncasecmp(s, "BOUNDS", 6) == 0 ||
        strncasecmp(s, "BOUND", 5) == 0) {
        return LP_SECTION_BOUNDS;
    }

    /* Integers */
    if (strncasecmp(s, "GENERALS", 8) == 0 ||
        strncasecmp(s, "GENERAL", 7) == 0 ||
        strncasecmp(s, "GEN", 3) == 0) {
        return LP_SECTION_GENERAL;
    }
    if (strncasecmp(s, "BINARIES", 8) == 0 ||
        strncasecmp(s, "BINARY", 6) == 0 ||
        strncasecmp(s, "BIN", 3) == 0) {
        return LP_SECTION_BINARY;
    }

    /* End */
    if (strncasecmp(s, "END", 3) == 0) {
        return LP_SECTION_END;
    }

    return LP_SECTION_NONE;
}

/* Get the next token */
static int next_token(LPParser *p, LPToken *tok) {
    /* Check pushback first */
    if (p->has_pushback) {
        *tok = p->pushback;
        p->has_pushback = 0;
        return 1;
    }

    memset(tok, 0, sizeof(*tok));

    if (!skip_to_next_content(p)) {
        tok->type = TOK_EOF;
        return 0;
    }

    char c = *p->pos;

    /* Operators */
    if (c == '+') {
        tok->type = TOK_PLUS;
        tok->text[0] = '+';
        p->pos++;
        return 1;
    }
    if (c == '-') {
        tok->type = TOK_MINUS;
        tok->text[0] = '-';
        p->pos++;
        return 1;
    }
    if (c == ':') {
        tok->type = TOK_COLON;
        tok->text[0] = ':';
        p->pos++;
        return 1;
    }

    /* Relation operators */
    if (c == '<' || c == '=') {
        if (c == '<') {
            p->pos++;
            if (*p->pos == '=') p->pos++;
            tok->type = TOK_LE;
            strncpy(tok->text, "<=", 3);
            return 1;
        }
        if (*p->pos == '=' && *(p->pos + 1) == '<') {
            p->pos += 2;
            tok->type = TOK_LE;
            strncpy(tok->text, "=<", 3);
            return 1;
        }
        if (*p->pos == '=' && (*(p->pos + 1) == '>' || isspace((unsigned char)*(p->pos + 1)) ||
                               *(p->pos + 1) == '\0' || *(p->pos + 1) == '\n')) {
            /* Check if it's => or standalone = */
            if (*(p->pos + 1) == '>') {
                p->pos += 2;
                tok->type = TOK_GE;
                strncpy(tok->text, "=>", 3);
                return 1;
            }
            /* Standalone = */
            p->pos++;
            tok->type = TOK_EQ;
            tok->text[0] = '=';
            return 1;
        }
        /* Just = */
        p->pos++;
        tok->type = TOK_EQ;
        tok->text[0] = '=';
        return 1;
    }
    if (c == '>') {
        p->pos++;
        if (*p->pos == '=') p->pos++;
        tok->type = TOK_GE;
        strncpy(tok->text, ">=", 3);
        return 1;
    }

    /* Number (starts with digit or .) */
    if (isdigit((unsigned char)c) || c == '.') {
        char *end;
        tok->value = strtod(p->pos, &end);
        if (end == p->pos) {
            set_error(p, "line %d: invalid number", p->line_num);
            return -1;
        }
        size_t len = (size_t)(end - p->pos);
        if (len >= LP_MAX_NAME) len = LP_MAX_NAME - 1;
        strncpy(tok->text, p->pos, len);
        tok->text[len] = '\0';
        tok->type = TOK_NUMBER;
        p->pos = end;
        return 1;
    }

    /* Name or keyword (starts with letter or allowed symbol) */
    if (isalpha((unsigned char)c) || c == '_') {
        char *start = p->pos;
        while (*p->pos && (isalnum((unsigned char)*p->pos) ||
               strchr("_!\"#$%&()/,.;?@`'{}|~", *p->pos))) {
            p->pos++;
        }

        size_t len = (size_t)(p->pos - start);
        if (len >= LP_MAX_NAME) len = LP_MAX_NAME - 1;
        strncpy(tok->text, start, len);
        tok->text[len] = '\0';

        /* Check for "free" keyword */
        if (strncasecmp(tok->text, "FREE", 4) == 0 && len == 4) {
            tok->type = TOK_FREE;
            return 1;
        }

        /* Check for section keyword at start of line */
        char *line_start = p->line;
        while (isspace((unsigned char)*line_start)) line_start++;
        if (start == line_start) {
            int is_max = 0;
            LPSection sec = check_keyword(tok->text, &is_max);
            if (sec != LP_SECTION_NONE) {
                tok->type = TOK_KEYWORD;
                tok->keyword_section = sec;
                tok->is_max = is_max;
                return 1;
            }
        }

        tok->type = TOK_NAME;
        return 1;
    }

    set_error(p, "line %d: unexpected character '%c'", p->line_num, c);
    return -1;
}

static void pushback_token(LPParser *p, const LPToken *tok) {
    p->pushback = *tok;
    p->has_pushback = 1;
}

/* Find or add a variable */
static int find_or_add_var(LPParser *p, const char *name) {
    /* Search existing */
    for (int i = 0; i < p->num_vars; i++) {
        if (strncmp(p->vars[i].name, name, LP_MAX_NAME) == 0) {
            return i;
        }
    }

    /* Add new */
    if (p->num_vars >= p->var_capacity) {
        int new_cap = p->var_capacity * 2;
        LPVariable *new_vars = (LPVariable*)realloc(p->vars, new_cap * sizeof(LPVariable));
        if (!new_vars) return -1;
        p->vars = new_vars;
        p->var_capacity = new_cap;
    }

    int idx = p->num_vars;
    memset(&p->vars[idx], 0, sizeof(LPVariable));
    strncpy(p->vars[idx].name, name, LP_MAX_NAME - 1);
    p->vars[idx].name[LP_MAX_NAME - 1] = '\0';
    p->vars[idx].lb = 0.0;
    p->vars[idx].ub = RALPH_LP_INFINITY;
    p->vars[idx].type = 'C';
    p->vars[idx].obj = 0.0;
    p->num_vars++;

    return idx;
}

/* ============================================================================
 * Expression Parser
 * ============================================================================ */

/* Parse a linear expression: [+/-] coef var [+/- coef var]...
 * Returns number of terms, or -1 on error.
 * Stops when encountering a relation operator or section keyword.
 */
static int parse_linear_expr(LPParser *p, int *indices, double *coeffs,
                             int max_terms, int for_objective) {
    int nnz = 0;
    double sign = 1.0;
    LPToken tok;

    while (1) {
        int ret = next_token(p, &tok);
        if (ret <= 0) break;

        /* Check for terminating tokens */
        if (tok.type == TOK_LE || tok.type == TOK_GE || tok.type == TOK_EQ) {
            pushback_token(p, &tok);
            break;
        }
        if (tok.type == TOK_KEYWORD) {
            pushback_token(p, &tok);
            break;
        }
        if (tok.type == TOK_COLON && !for_objective) {
            /* Constraint name - should have been handled before */
            pushback_token(p, &tok);
            break;
        }

        /* Sign */
        if (tok.type == TOK_PLUS) {
            sign = 1.0;
            continue;
        }
        if (tok.type == TOK_MINUS) {
            sign = -1.0;
            continue;
        }

        /* Coefficient and variable */
        double coef = sign;
        const char *var_name = NULL;

        if (tok.type == TOK_NUMBER) {
            coef = sign * tok.value;

            /* Next should be variable or operator */
            ret = next_token(p, &tok);
            if (ret <= 0) break;

            if (tok.type == TOK_NAME) {
                var_name = tok.text;
            } else if (tok.type == TOK_PLUS || tok.type == TOK_MINUS ||
                       tok.type == TOK_LE || tok.type == TOK_GE ||
                       tok.type == TOK_EQ || tok.type == TOK_KEYWORD) {
                /* Constant term - ignore for now */
                pushback_token(p, &tok);
                sign = 1.0;
                continue;
            } else {
                set_error(p, "line %d: expected variable name after coefficient",
                          p->line_num);
                return -1;
            }
        } else if (tok.type == TOK_NAME) {
            /* Variable with implicit coefficient 1 */
            var_name = tok.text;
        } else {
            set_error(p, "line %d: unexpected token '%s' in expression",
                      p->line_num, tok.text);
            return -1;
        }

        /* Add term */
        if (var_name) {
            if (nnz >= max_terms) {
                set_error(p, "line %d: too many terms in expression", p->line_num);
                return -1;
            }

            int var_idx = find_or_add_var(p, var_name);
            if (var_idx < 0) {
                set_error(p, "line %d: memory allocation failed", p->line_num);
                return -1;
            }

            /* Check if variable already in expression */
            int found = 0;
            for (int i = 0; i < nnz; i++) {
                if (indices[i] == var_idx) {
                    coeffs[i] += coef;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                indices[nnz] = var_idx;
                coeffs[nnz] = coef;
                nnz++;
            }
        }

        sign = 1.0;  /* Reset sign for next term */
    }

    return nnz;
}

/* ============================================================================
 * Section Parsers
 * ============================================================================ */

static int parse_objective(LPParser *p) {
    LPToken tok;
    int ret;

    int *indices = (int*)calloc(LP_MAX_TERMS, sizeof(int));
    double *coeffs = (double*)calloc(LP_MAX_TERMS, sizeof(double));
    if (!indices || !coeffs) {
        free(indices);
        free(coeffs);
        set_error(p, "memory allocation failed");
        return -1;
    }

    /* Check for optional objective name */
    ret = next_token(p, &tok);
    if (ret <= 0) {
        free(indices);
        free(coeffs);
        return ret < 0 ? -1 : 0;
    }

    if (tok.type == TOK_NAME) {
        /* Check if followed by colon */
        LPToken tok2;
        ret = next_token(p, &tok2);
        if (ret > 0 && tok2.type == TOK_COLON) {
            /* This is the objective name */
            strncpy(p->obj_name, tok.text, LP_MAX_NAME - 1);
            p->obj_name[LP_MAX_NAME - 1] = '\0';
        } else {
            /* Not a name, push both back */
            if (ret > 0) pushback_token(p, &tok2);
            pushback_token(p, &tok);
        }
    } else {
        pushback_token(p, &tok);
    }

    /* Parse objective expression */
    int nnz = parse_linear_expr(p, indices, coeffs, LP_MAX_TERMS, 1);
    if (nnz < 0) {
        free(indices);
        free(coeffs);
        return -1;
    }

    /* Store objective coefficients */
    for (int i = 0; i < nnz; i++) {
        p->vars[indices[i]].obj = coeffs[i];
    }

    free(indices);
    free(coeffs);
    p->has_objective = 1;
    return 0;
}

static int parse_constraint(LPParser *p) {
    LPToken tok;
    int ret;

    /* Allocate new constraint */
    if (p->num_cons >= p->con_capacity) {
        int new_cap = p->con_capacity * 2;
        LPConstraint *new_cons = (LPConstraint*)realloc(p->cons, new_cap * sizeof(LPConstraint));
        if (!new_cons) {
            set_error(p, "memory allocation failed");
            return -1;
        }
        p->cons = new_cons;
        p->con_capacity = new_cap;
    }

    LPConstraint *con = &p->cons[p->num_cons];
    memset(con, 0, sizeof(LPConstraint));
    con->capacity = 128;
    con->indices = (int*)calloc(con->capacity, sizeof(int));
    con->coeffs = (double*)calloc(con->capacity, sizeof(double));
    if (!con->indices || !con->coeffs) {
        free(con->indices);
        free(con->coeffs);
        set_error(p, "memory allocation failed");
        return -1;
    }

    /* Check for constraint name */
    ret = next_token(p, &tok);
    if (ret <= 0) {
        free(con->indices);
        free(con->coeffs);
        return ret < 0 ? -1 : 0;
    }

    if (tok.type == TOK_KEYWORD) {
        /* New section starting */
        pushback_token(p, &tok);
        free(con->indices);
        free(con->coeffs);
        return 0;
    }

    if (tok.type == TOK_NAME) {
        /* Check if followed by colon (constraint name) */
        LPToken tok2;
        ret = next_token(p, &tok2);
        if (ret > 0 && tok2.type == TOK_COLON) {
            strncpy(con->name, tok.text, LP_MAX_NAME - 1);
            con->name[LP_MAX_NAME - 1] = '\0';
        } else {
            /* Not a name, push both back */
            if (ret > 0) pushback_token(p, &tok2);
            pushback_token(p, &tok);
        }
    } else {
        pushback_token(p, &tok);
    }

    /* Parse LHS expression */
    int nnz = parse_linear_expr(p, con->indices, con->coeffs, con->capacity, 0);
    if (nnz < 0) {
        free(con->indices);
        free(con->coeffs);
        return -1;
    }
    con->nnz = nnz;

    /* Parse sense */
    ret = next_token(p, &tok);
    if (ret <= 0 || (tok.type != TOK_LE && tok.type != TOK_GE && tok.type != TOK_EQ)) {
        set_error(p, "line %d: expected constraint sense (<=, >=, =)", p->line_num);
        free(con->indices);
        free(con->coeffs);
        return -1;
    }

    if (tok.type == TOK_LE) con->sense = 'L';
    else if (tok.type == TOK_GE) con->sense = 'G';
    else con->sense = 'E';

    /* Parse RHS */
    ret = next_token(p, &tok);
    if (ret <= 0) {
        set_error(p, "line %d: expected RHS value", p->line_num);
        free(con->indices);
        free(con->coeffs);
        return -1;
    }

    double rhs_sign = 1.0;
    if (tok.type == TOK_MINUS) {
        rhs_sign = -1.0;
        ret = next_token(p, &tok);
        if (ret <= 0) {
            set_error(p, "line %d: expected RHS value after sign", p->line_num);
            free(con->indices);
            free(con->coeffs);
            return -1;
        }
    } else if (tok.type == TOK_PLUS) {
        ret = next_token(p, &tok);
        if (ret <= 0) {
            set_error(p, "line %d: expected RHS value after sign", p->line_num);
            free(con->indices);
            free(con->coeffs);
            return -1;
        }
    }

    if (tok.type != TOK_NUMBER) {
        set_error(p, "line %d: expected numeric RHS, got '%s'", p->line_num, tok.text);
        free(con->indices);
        free(con->coeffs);
        return -1;
    }
    con->rhs = rhs_sign * tok.value;

    p->num_cons++;
    return 1;
}

static int parse_constraints_section(LPParser *p) {
    p->has_constraints = 1;

    while (1) {
        int ret = parse_constraint(p);
        if (ret < 0) return -1;
        if (ret == 0) break;  /* New section or EOF */
    }

    return 0;
}

static int parse_bounds_section(LPParser *p) {
    LPToken tok;

    while (1) {
        int ret = next_token(p, &tok);
        if (ret <= 0) break;

        if (tok.type == TOK_KEYWORD) {
            pushback_token(p, &tok);
            break;
        }

        /* Formats:
         * lb <= var <= ub
         * var >= lb
         * var <= ub
         * var = val (fixed)
         * var free
         * -infinity <= var <= ub
         * lb <= var
         */

        double lb = 0.0, ub = RALPH_LP_INFINITY;
        const char *var_name = NULL;
        int has_lb = 0, has_ub = 0;
        int is_free = 0;

        /* Check for -infinity */
        if (tok.type == TOK_MINUS) {
            ret = next_token(p, &tok);
            if (ret <= 0) break;

            if (tok.type == TOK_NAME &&
                (strncasecmp(tok.text, "INFINITY", 8) == 0 ||
                 strncasecmp(tok.text, "INF", 3) == 0)) {
                lb = -RALPH_LP_INFINITY;
                has_lb = 1;

                /* Expect <= */
                ret = next_token(p, &tok);
                if (ret <= 0 || tok.type != TOK_LE) {
                    set_error(p, "line %d: expected <= after -infinity", p->line_num);
                    return -1;
                }

                /* Variable name */
                ret = next_token(p, &tok);
                if (ret <= 0 || tok.type != TOK_NAME) {
                    set_error(p, "line %d: expected variable name", p->line_num);
                    return -1;
                }
                var_name = tok.text;
            } else if (tok.type == TOK_NUMBER) {
                /* -number is lower bound */
                lb = -tok.value;
                has_lb = 1;

                /* Expect <= */
                ret = next_token(p, &tok);
                if (ret <= 0 || tok.type != TOK_LE) {
                    set_error(p, "line %d: expected <= after lower bound", p->line_num);
                    return -1;
                }

                /* Variable name */
                ret = next_token(p, &tok);
                if (ret <= 0 || tok.type != TOK_NAME) {
                    set_error(p, "line %d: expected variable name", p->line_num);
                    return -1;
                }
                var_name = tok.text;
            } else {
                set_error(p, "line %d: unexpected token after -", p->line_num);
                return -1;
            }
        } else if (tok.type == TOK_NUMBER) {
            /* Number is lower bound */
            lb = tok.value;
            has_lb = 1;

            /* Expect <= */
            ret = next_token(p, &tok);
            if (ret <= 0 || tok.type != TOK_LE) {
                set_error(p, "line %d: expected <= after lower bound", p->line_num);
                return -1;
            }

            /* Variable name */
            ret = next_token(p, &tok);
            if (ret <= 0 || tok.type != TOK_NAME) {
                set_error(p, "line %d: expected variable name", p->line_num);
                return -1;
            }
            var_name = tok.text;
        } else if (tok.type == TOK_NAME) {
            var_name = tok.text;
        } else {
            set_error(p, "line %d: unexpected token in BOUNDS", p->line_num);
            return -1;
        }

        /* Find variable */
        int var_idx = find_or_add_var(p, var_name);
        if (var_idx < 0) {
            set_error(p, "line %d: memory allocation failed", p->line_num);
            return -1;
        }

        /* Check what comes next */
        ret = next_token(p, &tok);
        if (ret <= 0) {
            /* Just had a variable name at end - treat as no change */
            if (has_lb) {
                p->vars[var_idx].lb = lb;
            }
            break;
        }

        if (tok.type == TOK_FREE) {
            is_free = 1;
        } else if (tok.type == TOK_LE) {
            /* var <= ub or lb <= var <= ub */
            ret = next_token(p, &tok);
            if (ret <= 0) {
                set_error(p, "line %d: expected upper bound", p->line_num);
                return -1;
            }

            double ub_sign = 1.0;
            if (tok.type == TOK_PLUS) {
                ret = next_token(p, &tok);
            } else if (tok.type == TOK_MINUS) {
                ub_sign = -1.0;
                ret = next_token(p, &tok);
            }

            if (ret <= 0) {
                set_error(p, "line %d: expected upper bound value", p->line_num);
                return -1;
            }

            if (tok.type == TOK_NAME &&
                (strncasecmp(tok.text, "INFINITY", 8) == 0 ||
                 strncasecmp(tok.text, "INF", 3) == 0)) {
                ub = ub_sign * RALPH_LP_INFINITY;
            } else if (tok.type == TOK_NUMBER) {
                ub = ub_sign * tok.value;
            } else {
                set_error(p, "line %d: expected numeric upper bound", p->line_num);
                return -1;
            }
            has_ub = 1;
        } else if (tok.type == TOK_GE) {
            /* var >= lb */
            ret = next_token(p, &tok);
            if (ret <= 0) {
                set_error(p, "line %d: expected lower bound", p->line_num);
                return -1;
            }

            double lb_sign = 1.0;
            if (tok.type == TOK_MINUS) {
                lb_sign = -1.0;
                ret = next_token(p, &tok);
            }

            if (ret <= 0 || tok.type != TOK_NUMBER) {
                set_error(p, "line %d: expected numeric lower bound", p->line_num);
                return -1;
            }
            lb = lb_sign * tok.value;
            has_lb = 1;
        } else if (tok.type == TOK_EQ) {
            /* var = fixed value */
            ret = next_token(p, &tok);
            if (ret <= 0) {
                set_error(p, "line %d: expected fixed value", p->line_num);
                return -1;
            }

            double val_sign = 1.0;
            if (tok.type == TOK_MINUS) {
                val_sign = -1.0;
                ret = next_token(p, &tok);
            }

            if (ret <= 0 || tok.type != TOK_NUMBER) {
                set_error(p, "line %d: expected numeric value", p->line_num);
                return -1;
            }
            lb = ub = val_sign * tok.value;
            has_lb = has_ub = 1;
        } else {
            /* No operator - push back for next iteration */
            pushback_token(p, &tok);
        }

        /* Apply bounds */
        if (is_free) {
            p->vars[var_idx].lb = -RALPH_LP_INFINITY;
            p->vars[var_idx].ub = RALPH_LP_INFINITY;
        } else {
            if (has_lb) p->vars[var_idx].lb = lb;
            if (has_ub) p->vars[var_idx].ub = ub;
        }
    }

    return 0;
}

static int parse_general_section(LPParser *p) {
    LPToken tok;

    while (1) {
        int ret = next_token(p, &tok);
        if (ret <= 0) break;

        if (tok.type == TOK_KEYWORD) {
            pushback_token(p, &tok);
            break;
        }

        if (tok.type != TOK_NAME) {
            set_error(p, "line %d: expected variable name in GENERAL", p->line_num);
            return -1;
        }

        int var_idx = find_or_add_var(p, tok.text);
        if (var_idx < 0) {
            set_error(p, "line %d: memory allocation failed", p->line_num);
            return -1;
        }

        p->vars[var_idx].type = 'I';
    }

    return 0;
}

static int parse_binary_section(LPParser *p) {
    LPToken tok;

    while (1) {
        int ret = next_token(p, &tok);
        if (ret <= 0) break;

        if (tok.type == TOK_KEYWORD) {
            pushback_token(p, &tok);
            break;
        }

        if (tok.type != TOK_NAME) {
            set_error(p, "line %d: expected variable name in BINARY", p->line_num);
            return -1;
        }

        int var_idx = find_or_add_var(p, tok.text);
        if (var_idx < 0) {
            set_error(p, "line %d: memory allocation failed", p->line_num);
            return -1;
        }

        p->vars[var_idx].type = 'B';
        p->vars[var_idx].lb = 0.0;
        p->vars[var_idx].ub = 1.0;
    }

    return 0;
}

/* ============================================================================
 * Main Parser
 * ============================================================================ */

static LPParser* lp_parser_create(void) {
    LPParser *p = (LPParser*)calloc(1, sizeof(LPParser));
    if (!p) return NULL;

    p->obj_sense = 1;  /* Minimize by default */

    p->var_capacity = 128;
    p->vars = (LPVariable*)calloc(p->var_capacity, sizeof(LPVariable));

    p->con_capacity = 128;
    p->cons = (LPConstraint*)calloc(p->con_capacity, sizeof(LPConstraint));

    if (!p->vars || !p->cons) {
        free(p->vars);
        free(p->cons);
        free(p);
        return NULL;
    }

    return p;
}

static void lp_parser_free(LPParser *p) {
    if (!p) return;

    if (p->file) {
        fclose(p->file);
        p->file = NULL;
    }

    free(p->vars);

    if (p->cons) {
        for (int i = 0; i < p->num_cons; i++) {
            free(p->cons[i].indices);
            free(p->cons[i].coeffs);
        }
        free(p->cons);
    }

    free(p);
}

/* ============================================================================
 * Public Interface
 * ============================================================================ */

int ralph_core_read_lp(RalphLPModel *model, const char *filename) {
    if (!model || !filename) return -1;

    LPParser *p = lp_parser_create();
    if (!p) return -1;

    p->file = fopen(filename, "r");
    if (!p->file) {
        lp_parser_free(p);
        return -1;
    }

    int parse_error = 0;
    LPSection current_section = LP_SECTION_NONE;

    /* Read first line */
    if (read_line(p) <= 0) {
        set_error(p, "empty file or read error");
        parse_error = 1;
    }

    /* Main parsing loop */
    while (!parse_error && !p->eof) {
        LPToken tok;
        int ret = next_token(p, &tok);
        if (ret < 0) {
            parse_error = 1;
            break;
        }
        if (ret == 0) break;

        if (tok.type == TOK_KEYWORD) {
            current_section = tok.keyword_section;

            if (current_section == LP_SECTION_OBJECTIVE) {
                p->obj_sense = tok.is_max ? -1 : 1;
                if (parse_objective(p) < 0) {
                    parse_error = 1;
                }
            } else if (current_section == LP_SECTION_CONSTRAINTS) {
                /* Consume optional "TO" after "SUBJECT" or "SUCH"
                 * Also consume "THAT" after "SUCH" */
                LPToken peek;
                if (next_token(p, &peek) > 0) {
                    if (peek.type == TOK_NAME &&
                        (strcasecmp(peek.text, "TO") == 0 ||
                         strcasecmp(peek.text, "THAT") == 0)) {
                        /* Consumed - do nothing */
                    } else {
                        pushback_token(p, &peek);
                    }
                }
                if (parse_constraints_section(p) < 0) {
                    parse_error = 1;
                }
            } else if (current_section == LP_SECTION_BOUNDS) {
                if (parse_bounds_section(p) < 0) {
                    parse_error = 1;
                }
            } else if (current_section == LP_SECTION_GENERAL) {
                if (parse_general_section(p) < 0) {
                    parse_error = 1;
                }
            } else if (current_section == LP_SECTION_BINARY) {
                if (parse_binary_section(p) < 0) {
                    parse_error = 1;
                }
            } else if (current_section == LP_SECTION_END) {
                p->has_end = 1;
                break;
            }
        }
    }

    /* Validate */
    if (!parse_error && !p->has_objective) {
        set_error(p, "missing objective section (MINIMIZE or MAXIMIZE)");
        parse_error = 1;
    }

    if (parse_error) {
        if (p->error[0] != '\0') {
            fprintf(stderr, "LP parse error: %s\n", p->error);
        }
        lp_parser_free(p);
        return -1;
    }

    /* Build model */
    ralph_lp_set_obj_sense(model, p->obj_sense == 1 ? RALPH_LP_OBJ_MINIMIZE : RALPH_LP_OBJ_MAXIMIZE);

    /* Add variables first (before setting names, since name array size depends on num_vars) */
    for (int j = 0; j < p->num_vars; j++) {
        RalphLPVarType type = RALPH_LP_VAR_CONTINUOUS;
        if (p->vars[j].type == 'I') type = RALPH_LP_VAR_INTEGER;
        if (p->vars[j].type == 'B') type = RALPH_LP_VAR_BINARY;

        ralph_lp_add_var(model, p->vars[j].lb, p->vars[j].ub, p->vars[j].obj, type);
    }

    /* Now set variable names (after all variables are added) */
    for (int j = 0; j < p->num_vars; j++) {
        ralph_lp_set_var_name(model, j, p->vars[j].name);
    }

    /* Add constraints first (before setting names) */
    for (int i = 0; i < p->num_cons; i++) {
        LPConstraint *con = &p->cons[i];
        RalphLPSense sense;
        if (con->sense == 'L') sense = RALPH_LP_SENSE_LESS_EQUAL;
        else if (con->sense == 'G') sense = RALPH_LP_SENSE_GREATER_EQUAL;
        else sense = RALPH_LP_SENSE_EQUAL;

        ralph_lp_add_constraint(model, con->nnz, con->indices, con->coeffs,
                                sense, con->rhs);
    }

    /* Now set constraint names */
    for (int i = 0; i < p->num_cons; i++) {
        if (p->cons[i].name[0] != '\0') {
            ralph_lp_set_con_name(model, i, p->cons[i].name);
        }
    }

    /* Set objective name if present */
    if (p->obj_name[0] != '\0') {
        /* Could store this somewhere if needed */
    }

    lp_parser_free(p);
    return 0;
}
