/* ============================================================================
 * vsh - Vanguard Shell
 * lexer.c - Tokenizer for shell input
 *
 * Scans raw input into a token stream. Handles quoting (single, double),
 * backslash escapes, multi-character operators, fd-prefixed redirections,
 * comments, and keyword recognition. All token storage is arena-allocated
 * so the entire token list is freed in one shot with the parse arena.
 * ============================================================================ */

#include "lexer.h"
#include "arena.h"
#include "safe_string.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ---- Internal helpers --------------------------------------------------- */

/* Current character (NUL at end of input) */
static inline char lex_cur(const Lexer *lex)
{
    if (lex->pos >= lex->len)
        return '\0';
    return lex->input[lex->pos];
}

/* Lookahead by n characters */
static inline char lex_peek(const Lexer *lex, int n)
{
    int idx = lex->pos + n;
    if (idx >= lex->len || idx < 0)
        return '\0';
    return lex->input[idx];
}

/* Advance the lexer position by one character, tracking line/col */
static inline void lex_advance(Lexer *lex)
{
    if (lex->pos < lex->len) {
        if (lex->input[lex->pos] == '\n') {
            lex->line++;
            lex->col = 1;
        } else {
            lex->col++;
        }
        lex->pos++;
    }
}

/* Create a token with the given type and value */
static Token make_token(TokenType type, const char *value, int line, int col)
{
    Token tok;
    tok.type     = type;
    tok.value    = (char *)value;
    tok.redir_fd = -1;
    tok.line     = line;
    tok.col      = col;
    return tok;
}

/* Set a lexer error message (arena-allocated) */
static void lex_error(Lexer *lex, const char *msg)
{
    if (!lex->error) {
        char buf[256];
        snprintf(buf, sizeof(buf), "lexer error at %d:%d: %s",
                 lex->line, lex->col, msg);
        lex->error = arena_strdup(lex->arena, buf);
    }
}

/* Skip whitespace (spaces and tabs only, NOT newlines) */
static void skip_whitespace(Lexer *lex)
{
    while (lex->pos < lex->len) {
        char c = lex->input[lex->pos];
        if (c == ' ' || c == '\t') {
            lex_advance(lex);
        } else {
            break;
        }
    }
}

/* Skip a comment: '#' through end of line (do not consume newline) */
static void skip_comment(Lexer *lex)
{
    while (lex->pos < lex->len && lex->input[lex->pos] != '\n')
        lex_advance(lex);
}

/* ---- Keyword table ------------------------------------------------------ */

typedef struct {
    const char *name;
    TokenType   type;
} KeywordEntry;

static const KeywordEntry keywords[] = {
    { "if",       TOK_IF       },
    { "then",     TOK_THEN     },
    { "elif",     TOK_ELIF     },
    { "else",     TOK_ELSE     },
    { "fi",       TOK_FI       },
    { "while",    TOK_WHILE    },
    { "for",      TOK_FOR      },
    { "do",       TOK_DO       },
    { "done",     TOK_DONE     },
    { "in",       TOK_IN       },
    { "function", TOK_FUNCTION },
    { "return",   TOK_RETURN   },
    { "local",    TOK_LOCAL    },
    { NULL,       TOK_WORD     }
};

/* Check if a completed word matches a shell keyword; return its type */
static TokenType check_keyword(const char *word)
{
    for (const KeywordEntry *kw = keywords; kw->name; kw++) {
        if (strcmp(word, kw->name) == 0)
            return kw->type;
    }
    return TOK_WORD;
}

/* ---- TokenList helpers -------------------------------------------------- */

#define TOKLIST_INIT_CAP 32

static TokenList *toklist_new(Arena *arena)
{
    TokenList *list = arena_alloc(arena, sizeof(TokenList));
    if (!list)
        return NULL;

    list->tokens   = arena_alloc(arena, sizeof(Token) * TOKLIST_INIT_CAP);
    list->count    = 0;
    list->capacity = TOKLIST_INIT_CAP;
    return list;
}

static bool toklist_add(TokenList *list, Token tok, Arena *arena)
{
    if (list->count >= list->capacity) {
        int new_cap = list->capacity * 2;
        Token *new_tokens = arena_alloc(arena, sizeof(Token) * new_cap);
        if (!new_tokens)
            return false;
        memcpy(new_tokens, list->tokens, sizeof(Token) * list->count);
        list->tokens   = new_tokens;
        list->capacity = new_cap;
    }
    list->tokens[list->count++] = tok;
    return true;
}

/* ---- Word building ------------------------------------------------------ */

/* Build a WORD token by accumulating characters from the input.
 * Handles unquoted text, single-quoted sections, double-quoted sections,
 * and backslash escapes. Returns the resulting token (WORD or keyword). */
static Token build_word(Lexer *lex)
{
    int start_line = lex->line;
    int start_col  = lex->col;

    SafeString *buf = sstr_new(SSTR_INIT_CAP);
    if (!buf) {
        lex_error(lex, "out of memory");
        return make_token(TOK_EOF, NULL, start_line, start_col);
    }

    bool in_quotes = false;  /* track if any quoting occurred */
    (void)in_quotes;

    while (lex->pos < lex->len) {
        char c = lex_cur(lex);

        /* ---- Single-quoted section ---- */
        if (c == '\'') {
            in_quotes = true;
            lex_advance(lex);  /* consume opening quote */
            while (lex->pos < lex->len && lex_cur(lex) != '\'') {
                sstr_append_char(buf, lex_cur(lex));
                lex_advance(lex);
            }
            if (lex->pos >= lex->len) {
                lex_error(lex, "unterminated single quote");
                break;
            }
            lex_advance(lex);  /* consume closing quote */
            continue;
        }

        /* ---- Double-quoted section ---- */
        if (c == '"') {
            in_quotes = true;
            lex_advance(lex);  /* consume opening quote */
            while (lex->pos < lex->len && lex_cur(lex) != '"') {
                char dc = lex_cur(lex);
                if (dc == '\\') {
                    /* In double quotes, backslash only escapes $, `, ", \, \n */
                    char next = lex_peek(lex, 1);
                    if (next == '$' || next == '`' || next == '"' ||
                        next == '\\' || next == '\n') {
                        lex_advance(lex);  /* consume backslash */
                        if (next == '\n') {
                            /* line continuation: skip both backslash and newline */
                            lex_advance(lex);
                        } else {
                            sstr_append_char(buf, next);
                            lex_advance(lex);
                        }
                    } else {
                        /* literal backslash */
                        sstr_append_char(buf, '\\');
                        lex_advance(lex);
                    }
                } else {
                    sstr_append_char(buf, dc);
                    lex_advance(lex);
                }
            }
            if (lex->pos >= lex->len) {
                lex_error(lex, "unterminated double quote");
                break;
            }
            lex_advance(lex);  /* consume closing quote */
            continue;
        }

        /* ---- Backslash escape (outside quotes) ---- */
        if (c == '\\') {
            char next = lex_peek(lex, 1);
            if (next == '\n') {
                /* Line continuation: skip backslash and newline */
                lex_advance(lex);
                lex_advance(lex);
                continue;
            }
            if (next == '\0') {
                /* Backslash at end of input: treat as literal */
                sstr_append_char(buf, '\\');
                lex_advance(lex);
                break;
            }
            lex_advance(lex);  /* consume backslash */
            sstr_append_char(buf, lex_cur(lex));
            lex_advance(lex);  /* consume escaped char */
            continue;
        }

        /* ---- Word-breaking characters ---- */
        if (c == ' ' || c == '\t' || c == '\n' ||
            c == '|' || c == '&' || c == ';' ||
            c == '>' || c == '<' || c == '(' || c == ')' ||
            c == '{' || c == '}' || c == '#') {
            break;
        }

        /* ---- Regular character ---- */
        sstr_append_char(buf, c);
        lex_advance(lex);
    }

    /* Copy the word into the arena and free the SafeString */
    char *value = arena_strdup(lex->arena, sstr_cstr(buf));
    sstr_free(buf);

    /* Check for keyword */
    TokenType type = check_keyword(value);

    return make_token(type, value, start_line, start_col);
}

/* ---- Public API --------------------------------------------------------- */

void lexer_init(Lexer *lex, const char *input, Arena *arena)
{
    lex->input = input;
    lex->pos   = 0;
    lex->len   = input ? (int)strlen(input) : 0;
    lex->line  = 1;
    lex->col   = 1;
    lex->arena = arena;
    lex->error = NULL;
}

Token lexer_next(Lexer *lex)
{
    /* Skip whitespace (not newlines) */
    skip_whitespace(lex);

    int tok_line = lex->line;
    int tok_col  = lex->col;

    /* EOF check */
    if (lex->pos >= lex->len)
        return make_token(TOK_EOF, NULL, tok_line, tok_col);

    char c = lex_cur(lex);

    /* ---- Comments ---- */
    if (c == '#') {
        skip_comment(lex);
        /* After skipping the comment, recurse to get the next real token.
         * The newline (if present) will be picked up on the next call. */
        return lexer_next(lex);
    }

    /* ---- Newline ---- */
    if (c == '\n') {
        lex_advance(lex);
        return make_token(TOK_NEWLINE, arena_strdup(lex->arena, "\n"),
                          tok_line, tok_col);
    }

    /* ---- Fd-prefixed redirections (e.g. 2>, 2>>, 0<, 2>&1) ---- */
    if (isdigit((unsigned char)c) &&
        (lex_peek(lex, 1) == '>' || lex_peek(lex, 1) == '<')) {
        int fd = c - '0';
        lex_advance(lex);  /* consume the digit */

        tok_col = lex->col;  /* update to position of the operator */
        char op = lex_cur(lex);

        Token tok;
        if (op == '>') {
            if (lex_peek(lex, 1) == '>') {
                lex_advance(lex);
                lex_advance(lex);
                tok = make_token(TOK_REDIR_APPEND,
                                 arena_strdup(lex->arena, ">>"),
                                 tok_line, tok_col);
            } else if (lex_peek(lex, 1) == '&') {
                /* 2>&1 style dup redirection */
                lex_advance(lex);
                lex_advance(lex);
                /* Capture the target fd/word after >& */
                SafeString *dup_val = sstr_new(8);
                while (lex->pos < lex->len &&
                       !isspace((unsigned char)lex_cur(lex)) &&
                       lex_cur(lex) != '|' && lex_cur(lex) != '&' &&
                       lex_cur(lex) != ';' && lex_cur(lex) != '\n') {
                    sstr_append_char(dup_val, lex_cur(lex));
                    lex_advance(lex);
                }
                tok = make_token(TOK_REDIR_DUP,
                                 arena_strdup(lex->arena, sstr_cstr(dup_val)),
                                 tok_line, tok_col);
                sstr_free(dup_val);
            } else {
                lex_advance(lex);
                tok = make_token(TOK_REDIR_OUT,
                                 arena_strdup(lex->arena, ">"),
                                 tok_line, tok_col);
            }
        } else {
            /* op == '<' */
            if (lex_peek(lex, 1) == '<') {
                lex_advance(lex);
                lex_advance(lex);
                tok = make_token(TOK_REDIR_HEREDOC,
                                 arena_strdup(lex->arena, "<<"),
                                 tok_line, tok_col);
            } else {
                lex_advance(lex);
                tok = make_token(TOK_REDIR_IN,
                                 arena_strdup(lex->arena, "<"),
                                 tok_line, tok_col);
            }
        }
        tok.redir_fd = fd;
        return tok;
    }

    /* ---- Multi-character operators (check longer matches first) ---- */
    char next = lex_peek(lex, 1);

    if (c == '|' && next == '|') {
        lex_advance(lex);
        lex_advance(lex);
        return make_token(TOK_OR, arena_strdup(lex->arena, "||"),
                          tok_line, tok_col);
    }
    if (c == '&' && next == '&') {
        lex_advance(lex);
        lex_advance(lex);
        return make_token(TOK_AND, arena_strdup(lex->arena, "&&"),
                          tok_line, tok_col);
    }
    if (c == '>' && next == '>') {
        lex_advance(lex);
        lex_advance(lex);
        return make_token(TOK_REDIR_APPEND, arena_strdup(lex->arena, ">>"),
                          tok_line, tok_col);
    }
    if (c == '<' && next == '<') {
        lex_advance(lex);
        lex_advance(lex);
        return make_token(TOK_REDIR_HEREDOC, arena_strdup(lex->arena, "<<"),
                          tok_line, tok_col);
    }

    /* ---- Single-character operators ---- */
    if (c == '|') {
        lex_advance(lex);
        return make_token(TOK_PIPE, arena_strdup(lex->arena, "|"),
                          tok_line, tok_col);
    }
    if (c == '&') {
        lex_advance(lex);
        return make_token(TOK_AMP, arena_strdup(lex->arena, "&"),
                          tok_line, tok_col);
    }
    if (c == ';') {
        lex_advance(lex);
        return make_token(TOK_SEMI, arena_strdup(lex->arena, ";"),
                          tok_line, tok_col);
    }
    if (c == '>') {
        lex_advance(lex);
        return make_token(TOK_REDIR_OUT, arena_strdup(lex->arena, ">"),
                          tok_line, tok_col);
    }
    if (c == '<') {
        lex_advance(lex);
        return make_token(TOK_REDIR_IN, arena_strdup(lex->arena, "<"),
                          tok_line, tok_col);
    }
    if (c == '(') {
        lex_advance(lex);
        return make_token(TOK_LPAREN, arena_strdup(lex->arena, "("),
                          tok_line, tok_col);
    }
    if (c == ')') {
        lex_advance(lex);
        return make_token(TOK_RPAREN, arena_strdup(lex->arena, ")"),
                          tok_line, tok_col);
    }
    if (c == '{') {
        lex_advance(lex);
        return make_token(TOK_LBRACE, arena_strdup(lex->arena, "{"),
                          tok_line, tok_col);
    }
    if (c == '}') {
        lex_advance(lex);
        return make_token(TOK_RBRACE, arena_strdup(lex->arena, "}"),
                          tok_line, tok_col);
    }
    if (c == '!') {
        lex_advance(lex);
        return make_token(TOK_BANG, arena_strdup(lex->arena, "!"),
                          tok_line, tok_col);
    }

    /* ---- Word (plain text, quoted strings, etc.) ---- */
    return build_word(lex);
}

Token lexer_peek(Lexer *lex)
{
    /* Save the lexer state */
    int saved_pos  = lex->pos;
    int saved_line = lex->line;
    int saved_col  = lex->col;

    Token tok = lexer_next(lex);

    /* Restore the lexer state */
    lex->pos  = saved_pos;
    lex->line = saved_line;
    lex->col  = saved_col;

    return tok;
}

TokenList *lexer_tokenize(Lexer *lex)
{
    TokenList *list = toklist_new(lex->arena);
    if (!list)
        return NULL;

    for (;;) {
        Token tok = lexer_next(lex);
        if (!toklist_add(list, tok, lex->arena))
            return NULL;
        if (tok.type == TOK_EOF)
            break;
        if (lex->error)
            break;
    }

    return list;
}

bool token_is_keyword(TokenType type)
{
    return type >= TOK_IF && type <= TOK_LOCAL;
}

const char *token_type_str(TokenType type)
{
    switch (type) {
    case TOK_WORD:          return "WORD";
    case TOK_PIPE:          return "PIPE";
    case TOK_AND:           return "AND";
    case TOK_OR:            return "OR";
    case TOK_SEMI:          return "SEMI";
    case TOK_AMP:           return "AMP";
    case TOK_REDIR_IN:      return "REDIR_IN";
    case TOK_REDIR_OUT:     return "REDIR_OUT";
    case TOK_REDIR_APPEND:  return "REDIR_APPEND";
    case TOK_REDIR_HEREDOC: return "REDIR_HEREDOC";
    case TOK_REDIR_DUP:     return "REDIR_DUP";
    case TOK_LPAREN:        return "LPAREN";
    case TOK_RPAREN:        return "RPAREN";
    case TOK_NEWLINE:       return "NEWLINE";
    case TOK_IF:            return "IF";
    case TOK_THEN:          return "THEN";
    case TOK_ELIF:          return "ELIF";
    case TOK_ELSE:          return "ELSE";
    case TOK_FI:            return "FI";
    case TOK_WHILE:         return "WHILE";
    case TOK_FOR:           return "FOR";
    case TOK_DO:            return "DO";
    case TOK_DONE:          return "DONE";
    case TOK_IN:            return "IN";
    case TOK_FUNCTION:      return "FUNCTION";
    case TOK_RETURN:        return "RETURN";
    case TOK_LOCAL:         return "LOCAL";
    case TOK_LBRACE:        return "LBRACE";
    case TOK_RBRACE:        return "RBRACE";
    case TOK_BANG:          return "BANG";
    case TOK_EOF:           return "EOF";
    }
    return "UNKNOWN";
}
