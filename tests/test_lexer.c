/* ============================================================================
 * vsh - Vanguard Shell
 * test_lexer.c - Lexer/tokenizer tests
 * ============================================================================ */

#include "lexer.h"
#include "arena.h"
#include "test.h"

/* Safe string compare wrapper to avoid -Wnonnull with NULL in macros */
static int safe_strcmp(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : 1;
    return strcmp(a, b);
}

#define ASSERT_TOK_TYPE(tl, idx, expected_type) do { \
    ASSERT_TRUE((idx) < (tl)->count); \
    if ((idx) < (tl)->count) { \
        ASSERT_EQ((int)(tl)->tokens[(idx)].type, (int)(expected_type)); \
    } \
} while(0)

#define ASSERT_TOK_VAL(tl, idx, expected_type, expected_val) do { \
    ASSERT_TRUE((idx) < (tl)->count); \
    if ((idx) < (tl)->count) { \
        ASSERT_EQ((int)(tl)->tokens[(idx)].type, (int)(expected_type)); \
        ASSERT_TRUE(safe_strcmp((tl)->tokens[(idx)].value, (expected_val)) == 0); \
    } \
} while(0)

void test_lexer(void) {
    printf("\n--- Lexer ---\n");
    Arena *arena = arena_create();
    Lexer lex;
    TokenList *tl;

    /* Simple command */
    lexer_init(&lex, "ls -la /tmp", arena);
    tl = lexer_tokenize(&lex);
    ASSERT_TRUE(tl != NULL);
    ASSERT_EQ(tl->count, 4); /* ls, -la, /tmp, EOF */
    ASSERT_TOK_VAL(tl, 0, TOK_WORD, "ls");
    ASSERT_TOK_VAL(tl, 1, TOK_WORD, "-la");
    ASSERT_TOK_VAL(tl, 2, TOK_WORD, "/tmp");
    ASSERT_TOK_TYPE(tl, 3, TOK_EOF);

    /* Pipeline */
    arena_reset(arena);
    lexer_init(&lex, "cat file | grep foo | wc -l", arena);
    tl = lexer_tokenize(&lex);
    ASSERT_TRUE(tl != NULL);
    ASSERT_TOK_VAL(tl, 0, TOK_WORD, "cat");
    ASSERT_TOK_VAL(tl, 1, TOK_WORD, "file");
    ASSERT_TOK_TYPE(tl, 2, TOK_PIPE);
    ASSERT_TOK_VAL(tl, 3, TOK_WORD, "grep");
    ASSERT_TOK_VAL(tl, 4, TOK_WORD, "foo");
    ASSERT_TOK_TYPE(tl, 5, TOK_PIPE);
    ASSERT_TOK_VAL(tl, 6, TOK_WORD, "wc");
    ASSERT_TOK_VAL(tl, 7, TOK_WORD, "-l");

    /* Operators */
    arena_reset(arena);
    lexer_init(&lex, "a && b || c ; d &", arena);
    tl = lexer_tokenize(&lex);
    ASSERT_TRUE(tl != NULL);
    ASSERT_TOK_VAL(tl, 0, TOK_WORD, "a");
    ASSERT_TOK_TYPE(tl, 1, TOK_AND);
    ASSERT_TOK_VAL(tl, 2, TOK_WORD, "b");
    ASSERT_TOK_TYPE(tl, 3, TOK_OR);
    ASSERT_TOK_VAL(tl, 4, TOK_WORD, "c");
    ASSERT_TOK_TYPE(tl, 5, TOK_SEMI);
    ASSERT_TOK_VAL(tl, 6, TOK_WORD, "d");
    ASSERT_TOK_TYPE(tl, 7, TOK_AMP);

    /* Redirections */
    arena_reset(arena);
    lexer_init(&lex, "echo hello > out.txt 2>&1", arena);
    tl = lexer_tokenize(&lex);
    ASSERT_TRUE(tl != NULL);
    ASSERT_TOK_VAL(tl, 0, TOK_WORD, "echo");
    ASSERT_TOK_VAL(tl, 1, TOK_WORD, "hello");
    ASSERT_TOK_TYPE(tl, 2, TOK_REDIR_OUT);
    ASSERT_TOK_VAL(tl, 3, TOK_WORD, "out.txt");

    /* Single quotes (literal) */
    arena_reset(arena);
    lexer_init(&lex, "echo 'hello world'", arena);
    tl = lexer_tokenize(&lex);
    ASSERT_TRUE(tl != NULL);
    ASSERT_TOK_VAL(tl, 0, TOK_WORD, "echo");
    ASSERT_TOK_VAL(tl, 1, TOK_WORD, "hello world");

    /* Double quotes */
    arena_reset(arena);
    lexer_init(&lex, "echo \"hello world\"", arena);
    tl = lexer_tokenize(&lex);
    ASSERT_TRUE(tl != NULL);
    ASSERT_TOK_VAL(tl, 0, TOK_WORD, "echo");
    ASSERT_TOK_VAL(tl, 1, TOK_WORD, "hello world");

    /* Keywords */
    arena_reset(arena);
    lexer_init(&lex, "if true then echo yes fi", arena);
    tl = lexer_tokenize(&lex);
    ASSERT_TRUE(tl != NULL);
    ASSERT_TOK_VAL(tl, 0, TOK_IF, "if");
    ASSERT_TOK_VAL(tl, 1, TOK_WORD, "true");
    ASSERT_TOK_VAL(tl, 2, TOK_THEN, "then");
    ASSERT_TOK_VAL(tl, 3, TOK_WORD, "echo");
    ASSERT_TOK_VAL(tl, 4, TOK_WORD, "yes");
    ASSERT_TOK_VAL(tl, 5, TOK_FI, "fi");

    /* Comments */
    arena_reset(arena);
    lexer_init(&lex, "echo hello # this is a comment", arena);
    tl = lexer_tokenize(&lex);
    ASSERT_TRUE(tl != NULL);
    ASSERT_TOK_VAL(tl, 0, TOK_WORD, "echo");
    ASSERT_TOK_VAL(tl, 1, TOK_WORD, "hello");
    ASSERT_TOK_TYPE(tl, 2, TOK_EOF);

    /* Backslash escape */
    arena_reset(arena);
    lexer_init(&lex, "echo hello\\ world", arena);
    tl = lexer_tokenize(&lex);
    ASSERT_TRUE(tl != NULL);
    ASSERT_TOK_VAL(tl, 0, TOK_WORD, "echo");
    ASSERT_TOK_VAL(tl, 1, TOK_WORD, "hello world");

    /* Empty input */
    arena_reset(arena);
    lexer_init(&lex, "", arena);
    tl = lexer_tokenize(&lex);
    ASSERT_TRUE(tl != NULL);
    ASSERT_EQ(tl->count, 1);
    ASSERT_TOK_TYPE(tl, 0, TOK_EOF);

    /* Append redirection */
    arena_reset(arena);
    lexer_init(&lex, "echo hi >> log.txt", arena);
    tl = lexer_tokenize(&lex);
    ASSERT_TRUE(tl != NULL);
    ASSERT_TOK_VAL(tl, 0, TOK_WORD, "echo");
    ASSERT_TOK_VAL(tl, 1, TOK_WORD, "hi");
    ASSERT_TOK_TYPE(tl, 2, TOK_REDIR_APPEND);
    ASSERT_TOK_VAL(tl, 3, TOK_WORD, "log.txt");

    arena_destroy(arena);
    printf("  Lexer tests complete\n");
}
