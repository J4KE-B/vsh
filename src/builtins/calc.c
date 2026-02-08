/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/calc.c - Math expression evaluator with recursive descent parsing
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <float.h>

/* ---- Token types -------------------------------------------------------- */

typedef enum {
    TOK_NUM,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_POWER,    /* ** or ^ */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_IDENT,    /* function name or constant */
    TOK_END,
    TOK_ERROR
} TokenType;

typedef struct {
    TokenType type;
    double    num;
    char      ident[32];
} Token;

/* ---- Parser state ------------------------------------------------------- */

typedef struct {
    const char *input;
    int         pos;
    Token       cur;
    int         error;
    char        errmsg[128];
} Parser;

/* ---- Lexer -------------------------------------------------------------- */

static void skip_whitespace(Parser *p) {
    while (p->input[p->pos] == ' ' || p->input[p->pos] == '\t')
        p->pos++;
}

static void next_token(Parser *p) {
    skip_whitespace(p);

    char c = p->input[p->pos];

    if (c == '\0') {
        p->cur.type = TOK_END;
        return;
    }

    /* Numbers: digits or leading dot */
    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)p->input[p->pos + 1]))) {
        char *end;
        p->cur.num = strtod(p->input + p->pos, &end);
        p->pos = end - p->input;
        p->cur.type = TOK_NUM;
        return;
    }

    /* Identifiers (functions and constants) */
    if (isalpha((unsigned char)c) || c == '_') {
        int start = p->pos;
        while (isalnum((unsigned char)p->input[p->pos]) || p->input[p->pos] == '_')
            p->pos++;
        int len = p->pos - start;
        if (len >= (int)sizeof(p->cur.ident)) len = (int)sizeof(p->cur.ident) - 1;
        memcpy(p->cur.ident, p->input + start, len);
        p->cur.ident[len] = '\0';
        p->cur.type = TOK_IDENT;
        return;
    }

    /* Two-character operators */
    if (c == '*' && p->input[p->pos + 1] == '*') {
        p->pos += 2;
        p->cur.type = TOK_POWER;
        return;
    }

    /* Single-character operators */
    p->pos++;
    switch (c) {
    case '+': p->cur.type = TOK_PLUS;    return;
    case '-': p->cur.type = TOK_MINUS;   return;
    case '*': p->cur.type = TOK_STAR;    return;
    case '/': p->cur.type = TOK_SLASH;   return;
    case '%': p->cur.type = TOK_PERCENT; return;
    case '^': p->cur.type = TOK_POWER;   return;
    case '(': p->cur.type = TOK_LPAREN;  return;
    case ')': p->cur.type = TOK_RPAREN;  return;
    default:
        p->cur.type = TOK_ERROR;
        snprintf(p->errmsg, sizeof(p->errmsg),
                 "unexpected character '%c'", c);
        p->error = 1;
        return;
    }
}

/* ---- Recursive descent parser ------------------------------------------- */

static double parse_expr(Parser *p);

static void parser_error(Parser *p, const char *msg) {
    if (!p->error) {
        snprintf(p->errmsg, sizeof(p->errmsg), "%s", msg);
        p->error = 1;
    }
}

/* primary → NUMBER | '(' expr ')' | FUNC '(' expr ')' | CONST */
static double parse_primary(Parser *p) {
    if (p->error) return 0;

    if (p->cur.type == TOK_NUM) {
        double val = p->cur.num;
        next_token(p);
        return val;
    }

    if (p->cur.type == TOK_LPAREN) {
        next_token(p);  /* consume '(' */
        double val = parse_expr(p);
        if (p->cur.type != TOK_RPAREN) {
            parser_error(p, "expected closing ')'");
            return 0;
        }
        next_token(p);  /* consume ')' */
        return val;
    }

    if (p->cur.type == TOK_IDENT) {
        char name[32];
        strncpy(name, p->cur.ident, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';

        /* Check for constants */
        if (strcmp(name, "pi") == 0 || strcmp(name, "PI") == 0) {
            next_token(p);
            return 3.14159265358979323846;
        }
        if (strcmp(name, "e") == 0 || strcmp(name, "E") == 0) {
            /* Make sure it's not 'e' as part of scientific notation -
             * strtod already handles that, so if we get here it's the
             * constant */
            next_token(p);
            return 2.71828182845904523536;
        }

        /* Must be a function call: FUNC '(' expr ')' */
        next_token(p);  /* consume identifier */
        if (p->cur.type != TOK_LPAREN) {
            char msg[64];
            snprintf(msg, sizeof(msg), "unknown identifier '%s'", name);
            parser_error(p, msg);
            return 0;
        }
        next_token(p);  /* consume '(' */
        double arg = parse_expr(p);
        if (p->cur.type != TOK_RPAREN) {
            parser_error(p, "expected ')' after function argument");
            return 0;
        }
        next_token(p);  /* consume ')' */

        /* Dispatch function */
        if (strcmp(name, "sqrt") == 0) {
            if (arg < 0) { parser_error(p, "sqrt of negative number"); return 0; }
            return sqrt(arg);
        }
        if (strcmp(name, "sin") == 0)   return sin(arg);
        if (strcmp(name, "cos") == 0)   return cos(arg);
        if (strcmp(name, "tan") == 0)   return tan(arg);
        if (strcmp(name, "log") == 0) {
            if (arg <= 0) { parser_error(p, "log of non-positive number"); return 0; }
            return log(arg);
        }
        if (strcmp(name, "log10") == 0) {
            if (arg <= 0) { parser_error(p, "log10 of non-positive number"); return 0; }
            return log10(arg);
        }
        if (strcmp(name, "abs") == 0)   return fabs(arg);
        if (strcmp(name, "ceil") == 0)  return ceil(arg);
        if (strcmp(name, "floor") == 0) return floor(arg);

        char msg[64];
        snprintf(msg, sizeof(msg), "unknown function '%s'", name);
        parser_error(p, msg);
        return 0;
    }

    parser_error(p, "expected number, '(', or function");
    return 0;
}

/* unary → ['-' | '+'] primary */
static double parse_unary(Parser *p) {
    if (p->error) return 0;

    if (p->cur.type == TOK_MINUS) {
        next_token(p);
        return -parse_unary(p);
    }
    if (p->cur.type == TOK_PLUS) {
        next_token(p);
        return parse_unary(p);
    }
    return parse_primary(p);
}

/* power → unary (('**' | '^') power)?    -- right-associative via recursion */
static double parse_power(Parser *p) {
    if (p->error) return 0;

    double base = parse_unary(p);
    if (p->cur.type == TOK_POWER) {
        next_token(p);
        double exp = parse_power(p);  /* right-associative: recurse */
        return pow(base, exp);
    }
    return base;
}

/* term → power (('*' | '/' | '%') power)* */
static double parse_term(Parser *p) {
    if (p->error) return 0;

    double left = parse_power(p);
    while (p->cur.type == TOK_STAR || p->cur.type == TOK_SLASH ||
           p->cur.type == TOK_PERCENT) {
        TokenType op = p->cur.type;
        next_token(p);
        double right = parse_power(p);
        if (op == TOK_STAR) {
            left *= right;
        } else if (op == TOK_SLASH) {
            if (right == 0) {
                parser_error(p, "division by zero");
                return 0;
            }
            left /= right;
        } else {
            if (right == 0) {
                parser_error(p, "modulo by zero");
                return 0;
            }
            left = fmod(left, right);
        }
    }
    return left;
}

/* expr → term (('+' | '-') term)* */
static double parse_expr(Parser *p) {
    if (p->error) return 0;

    double left = parse_term(p);
    while (p->cur.type == TOK_PLUS || p->cur.type == TOK_MINUS) {
        TokenType op = p->cur.type;
        next_token(p);
        double right = parse_term(p);
        if (op == TOK_PLUS)
            left += right;
        else
            left -= right;
    }
    return left;
}

/* ---- Main entry point --------------------------------------------------- */

/*
 * calc EXPRESSION
 *
 * Evaluate a mathematical expression and print the result.
 * Supports: +, -, *, /, %, ** (^), parentheses, constants (pi, e),
 * and functions (sqrt, sin, cos, tan, log, log10, abs, ceil, floor).
 */
int builtin_calc(Shell *shell, int argc, char **argv) {
    (void)shell;

    if (argc < 2) {
        fprintf(stderr, "Usage: calc EXPRESSION\n");
        fprintf(stderr, "  Operators: + - * / %% ** ^\n");
        fprintf(stderr, "  Constants: pi, e\n");
        fprintf(stderr, "  Functions: sqrt sin cos tan log log10 abs ceil floor\n");
        return 1;
    }

    /* Join all arguments into a single expression string */
    size_t total_len = 0;
    for (int i = 1; i < argc; i++)
        total_len += strlen(argv[i]) + 1;

    char *expr = malloc(total_len + 1);
    if (!expr) {
        fprintf(stderr, "vsh: calc: out of memory\n");
        return 1;
    }
    expr[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat(expr, " ");
        strcat(expr, argv[i]);
    }

    /* Initialize parser */
    Parser parser;
    memset(&parser, 0, sizeof(parser));
    parser.input = expr;
    parser.pos   = 0;
    parser.error = 0;

    next_token(&parser);
    double result = parse_expr(&parser);

    if (parser.error) {
        fprintf(stderr, "vsh: calc: %s\n", parser.errmsg);
        free(expr);
        return 1;
    }

    if (parser.cur.type != TOK_END) {
        fprintf(stderr, "vsh: calc: unexpected token at position %d\n",
                parser.pos);
        free(expr);
        return 1;
    }

    free(expr);

    /* Print result: integer format if exact, otherwise up to 10 significant digits */
    if (isfinite(result) && result == floor(result) &&
        fabs(result) < 1e15) {
        printf("%.0f\n", result);
    } else {
        printf("%.10g\n", result);
    }

    return 0;
}
