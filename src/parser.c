/* ============================================================================
 * vsh - Vanguard Shell
 * parser.c - Recursive descent parser producing an AST
 *
 * Converts a token stream (from the lexer) into an abstract syntax tree.
 * The parser uses recursive descent following the shell grammar defined in
 * parser.h. All AST nodes and auxiliary storage are arena-allocated so the
 * entire tree is freed in one shot after execution.
 * ============================================================================ */

#include "parser.h"
#include "arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Token helpers ------------------------------------------------------ */

/* Return the current token without advancing. */
static Token *cur_token(Parser *parser)
{
    if (parser->pos >= parser->tokens->count)
        return &parser->tokens->tokens[parser->tokens->count - 1]; /* EOF */
    return &parser->tokens->tokens[parser->pos];
}

/* Advance past the current token, returning a pointer to the one consumed. */
static Token *advance(Parser *parser)
{
    Token *tok = cur_token(parser);
    if (parser->pos < parser->tokens->count)
        parser->pos++;
    return tok;
}

/* Return true if the current token matches the given type. */
static bool check(Parser *parser, TokenType type)
{
    return cur_token(parser)->type == type;
}

/* If the current token matches, advance and return true; else false. */
static bool match(Parser *parser, TokenType type)
{
    if (check(parser, type)) {
        advance(parser);
        return true;
    }
    return false;
}

/* Format an error message with line/col info and set the error flag. */
static void parser_error_at(Parser *parser, Token *tok, const char *msg)
{
    if (parser->had_error)
        return; /* keep the first error */

    parser->had_error = true;

    char buf[256];
    if (tok->type == TOK_EOF) {
        snprintf(buf, sizeof(buf), "parse error at end of input: %s", msg);
    } else {
        snprintf(buf, sizeof(buf), "parse error at line %d col %d near '%s': %s",
                 tok->line, tok->col,
                 tok->value ? tok->value : token_type_str(tok->type),
                 msg);
    }
    parser->error = arena_strdup(parser->arena, buf);
}

/* If the current token matches, advance and return the consumed token.
 * Otherwise set an error and return NULL. */
static Token *expect(Parser *parser, TokenType type)
{
    if (check(parser, type))
        return advance(parser);

    char buf[128];
    snprintf(buf, sizeof(buf), "expected '%s'", token_type_str(type));
    parser_error_at(parser, cur_token(parser), buf);
    return NULL;
}

/* Skip any NEWLINE tokens at the current position. */
static void skip_newlines(Parser *parser)
{
    while (check(parser, TOK_NEWLINE))
        advance(parser);
}

/* ---- Node constructors (arena-allocated) -------------------------------- */

static ASTNode *make_command_node(Arena *arena)
{
    ASTNode *node = arena_calloc(arena, 1, sizeof(ASTNode));
    node->type = NODE_COMMAND;
    return node;
}

static ASTNode *make_pipeline_node(Arena *arena)
{
    ASTNode *node = arena_calloc(arena, 1, sizeof(ASTNode));
    node->type = NODE_PIPELINE;
    return node;
}

static ASTNode *make_binary_node(Arena *arena, ASTNodeType type,
                                 ASTNode *left, ASTNode *right)
{
    ASTNode *node = arena_calloc(arena, 1, sizeof(ASTNode));
    node->type = type;
    node->binary.left = left;
    node->binary.right = right;
    return node;
}

static ASTNode *make_background_node(Arena *arena, ASTNode *child)
{
    ASTNode *node = arena_calloc(arena, 1, sizeof(ASTNode));
    node->type = NODE_BACKGROUND;
    node->child = child;
    return node;
}

static ASTNode *make_if_node(Arena *arena)
{
    ASTNode *node = arena_calloc(arena, 1, sizeof(ASTNode));
    node->type = NODE_IF;
    return node;
}

static ASTNode *make_while_node(Arena *arena)
{
    ASTNode *node = arena_calloc(arena, 1, sizeof(ASTNode));
    node->type = NODE_WHILE;
    return node;
}

static ASTNode *make_for_node(Arena *arena)
{
    ASTNode *node = arena_calloc(arena, 1, sizeof(ASTNode));
    node->type = NODE_FOR;
    return node;
}

static ASTNode *make_function_node(Arena *arena)
{
    ASTNode *node = arena_calloc(arena, 1, sizeof(ASTNode));
    node->type = NODE_FUNCTION;
    return node;
}

static ASTNode *make_subshell_node(Arena *arena, ASTNode *child)
{
    ASTNode *node = arena_calloc(arena, 1, sizeof(ASTNode));
    node->type = NODE_SUBSHELL;
    node->child = child;
    return node;
}

static ASTNode *make_block_node(Arena *arena, ASTNode *child)
{
    ASTNode *node = arena_calloc(arena, 1, sizeof(ASTNode));
    node->type = NODE_BLOCK;
    node->child = child;
    return node;
}

/* ---- Forward declarations ----------------------------------------------- */

static ASTNode *parse_list(Parser *parser);
static ASTNode *parse_pipeline(Parser *parser);
static ASTNode *parse_command(Parser *parser);
static ASTNode *parse_simple_cmd(Parser *parser);
static ASTNode *parse_if(Parser *parser);
static ASTNode *parse_while(Parser *parser);
static ASTNode *parse_for(Parser *parser);
static ASTNode *parse_function(Parser *parser);
static void     parse_redirection(Parser *parser, CommandNode *cmd);

/* ---- Predicate helpers -------------------------------------------------- */

/* Return true if the token type is a redirection operator. */
static bool is_redir(TokenType t)
{
    return t == TOK_REDIR_IN || t == TOK_REDIR_OUT ||
           t == TOK_REDIR_APPEND || t == TOK_REDIR_HEREDOC ||
           t == TOK_REDIR_DUP;
}

/* Return true if the current token can start a command. */
static bool at_command_start(Parser *parser)
{
    TokenType t = cur_token(parser)->type;
    return t == TOK_WORD   || t == TOK_IF    || t == TOK_WHILE  ||
           t == TOK_FOR    || t == TOK_LBRACE || t == TOK_LPAREN ||
           t == TOK_FUNCTION || t == TOK_BANG || is_redir(t);
}

/* ---- Parse functions ---------------------------------------------------- */

/*
 * parse_redirection - parse a redirection operator + target filename.
 *
 * Appends the new Redirection node to the front of cmd->redirs.
 */
static void parse_redirection(Parser *parser, CommandNode *cmd)
{
    Token *op = advance(parser);

    /* Map token type → redir type and default fd. */
    RedirType rtype;
    int default_fd;

    switch (op->type) {
    case TOK_REDIR_IN:
        rtype = REDIR_INPUT;
        default_fd = 0;
        break;
    case TOK_REDIR_OUT:
        rtype = REDIR_OUTPUT;
        default_fd = 1;
        break;
    case TOK_REDIR_APPEND:
        rtype = REDIR_APPEND;
        default_fd = 1;
        break;
    case TOK_REDIR_HEREDOC:
        rtype = REDIR_HEREDOC;
        default_fd = 0;
        break;
    case TOK_REDIR_DUP:
        /* Determine dup direction from the operator text. */
        if (op->value && op->value[0] == '<') {
            rtype = REDIR_DUP_IN;
            default_fd = 0;
        } else {
            rtype = REDIR_DUP_OUT;
            default_fd = 1;
        }
        break;
    default:
        parser_error_at(parser, op, "unexpected redirection operator");
        return;
    }

    /* Expect the target word. */
    Token *target = expect(parser, TOK_WORD);
    if (!target)
        return;

    Redirection *redir = arena_calloc(parser->arena, 1, sizeof(Redirection));
    redir->type   = rtype;
    redir->fd     = (op->redir_fd >= 0) ? op->redir_fd : default_fd;
    redir->target = arena_strdup(parser->arena, target->value);
    redir->next   = cmd->redirs;
    cmd->redirs   = redir;
}

/*
 * parse_simple_cmd - collect WORD tokens and redirections into a CommandNode.
 *
 * argv is arena-allocated, starting with capacity 8 and doubling as needed.
 */
static ASTNode *parse_simple_cmd(Parser *parser)
{
    ASTNode *node = make_command_node(parser->arena);
    CommandNode *cmd = &node->cmd;

    int cap = 8;
    cmd->argv = arena_alloc(parser->arena, cap * sizeof(char *));
    cmd->argc = 0;
    cmd->redirs = NULL;

    while (!parser->had_error) {
        TokenType t = cur_token(parser)->type;

        if (is_redir(t)) {
            parse_redirection(parser, cmd);
        } else if (t == TOK_WORD) {
            Token *tok = advance(parser);
            if (cmd->argc >= cap) {
                int newcap = cap * 2;
                char **newargv = arena_alloc(parser->arena,
                                             newcap * sizeof(char *));
                memcpy(newargv, cmd->argv, cmd->argc * sizeof(char *));
                cmd->argv = newargv;
                cap = newcap;
            }
            cmd->argv[cmd->argc++] = arena_strdup(parser->arena, tok->value);
        } else {
            break;
        }
    }

    /* Null-terminate the argv array. */
    if (cmd->argc >= cap) {
        int newcap = cap + 1;
        char **newargv = arena_alloc(parser->arena, newcap * sizeof(char *));
        memcpy(newargv, cmd->argv, cmd->argc * sizeof(char *));
        cmd->argv = newargv;
    }
    cmd->argv[cmd->argc] = NULL;

    if (cmd->argc == 0 && cmd->redirs == NULL) {
        parser_error_at(parser, cur_token(parser), "expected a command");
        return NULL;
    }

    return node;
}

/*
 * parse_if - parse an if/elif/else/fi compound command.
 *
 * The 'if' (or 'elif') keyword has already been matched by the caller when
 * this is invoked for the initial 'if'. For elif chains we recurse.
 */
static ASTNode *parse_if(Parser *parser)
{
    /* The 'if' keyword was already consumed by parse_command. */
    skip_newlines(parser);

    ASTNode *node = make_if_node(parser->arena);

    /* Parse the condition list. */
    node->if_node.condition = parse_list(parser);
    if (parser->had_error) return NULL;

    skip_newlines(parser);
    if (!expect(parser, TOK_THEN)) return NULL;
    skip_newlines(parser);

    /* Parse the then body. */
    node->if_node.then_body = parse_list(parser);
    if (parser->had_error) return NULL;

    skip_newlines(parser);

    /* Handle elif → create a nested IfNode in else_body. */
    if (check(parser, TOK_ELIF)) {
        advance(parser); /* consume 'elif' */
        skip_newlines(parser);

        ASTNode *elif_node = make_if_node(parser->arena);

        elif_node->if_node.condition = parse_list(parser);
        if (parser->had_error) return NULL;

        skip_newlines(parser);
        if (!expect(parser, TOK_THEN)) return NULL;
        skip_newlines(parser);

        elif_node->if_node.then_body = parse_list(parser);
        if (parser->had_error) return NULL;

        skip_newlines(parser);

        /* Recursively handle further elif/else chains. */
        if (check(parser, TOK_ELIF)) {
            /* Wrap remaining elif chain: pretend we saw 'if' again. */
            advance(parser);
            ASTNode *rest = make_if_node(parser->arena);

            /* We need to re-parse as if this is a nested if. Reuse code
             * by backing up and recursing, but that's messy. Instead, do
             * inline recursive elif parsing. */
            skip_newlines(parser);
            rest->if_node.condition = parse_list(parser);
            if (parser->had_error) return NULL;
            skip_newlines(parser);
            if (!expect(parser, TOK_THEN)) return NULL;
            skip_newlines(parser);
            rest->if_node.then_body = parse_list(parser);
            if (parser->had_error) return NULL;
            skip_newlines(parser);

            /* Continue the chain by letting the outermost elif_node handle
             * the rest. This gets complicated -- let's simplify by using a
             * loop-based approach instead. */
            elif_node->if_node.else_body = rest;

            /* Handle remaining elif/else for the rest node. */
            while (check(parser, TOK_ELIF)) {
                advance(parser);
                skip_newlines(parser);
                ASTNode *next = make_if_node(parser->arena);
                next->if_node.condition = parse_list(parser);
                if (parser->had_error) return NULL;
                skip_newlines(parser);
                if (!expect(parser, TOK_THEN)) return NULL;
                skip_newlines(parser);
                next->if_node.then_body = parse_list(parser);
                if (parser->had_error) return NULL;
                skip_newlines(parser);
                rest->if_node.else_body = next;
                rest = next;
            }

            if (check(parser, TOK_ELSE)) {
                advance(parser);
                skip_newlines(parser);
                rest->if_node.else_body = parse_list(parser);
                if (parser->had_error) return NULL;
                skip_newlines(parser);
            }
        } else if (check(parser, TOK_ELSE)) {
            advance(parser);
            skip_newlines(parser);
            elif_node->if_node.else_body = parse_list(parser);
            if (parser->had_error) return NULL;
            skip_newlines(parser);
        }

        node->if_node.else_body = elif_node;
    } else if (check(parser, TOK_ELSE)) {
        advance(parser); /* consume 'else' */
        skip_newlines(parser);
        node->if_node.else_body = parse_list(parser);
        if (parser->had_error) return NULL;
        skip_newlines(parser);
    }

    if (!expect(parser, TOK_FI)) return NULL;

    return node;
}

/*
 * parse_while - parse a while/do/done loop.
 */
static ASTNode *parse_while(Parser *parser)
{
    /* 'while' already consumed by parse_command. */
    skip_newlines(parser);

    ASTNode *node = make_while_node(parser->arena);

    node->while_node.condition = parse_list(parser);
    if (parser->had_error) return NULL;

    skip_newlines(parser);
    if (!expect(parser, TOK_DO)) return NULL;
    skip_newlines(parser);

    node->while_node.body = parse_list(parser);
    if (parser->had_error) return NULL;

    skip_newlines(parser);
    if (!expect(parser, TOK_DONE)) return NULL;

    return node;
}

/*
 * parse_for - parse a for/in/do/done loop.
 *
 *   for WORD [in WORD* (';'|NL)] do list done
 */
static ASTNode *parse_for(Parser *parser)
{
    /* 'for' already consumed by parse_command. */
    skip_newlines(parser);

    ASTNode *node = make_for_node(parser->arena);

    Token *var = expect(parser, TOK_WORD);
    if (!var) return NULL;
    node->for_node.varname = arena_strdup(parser->arena, var->value);

    /* Optional word list: 'in' WORD* (';' | NEWLINE) */
    skip_newlines(parser);
    if (check(parser, TOK_IN)) {
        advance(parser); /* consume 'in' */

        /* Collect words until ';', NEWLINE, or 'do'. */
        int cap = 8;
        char **words = arena_alloc(parser->arena, cap * sizeof(char *));
        int nwords = 0;

        while (check(parser, TOK_WORD)) {
            Token *w = advance(parser);
            if (nwords >= cap) {
                int newcap = cap * 2;
                char **nw = arena_alloc(parser->arena,
                                        newcap * sizeof(char *));
                memcpy(nw, words, nwords * sizeof(char *));
                words = nw;
                cap = newcap;
            }
            words[nwords++] = arena_strdup(parser->arena, w->value);
        }

        node->for_node.words = words;
        node->for_node.nwords = nwords;

        /* Consume optional ';' or NEWLINE separating word list from 'do'. */
        if (check(parser, TOK_SEMI) || check(parser, TOK_NEWLINE))
            advance(parser);
    }

    skip_newlines(parser);
    if (!expect(parser, TOK_DO)) return NULL;
    skip_newlines(parser);

    node->for_node.body = parse_list(parser);
    if (parser->had_error) return NULL;

    skip_newlines(parser);
    if (!expect(parser, TOK_DONE)) return NULL;

    return node;
}

/*
 * parse_function - parse a function definition.
 *
 * Two forms:
 *   name '(' ')' '{' list '}'
 *   'function' name ['(' ')'] '{' list '}'
 *
 * The caller has already determined which form we're in:
 *   - For 'function' keyword form: TOK_FUNCTION was consumed by parse_command.
 *   - For name() form: the WORD was consumed and passed via the name parameter.
 */
static ASTNode *parse_function_body(Parser *parser, const char *name)
{
    ASTNode *node = make_function_node(parser->arena);
    node->func.name = arena_strdup(parser->arena, name);

    skip_newlines(parser);
    if (!expect(parser, TOK_LBRACE)) return NULL;
    skip_newlines(parser);

    node->func.body = parse_list(parser);
    if (parser->had_error) return NULL;

    skip_newlines(parser);
    if (!expect(parser, TOK_RBRACE)) return NULL;

    return node;
}

static ASTNode *parse_function(Parser *parser)
{
    /* 'function' keyword was already consumed. */
    Token *name_tok = expect(parser, TOK_WORD);
    if (!name_tok) return NULL;

    /* Optional '(' ')' */
    if (check(parser, TOK_LPAREN)) {
        advance(parser);
        if (!expect(parser, TOK_RPAREN)) return NULL;
    }

    return parse_function_body(parser, name_tok->value);
}

/*
 * parse_command - dispatch to the correct parser based on the current token.
 *
 * Handles compound commands (if, while, for, brace groups, subshells,
 * function definitions) and falls through to parse_simple_cmd for
 * regular commands.
 */
static ASTNode *parse_command(Parser *parser)
{
    Token *tok = cur_token(parser);

    switch (tok->type) {
    case TOK_IF:
        advance(parser);
        return parse_if(parser);

    case TOK_WHILE:
        advance(parser);
        return parse_while(parser);

    case TOK_FOR:
        advance(parser);
        return parse_for(parser);

    case TOK_FUNCTION:
        advance(parser);
        return parse_function(parser);

    case TOK_LBRACE: {
        advance(parser);
        skip_newlines(parser);
        ASTNode *body = parse_list(parser);
        if (parser->had_error) return NULL;
        skip_newlines(parser);
        if (!expect(parser, TOK_RBRACE)) return NULL;
        return make_block_node(parser->arena, body);
    }

    case TOK_LPAREN: {
        advance(parser);
        skip_newlines(parser);
        ASTNode *body = parse_list(parser);
        if (parser->had_error) return NULL;
        skip_newlines(parser);
        if (!expect(parser, TOK_RPAREN)) return NULL;
        return make_subshell_node(parser->arena, body);
    }

    case TOK_WORD: {
        /* Check for function definition: WORD '(' ')' '{' list '}' */
        if (parser->pos + 2 < parser->tokens->count) {
            Token *next1 = &parser->tokens->tokens[parser->pos + 1];
            Token *next2 = &parser->tokens->tokens[parser->pos + 2];
            if (next1->type == TOK_LPAREN && next2->type == TOK_RPAREN) {
                Token *name_tok = advance(parser);
                advance(parser); /* consume '(' */
                advance(parser); /* consume ')' */
                return parse_function_body(parser, name_tok->value);
            }
        }
        return parse_simple_cmd(parser);
    }

    default:
        /* Redirections or other tokens that start a simple command. */
        if (is_redir(tok->type))
            return parse_simple_cmd(parser);

        parser_error_at(parser, tok, "unexpected token");
        return NULL;
    }
}

/*
 * parse_pipeline - parse one or more commands separated by '|'.
 *
 * Optionally prefixed with '!' for negation.
 * If there is only one command and no negation, returns the command
 * directly (avoids unnecessary pipeline wrapper).
 */
static ASTNode *parse_pipeline(Parser *parser)
{
    bool negated = false;
    if (check(parser, TOK_BANG)) {
        advance(parser);
        negated = true;
    }

    ASTNode *first = parse_command(parser);
    if (parser->had_error) return NULL;

    /* Check for pipe operators. */
    if (!check(parser, TOK_PIPE) && !negated)
        return first;

    /* We have a pipeline (or negation). Collect all commands. */
    int cap = 4;
    ASTNode **cmds = arena_alloc(parser->arena, cap * sizeof(ASTNode *));
    int count = 0;

    cmds[count++] = first;

    while (match(parser, TOK_PIPE)) {
        skip_newlines(parser);
        ASTNode *cmd = parse_command(parser);
        if (parser->had_error) return NULL;

        if (count >= cap) {
            int newcap = cap * 2;
            ASTNode **nc = arena_alloc(parser->arena,
                                       newcap * sizeof(ASTNode *));
            memcpy(nc, cmds, count * sizeof(ASTNode *));
            cmds = nc;
            cap = newcap;
        }
        cmds[count++] = cmd;
    }

    /* If only one command but negated, wrap in pipeline for the flag. */
    ASTNode *node = make_pipeline_node(parser->arena);
    node->pipeline.commands = cmds;
    node->pipeline.count = count;
    node->pipeline.negated = negated;

    return node;
}

/*
 * parse_list - parse a sequence of pipelines separated by control operators.
 *
 * Operators:
 *   ';'  / NEWLINE → SEQUENCE node
 *   '&'            → BACKGROUND wrapper, then SEQUENCE
 *   '&&'           → AND node
 *   '||'           → OR node
 *
 * Trailing ';', '&', or NEWLINE are consumed but do not require a
 * following pipeline.
 */
static ASTNode *parse_list(Parser *parser)
{
    skip_newlines(parser);

    if (!at_command_start(parser))
        return NULL;

    ASTNode *left = parse_pipeline(parser);
    if (parser->had_error) return NULL;

    for (;;) {
        if (check(parser, TOK_AND)) {
            advance(parser);
            skip_newlines(parser);
            if (!at_command_start(parser))
                break;
            ASTNode *right = parse_pipeline(parser);
            if (parser->had_error) return NULL;
            left = make_binary_node(parser->arena, NODE_AND, left, right);
        } else if (check(parser, TOK_OR)) {
            advance(parser);
            skip_newlines(parser);
            if (!at_command_start(parser))
                break;
            ASTNode *right = parse_pipeline(parser);
            if (parser->had_error) return NULL;
            left = make_binary_node(parser->arena, NODE_OR, left, right);
        } else if (check(parser, TOK_AMP)) {
            advance(parser);
            left = make_background_node(parser->arena, left);
            skip_newlines(parser);
            if (!at_command_start(parser))
                break;
            ASTNode *right = parse_pipeline(parser);
            if (parser->had_error) return NULL;
            left = make_binary_node(parser->arena, NODE_SEQUENCE, left, right);
        } else if (check(parser, TOK_SEMI) || check(parser, TOK_NEWLINE)) {
            advance(parser);
            skip_newlines(parser);
            if (!at_command_start(parser))
                break;
            ASTNode *right = parse_pipeline(parser);
            if (parser->had_error) return NULL;
            left = make_binary_node(parser->arena, NODE_SEQUENCE, left, right);
        } else {
            break;
        }
    }

    return left;
}

/*
 * parse_program - entry point for parsing a complete program.
 *
 * Parses a list and expects TOK_EOF at the end.
 */
static ASTNode *parse_program(Parser *parser)
{
    skip_newlines(parser);

    ASTNode *root = NULL;
    if (!check(parser, TOK_EOF)) {
        root = parse_list(parser);
        if (parser->had_error) return NULL;
    }

    skip_newlines(parser);
    if (!check(parser, TOK_EOF)) {
        parser_error_at(parser, cur_token(parser),
                        "unexpected token after end of command");
        return NULL;
    }

    return root;
}

/* ---- Public API --------------------------------------------------------- */

/*
 * parser_init - initialize the parser state.
 */
void parser_init(Parser *parser, TokenList *tokens, Arena *arena)
{
    parser->tokens    = tokens;
    parser->pos       = 0;
    parser->arena     = arena;
    parser->error     = NULL;
    parser->had_error = false;
}

/*
 * parser_parse - tokenize and parse, returning the AST root (or NULL on error).
 */
ASTNode *parser_parse(Parser *parser)
{
    return parse_program(parser);
}

/*
 * parser_error - return the error message string, or NULL if no error.
 */
const char *parser_error(const Parser *parser)
{
    return parser->error;
}

/* ---- Debug AST printer -------------------------------------------------- */

static const char *node_type_str(ASTNodeType type)
{
    switch (type) {
    case NODE_COMMAND:    return "COMMAND";
    case NODE_PIPELINE:   return "PIPELINE";
    case NODE_AND:        return "AND";
    case NODE_OR:         return "OR";
    case NODE_SEQUENCE:   return "SEQUENCE";
    case NODE_BACKGROUND: return "BACKGROUND";
    case NODE_NEGATE:     return "NEGATE";
    case NODE_SUBSHELL:   return "SUBSHELL";
    case NODE_IF:         return "IF";
    case NODE_WHILE:      return "WHILE";
    case NODE_FOR:        return "FOR";
    case NODE_FUNCTION:   return "FUNCTION";
    case NODE_BLOCK:      return "BLOCK";
    }
    return "UNKNOWN";
}

static void print_indent(int indent)
{
    for (int i = 0; i < indent; i++)
        fprintf(stderr, "  ");
}

static const char *redir_type_str(RedirType type)
{
    switch (type) {
    case REDIR_INPUT:   return "<";
    case REDIR_OUTPUT:  return ">";
    case REDIR_APPEND:  return ">>";
    case REDIR_HEREDOC: return "<<";
    case REDIR_DUP_OUT: return ">&";
    case REDIR_DUP_IN:  return "<&";
    }
    return "?";
}

void ast_print(const ASTNode *node, int indent)
{
    if (!node) {
        print_indent(indent);
        fprintf(stderr, "(null)\n");
        return;
    }

    print_indent(indent);
    fprintf(stderr, "%s", node_type_str(node->type));

    switch (node->type) {
    case NODE_COMMAND:
        fprintf(stderr, " [");
        for (int i = 0; i < node->cmd.argc; i++) {
            if (i > 0) fprintf(stderr, ", ");
            fprintf(stderr, "'%s'", node->cmd.argv[i]);
        }
        fprintf(stderr, "]");
        if (node->cmd.redirs) {
            fprintf(stderr, " redirections:");
            for (Redirection *r = node->cmd.redirs; r; r = r->next) {
                fprintf(stderr, " %d%s%s", r->fd, redir_type_str(r->type),
                        r->target);
            }
        }
        fprintf(stderr, "\n");
        break;

    case NODE_PIPELINE:
        fprintf(stderr, "%s (%d commands)\n",
                node->pipeline.negated ? " (negated)" : "",
                node->pipeline.count);
        for (int i = 0; i < node->pipeline.count; i++)
            ast_print(node->pipeline.commands[i], indent + 1);
        break;

    case NODE_AND:
    case NODE_OR:
    case NODE_SEQUENCE:
        fprintf(stderr, "\n");
        ast_print(node->binary.left, indent + 1);
        ast_print(node->binary.right, indent + 1);
        break;

    case NODE_BACKGROUND:
    case NODE_NEGATE:
        fprintf(stderr, "\n");
        ast_print(node->child, indent + 1);
        break;

    case NODE_SUBSHELL:
    case NODE_BLOCK:
        fprintf(stderr, "\n");
        ast_print(node->child, indent + 1);
        break;

    case NODE_IF:
        fprintf(stderr, "\n");
        print_indent(indent + 1);
        fprintf(stderr, "condition:\n");
        ast_print(node->if_node.condition, indent + 2);
        print_indent(indent + 1);
        fprintf(stderr, "then:\n");
        ast_print(node->if_node.then_body, indent + 2);
        if (node->if_node.else_body) {
            print_indent(indent + 1);
            fprintf(stderr, "else:\n");
            ast_print(node->if_node.else_body, indent + 2);
        }
        break;

    case NODE_WHILE:
        fprintf(stderr, "\n");
        print_indent(indent + 1);
        fprintf(stderr, "condition:\n");
        ast_print(node->while_node.condition, indent + 2);
        print_indent(indent + 1);
        fprintf(stderr, "body:\n");
        ast_print(node->while_node.body, indent + 2);
        break;

    case NODE_FOR:
        fprintf(stderr, " var='%s'", node->for_node.varname);
        if (node->for_node.nwords > 0) {
            fprintf(stderr, " in [");
            for (int i = 0; i < node->for_node.nwords; i++) {
                if (i > 0) fprintf(stderr, ", ");
                fprintf(stderr, "'%s'", node->for_node.words[i]);
            }
            fprintf(stderr, "]");
        }
        fprintf(stderr, "\n");
        print_indent(indent + 1);
        fprintf(stderr, "body:\n");
        ast_print(node->for_node.body, indent + 2);
        break;

    case NODE_FUNCTION:
        fprintf(stderr, " name='%s'\n", node->func.name);
        print_indent(indent + 1);
        fprintf(stderr, "body:\n");
        ast_print(node->func.body, indent + 2);
        break;
    }
}
