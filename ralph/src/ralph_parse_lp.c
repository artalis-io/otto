/*
 * Ralph API - LP/MPS String Parsers
 *
 * Parses LP and MPS formats from string buffers (for API use).
 * Based on lp_reader.c but adapted for string input.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

#include "ralph_api.h"
#include "ralph.h"

#define LP_MAX_NAME 256
#define LP_MAX_TERMS 1000

/* Error messages (static) */
static const char *err_memory = "memory allocation failed";
static const char *err_empty = "empty problem definition";
static const char *err_no_obj = "missing objective section (MINIMIZE or MAXIMIZE)";
static const char *err_no_sense = "expected constraint sense (<=, >=, =)";
static const char *err_no_rhs = "expected RHS value";
static const char *err_syntax = "syntax error";
static const char *err_too_many_terms = "too many terms in expression";
static const char *err_invalid_number = "invalid number";

/* ============================================================================
 * LP Section Types
 * ============================================================================ */

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

typedef struct {
    LPTokenType type;
    char text[LP_MAX_NAME];
    double value;
    LPSection keyword_section;
    int is_max;
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

/* String parser state */
typedef struct {
    const char *str;
    size_t len;
    size_t pos;
    int line_num;

    /* Pushback token */
    LPToken pushback;
    int has_pushback;

    /* Problem data */
    int obj_sense;
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

    /* Error */
    const char *error;
} LPStringParser;

/* ============================================================================
 * Helpers
 * ============================================================================ */

static int at_eof(LPStringParser *p) {
    return p->pos >= p->len;
}

static char peek_char(LPStringParser *p) {
    if (at_eof(p)) return '\0';
    return p->str[p->pos];
}

static char next_char(LPStringParser *p) {
    if (at_eof(p)) return '\0';
    char c = p->str[p->pos++];
    if (c == '\n') p->line_num++;
    return c;
}

static void skip_whitespace(LPStringParser *p) {
    while (!at_eof(p)) {
        char c = peek_char(p);
        if (c == ' ' || c == '\t' || c == '\r') {
            next_char(p);
        } else {
            break;
        }
    }
}

static void skip_to_next_content(LPStringParser *p) {
    while (!at_eof(p)) {
        skip_whitespace(p);

        char c = peek_char(p);

        /* Skip comments (backslash to end of line) */
        if (c == '\\') {
            while (!at_eof(p) && peek_char(p) != '\n') {
                next_char(p);
            }
            if (peek_char(p) == '\n') next_char(p);
            continue;
        }

        /* Skip newlines */
        if (c == '\n') {
            next_char(p);
            continue;
        }

        /* Found content */
        break;
    }
}

static int is_name_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static int is_name_char(char c) {
    return isalnum((unsigned char)c) || strchr("_!\"#$%&()/,.;?@`'{}|~", c);
}

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

    /* Constraints */
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

static int is_at_line_start(LPStringParser *p, size_t token_start) {
    /* Check if token_start is at the beginning of a line */
    if (token_start == 0) return 1;
    for (size_t i = token_start - 1; i > 0; i--) {
        char c = p->str[i];
        if (c == '\n') return 1;
        if (c != ' ' && c != '\t' && c != '\r') return 0;
    }
    return 1;
}

static int next_token(LPStringParser *p, LPToken *tok) {
    if (p->has_pushback) {
        *tok = p->pushback;
        p->has_pushback = 0;
        return 1;
    }

    memset(tok, 0, sizeof(*tok));
    skip_to_next_content(p);

    if (at_eof(p)) {
        tok->type = TOK_EOF;
        return 0;
    }

    size_t token_start = p->pos;
    char c = peek_char(p);

    /* Operators */
    if (c == '+') {
        tok->type = TOK_PLUS;
        tok->text[0] = '+';
        next_char(p);
        return 1;
    }
    if (c == '-') {
        tok->type = TOK_MINUS;
        tok->text[0] = '-';
        next_char(p);
        return 1;
    }
    if (c == ':') {
        tok->type = TOK_COLON;
        tok->text[0] = ':';
        next_char(p);
        return 1;
    }

    /* Relation operators */
    if (c == '<') {
        next_char(p);
        if (peek_char(p) == '=') next_char(p);
        tok->type = TOK_LE;
        strncpy(tok->text, "<=", 3);
        return 1;
    }
    if (c == '>') {
        next_char(p);
        if (peek_char(p) == '=') next_char(p);
        tok->type = TOK_GE;
        strncpy(tok->text, ">=", 3);
        return 1;
    }
    if (c == '=') {
        next_char(p);
        char c2 = peek_char(p);
        if (c2 == '<') {
            next_char(p);
            tok->type = TOK_LE;
            strncpy(tok->text, "=<", 3);
        } else if (c2 == '>') {
            next_char(p);
            tok->type = TOK_GE;
            strncpy(tok->text, "=>", 3);
        } else {
            tok->type = TOK_EQ;
            tok->text[0] = '=';
        }
        return 1;
    }

    /* Number */
    if (isdigit((unsigned char)c) || c == '.') {
        char numbuf[64];
        int ni = 0;
        while (!at_eof(p) && ni < 63) {
            c = peek_char(p);
            if (isdigit((unsigned char)c) || c == '.' ||
                c == 'e' || c == 'E' || c == '+' || c == '-') {
                /* Handle + and - only after e/E */
                if ((c == '+' || c == '-') && ni > 0 &&
                    numbuf[ni-1] != 'e' && numbuf[ni-1] != 'E') {
                    break;
                }
                numbuf[ni++] = (char)next_char(p);
            } else {
                break;
            }
        }
        numbuf[ni] = '\0';

        char *end;
        tok->value = strtod(numbuf, &end);
        if (end == numbuf) {
            p->error = err_invalid_number;
            return -1;
        }
        strncpy(tok->text, numbuf, LP_MAX_NAME - 1);
        tok->type = TOK_NUMBER;
        return 1;
    }

    /* Name or keyword */
    if (is_name_start(c)) {
        int ni = 0;
        while (!at_eof(p) && ni < LP_MAX_NAME - 1) {
            c = peek_char(p);
            if (is_name_char(c)) {
                tok->text[ni++] = (char)next_char(p);
            } else {
                break;
            }
        }
        tok->text[ni] = '\0';

        /* Check for "free" keyword */
        if (strcasecmp(tok->text, "FREE") == 0) {
            tok->type = TOK_FREE;
            return 1;
        }

        /* Check for section keyword at line start */
        if (is_at_line_start(p, token_start)) {
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

    p->error = err_syntax;
    return -1;
}

static void pushback_token(LPStringParser *p, const LPToken *tok) {
    p->pushback = *tok;
    p->has_pushback = 1;
}

static int find_or_add_var(LPStringParser *p, const char *name) {
    for (int i = 0; i < p->num_vars; i++) {
        if (strcmp(p->vars[i].name, name) == 0) {
            return i;
        }
    }

    if (p->num_vars >= p->var_capacity) {
        int new_cap = p->var_capacity * 2;
        LPVariable *new_vars = (LPVariable *)realloc(p->vars, new_cap * sizeof(LPVariable));
        if (!new_vars) return -1;
        p->vars = new_vars;
        p->var_capacity = new_cap;
    }

    int idx = p->num_vars;
    memset(&p->vars[idx], 0, sizeof(LPVariable));
    strncpy(p->vars[idx].name, name, LP_MAX_NAME - 1);
    p->vars[idx].lb = 0.0;
    p->vars[idx].ub = RALPH_INFINITY;
    p->vars[idx].type = 'C';
    p->num_vars++;

    return idx;
}

/* ============================================================================
 * Expression Parser
 * ============================================================================ */

static int parse_linear_expr(LPStringParser *p, int *indices, double *coeffs,
                             int max_terms, int for_objective) {
    int nnz = 0;
    double sign = 1.0;
    LPToken tok;

    while (1) {
        int ret = next_token(p, &tok);
        if (ret <= 0) break;

        if (tok.type == TOK_LE || tok.type == TOK_GE || tok.type == TOK_EQ) {
            pushback_token(p, &tok);
            break;
        }
        if (tok.type == TOK_KEYWORD) {
            pushback_token(p, &tok);
            break;
        }
        if (tok.type == TOK_COLON && !for_objective) {
            pushback_token(p, &tok);
            break;
        }

        if (tok.type == TOK_PLUS) {
            sign = 1.0;
            continue;
        }
        if (tok.type == TOK_MINUS) {
            sign = -1.0;
            continue;
        }

        double coef = sign;
        const char *var_name = NULL;

        if (tok.type == TOK_NUMBER) {
            coef = sign * tok.value;

            ret = next_token(p, &tok);
            if (ret <= 0) break;

            if (tok.type == TOK_NAME) {
                var_name = tok.text;
            } else if (tok.type == TOK_PLUS || tok.type == TOK_MINUS ||
                       tok.type == TOK_LE || tok.type == TOK_GE ||
                       tok.type == TOK_EQ || tok.type == TOK_KEYWORD) {
                pushback_token(p, &tok);
                sign = 1.0;
                continue;
            } else {
                p->error = err_syntax;
                return -1;
            }
        } else if (tok.type == TOK_NAME) {
            var_name = tok.text;
        } else {
            p->error = err_syntax;
            return -1;
        }

        if (var_name) {
            if (nnz >= max_terms) {
                p->error = err_too_many_terms;
                return -1;
            }

            int var_idx = find_or_add_var(p, var_name);
            if (var_idx < 0) {
                p->error = err_memory;
                return -1;
            }

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

        sign = 1.0;
    }

    return nnz;
}

/* ============================================================================
 * Section Parsers
 * ============================================================================ */

static int parse_objective(LPStringParser *p) {
    LPToken tok;

    int *indices = (int *)calloc(LP_MAX_TERMS, sizeof(int));
    double *coeffs = (double *)calloc(LP_MAX_TERMS, sizeof(double));
    if (!indices || !coeffs) {
        free(indices);
        free(coeffs);
        p->error = err_memory;
        return -1;
    }

    /* Check for optional objective name or leading colon (from "max:" syntax) */
    int ret = next_token(p, &tok);
    if (ret > 0 && tok.type == TOK_COLON) {
        /* Skip bare colon after "max:" or "min:" */
        /* Get next token for objective name check */
        ret = next_token(p, &tok);
    }

    if (ret > 0 && tok.type == TOK_NAME) {
        LPToken tok2;
        if (next_token(p, &tok2) > 0 && tok2.type == TOK_COLON) {
            strncpy(p->obj_name, tok.text, LP_MAX_NAME - 1);
        } else {
            if (tok2.type != TOK_EOF) pushback_token(p, &tok2);
            pushback_token(p, &tok);
        }
    } else if (ret > 0) {
        pushback_token(p, &tok);
    }

    int nnz = parse_linear_expr(p, indices, coeffs, LP_MAX_TERMS, 1);
    if (nnz < 0) {
        free(indices);
        free(coeffs);
        return -1;
    }

    for (int i = 0; i < nnz; i++) {
        p->vars[indices[i]].obj = coeffs[i];
    }

    free(indices);
    free(coeffs);
    p->has_objective = 1;
    return 0;
}

static int parse_constraint(LPStringParser *p) {
    LPToken tok;

    if (p->num_cons >= p->con_capacity) {
        int new_cap = p->con_capacity * 2;
        LPConstraint *new_cons = (LPConstraint *)realloc(p->cons, new_cap * sizeof(LPConstraint));
        if (!new_cons) {
            p->error = err_memory;
            return -1;
        }
        p->cons = new_cons;
        p->con_capacity = new_cap;
    }

    LPConstraint *con = &p->cons[p->num_cons];
    memset(con, 0, sizeof(LPConstraint));
    con->capacity = 128;
    con->indices = (int *)calloc(con->capacity, sizeof(int));
    con->coeffs = (double *)calloc(con->capacity, sizeof(double));
    if (!con->indices || !con->coeffs) {
        free(con->indices);
        free(con->coeffs);
        p->error = err_memory;
        return -1;
    }

    int ret = next_token(p, &tok);
    if (ret <= 0) {
        free(con->indices);
        free(con->coeffs);
        return 0;
    }

    if (tok.type == TOK_KEYWORD) {
        pushback_token(p, &tok);
        free(con->indices);
        free(con->coeffs);
        return 0;
    }

    if (tok.type == TOK_NAME) {
        LPToken tok2;
        if (next_token(p, &tok2) > 0 && tok2.type == TOK_COLON) {
            strncpy(con->name, tok.text, LP_MAX_NAME - 1);
        } else {
            if (tok2.type != TOK_EOF) pushback_token(p, &tok2);
            pushback_token(p, &tok);
        }
    } else {
        pushback_token(p, &tok);
    }

    int nnz = parse_linear_expr(p, con->indices, con->coeffs, con->capacity, 0);
    if (nnz < 0) {
        free(con->indices);
        free(con->coeffs);
        return -1;
    }
    con->nnz = nnz;

    ret = next_token(p, &tok);
    if (ret <= 0 || (tok.type != TOK_LE && tok.type != TOK_GE && tok.type != TOK_EQ)) {
        p->error = err_no_sense;
        free(con->indices);
        free(con->coeffs);
        return -1;
    }

    if (tok.type == TOK_LE) con->sense = 'L';
    else if (tok.type == TOK_GE) con->sense = 'G';
    else con->sense = 'E';

    ret = next_token(p, &tok);
    if (ret <= 0) {
        p->error = err_no_rhs;
        free(con->indices);
        free(con->coeffs);
        return -1;
    }

    double rhs_sign = 1.0;
    if (tok.type == TOK_MINUS) {
        rhs_sign = -1.0;
        ret = next_token(p, &tok);
    } else if (tok.type == TOK_PLUS) {
        ret = next_token(p, &tok);
    }

    if (ret <= 0 || tok.type != TOK_NUMBER) {
        p->error = err_no_rhs;
        free(con->indices);
        free(con->coeffs);
        return -1;
    }
    con->rhs = rhs_sign * tok.value;

    p->num_cons++;
    return 1;
}

static int parse_constraints_section(LPStringParser *p) {
    while (1) {
        int ret = parse_constraint(p);
        if (ret < 0) return -1;
        if (ret == 0) break;
    }
    return 0;
}

static int parse_bounds_section(LPStringParser *p) {
    LPToken tok;

    while (1) {
        int ret = next_token(p, &tok);
        if (ret <= 0) break;

        if (tok.type == TOK_KEYWORD) {
            pushback_token(p, &tok);
            break;
        }

        double lb = 0.0, ub = RALPH_INFINITY;
        const char *var_name = NULL;
        int has_lb = 0, has_ub = 0;
        int is_free = 0;

        /* Handle -infinity or negative number as lower bound */
        if (tok.type == TOK_MINUS) {
            ret = next_token(p, &tok);
            if (ret <= 0) break;

            if (tok.type == TOK_NAME &&
                (strcasecmp(tok.text, "INFINITY") == 0 ||
                 strcasecmp(tok.text, "INF") == 0)) {
                lb = -RALPH_INFINITY;
                has_lb = 1;

                ret = next_token(p, &tok);
                if (ret <= 0 || tok.type != TOK_LE) {
                    p->error = err_syntax;
                    return -1;
                }

                ret = next_token(p, &tok);
                if (ret <= 0 || tok.type != TOK_NAME) {
                    p->error = err_syntax;
                    return -1;
                }
                var_name = tok.text;
            } else if (tok.type == TOK_NUMBER) {
                lb = -tok.value;
                has_lb = 1;

                ret = next_token(p, &tok);
                if (ret <= 0 || tok.type != TOK_LE) {
                    p->error = err_syntax;
                    return -1;
                }

                ret = next_token(p, &tok);
                if (ret <= 0 || tok.type != TOK_NAME) {
                    p->error = err_syntax;
                    return -1;
                }
                var_name = tok.text;
            } else {
                p->error = err_syntax;
                return -1;
            }
        } else if (tok.type == TOK_NUMBER) {
            lb = tok.value;
            has_lb = 1;

            ret = next_token(p, &tok);
            if (ret <= 0 || tok.type != TOK_LE) {
                p->error = err_syntax;
                return -1;
            }

            ret = next_token(p, &tok);
            if (ret <= 0 || tok.type != TOK_NAME) {
                p->error = err_syntax;
                return -1;
            }
            var_name = tok.text;
        } else if (tok.type == TOK_NAME) {
            var_name = tok.text;
        } else {
            p->error = err_syntax;
            return -1;
        }

        int var_idx = find_or_add_var(p, var_name);
        if (var_idx < 0) {
            p->error = err_memory;
            return -1;
        }

        ret = next_token(p, &tok);
        if (ret <= 0) {
            if (has_lb) p->vars[var_idx].lb = lb;
            break;
        }

        if (tok.type == TOK_FREE) {
            is_free = 1;
        } else if (tok.type == TOK_LE) {
            ret = next_token(p, &tok);
            if (ret <= 0) {
                p->error = err_syntax;
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
                p->error = err_syntax;
                return -1;
            }

            if (tok.type == TOK_NAME &&
                (strcasecmp(tok.text, "INFINITY") == 0 ||
                 strcasecmp(tok.text, "INF") == 0)) {
                ub = ub_sign * RALPH_INFINITY;
            } else if (tok.type == TOK_NUMBER) {
                ub = ub_sign * tok.value;
            } else {
                p->error = err_syntax;
                return -1;
            }
            has_ub = 1;
        } else if (tok.type == TOK_GE) {
            ret = next_token(p, &tok);
            if (ret <= 0) {
                p->error = err_syntax;
                return -1;
            }

            double lb_sign = 1.0;
            if (tok.type == TOK_MINUS) {
                lb_sign = -1.0;
                ret = next_token(p, &tok);
            }

            if (ret <= 0 || tok.type != TOK_NUMBER) {
                p->error = err_syntax;
                return -1;
            }
            lb = lb_sign * tok.value;
            has_lb = 1;
        } else if (tok.type == TOK_EQ) {
            ret = next_token(p, &tok);
            if (ret <= 0) {
                p->error = err_syntax;
                return -1;
            }

            double val_sign = 1.0;
            if (tok.type == TOK_MINUS) {
                val_sign = -1.0;
                ret = next_token(p, &tok);
            }

            if (ret <= 0 || tok.type != TOK_NUMBER) {
                p->error = err_syntax;
                return -1;
            }
            lb = ub = val_sign * tok.value;
            has_lb = has_ub = 1;
        } else {
            pushback_token(p, &tok);
        }

        if (is_free) {
            p->vars[var_idx].lb = -RALPH_INFINITY;
            p->vars[var_idx].ub = RALPH_INFINITY;
        } else {
            if (has_lb) p->vars[var_idx].lb = lb;
            if (has_ub) p->vars[var_idx].ub = ub;
        }
    }

    return 0;
}

static int parse_general_section(LPStringParser *p) {
    LPToken tok;

    while (1) {
        int ret = next_token(p, &tok);
        if (ret <= 0) break;

        if (tok.type == TOK_KEYWORD) {
            pushback_token(p, &tok);
            break;
        }

        if (tok.type != TOK_NAME) {
            p->error = err_syntax;
            return -1;
        }

        int var_idx = find_or_add_var(p, tok.text);
        if (var_idx < 0) {
            p->error = err_memory;
            return -1;
        }

        p->vars[var_idx].type = 'I';
    }

    return 0;
}

static int parse_binary_section(LPStringParser *p) {
    LPToken tok;

    while (1) {
        int ret = next_token(p, &tok);
        if (ret <= 0) break;

        if (tok.type == TOK_KEYWORD) {
            pushback_token(p, &tok);
            break;
        }

        if (tok.type != TOK_NAME) {
            p->error = err_syntax;
            return -1;
        }

        int var_idx = find_or_add_var(p, tok.text);
        if (var_idx < 0) {
            p->error = err_memory;
            return -1;
        }

        p->vars[var_idx].type = 'B';
        p->vars[var_idx].lb = 0.0;
        p->vars[var_idx].ub = 1.0;
    }

    return 0;
}

/* ============================================================================
 * Parser Lifecycle
 * ============================================================================ */

static LPStringParser *lp_string_parser_create(const char *str, size_t len) {
    LPStringParser *p = (LPStringParser *)calloc(1, sizeof(LPStringParser));
    if (!p) return NULL;

    p->str = str;
    p->len = len;
    p->pos = 0;
    p->line_num = 1;
    p->obj_sense = 1;  /* Minimize by default */

    p->var_capacity = 64;
    p->vars = (LPVariable *)calloc(p->var_capacity, sizeof(LPVariable));

    p->con_capacity = 64;
    p->cons = (LPConstraint *)calloc(p->con_capacity, sizeof(LPConstraint));

    if (!p->vars || !p->cons) {
        free(p->vars);
        free(p->cons);
        free(p);
        return NULL;
    }

    return p;
}

static void lp_string_parser_free(LPStringParser *p) {
    if (!p) return;

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

int ralph_api_parse_lp(const char *lp_string, size_t len,
                       RalphModel *model,
                       const char **error_msg) {
    if (!lp_string || len == 0) {
        if (error_msg) *error_msg = err_empty;
        return -1;
    }

    LPStringParser *p = lp_string_parser_create(lp_string, len);
    if (!p) {
        if (error_msg) *error_msg = err_memory;
        return -1;
    }

    int parse_error = 0;

    /* Main parsing loop */
    while (!parse_error && !at_eof(p)) {
        LPToken tok;
        int ret = next_token(p, &tok);
        if (ret < 0) {
            parse_error = 1;
            break;
        }
        if (ret == 0) break;

        if (tok.type == TOK_KEYWORD) {
            LPSection section = tok.keyword_section;

            if (section == LP_SECTION_OBJECTIVE) {
                p->obj_sense = tok.is_max ? -1 : 1;
                if (parse_objective(p) < 0) {
                    parse_error = 1;
                }
            } else if (section == LP_SECTION_CONSTRAINTS) {
                /* Consume optional "TO" or "THAT" */
                LPToken peek;
                if (next_token(p, &peek) > 0) {
                    if (peek.type == TOK_NAME &&
                        (strcasecmp(peek.text, "TO") == 0 ||
                         strcasecmp(peek.text, "THAT") == 0)) {
                        /* Consumed */
                    } else {
                        pushback_token(p, &peek);
                    }
                }
                if (parse_constraints_section(p) < 0) {
                    parse_error = 1;
                }
            } else if (section == LP_SECTION_BOUNDS) {
                if (parse_bounds_section(p) < 0) {
                    parse_error = 1;
                }
            } else if (section == LP_SECTION_GENERAL) {
                if (parse_general_section(p) < 0) {
                    parse_error = 1;
                }
            } else if (section == LP_SECTION_BINARY) {
                if (parse_binary_section(p) < 0) {
                    parse_error = 1;
                }
            } else if (section == LP_SECTION_END) {
                break;
            }
        }
    }

    /* Validate */
    if (!parse_error && !p->has_objective) {
        p->error = err_no_obj;
        parse_error = 1;
    }

    if (parse_error) {
        if (error_msg) *error_msg = p->error ? p->error : err_syntax;
        lp_string_parser_free(p);
        return -1;
    }

    /* Build model */
    ralph_set_obj_sense(model, p->obj_sense == 1 ? RALPH_MINIMIZE : RALPH_MAXIMIZE);

    for (int j = 0; j < p->num_vars; j++) {
        RalphVarType type = RALPH_CONTINUOUS;
        if (p->vars[j].type == 'I') type = RALPH_INTEGER;
        if (p->vars[j].type == 'B') type = RALPH_BINARY;

        ralph_add_var(model, p->vars[j].lb, p->vars[j].ub, p->vars[j].obj, type);
    }

    for (int j = 0; j < p->num_vars; j++) {
        ralph_set_var_name(model, j, p->vars[j].name);
    }

    for (int i = 0; i < p->num_cons; i++) {
        LPConstraint *con = &p->cons[i];
        RalphSense sense;
        if (con->sense == 'L') sense = RALPH_LESS_EQUAL;
        else if (con->sense == 'G') sense = RALPH_GREATER_EQUAL;
        else sense = RALPH_EQUAL;

        ralph_add_constraint(model, con->nnz, con->indices, con->coeffs,
                            sense, con->rhs);
    }

    for (int i = 0; i < p->num_cons; i++) {
        if (p->cons[i].name[0] != '\0') {
            ralph_set_con_name(model, i, p->cons[i].name);
        }
    }

    lp_string_parser_free(p);
    return 0;
}

int ralph_api_parse_mps(const char *mps_string, size_t len,
                        RalphModel *model,
                        const char **error_msg) {
    /* MPS parsing is more complex - for now, return unsupported */
    /* TODO: Implement MPS string parser */
    (void)mps_string;
    (void)len;
    (void)model;
    if (error_msg) *error_msg = "MPS format not yet supported in API (use LP format)";
    return -1;
}
