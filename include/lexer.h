/* ============================================================================
 * vsh - Vanguard Shell
 * lexer.h - Tokenizer for shell input
 *
 * Converts raw input string into a stream of tokens, handling:
 * - Single and double quoting
 * - Backslash escapes
 * - Operators (|, &&, ||, ;, &, >, >>, <, <<)
 * - Variable references ($VAR, ${VAR})
 * ============================================================================ */

#ifndef VSH_LEXER_H
#define VSH_LEXER_H

#include <stddef.h>
#include <stdbool.h>

typedef struct Arena Arena;

typedef enum TokenType {
    TOK_WORD,          /* A plain word or quoted string */
    TOK_PIPE,          /* | */
    TOK_AND,           /* && */
    TOK_OR,            /* || */
    TOK_SEMI,          /* ; */
    TOK_AMP,           /* & (background) */
    TOK_REDIR_IN,      /* < */
    TOK_REDIR_OUT,     /* > */
    TOK_REDIR_APPEND,  /* >> */
    TOK_REDIR_HEREDOC, /* << */
    TOK_REDIR_DUP,     /* 2>&1 or >&2 etc */
    TOK_LPAREN,        /* ( */
    TOK_RPAREN,        /* ) */
    TOK_NEWLINE,       /* \n */
    TOK_IF,            /* if keyword */
    TOK_THEN,          /* then keyword */
    TOK_ELIF,          /* elif keyword */
    TOK_ELSE,          /* else keyword */
    TOK_FI,            /* fi keyword */
    TOK_WHILE,         /* while keyword */
    TOK_FOR,           /* for keyword */
    TOK_DO,            /* do keyword */
    TOK_DONE,          /* done keyword */
    TOK_IN,            /* in keyword */
    TOK_FUNCTION,      /* function keyword */
    TOK_RETURN,        /* return keyword */
    TOK_LOCAL,         /* local keyword */
    TOK_LBRACE,        /* { */
    TOK_RBRACE,        /* } */
    TOK_BANG,          /* ! (negation) */
    TOK_EOF            /* End of input */
} TokenType;

typedef struct Token {
    TokenType    type;
    char        *value;       /* Token text (arena-allocated) */
    int          redir_fd;    /* For redirections: the fd number (e.g., 2 in 2>) */
    int          line;        /* Source line number */
    int          col;         /* Source column number */
} Token;

/* Token array (dynamic, arena-allocated) */
typedef struct TokenList {
    Token  *tokens;
    int     count;
    int     capacity;
} TokenList;

typedef struct Lexer {
    const char *input;
    int         pos;
    int         len;
    int         line;
    int         col;
    Arena      *arena;
    char       *error;       /* Error message (arena-allocated) */
} Lexer;

/* Initialize the lexer with input and arena */
void lexer_init(Lexer *lex, const char *input, Arena *arena);

/* Tokenize the entire input, returns a TokenList */
TokenList *lexer_tokenize(Lexer *lex);

/* Get next token (for streaming use) */
Token lexer_next(Lexer *lex);

/* Peek at next token without consuming */
Token lexer_peek(Lexer *lex);

/* Check if a token type is a keyword */
bool token_is_keyword(TokenType type);

/* Get string representation of token type (for debugging) */
const char *token_type_str(TokenType type);

#endif /* VSH_LEXER_H */
