/* ============================================================================
 * vsh - Vanguard Shell
 * test_parser.c - Parser tests
 * ============================================================================ */

#include "parser.h"
#include "lexer.h"
#include "arena.h"
#include "test.h"

/* Helper: parse a string and return the AST */
static ASTNode *parse_str(const char *input, Arena *arena) {
    Lexer lex;
    lexer_init(&lex, input, arena);
    TokenList *tl = lexer_tokenize(&lex);
    if (!tl) return NULL;

    Parser parser;
    parser_init(&parser, tl, arena);
    return parser_parse(&parser);
}

void test_parser(void) {
    printf("\n--- Parser ---\n");
    Arena *arena = arena_create();
    ASTNode *ast;

    /* Simple command */
    ast = parse_str("ls -la /tmp", arena);
    ASSERT_TRUE(ast != NULL);
    if (ast) {
        ASSERT_EQ((int)ast->type, (int)NODE_COMMAND);
        ASSERT_EQ(ast->cmd.argc, 3);
        ASSERT_STR_EQ(ast->cmd.argv[0], "ls");
        ASSERT_STR_EQ(ast->cmd.argv[1], "-la");
        ASSERT_STR_EQ(ast->cmd.argv[2], "/tmp");
    }

    /* Pipeline */
    arena_reset(arena);
    ast = parse_str("cat file | grep foo", arena);
    ASSERT_TRUE(ast != NULL);
    if (ast) {
        ASSERT_EQ((int)ast->type, (int)NODE_PIPELINE);
        ASSERT_EQ(ast->pipeline.count, 2);
        ASSERT_EQ((int)ast->pipeline.commands[0]->type, (int)NODE_COMMAND);
        ASSERT_STR_EQ(ast->pipeline.commands[0]->cmd.argv[0], "cat");
        ASSERT_EQ((int)ast->pipeline.commands[1]->type, (int)NODE_COMMAND);
        ASSERT_STR_EQ(ast->pipeline.commands[1]->cmd.argv[0], "grep");
    }

    /* AND operator */
    arena_reset(arena);
    ast = parse_str("true && echo yes", arena);
    ASSERT_TRUE(ast != NULL);
    if (ast) {
        ASSERT_EQ((int)ast->type, (int)NODE_AND);
        ASSERT_TRUE(ast->binary.left != NULL);
        ASSERT_TRUE(ast->binary.right != NULL);
        ASSERT_EQ((int)ast->binary.left->type, (int)NODE_COMMAND);
        ASSERT_EQ((int)ast->binary.right->type, (int)NODE_COMMAND);
    }

    /* OR operator */
    arena_reset(arena);
    ast = parse_str("false || echo no", arena);
    ASSERT_TRUE(ast != NULL);
    if (ast) {
        ASSERT_EQ((int)ast->type, (int)NODE_OR);
    }

    /* Sequence (semicolon) */
    arena_reset(arena);
    ast = parse_str("echo a ; echo b", arena);
    ASSERT_TRUE(ast != NULL);
    if (ast) {
        ASSERT_EQ((int)ast->type, (int)NODE_SEQUENCE);
    }

    /* Background */
    arena_reset(arena);
    ast = parse_str("sleep 10 &", arena);
    ASSERT_TRUE(ast != NULL);
    if (ast) {
        ASSERT_EQ((int)ast->type, (int)NODE_BACKGROUND);
    }

    /* Redirections */
    arena_reset(arena);
    ast = parse_str("echo hello > out.txt", arena);
    ASSERT_TRUE(ast != NULL);
    if (ast) {
        ASSERT_EQ((int)ast->type, (int)NODE_COMMAND);
        ASSERT_TRUE(ast->cmd.redirs != NULL);
        ASSERT_EQ((int)ast->cmd.redirs->type, (int)REDIR_OUTPUT);
        ASSERT_STR_EQ(ast->cmd.redirs->target, "out.txt");
    }

    /* Input redirection */
    arena_reset(arena);
    ast = parse_str("sort < data.txt", arena);
    ASSERT_TRUE(ast != NULL);
    if (ast) {
        ASSERT_EQ((int)ast->type, (int)NODE_COMMAND);
        ASSERT_TRUE(ast->cmd.redirs != NULL);
        ASSERT_EQ((int)ast->cmd.redirs->type, (int)REDIR_INPUT);
    }

    /* Empty input - should return NULL (no commands) */
    arena_reset(arena);
    ast = parse_str("", arena);
    /* Empty input is valid, ast may be NULL */

    /* Complex pipeline */
    arena_reset(arena);
    ast = parse_str("cat file | grep foo | wc -l > count.txt", arena);
    ASSERT_TRUE(ast != NULL);
    if (ast) {
        ASSERT_EQ((int)ast->type, (int)NODE_PIPELINE);
        ASSERT_EQ(ast->pipeline.count, 3);
    }

    arena_destroy(arena);
    printf("  Parser tests complete\n");
}
