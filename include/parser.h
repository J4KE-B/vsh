/* ============================================================================
 * vsh - Vanguard Shell
 * parser.h - Recursive descent parser producing an AST
 *
 * Grammar:
 *   program     → list EOF
 *   list        → pipeline ((';' | '&' | '&&' | '||') pipeline)* [';' | '&']
 *   pipeline    → ['!'] command ('|' command)*
 *   command     → simple_cmd | compound_cmd | function_def
 *   simple_cmd  → (assignment | redirection | WORD)+
 *   compound_cmd→ if_cmd | while_cmd | for_cmd | '{' list '}' | '(' list ')'
 *   if_cmd      → 'if' list 'then' list ('elif' list 'then' list)* ['else' list] 'fi'
 *   while_cmd   → 'while' list 'do' list 'done'
 *   for_cmd     → 'for' WORD ['in' WORD*] ';'|NL 'do' list 'done'
 *   function_def→ WORD '(' ')' '{' list '}'
 * ============================================================================ */

#ifndef VSH_PARSER_H
#define VSH_PARSER_H

#include "lexer.h"
#include <stdbool.h>

typedef struct Arena Arena;

/* ---- AST Node Types ----------------------------------------------------- */
typedef enum ASTNodeType {
    NODE_COMMAND,      /* Simple command: name + args + redirections */
    NODE_PIPELINE,     /* cmd1 | cmd2 | cmd3 */
    NODE_AND,          /* left && right */
    NODE_OR,           /* left || right */
    NODE_SEQUENCE,     /* left ; right */
    NODE_BACKGROUND,   /* command & */
    NODE_NEGATE,       /* ! command */
    NODE_SUBSHELL,     /* ( list ) */
    NODE_IF,           /* if/then/elif/else/fi */
    NODE_WHILE,        /* while/do/done */
    NODE_FOR,          /* for/in/do/done */
    NODE_FUNCTION,     /* function definition */
    NODE_BLOCK,        /* { list } */
} ASTNodeType;

/* ---- Redirection -------------------------------------------------------- */
typedef enum RedirType {
    REDIR_INPUT,       /* < file */
    REDIR_OUTPUT,      /* > file */
    REDIR_APPEND,      /* >> file */
    REDIR_HEREDOC,     /* << DELIM */
    REDIR_DUP_OUT,     /* >&N */
    REDIR_DUP_IN,      /* <&N */
} RedirType;

typedef struct Redirection {
    RedirType    type;
    int          fd;       /* Source fd (-1 for default: 0 for input, 1 for output) */
    char        *target;   /* Filename or fd number as string */
    struct Redirection *next;
} Redirection;

/* ---- AST Nodes ---------------------------------------------------------- */

/* Simple command: argv + redirections */
typedef struct CommandNode {
    char       **argv;      /* Null-terminated argument array */
    int          argc;      /* Number of arguments */
    Redirection *redirs;    /* Linked list of redirections */
    char       **assignments; /* VAR=value assignments before command */
    int          nassign;
} CommandNode;

/* Pipeline: array of commands connected by pipes */
typedef struct PipelineNode {
    struct ASTNode **commands;
    int              count;
    bool             negated; /* ! prefix */
} PipelineNode;

/* Binary operator (AND, OR, SEQUENCE) */
typedef struct BinaryNode {
    struct ASTNode *left;
    struct ASTNode *right;
} BinaryNode;

/* If statement */
typedef struct IfNode {
    struct ASTNode *condition;
    struct ASTNode *then_body;
    struct ASTNode *else_body;  /* NULL or another IfNode (elif) or body (else) */
} IfNode;

/* While loop */
typedef struct WhileNode {
    struct ASTNode *condition;
    struct ASTNode *body;
} WhileNode;

/* For loop */
typedef struct ForNode {
    char           *varname;
    char          **words;     /* Words to iterate over */
    int             nwords;
    struct ASTNode *body;
} ForNode;

/* Function definition */
typedef struct FunctionNode {
    char           *name;
    struct ASTNode *body;
} FunctionNode;

/* ---- The AST Node ------------------------------------------------------- */
typedef struct ASTNode {
    ASTNodeType type;
    union {
        CommandNode   cmd;
        PipelineNode  pipeline;
        BinaryNode    binary;
        IfNode        if_node;
        WhileNode     while_node;
        ForNode       for_node;
        FunctionNode  func;
        struct ASTNode *child;  /* For BACKGROUND, NEGATE, SUBSHELL, BLOCK */
    };
} ASTNode;

/* ---- Parser ------------------------------------------------------------- */
typedef struct Parser {
    TokenList *tokens;
    int        pos;
    Arena     *arena;
    char      *error;       /* Error message (arena-allocated) */
    bool       had_error;
} Parser;

/* Initialize the parser */
void parser_init(Parser *parser, TokenList *tokens, Arena *arena);

/* Parse the token stream into an AST */
ASTNode *parser_parse(Parser *parser);

/* Get the error message (if parsing failed) */
const char *parser_error(const Parser *parser);

/* Debug: print the AST (for development) */
void ast_print(const ASTNode *node, int indent);

#endif /* VSH_PARSER_H */
